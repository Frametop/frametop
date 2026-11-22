#!/usr/bin/env python3
"""Offline test of the fit watch (gaze/gazecheck.py, GAZE_FIT_WATCH): the fit check keeping a
good fit as yours, the put-on check in the quick check that opens when the headset goes on
(holds the dot, says what to move, lets go once it's back or on a click), and the fit pill
with our tracker (gaze/fitpill.py: which way to nudge the headset from your saved fit, then
"Looks good"; from ft-eyes' glint jump without a saved fit; a calibration's seating saved as
your fit, and kept over a fit check's).

fit-coach-check-test.py's harness: ft-gazed's Service with its sockets renamed, HOME in a temp
folder, a fake pointer helper, a fake ft-gaze, a fake ft-eyes control socket (its finds from
the shipped reference, offset as the test says, and its status with the glint jump), and the
panel's commands recorded instead of sent. Safe next to the live gaze service.

  gaze/test/fit-watch-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-fit-watch-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE, and frametop.conf, follow HOME

import importlib.machinery  # noqa: E402
import importlib.util  # noqa: E402
import json  # noqa: E402
import selectors  # noqa: E402
import shutil  # noqa: E402
import socket  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.join(HERE, "..")
sys.path.insert(0, GAZE)
loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(GAZE, "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)
import fitcoach  # noqa: E402
import fitpill  # noqa: E402
import gazecheck  # noqa: E402  (the module ft-gazed imported)

tag = f"ft_gaze_fit_watch_test_{os.getpid()}"
gazed.ME = f"\0{tag}_gazed"
gazed.POINTER = gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.SCREENS = f"\0{tag}_screens"
gazecheck.PANEL = f"\0{tag}_panel"
gazed.EYES_SOCKET = gazecheck.EYES = f"\0{tag}_eyes"
SETTINGS = {"tracker": "steam"}   # SteamVR's tracker in use first, then ours
gazed.read_settings = lambda: (SETTINGS["tracker"], "auto", "auto", 55.0)
gazed.own_installed = lambda: True
gazed.TrackedEye = lambda: lambda now=None: None
logs = []
gazed.log = gazecheck.log = lambda msg: logs.append(msg)

# The fake ft-gaze: 90 samples a second, SteamVR sees both eyes, its gaze from LOOK.
LOOK = os.path.join(HOME, "look.json")
FAKE = os.path.join(HOME, "ft-gaze")


def look(yaw, pitch):
    with open(LOOK + ".new", "w") as f:
        json.dump([yaw, pitch], f)
    os.replace(LOOK + ".new", LOOK)


look(0.0, 0.0)
with open(FAKE, "w") as f:
    f.write(f'''import json, os, select, sys, time
eyes = False   # "sources" with left: each eye's reading too, the same as mmap1's
while True:
    if select.select([sys.stdin], [], [], 1 / 90)[0]:
        got = os.read(0, 4096)
        if not got:
            break
        for line in got.decode().splitlines():
            if line.startswith("sources "):
                eyes = "left" in line or "all" in line
    try:
        hy, hp = json.load(open({LOOK!r}))
    except (OSError, ValueError):
        continue
    src = {{"mmap1": {{"hy": hy, "hp": hp, "unc": [0.001, 0.001], "open": [0.8, 0.8]}}}}
    if eyes:
        src["left"] = src["right"] = {{"hy": hy, "hp": hp}}
    print(json.dumps({{"t": time.monotonic(), "src": src}}), flush=True)
''')
SLEEPER = [sys.executable, "-c", "import sys; sys.stdin.read()"]  # quits when its stdin closes
started = {"eyes": 0}


def start_helper(self):
    self.proc = subprocess.Popen([sys.executable, FAKE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
    self.proc_sources = self.wanted_sources()
    os.set_blocking(self.proc.stdout.fileno(), False)
    os.set_blocking(self.proc.stderr.fileno(), False)
    self.sel.register(self.proc.stdout, selectors.EVENT_READ, "stdout")
    self.sel.register(self.proc.stderr, selectors.EVENT_READ, "stderr")
    self.buf = b""


def start_eyes(self):
    started["eyes"] += 1
    self.eyes_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.eyes_proc.stderr.fileno(), False)
    self.sel.register(self.eyes_proc.stderr, selectors.EVENT_READ, "eyes")


def start_panel(self):
    self.panel_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.panel_proc.stderr.fileno(), False)
    self.sel.register(self.panel_proc.stderr, selectors.EVENT_READ, "panel")


panel = []
gazed.Service.start_helper = start_helper
gazed.Service.start_eyes = start_eyes
gazecheck.Checks.start_panel = start_panel
gazecheck.Checks.to_panel = lambda self, command: panel.append(command)

# The fake ft-eyes: "fit" answers with finds where the shipped reference puts eyes offset by
# eyes_state["offset"] (yaw, pitch degrees, both eyes alike).
REF = fitcoach.Reference.load()
eyes_state = {"offset": (0.0, 0.0), "jump": (0.0, 0.0), "asked": 0, "feature": "pupil"}
eyes = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
eyes.bind(gazed.EYES_SOCKET)
eyes.settimeout(0.2)


def finds():
    yaw, pitch = eyes_state["offset"]
    out = {}
    for cam in (0, 1):
        (cx, cy), ((a, b), (c, d)) = REF.eyes[cam]["centre"], REF.eyes[cam]["jacobian"]
        out[cam] = {"n": 90, "finds": 90, "found": 1.0, "x": cx + a * yaw + b * pitch, "y": cy + c * yaw + d * pitch,
                    "a": 28.0, "spacing": 15.0, "extra": 0.0}
    return out


def eyes_answer():
    while True:
        try:
            data, addr = eyes.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        w = data.decode().split()
        if w[0] == "status":
            yaw, pitch = eyes_state["jump"]   # the slip, as both pupils' move in degrees
            jump = {}
            for cam, name in ((0, "right"), (1, "left")):
                (a, b), (c, d) = REF.eyes[cam]["jacobian"]
                jump[name] = {"reseat": False, "jump": bool(yaw or pitch),
                              "jump_px": [a * yaw + b * pitch, c * yaw + d * pitch]}
            reply = json.dumps({"calibration": {"dots": 27}, "calibrating": False, "dots": 27, "eyes": jump,
                                "feature": eyes_state["feature"]})
        elif w[0] == "fit":
            eyes_state["asked"] += 1
            reply = json.dumps(finds())
        else:
            reply = f"fail unknown command {w[0]}"
        if addr:
            try:
                eyes.sendto(reply.encode(), addr)
            except OSError:
                pass  # the service gave up on it (as ft-eyes' serve() does)


threading.Thread(target=eyes_answer, daemon=True).start()

HELPER = {"gaze": False}
helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
helper.bind(gazed.POINTER)
helper.settimeout(0.2)


def helper_answer():
    while True:
        try:
            data, addr = helper.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        if data.startswith(b"gaze ?") and addr:
            helper.sendto(b"ok on worn" if HELPER["gaze"] else b"ok off worn", addr)


threading.Thread(target=helper_answer, daemon=True).start()
svc = gazed.Service(None, False, gazed.POINTER)
threading.Thread(target=svc.run, daemon=True).start()
ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
ctl.bind("")
ctl.settimeout(3)


def ask(cmd):
    ctl.sendto(cmd.encode(), gazed.ME)
    return ctl.recv(65536).decode()


failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def wait(cond, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def since(n):
    """The panel's commands from n on."""
    return panel[n:]


