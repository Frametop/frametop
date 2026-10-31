#!/usr/bin/env python3
"""Offline test of the gaze checks' G4 fixes (gaze/gazecheck.py, docs/charts/README.md G4):

  Z1 (DEE-270)  a full calibration marks the screens it hid (SCREENS_HIDDEN); the next gaze
                service shows them again if the last one died before close()
  Z2 (DEE-271)  our tracker's calibration saved (calib-fit's "ok") counts at once, so need_full
                doesn't open a second full calibration on ft-eyes' stale status
  Z3 (DEE-272)  a fit check asked for while idle runs once the tracker sends, not once a gaze
                comes (our tracker sends none before its first calibration)
  Z4 (DEE-273)  a check closed because the eyes went makes their coming back a don, which
                re-arms the full calibration

Drives Checks with a fake service and a fake ft-screens socket: nothing reaches the live gaze
service, ft-screens, the pointer helper, ft-eyes, or SteamVR. HOME is a temp folder (the marker
and checks.jsonl go there).

  gaze/test/restart-checks-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-restart-checks-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE follows HOME

import selectors  # noqa: E402
import shutil  # noqa: E402
import socket  # noqa: E402
import sys  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402
import types  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gazecheck  # noqa: E402

tag = f"ft_gaze_restart_checks_test_{os.getpid()}"
gazecheck.PANEL = f"\0{tag}_panel"
gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.EYES = f"\0{tag}_eyes"
gazecheck.SCREENS = f"\0{tag}_screens"
logs = []
gazecheck.log = lambda msg: logs.append(msg)

failures = []


def check(label, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {label}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


# The fake ft-screens: "state" says shown (always mode, not hidden), the rest "ok".
screens_got = []
screens = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
screens.bind(gazecheck.SCREENS)


def serve():
    while True:
        data, addr = screens.recvfrom(256)
        text = data.decode()
        screens_got.append(text)
        if addr:
            screens.sendto(b"ok always 0" if text == "state" else b"ok", addr)


threading.Thread(target=serve, daemon=True).start()


def took():
    time.sleep(0.05)
    got = [m for m in screens_got if m != "state"]
    screens_got.clear()
    return got


class Weights:
    def add(self, miss):
        pass


def new_svc():
    return types.SimpleNamespace(one_eye=None, weights={"own": Weights(), "steam": Weights()}, kind="own",
                                 own={"calibration": None}, own_at=time.monotonic(), last_sample=0.0,
                                 waking=lambda: False, update_awake=lambda: None, mode="none", models={},
                                 steam=None)


def real_ask(addr, command, timeout=1.0):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.bind("")
    c.settimeout(timeout)
    try:
        c.sendto(command.encode(), addr)
        return c.recv(256).decode()
    except OSError:
        return ""
    finally:
        c.close()


# ft-eyes' calib-start says ok; ft-screens is the fake above.
gazecheck.ask = lambda addr, command, timeout=1.0: "ok" if addr == gazecheck.EYES else real_ask(addr, command, timeout)
gazecheck.Checks.to_panel = lambda self, command: None
gazecheck.Checks.to_helper = lambda self, command: None

# Z1: the calibration hides the screens and marks it; a gaze service that dies leaves the mark,
# and the next one shows them.
svc = new_svc()
checks = gazecheck.Checks(svc, selectors.DefaultSelector())
checks.panel_proc = object()  # "the panel runs"
checks.can_run = lambda: True
checks.blind = lambda: True
took()
check("a full calibration opens", checks.start("full", "test"), "ok")
check("...hides the screens", took(), ["hide"])
check("...and marks them hidden", gazecheck.SCREENS_HIDDEN.exists(), True)
# ft-gazed dies here: nothing calls close(). The next one's Checks:
svc2 = new_svc()
gazecheck.Checks(svc2, selectors.DefaultSelector())
check("the next gaze service shows the screens again", took(), ["show"])
check("...and the mark goes", gazecheck.SCREENS_HIDDEN.exists(), False)
gazecheck.Checks(new_svc(), selectors.DefaultSelector())
check("a service after that shows nothing", took(), [])
checks.close("test")
check("close() shows them (the first service, had it lived)", took(), ["show"])
check("...with no mark left", gazecheck.SCREENS_HIDDEN.exists(), False)

# Z2: calib-fit's "ok" counts at once, before ft-eyes' next status.
svc = new_svc()
checks = gazecheck.Checks(svc, selectors.DefaultSelector())
checks.panel_proc = object()
checks.can_run = lambda: True
check("our tracker uncalibrated, as ft-eyes said", checks.calibrated(), False)
check("a full calibration opens", checks.start("full", "asked for"), "ok")
c = checks.check
c["dots"], c["captured"], c["fusion"] = c["dots"][:3], 3, []
replies = []
checks.ask_eyes = lambda command, timeout, done: replies.append(done)
checks.fit_fusion = lambda pts: None
checks.finish()
replies[0]("ok 3 dots, fit median 0.80 deg")
check("calib-fit said ok: calibrated now, not at ft-eyes' next status", checks.calibrated(), True)
check("...the calibration closed", checks.check, None)
checks.gaze_on = True
checks.full_armed = True  # (a calibration asked for leaves it armed)
checks.need_full("test")
check("...and need_full opens no second one", checks.check, None)
took()

# Z3: a fit check asked for while idle runs once the tracker sends, even with no gaze.
svc = new_svc()
svc.own = {"calibration": None}
checks = gazecheck.Checks(svc, selectors.DefaultSelector())
started = []
checks.command = lambda words, queue=True: started.append(words[0]) or "ok"
checks.pending = (["fitcheck"], time.monotonic())
svc.waking = lambda: True
svc.last_sample = 0.0  # no gaze: our tracker isn't calibrated
checks.sample_at = time.monotonic()  # but it sends
checks.run_pending()
check("a fit check asked for while idle runs once the tracker sends", (started, checks.pending), (["fitcheck"], None))

# Z4: the eyes gone for EYES_GONE close a full calibration; their coming back before AWAY_MIN is a
# don, and re-arms it.
svc = new_svc()
checks = gazecheck.Checks(svc, selectors.DefaultSelector())
checks.panel_proc = object()
checks.can_run = lambda: True
check("a full calibration opens", checks.start("full", "test"), "ok")
checks.full_armed = False  # as need_full leaves it
checks.away = False
checks.seen_at = time.monotonic() - gazecheck.EYES_GONE - 0.1  # eyes gone 2.1 s: under AWAY_MIN
checks.tick()
check("eyes gone for EYES_GONE: the calibration closes", checks.check, None)
check("...and the headset counts as away", checks.away, True)
sample = {"src": {"mmap1": {"unc": [0.001, 0.001]}}}
checks.on_sample(sample)
checks.back_since -= gazecheck.DON_DELAY  # the eyes back for DON_DELAY
checks.auto_quick = lambda reason: None
checks.need_full = lambda reason: None
checks.slip_watch = lambda now: None
checks.periodic()
check("eyes back for DON_DELAY: the don re-arms the full calibration", (checks.away, checks.full_armed), (False, True))
took()
finally_ok = not failures
print("all passed" if finally_ok else "FAILED: " + ", ".join(failures), flush=True)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(0 if finally_ok else 1)
