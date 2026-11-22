#!/usr/bin/env python3
"""Offline test of our tracker's glint-jump rule and click edge guard (2026-11-22).

EYES_JUMP=both (the default): a glint jump counts only while both mapped eyes see one, since
the headset moving moves both and a misread glint moves one. An eye the calibration doesn't
map can't vouch, so then the other's own jump counts. A click nudges only the eyes whose
calibration dots it lies within (EDGE_MARGIN to spare): out there the fit guesses, and the
lesson would teach that guess.

Pupil positions are written into ft-eyes' eye histories as its frames would leave them: no
cameras, SteamVR or live gaze service. Needs the tracker's Python (numpy, OpenCV).

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/jump-rule-test.py
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

HOME = tempfile.mkdtemp(prefix="ft-eyes-jump-rule-test-")
os.environ.update(HOME=HOME, FT_EYES_CAMS="/nonexistent", FT_EYES_STATE=os.path.join(HOME, "eyes"),
                  FT_EYES_MODEL="classic")
os.environ.pop("FT_EYES_JUMP", None)
GAZE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAZE))
sys.path.insert(0, str(GAZE / "tracker"))

import numpy as np  # noqa: E402

import gazecheck  # noqa: E402

loader = importlib.machinery.SourceFileLoader("fteyes", str(GAZE / "tracker" / "ft-eyes"))
fe = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(fe)
em = fe.eyes_model

failures = []


def check(what, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(what)


DOTS = [d[:2] for d in gazecheck.check_dots("full", True)]
CENTRE = {0: (300.0, 200.0), 1: (330.0, 205.0)}


def pupil(eye, yaw, pitch):
    x0, y0 = CENTRE[eye]
    return x0 - 4.0 * yaw, y0 - 4.0 * pitch


class Clock:
    t = 100.0


def hold(tracker, yaw, pitch, seen=(0, 1)):
    """Half a second at (yaw, pitch): 45 frames per eye seen. Returns the window's (start, end)."""
    t0 = Clock.t
    for i in range(45):
        for e in tracker.eyes:
            if e.eye in seen:
                e.history.append((t0 + i / 90, *pupil(e.eye, yaw, pitch), float("nan"), float("nan")))
    Clock.t = t0 + 1.0
    return t0, t0 + 0.5


def calibrate(seen_at=lambda yaw: (0, 1)):
    tracker = fe.Tracker()
    tracker.command("calib-start")
    for yaw, pitch in DOTS:
        t0, t1 = hold(tracker, yaw, pitch, seen_at(yaw))
        tracker.command(f"calib-point {t0:.6f} {t1:.6f} {yaw:.4f} {pitch:.4f}")
    tracker.command("calib-fit")
    return tracker


# --- Shift on its own ---
a, b = em.Shift(), em.Shift()
a.base, a.jump = np.array([1.0, 0.0]), np.array([3.0, 0.0])
check("each: its own jump is in force", a.in_force(b, "each"), True)
check("both: not while the other eye sees none", a.in_force(b, "both"), False)
check("...its shift is then the clicks' alone", a.current(b, "both").tolist(), [1.0, 0.0])
b.jump = np.array([0.0, 4.0])
check("both: in force once the other sees one too", (a.in_force(b, "both"), a.current(b, "both").tolist()),
      (True, [4.0, 0.0]))
check("both, no other eye mapped: its own jump counts", a.in_force(None, "both"), True)
check("no jump: never in force", em.Shift().in_force(a, "each"), False)

# --- teach: whether jumps were in force is read before any lesson; an eye left untaught keeps its jump ---
a, b = em.Shift(), em.Shift()
for sh in (a, b):
    sh.meas, sh.jump = [np.zeros(2)] * 3, np.array([6.0, 0.0])
em.teach({0: a, 1: b}, {0: (np.array([6.0, 0.0]), None)}, "both", [0, 1])
check("teach: the taught eye's older clicks dropped, its jump cleared", (len(a.meas), a.in_force(b, "both")), (1, False))
check("...the untaught eye keeps its jump in force (vouched)", (b.vouched, b.in_force(a, "both"), b.current(a, "both").tolist()),
      (True, True, [6.0, 0.0]))
em.teach({0: a, 1: b}, {1: (np.array([6.0, 0.0]), None)}, "both", [0, 1])
check("...its own lesson later drops its older clicks and ends the vouch", (len(b.meas), b.vouched), (1, False))
b.jump, b.vouched = np.array([6.0, 0.0]), True
b.glint(np.zeros(2), 0.0)   # ref is None: this glint estimate becomes it, then reads no jump
check("...a glint estimate back where it was ends it too", (b.vouched, bool(np.any(b.jump))), (False, False))

# --- Calibration.inside ---
cal = em.Calibration(box={0: [-30.0, 30.0, -18.0, 18.0]})
check("inside its dots", cal.inside(0, (29.0, -17.0)), True)
check("...within the margin past them", cal.inside(0, (31.5, 0.0)), True)
check("...past the margin: outside", cal.inside(0, (33.0, 0.0)), False)
check("an eye with no extent known: inside", cal.inside(1, (80.0, 0.0)), True)

