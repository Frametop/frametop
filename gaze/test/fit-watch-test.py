#!/usr/bin/env python3
"""Offline test of the fit watch (gaze/gazecheck.py, GAZE_FIT_WATCH): the fit check keeping a
good fit as yours, the put-on check in the quick check that opens when the headset goes on
(holds the dot, says what to move, lets go once it's back or on a click), and the slip note
with our tracker (ft-eyes' glint jump past the threshold for SLIP_HOLD; gone with the slip;
not again within NOTE_COOLDOWN).

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
while True:
    if select.select([sys.stdin], [], [], 1 / 90)[0]:
        if not os.read(0, 4096):
            break
    try:
        hy, hp = json.load(open({LOOK!r}))
    except (OSError, ValueError):
        continue
    print(json.dumps({{"t": time.monotonic(), "src": {{"mmap1": {{"hy": hy, "hp": hp, "unc": [0.001, 0.001],
          "open": [0.8, 0.8]}}}}}}), flush=True)
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
eyes_state = {"offset": (0.0, 0.0), "jump": (0.0, 0.0), "asked": 0}
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
            reply = json.dumps({"calibration": {"dots": 27}, "calibrating": False, "dots": 27, "eyes": jump})
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

# The slip note: our tracker, gaze mode on.
SETTINGS["tracker"] = "own"
HELPER["gaze"] = True
with open(gazed.CONF, "w") as f:
    f.write("GAZE_FIT_WATCH=1\n# our tracker now\n")
check("our tracker in use", wait(lambda: svc.kind == "own" and svc.eyes_proc is not None, 6), True)
check("gaze mode on", wait(lambda: svc.checks.gaze_on is True, 4), True)
check("ft-eyes' status comes", wait(lambda: time.monotonic() - svc.own_at < 2, 4), True)
n = len(panel)
time.sleep(2)
check("no slip: no note", "show note" in since(n), False)
eyes_state["jump"] = (0.0, -12.0)    # the pupils dropped: the headset rode up
start = time.monotonic()
check("a slip: the note", wait(lambda: "show note" in since(n), gazecheck.SLIP_HOLD + 4), True)
check("  not before SLIP_HOLD", time.monotonic() - start >= gazecheck.SLIP_HOLD, True)
check("  saying so", "title Your headset slipped" in since(n), True)
check("  and which way", any(c.startswith("text Let the headset down") for c in since(n)), True)
check("  no pointer hiding for it", any(c == "calpanel 1" for c in since(n)), False)
n = len(panel)
eyes_state["jump"] = (0.0, 0.0)
check("the slip gone: the note goes", wait(lambda: "hide" in since(n), 4), True)
n = len(panel)
eyes_state["jump"] = (0.0, -12.0)
time.sleep(gazecheck.SLIP_HOLD + 3)
check("again within NOTE_COOLDOWN: no note", "show note" in since(n), False)
eyes_state["jump"] = (0.0, 0.0)

print("all passed" if not failures else f"{len(failures)} failed: {failures}", flush=True)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(1 if failures else 0)