def quick(label):
    """Opens the quick check as the headset going on does; returns where its panel commands start."""
    n = len(panel)
    check(label, svc.checks.start("quick", gazecheck.PUTON_REASON), "ok")
    return n


def puton():
    return ((svc.checks.check or {}).get("puton") or {}).get("state")


def closed_with_capture(seconds=8):
    return wait(lambda: svc.checks.check is None and any(m.startswith("quick check done") for m in logs[mark:]), seconds)


ask("wake 60")
check("the tracker sends", wait(lambda: time.monotonic() - svc.checks.sample_at < 1, 6), True)
check("the eyes count as seen", wait(lambda: svc.checks.eyes_seen(), 3), True)

# The fit watch off (the default): the quick check is as it was.
mark = len(logs)
quick("fit watch off: a quick check opens")
check("  without a put-on check", puton(), None)
check("  and takes its dot", closed_with_capture(), True)

# A good fit in the fit check is kept as yours.
check("no saved fit yet", os.path.exists(fitcoach.BASELINE), False)
check("fitcheck is taken", ask("fitcheck").startswith("ok"), True)
check("the fit check opens", wait(lambda: (svc.checks.check or {}).get("kind") == "fit", 6), True)
check("it reads a good fit", wait(lambda: ((svc.checks.check or {}).get("coach") or {}).get("good") is not None, 6), True)
ask("calquit")
check("closing it saves the fit as yours", wait(lambda: os.path.exists(fitcoach.BASELINE), 2), True)
check("with both eyes", sorted(json.load(open(fitcoach.BASELINE))["eyes"]), ["0", "1"])

