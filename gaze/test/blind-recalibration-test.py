#!/usr/bin/env python3
"""Offline test of our own eye tracker's calibration going blind (gaze/gazecheck.py, OWN_GONE):
a full calibration while ours is calibrated but sends no gaze. Live 2026-10-18 a calibration
with the left eye closed mapped only the right eye; the next one, with the right eye closed,
took no dot, since every click waited for our tracker's gaze ("Waiting: the eye tracker isn't
sending a gaze"). Now the dot stands in for the gaze while there's none, as in the first
calibration (first-calibration-test.py), and ft-eyes checks the pupils it sees.

Same harness as first-calibration-test.py: ft-gazed's Service with its sockets renamed and HOME
in a temp folder, a fake pointer helper (gaze mode on, headset worn), a fake ft-gaze (SteamVR
sees both eyes; ours sends a gaze while OWN_FLAG exists), a fake ft-eyes control socket
(calibrated), and no panel. Nothing reaches the live gaze service, the pointer helper, ft-eyes,
or SteamVR, so it's safe next to them.

  gaze/test/blind-recalibration-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-blind-recal-test-")
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
import gazecheck  # noqa: E402  (the module ft-gazed imported)

tag = f"ft_gaze_blind_recal_test_{os.getpid()}"
gazed.ME = f"\0{tag}_gazed"
gazed.POINTER = gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.SCREENS = f"\0{tag}_screens"
gazecheck.PANEL = f"\0{tag}_panel"
gazed.EYES_SOCKET = gazecheck.EYES = f"\0{tag}_eyes"
gazed.read_settings = lambda: ("own", "auto", "auto", 55.0)
gazed.TrackedEye = lambda: lambda now=None: None  # both eyes, whatever SteamVR's settings say
DOTS = 4
real_dots = gazecheck.check_dots
gazecheck.check_dots = lambda kind, own: real_dots(kind, own)[:DOTS]  # a short calibration
gazecheck.Fusion.MIN = DOTS   # ...whose dots still fit a fusion (SteamVR's eyes at each)
gazecheck.LOOKED_AWAY = 1e9   # the fake SteamVR's eyes stay put whatever the dot (looked-away-test.py has that check)
logs = []
gazed.log = gazecheck.log = lambda msg: logs.append(msg)

# The fake ft-gaze: 90 samples a second, SteamVR sees both eyes, ours sends a gaze (the right
# eye's, at 1, 2) while OWN_FLAG exists, and {"ok":0} as with a stale file otherwise.
FAKE = os.path.join(HOME, "ft-gaze")
OWN_FLAG = os.path.join(HOME, "own-gaze")
with open(FAKE, "w") as f:
    f.write('''import json, os, select, sys, time
flag = sys.argv[1]
while True:
    if select.select([sys.stdin], [], [], 1 / 90)[0]:
        if not os.read(0, 4096):
            break
    own = {"hy": 1.0, "hp": 2.0, "eyes": [None, [1.0, 2.0]]} if os.path.exists(flag) else {"ok": 0}
    print(json.dumps({"t": time.monotonic(), "src": {"mmap1": {"hy": 1.0, "hp": 2.0, "unc": [0.001, 0.001],
          "open": [0.8, 0.8]}, "left": {"hy": 0.0, "hp": 2.0}, "right": {"hy": 2.0, "hp": 2.0},
          "own": own}}), flush=True)
''')
SLEEPER = [sys.executable, "-c", "import sys; sys.stdin.read()"]  # quits when its stdin closes


def own_gaze(on):
    if on:
        open(OWN_FLAG, "w").close()
    elif os.path.exists(OWN_FLAG):
        os.remove(OWN_FLAG)


def start_helper(self):
    """ft-gaze, straight from here instead of the dev container."""
    self.proc = subprocess.Popen([sys.executable, FAKE, OWN_FLAG], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
    self.proc_sources = self.wanted_sources()
    os.set_blocking(self.proc.stdout.fileno(), False)
    os.set_blocking(self.proc.stderr.fileno(), False)
    self.sel.register(self.proc.stdout, selectors.EVENT_READ, "stdout")
    self.sel.register(self.proc.stderr, selectors.EVENT_READ, "stderr")
    self.buf = b""


def start_eyes(self):
    """ft-eyes' process: a stand-in. Its control socket is the fake below."""
    self.eyes_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.eyes_proc.stderr.fileno(), False)
    self.sel.register(self.eyes_proc.stderr, selectors.EVENT_READ, "eyes")


def start_panel(self):
    self.panel_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.panel_proc.stderr.fileno(), False)
    self.sel.register(self.panel_proc.stderr, selectors.EVENT_READ, "panel")


gazed.Service.start_helper = start_helper
gazed.Service.start_eyes = start_eyes
gazecheck.Checks.start_panel = start_panel

