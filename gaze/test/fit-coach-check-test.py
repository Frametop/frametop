#!/usr/bin/env python3
"""Offline test of the fit coach in the headset fit check (gaze/gazecheck.py, "fit"): with
SteamVR's tracker in use, the check keeps ft-eyes running (the lease), asks it for the last
second of pupil finds ("fit"), judges them only while the gaze is straight ahead, and puts the
coach's hint first in the panel's hints.

The harness is first-calibration-test.py's: ft-gazed's Service with its sockets renamed, HOME in
a temp folder, a fake pointer helper, a fake ft-gaze (its gaze from a file the test writes), a
fake ft-eyes control socket (its finds from the shipped reference, offset as the test says),
and the panel's commands recorded instead of sent. Safe next to the live gaze service.

  gaze/test/fit-coach-check-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-fit-coach-test-")
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

tag = f"ft_gaze_fit_coach_test_{os.getpid()}"
gazed.ME = f"\0{tag}_gazed"
gazed.POINTER = gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.SCREENS = f"\0{tag}_screens"
gazecheck.PANEL = f"\0{tag}_panel"
gazed.EYES_SOCKET = gazecheck.EYES = f"\0{tag}_eyes"
gazed.read_settings = lambda: ("steam", "auto", "auto", 55.0)  # SteamVR's tracker in use
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
eyes_state = {"offset": (0.0, 0.0), "asked": 0}
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
            reply = json.dumps({"calibration": None, "calibrating": False, "dots": 0,
                                "eyes": {"right": {"reseat": False}, "left": {"reseat": False}}})
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
            helper.sendto(b"ok off worn", addr)


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


def coach_line():
    """The first hint line the panel got last, if it's the coach's."""
    for cmd in reversed(panel):
        if cmd.startswith("hints "):
            first = cmd[len("hints "):].split("|")[0]
            return first if first.startswith("Headset position") else None
    return None


ask("wake 60")
check("the tracker sends", wait(lambda: time.monotonic() - svc.checks.sample_at < 1, 6), True)
check("SteamVR's tracker in use: ft-eyes isn't running", svc.eyes_proc is None, True)
check("fitcheck is taken", ask("fitcheck").startswith("ok"), True)
check("the fit check opens", wait(lambda: (svc.checks.check or {}).get("kind") == "fit", 6), True)
check("the fit check starts ft-eyes for the coach", wait(lambda: svc.eyes_proc is not None, 3), True)
check("and asks it for finds", wait(lambda: eyes_state["asked"] > 0, 3), True)
check("looking ahead, well fitted: good",
      wait(lambda: (coach_line() or "").startswith("Headset position: good."), 4), True)
check("marked a first guess (the reference is from one face)", "first guess" in (coach_line() or ""), True)

eyes_state["offset"] = (0.0, 12.0)   # both eyes appear to look up: the headset is low
check("the headset low: push it up",
      wait(lambda: "Push the headset up" in (coach_line() or ""), 4), True)
check("it's the first hint line", (coach_line() or "").startswith("Headset position: Push"), True)

look(25.0, 0.0)                      # looking away: the pupils move with the gaze, so no new reading
check("looking away: the last centred reading, marked so",
      wait(lambda: "when you last looked straight ahead" in (coach_line() or ""), 4), True)
eyes_state["offset"] = (0.0, 0.0)
time.sleep(1.5)
check("and it isn't judged meanwhile", "Push the headset up" in (coach_line() or ""), True)
look(0.0, 0.0)
check("looking ahead again: judged again", wait(lambda: (coach_line() or "").startswith("Headset position: good."), 4), True)

ask("calquit")
check("closing logs the last reading", wait(lambda: any(m.startswith("fit coach, last reading: ok") for m in logs), 2), True)
check("the check closed", wait(lambda: svc.checks.check is None, 2), True)
check("ft-eyes stops once its lease runs out",
      wait(lambda: svc.eyes_proc is None, gazecheck.COACH_LEASE + 4), True)

print("all passed" if not failures else f"{len(failures)} failed: {failures}", flush=True)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(1 if failures else 0)