gazed.CONF.parent.mkdir(parents=True, exist_ok=True)
with open(gazed.CONF, "w") as f:
    f.write("GAZE_FIT_WATCH=1\n")

# Put on as saved: the dot is held for the reading, then taken.
mark = len(logs)
quick("put on as saved: the quick check opens")
check("  reading the fit first", puton(), "reading")
check("  sits as saved", wait(lambda: any("sits as your saved fit" in m for m in logs[mark:]), 5), True)
check("  then the dot is taken", closed_with_capture(), True)

# Put on low: it says to push it up, holds the dot past the quick check's timeout, lets go once back.
eyes_state["offset"] = (0.0, 12.0)
mark = len(logs)
n = quick("put on low: the quick check opens")
check("  the headset moved", wait(lambda: puton() == "moved", 5), True)
check("  the panel widens", "show five" in since(n), True)
check("  and says to push it up", wait(lambda: any(c.startswith("text Push the headset up") for c in since(n)), 2), True)
time.sleep(gazecheck.QUICK_TIMEOUT + 1)
check("  the dot waits past the quick check's timeout", (svc.checks.check or {}).get("captured"), 0)
eyes_state["offset"] = (0.0, 0.0)
check("  pushed back: let go", wait(lambda: any("is back as your saved fit" in m for m in logs[mark:]), 5), True)
check("  saying so", "note Good: it's back where it was" in since(n), True)
check("  then the dot is taken", closed_with_capture(), True)

# Put on low, then a click: taken as it sits.
eyes_state["offset"] = (0.0, 12.0)
mark = len(logs)
quick("put on low again")
check("  the headset moved", wait(lambda: puton() == "moved", 5), True)
ask("calaccept")
check("  a click lets go", wait(lambda: any("a click" in m for m in logs[mark:]), 3), True)
check("  and takes the dot", closed_with_capture(), True)
eyes_state["offset"] = (0.0, 0.0)

# The fit pill: our tracker, gaze mode on.
fitpill.AGAIN = 1.0   # (not 10 s between pills in a test)
SETTINGS["tracker"] = "own"
HELPER["gaze"] = True
with open(gazed.CONF, "w") as f:
    f.write("GAZE_FIT_WATCH=1\n# our tracker now\n")
check("our tracker in use", wait(lambda: svc.kind == "own" and svc.eyes_proc is not None, 6), True)
check("gaze mode on", wait(lambda: svc.checks.gaze_on is True, 4), True)
check("ft-eyes' status comes", wait(lambda: time.monotonic() - svc.own_at < 2, 4), True)

# The fit check's saved fit isn't a calibration's seating: the glint jump since the last click.
n = len(panel)
eyes_state["offset"] = (0.0, -12.0)   # (the pupils aren't what it reads)
time.sleep(fitpill.SHOW_HOLD + 2)
check("a fit check's saved fit: pupils off, no pill", "show pill" in since(n), False)
eyes_state["offset"] = (0.0, 0.0)
eyes_state["jump"] = (0.0, -12.0)
check("  the glint jump opens it", wait(lambda: "show pill" in since(n), fitpill.SHOW_HOLD + 4), True)
check("  says which way", "text Nudge down" in since(n), True)
eyes_state["jump"] = (0.0, 0.0)
check("  the jump gone: looks good", wait(lambda: "text Looks good" in since(n), 4), True)
check("  then it goes", wait(lambda: "hide" in since(n), fitpill.GOOD_SECONDS + 2), True)