# The fake ft-eyes: calibrated (the right eye only, as after a calibration with the left closed).
eyes_state = {"cal": {"made": "test", "dots": 27}, "points": []}
eyes = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
eyes.bind(gazed.EYES_SOCKET)
eyes.settimeout(0.2)


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
            reply = json.dumps({"calibration": eyes_state["cal"], "calibrating": False, "dots": 0, "mapped": ["right"],
                                "eyes": {"right": {"reseat": False}, "left": {"reseat": False}}})
        elif w[0] == "calib-start":
            eyes_state["points"] = []
            reply = "ok"
        elif w[0] == "calib-point":
            eyes_state["points"].append(tuple(map(float, w[1:5])))
            reply = "ok 0 50 nan 1.00 (left eye only: the right eye was seen in only 0 frames)"
        elif w[0] == "calib-fit":
            eyes_state["cal"] = {"made": "test", "dots": len(eyes_state["points"])}
            reply = f"ok {len(eyes_state['points'])} dots, fit median 1.00 deg, eyes right not used (0 dots, needs 12), left 1.00 ({len(eyes_state['points'])} dots)"
        elif w[0] == "click":
            continue
        else:
            reply = f"fail unknown command {w[0]}"
        if addr:
            eyes.sendto(reply.encode(), addr)


threading.Thread(target=eyes_answer, daemon=True).start()

helper_state = {"reply": "ok on worn", "heard": []}
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
            helper.sendto(helper_state["reply"].encode(), addr)
        else:
            helper_state["heard"].append(data.decode())


threading.Thread(target=helper_answer, daemon=True).start()
own_gaze(True)
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


def check_state():
    return svc.checks.check or {}


def click_dot(i, settle=0.0):
    """Wait for dot i, then click: is it taken (within 2 s)?"""
    wait(lambda: check_state().get("i") == i and not check_state().get("done_at"), 3)
    time.sleep(gazecheck.CHECK_SETTLE + gazecheck.CHECK_WINDOW + 0.1 + settle)
    before = check_state().get("captured", 0)
    ask("calaccept")
    return wait(lambda: check_state().get("captured", 0) > before, 2)


check("ours runs, calibrated", ask("wake 60") == "ok" and wait(lambda: svc.checks.calibrated() is True, 6), True)
check("SteamVR sees the eyes (the fake ft-gaze is sending)", wait(svc.checks.eyes_seen, 6), True)
check("calibrate: a full calibration opens",
      ask("calibrate").startswith("ok") and wait(lambda: check_state().get("kind") == "full", 6), True)
check("not blind: ours is calibrated", check_state().get("blind"), False)

check("dot 1, ours sends a gaze: taken", click_dot(0), True)
check("...on our gaze, not blind", (check_state().get("run_blind"), check_state().get("went_blind")), (False, False))

# Dot 2: the eye ours maps closes, and its gaze stops. The old way (OWN_GONE never) the click waits.
own_gaze(False)
gazecheck.OWN_GONE = 1e9
check("dot 2, no gaze from ours, the old way: the click takes nothing", click_dot(1), False)
check("...and the panel says it waits for a gaze",
      wait(lambda: "isn't sending a gaze" in check_state().get("note", ""), 2), True)
gazecheck.OWN_GONE = 0.5
check("now: the dot stands in for the gaze, and the click waiting there takes it",
      wait(lambda: check_state().get("captured") == 2, 3), True)
check("...said once in the log", sum("our tracker sends no gaze" in m for m in logs), 1)
t0, t1, yaw, pitch = eyes_state["points"][-1]
dot = gazecheck.check_dots("full", True)[1]
check("ft-eyes got dot 2's direction", (round(yaw, 3), round(pitch, 3)), (round(dot[0], 3), round(dot[1], 3)))

own_gaze(True)
check("dot 3, ours sends a gaze again: taken on it", click_dot(2) and not check_state().get("run_blind"), True)
own_gaze(False)
check("dot 4, gone again: blind once OWN_GONE passes, taken", click_dot(3, gazecheck.OWN_GONE), True)
t0, t1, yaw, pitch = eyes_state["points"][-1]
check("its window is the look up to the click (about CHECK_WINDOW)", abs((t1 - t0) - gazecheck.CHECK_WINDOW) < 0.15, True)
check("still said once in the log", sum("our tracker sends no gaze" in m for m in logs), 1)
check("all dots: ours fits its calibration", wait(lambda: not svc.checks.check and len(eyes_state["points"]) == DOTS, 5), True)
check("the fit's reply in the log", any("left 1.00 (4 dots)" in m for m in logs), True)

# A check of our gaze (five) has nothing to check without one: it waits for ours to send, and
# never goes blind.
check("five check without our gaze: it waits for ours to send", ask("fivecheck"), "ok waking the eye tracker first")
check("...and doesn't open meanwhile", wait(lambda: svc.checks.check is not None, 3), False)
own_gaze(True)
check("ours sends: the five check opens", wait(lambda: check_state().get("kind") == "five", 6), True)
own_gaze(False)
check("its click without our gaze takes nothing", click_dot(0, gazecheck.OWN_GONE), False)
check("...and it never went blind", (check_state().get("run_blind"), check_state().get("went_blind")), (False, False))
ask("calquit")

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
svc.running = False
time.sleep(0.7)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(1 if failures else 0)
