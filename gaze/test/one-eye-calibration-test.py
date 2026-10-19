#!/usr/bin/env python3
"""Offline test of our tracker with one eye lost (2026-10-18): a calibration dot is taken with
the eye that was seen when the other wasn't (ft-eyes calib-point), each eye is fitted on its
own dots and an eye seen at too few is left out (calib-fit, eyes_model.Calibration.fit), and
ft-gazed eases the step when an eye drops out (gazecal.EyeBridge). A second person's eyes each
lost the pupil looking 20 degrees outward, so until now their outer dots failed, and the rest
fitted both eyes from the middle only.

Pupil positions are written into ft-eyes' eye histories as its frames would leave them: no
cameras, SteamVR or live gaze service. Needs the tracker's Python (numpy, OpenCV).

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/one-eye-calibration-test.py
"""
import importlib.machinery
import importlib.util
import json
import math
import os
import shutil
import sys
import tempfile
from pathlib import Path

HOME = tempfile.mkdtemp(prefix="ft-eyes-one-eye-test-")
os.environ.update(HOME=HOME, FT_EYES_CAMS="/nonexistent", FT_EYES_STATE=os.path.join(HOME, "eyes"),
                  FT_EYES_MODEL="classic")
GAZE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAZE))
sys.path.insert(0, str(GAZE / "tracker"))

import gazecheck  # noqa: E402
from gazecal import EyeBridge, EyeWeights, GazeLock  # noqa: E402