# A full calibration's seating is saved as your fit, and a fit check keeps it.
os.makedirs(os.path.dirname(fitcoach.DOTS), exist_ok=True)
with open(fitcoach.DOTS, "w") as f:
    for yaw, pitch in [(0, 0), (15, 0), (-15, 0), (0, 9), (0, -9), (10, 6), (-10, -6), (10, -6)]:
        eye = {}
        for cam in (0, 1):
            (cx, cy), ((a, b), (c, d)) = REF.eyes[cam]["centre"], REF.eyes[cam]["jacobian"]
            eye[str(cam)] = {"pupil": [cx + a * yaw + b * pitch, cy + c * yaw + d * pitch], "mid": None}
        f.write(json.dumps({"made": "2026-11-22 09:05:53", "truth": [yaw, pitch], "eye": eye}) + "\n")
was = open(fitcoach.BASELINE).read()
eyes_state["feature"] = "net:test"
check("ft-eyes' status says the eye net", wait(lambda: svc.own.get("feature") == "net:test", 3), True)
mark = len(logs)
svc.checks.save_seating()
check("with the eye net, no seating saved", open(fitcoach.BASELINE).read(), was)
check("  saying why", any("needs the pupil finder" in m for m in logs[mark:]), True)
eyes_state["feature"] = "pupil"
check("  ft-eyes' status says the pupil finder again", wait(lambda: svc.own.get("feature") == "pupil", 3), True)
svc.checks.save_seating()
seat = fitcoach.load_baseline()
check("the calibration's seating saved", fitcoach.from_calibration(seat), True)
check("  with each eye's Jacobian", all(seat["eyes"][c].get("jacobian") for c in ("0", "1")), True)
mark = len(logs)
check("fitcheck is taken", ask("fitcheck").startswith("ok"), True)
check("it reads a good fit", wait(lambda: ((svc.checks.check or {}).get("coach") or {}).get("good") is not None, 6), True)
ask("calquit")
check("  closed", wait(lambda: svc.checks.check is None, 3), True)
check("the calibration's seating stays", fitcoach.from_calibration(fitcoach.load_baseline()), True)
check("  saying so", any("calibration's seating stays" in m for m in logs[mark:]), True)

# Against the seating: ft-eyes' pupils, read all the while.
n = len(panel)
asked = eyes_state["asked"]
check("  read all the while", wait(lambda: eyes_state["asked"] > asked + 2, 4), True)
check("sits as calibrated: no pill", "show pill" in since(n), False)
check("ft-gaze sends SteamVR's eyes for the looks", svc.proc_sources, "own,mmap1,left,right")
eyes_state["offset"] = (0.0, -12.0)   # the pupils sit low: the headset rode up
start = time.monotonic()
check("off: the pill", wait(lambda: "show pill" in since(n), fitpill.SHOW_HOLD + 4), True)
check("  not before SHOW_HOLD", time.monotonic() - start >= fitpill.SHOW_HOLD, True)
check("  says which way", "text Nudge down" in since(n), True)
check("  with its arrow", "mark down" in since(n), True)
check("  no pointer hiding for it", any(c == "calpanel 1" for c in since(n)), False)
check("  in the status", svc.checks.status()["fit_pill"], "Nudge down")
eyes_state["offset"] = (0.0, -4.0)
check("nudged partway: slightly", wait(lambda: "text Nudge slightly down" in since(n), 3), True)
eyes_state["offset"] = (0.0, 0.0)
check("back: looks good", wait(lambda: "text Looks good" in since(n), 4), True)
check("  with a tick", "mark good" in since(n), True)
check("  then it goes", wait(lambda: "hide" in since(n), fitpill.GOOD_SECONDS + 2), True)
eyes_state["offset"] = (-7.0, 0.0)
n = len(panel)
check("off to the side: the pill", wait(lambda: "show pill" in since(n), fitpill.SHOW_HOLD + 4), True)
check("  says which way", "text Nudge slightly right" in since(n), True)
HELPER["gaze"] = False
n = len(panel)
check("gaze mode off: it goes", wait(lambda: "hide" in since(n), 4), True)
eyes_state["offset"] = (0.0, 0.0)
HELPER["gaze"] = True
check("gaze mode on again", wait(lambda: svc.checks.gaze_on is True, 4), True)