# --- the tracker: status and the rule ---
check("the rule defaults to both", fe.JUMP, "both")
tracker = calibrate()
st = tracker.status()
check("status names the rule and the margin", (st["jump_rule"], st["edge_margin"]), ("both", em.EDGE_MARGIN))
check("the calibration keeps each eye's dots' extent", sorted(tracker.cal.box), [0, 1])
check("...out to the wider ring", [round(v) for v in tracker.cal.box[0]], [-30, 30, -18, 18])
saved = em.Calibration.load(fe.CALIBRATION)
check("...saved with it", saved.box == tracker.cal.box, True)
right, left = tracker.eyes
right.shift.jump = np.array([5.0, 0.0])
check("one eye's jump (a misread glint): not in force", (right.jump_on(), right.shift_now().tolist()),
      (False, right.shift.base.tolist()))
st = tracker.status()["eyes"]["right"]
check("...status: held but not in force", (st["jump"], st["jump_px"], st["jump_held_px"]),
      (False, [0.0, 0.0], [5.0, 0.0]))
left.shift.jump = np.array([4.0, 1.0])
check("both eyes' jumps (the headset moved): in force", (right.jump_on(), left.jump_on()), (True, True))
fe.JUMP = "each"
left.shift.jump = np.zeros(2)
check("EYES_JUMP=each: one eye's own jump counts", right.jump_on(), True)
fe.JUMP = "both"

# --- clicks: both jumps in force reset both eyes' clicks, though the first eye's lesson clears its jump ---
for e in tracker.eyes:
    e.shift.meas = [np.zeros(2)] * 3
right.shift.jump, left.shift.jump = np.array([5.0, 0.0]), np.array([4.0, 1.0])
t0, t1 = hold(tracker, 10.0, 5.0)
r = tracker.command(f"click {t1:.6f} 10.0 5.0")
check("click inside the dots: both eyes measured", ("nan" in r, r.startswith("ok ")), (False, True))
check("...both eyes' older clicks dropped", (len(right.shift.meas), len(left.shift.meas)), (1, 1))
check("...both jumps cleared", (right.jump_on(), left.jump_on()), (False, False))

# --- clicks: outside an eye's dots teach it nothing ---
before = [len(e.shift.meas) for e in tracker.eyes]
t0, t1 = hold(tracker, 40.0, 0.0)
r = tracker.command(f"click {t1:.6f} 40.0 0.0")
check("click 10 degrees past the dots: nan for both", r, "ok nan nan nan nan")
check("...no eye taught", [len(e.shift.meas) for e in tracker.eyes], before)
rec = json.loads(fe.CLICKS.read_text().splitlines()[-1])
check("...the clicks log says why", {k: v.get("skipped") for k, v in rec["eyes"].items()},
      {"0": "outside its calibration dots", "1": "outside its calibration dots"})

# --- a click past one eye's dots only: the other is taught, and the first keeps the jump both had ---
def outward_lost(yaw):   # the right eye lost looking 20+ right (yaw -), the left 20+ left
    return tuple(e for e in (0, 1) if not (e == 0 and yaw <= -20 or e == 1 and yaw >= 20))


tracker = calibrate(outward_lost)
right, left = tracker.eyes
check("each eye's dots stop where it was lost", (tracker.cal.box[0][0] > -20, tracker.cal.box[1][1] < 20), (True, True))
for e in tracker.eyes:
    e.shift.meas, e.shift.jump = [np.zeros(2)] * 3, np.array([5.0, 0.0])
t0, t1 = hold(tracker, 25.0, 0.0)
r = tracker.command(f"click {t1:.6f} 25.0 0.0")
check("click at 25 left, past the left eye's dots: only the right taught", r.split()[3:], ["nan", "nan"])
check("...the right's older clicks dropped", len(right.shift.meas), 1)
check("...the left keeps its jump in force", (left.jump_on(), left.shift_now().tolist()),
      (True, (left.shift.base + left.shift.jump).tolist()))
rec = json.loads(fe.CLICKS.read_text().splitlines()[-1])
check("...the log's before is the shift in force", rec["eyes"]["0"]["before"], [5.0, 0.0])
t0, t1 = hold(tracker, 0.0, 0.0)
tracker.command(f"click {t1:.6f} 0.0 0.0")
check("the next click in the middle: the left's older clicks dropped too", len(left.shift.meas), 1)

# --- one eye mapped: its own jump counts, since the other can't vouch ---
tracker = calibrate(lambda yaw: (1,))
right, left = tracker.eyes
check("only the left eye mapped", (right.cal is None, left.cal is not None), (True, True))
left.shift.jump = np.array([3.0, 0.0])
check("...its own jump is in force", left.jump_on(), True)

# --- a calibration saved before boxes: refilled from its dots on load ---
tracker = calibrate()
d = json.loads(fe.CALIBRATION.read_text())
d.pop("box")
fe.CALIBRATION.write_text(json.dumps(d))
check("an old calibration file has no extents", em.Calibration.load(fe.CALIBRATION).box, {})
check("...the tracker refills them from its dots", sorted(fe.Tracker().cal.box), [0, 1])

shutil.rmtree(HOME, ignore_errors=True)
if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all ok")