loader = importlib.machinery.SourceFileLoader("fteyes", str(GAZE / "tracker" / "ft-eyes"))
fe = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(fe)
loader = importlib.machinery.SourceFileLoader("ftgazed", str(GAZE / "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(gazed)

failures = []


def check(what, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(what)


DOTS = [d[:2] for d in gazecheck.check_dots("full", True)]   # 27 dots, out to +-20
CENTRE = {0: (300.0, 200.0), 1: (330.0, 205.0)}               # right, left: pupil looking ahead


def pupil(eye, yaw, pitch):
    """Where an eye's pupil sits for a gaze: 4 px a degree, yaw +left moves it to lower x."""
    x0, y0 = CENTRE[eye]
    return x0 - 4.0 * yaw + 0.02 * yaw * pitch, y0 - 4.0 * pitch


class Clock:
    t = 100.0


def look(tracker, yaw, pitch, seen=(0, 1), jitter=()):
    """Half a second at a dot: 45 frames per eye that's seen (jitter: those that jump about),
    then calib-point over that window."""
    t0 = Clock.t
    for i in range(45):
        t = t0 + i / 90
        for e in tracker.eyes:
            if e.eye not in seen:
                continue
            x, y = pupil(e.eye, yaw, pitch)
            if e.eye in jitter:
                x += 0.5 * (i - 22)   # drifting: half its frames over 5.5 px from the middle
            e.history.append((t, x, y, math.nan, math.nan))
    Clock.t = t0 + 1.0
    return tracker.command(f"calib-point {t0:.6f} {t0 + 0.5:.6f} {yaw:.4f} {pitch:.4f}")


def outward_lost(yaw):
    """The second person: the right eye lost looking 14+ degrees right (yaw -), the left 14+ left."""
    return tuple(e for e in (0, 1) if not (e == 0 and yaw <= -14 or e == 1 and yaw >= 14))


# --- calib-point: one eye is enough for a dot ---
tracker = fe.Tracker()
check("calib-start", tracker.command("calib-start"), "ok")
r = look(tracker, 0.0, 0.0)
check("both eyes seen: taken", r.split()[:3], ["ok", "45", "45"])
check("...with no note", "only" in r, False)
r = look(tracker, -20.0, 0.0, seen=(1,))
check("right eye lost: taken with the left", r.startswith("ok 0 45"), True)
check("...and says so", r.endswith("(left eye only: the right eye was seen in only 0 frames)"), True)
check("...the dot holds the left eye alone", sorted(tracker.dots[-1]["eye"]), [1])
r = look(tracker, 10.0, 0.0, jitter=(1,))
check("left eye jumping about: taken with the right", "(right eye only: the left eye moved" in r, True)
check("...the dot holds the right eye alone", sorted(tracker.dots[-1]["eye"]), [0])
n = len(tracker.dots)
r = look(tracker, 0.0, 10.0, seen=())
check("neither eye seen: not taken", r, "fail neither eye was seen enough (right 0 frames, left 0 frames)")
r = look(tracker, 0.0, 10.0, jitter=(0, 1))
check("both moved: not taken, as before", r, "fail the right eye moved (5.5 px)")
r = look(tracker, 0.0, 10.0, seen=(1,), jitter=(1,))
check("one lost and the other moved: the move is the reason", r, "fail the left eye moved (5.5 px)")
check("...none of those kept", len(tracker.dots), n)

# The panel's words for them (gazecheck.reject_reason).
check("reject_reason: neither eye", gazecheck.reject_reason(
    "fail neither eye was seen enough (right 3 frames, left 0 frames)", None),
    ("eyes not seen", "our tracker saw neither eye well enough", True))
check("reject_reason: one eye moved, as before", gazecheck.reject_reason("fail the left eye moved (12.0 px)", None),
      ("left eye moved", "your left eye moved while you looked", False))

# --- calib-fit: a full calibration where each eye is lost looking outward ---
tracker = fe.Tracker()
tracker.command("calib-start")
for yaw, pitch in DOTS:
    look(tracker, yaw, pitch, seen=outward_lost(yaw))
seen = {c: sum(1 for k in tracker.dots if c in k["eye"]) for c in (0, 1)}
check("all 27 dots taken", len(tracker.dots), 27)
check("each eye at the dots it was seen at", seen, {0: 22, 1: 22})
r = tracker.command("calib-fit")
check("fitted", r.startswith("ok 27 dots, fit median 0.0"), True)
check("...each eye's dots in the reply", r.endswith("(22 dots)") and "(22 dots), left 0.0" in r, True)
check("both eyes mapped", tracker.status()["mapped"], ["right", "left"])
check("...the dots per eye saved with it", tracker.cal.info["eye_dots"], {"right": 22, "left": 22})
right, left = tracker.eyes
check("each eye calibrated", (right.cal is not None, left.cal is not None), (True, True))
g = tracker.cal.gaze(1, *pupil(1, -20.0, 0.0))
check("the left eye at 20 right (only it saw that dot): on it", (round(g[0], 1), round(g[1], 1)), (-20.0, 0.0))
rows = [json.loads(x) for x in open(fe.CALIBRATION.with_name("calibration-dots.jsonl"))]
check("calibration-dots.jsonl keeps the one-eye dots", sorted(len(x["eye"]) for x in rows)[:4], [1, 1, 1, 1])
check("...and its spreads come back from them", sorted(fe.spread_from_dots(tracker.cal)), [0, 1])
loaded = fe.eyes_model.Calibration.load(fe.CALIBRATION)
check("the saved calibration maps both", loaded.eyes(), [0, 1])

# --- calib-fit with one eye seen at too few dots: the other alone ---
tracker = fe.Tracker()
tracker.command("calib-start")
for i, (yaw, pitch) in enumerate(DOTS):
    look(tracker, yaw, pitch, seen=(1,) if i % 3 else (0, 1))   # the right eye at 9 of 27
r = tracker.command("calib-fit")
check("right eye at 9 dots: fitted with the left alone", r.endswith("(27 dots)")
      and "right not used (9 dots, needs 12)" in r, True)
check("...the left mapped only", tracker.status()["mapped"], ["left"])
right, left = tracker.eyes
check("...the right eye gives no gaze", (right.cal, left.cal is not None), (None, True))
check("...a click teaches the left only", tracker.command(f"click {Clock.t:.6f} 0 0").split()[1:3], ["nan", "nan"])
fresh = fe.Tracker()
check("...and a restart says so", (fresh.cal.eyes(), fresh.eyes[0].cal), ([1], None))

# --- calib-fit with too few dots for either eye ---
tracker = fe.Tracker()
tracker.command("calib-start")
for i, (yaw, pitch) in enumerate(DOTS[:14]):
    look(tracker, yaw, pitch, seen=(0,) if i % 2 else (1,))
r = tracker.command("calib-fit")
check("7 dots each: no calibration", r, "fail neither eye was seen at enough dots (right 7, left 7; need 12)")
check("...the last one stays", fe.eyes_model.Calibration.load(fe.CALIBRATION).eyes(), [1])

# --- gazecal.EyeBridge: no jump when an eye drops out ---
b = EyeBridge()
for i in range(90):   # both eyes, a degree apart: their mean is the gaze
    out = b(i / 90, [(4.0, 2.0), (6.0, 2.0)], (5.0, 2.0))
check("both eyes: the combined gaze", out, (5.0, 2.0))
t = 1.0
out = b(t, [None, (6.0, 2.0)], (6.0, 2.0))
check("the left drops out: the gaze stays put", (abs(out[0] - 5.0) < 0.05, out[1]), (True, 2.0))
out = b(t + 0.3, [None, (6.0, 2.0)], (6.0, 2.0))
check("...eases toward the right eye", 5.5 < out[0] < 5.8, True)
out = b(t + 2.0, [None, (6.0, 2.0)], (6.0, 2.0))
check("...and is the right eye's after 2 s", round(out[0], 2), 6.0)
out = b(t + 2.1, [(4.0, 2.0), (6.0, 2.0)], (5.0, 2.0))
check("both back: the combined gaze", out, (5.0, 2.0))
check("no eyes learned yet: as it comes", EyeBridge()(0.0, [None, (6.0, 2.0)], (6.0, 2.0)), (6.0, 2.0))


# --- ft-gazed's own path uses it ---
def service():
    svc = gazed.Service.__new__(gazed.Service)
    svc.override, svc.tracker, svc.source, svc.one_eye = None, "own", "mmap1", None
    svc.models = {name: gazed.Correction() for name in gazed.SOURCES}
    svc.counts = dict.fromkeys(("samples", "sent", "blinks", "one_eye", "one_eye_used", "lost_left", "lost_right",
                                "looking_down", "dropped"), 0)
    from collections import deque
    svc.opens, svc.vergence = (deque(maxlen=90), deque(maxlen=90)), deque(maxlen=90)
    svc.lost, svc.bad_at, svc.fallback = [False, False], [0.0, 0.0], gazed.EyeFallback()
    svc.fix, svc.last_sample, svc.low_until = GazeLock(radius=1.0), 0.0, [0.0, 0.0]
    svc.bridge = EyeBridge()
    svc.fix = lambda gy, gp, t, then, q: (gy, gp)   # what goes into the lock
    svc.weights, svc.fusion_on, svc.fusion = {"own": EyeWeights()}, False, None
    svc.sent = []
    svc.send = lambda t, hy, hp, rhy, rhp, eyes: svc.sent.append((round(hy, 2), round(hp, 2)))
    return svc


svc = service()
for i in range(90):
    svc.on_eyes_sample({"t": i / 90, "src": {"mmap1": {"unc": [0.001, 0.001], "open": [0.8, 0.8]},
                                             "own": {"hy": 5.0, "hp": 2.0, "eyes": [[4.0, 2.0], [6.0, 2.0]]}}}, True)
svc.on_eyes_sample({"t": 1.0, "src": {"mmap1": {"unc": [0.001, 0.001], "open": [0.8, 0.8]},
                                      "own": {"hy": 6.0, "hp": 2.0, "eyes": [None, [6.0, 2.0]]}}}, True)
check("ft-gazed, our tracker: both eyes, then the left lost: no jump",
      (svc.sent[-2], abs(svc.sent[-1][0] - 5.0) < 0.05), ((5.0, 2.0), True))
check("...counted as one eye", svc.counts["one_eye"], 1)

shutil.rmtree(HOME, ignore_errors=True)
print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
sys.exit(1 if failures else 0)