# Looking aside: the reading follows SteamVR's gaze, so no pill.
n = len(panel)
look(12.0, -6.0)
eyes_state["offset"] = (12.0, -6.0)   # the pupils where the seating puts that look
asked = eyes_state["asked"]
time.sleep(fitpill.SHOW_HOLD + 2)
check("looking aside, seated: no pill", "show pill" in since(n), False)
check("  read all the while", eyes_state["asked"] > asked + 2, True)
look(25.0, 0.0)
eyes_state["offset"] = (25.0, -12.0)   # past PILL_CENTRE: not read, slipped or not
time.sleep(1.5)                        # (the window turns over to the new look)
asked = eyes_state["asked"]
time.sleep(fitpill.SHOW_HOLD + 1)
check("looking far aside: not read", eyes_state["asked"], asked)
check("  no pill", "show pill" in since(n), False)


class Fusion:   # SteamVR's affine: raw, it reads looks out from the centre too far
    def __init__(self, made):
        self.info = {"how": "calibration", "made": made}

    def steam_gaze(self, left, right, clicks=True):
        return (left[0] * 0.6, left[1] * 0.6)


made = fitcoach.load_baseline()["made"]
svc.fusion = Fusion(made)
look(20.0, 0.0)                       # raw past PILL_CENTRE; through the affine 12
eyes_state["offset"] = (12.0, 0.0)
svc.checks.pill_reading = None
check("SteamVR's look goes through its affine, so the reading is the seating's",
      wait(lambda: svc.checks.pill_reading is not None and abs(svc.checks.pill_reading[1]["left"]) < 0.5, 4), True)
svc.fusion = Fusion("2026-10-01 10:00:00")
look(10.0, 0.0)                       # an older calibration's affine: raw, 10 off the 12 it reads
time.sleep(1.5)
svc.checks.pill_reading = None
check("  not an older calibration's affine",
      wait(lambda: svc.checks.pill_reading is not None and abs(svc.checks.pill_reading[1]["left"] - 2.0) < 0.5, 4), True)
svc.fusion = None
look(0.0, 0.0)
eyes_state["offset"] = (0.0, 0.0)

# A check opening closes it.
eyes_state["offset"] = (0.0, -12.0)
n = len(panel)
check("off again: the pill", wait(lambda: "show pill" in since(n), fitpill.SHOW_HOLD + 4), True)
check("a fit check opens", ask("fitcheck").startswith("ok"), True)
check("  over the pill", wait(lambda: svc.checks.check is not None and not svc.checks.pill.shown, 3), True)
ask("calquit")
check("  closed", wait(lambda: svc.checks.check is None, 3), True)
eyes_state["offset"] = (0.0, 0.0)

# With the eye net, the glint jump even with a seating saved.
eyes_state["feature"] = "net:test"
wait(lambda: svc.own.get("feature") == "net:test", 3)
n = len(panel)
eyes_state["offset"] = (0.0, -12.0)
time.sleep(fitpill.SHOW_HOLD + 2)
check("the eye net: pupils off, no pill", "show pill" in since(n), False)
eyes_state["offset"] = (0.0, 0.0)
eyes_state["jump"] = (0.0, -12.0)
check("  the glint jump opens it", wait(lambda: "show pill" in since(n), fitpill.SHOW_HOLD + 4), True)
eyes_state["jump"] = (0.0, 0.0)
eyes_state["feature"] = "pupil"

print("all passed" if not failures else f"{len(failures)} failed: {failures}", flush=True)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(1 if failures else 0)
