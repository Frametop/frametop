#!/usr/bin/env python3
"""Offline test of our tracker's slip filter (gaze/tracker/slipfilter.py) and ft-eyes' parts for
it: clicks that fade, both eyes judged together, SteamVR's steady looks and the glint estimate
as weaker readings with biases of their own, the headset off and on, the "adjusting" lease,
state.json (and the old shift's), the status the fit pill reads, and slip.jsonl.

Pupil positions are written into ft-eyes' eye histories as its frames would leave them: no
cameras, SteamVR or live gaze service. Needs the tracker's Python (numpy, OpenCV).

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/slip-filter-test.py
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

HOME = tempfile.mkdtemp(prefix="ft-eyes-slip-filter-test-")
os.environ.update(HOME=HOME, FT_EYES_CAMS="/nonexistent", FT_EYES_STATE=os.path.join(HOME, "eyes"),
                  FT_EYES_MODEL="classic")
GAZE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAZE))
sys.path.insert(0, str(GAZE / "tracker"))

import numpy as np  # noqa: E402

import gazecheck  # noqa: E402
import slipfilter as sf  # noqa: E402

loader = importlib.machinery.SourceFileLoader("fteyes", str(GAZE / "tracker" / "ft-eyes"))
fe = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(fe)

failures = []


def check(what, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(what)


JAC = [[-4.0, 0.0], [0.0, -4.0]]   # 4 px of pupil a degree, as pupil() below
PX = 4.0


def near(v, want, tol):
    return bool(np.all(np.abs(np.asarray(v, float) - np.asarray(want, float)) <= tol))


# --- The filter on its own ---
f = sf.SlipFilter(JAC)
check("fresh: no slip", f.s.tolist(), [0.0, 0.0])
check("...sure to START_SD", round(f.slip_sd(), 3), sf.START_SD)
check("slip_deg: the pupil low in the image (y +) reads as the eyes high", near(sf.SlipFilter(JAC, (0, -8)).slip_deg(), (0, 2), 1e-9),
      True)

f = sf.SlipFilter(JAC)
for i in range(5):
    f.click((8.0, 0.0), 10.0 + i)
check("five clicks at 8 px: the slip nearly there", near(f.s, (8, 0), 1.2), True)
g = sf.SlipFilter(JAC)
g.click((8.0, 0.0), 1.0)
check("...the first click right after a calibration counts for over half", g.s[0] > 4.0, True)
check("...and counted", f.clicks, 5)
sd = f.slip_sd()
f.predict(10.0 + 4 + 600)
check("ten minutes without readings: less sure", f.slip_sd() > 2 * sd, True)
f.click((0.0, 0.0), 10.0 + 4 + 600)
check("...so the next click counts for most of its way (the old median of five kept 8)", f.s[0] < 4.0, True)

f = sf.SlipFilter(JAC)
f.predict(0.0)
f.predict(-5.0)   # a click's time is before the filter's newest: taken as now
check("a time before the last doesn't go back", f.t, 0.0)

# --- Clicks judged across both eyes ---
two = {0: sf.SlipFilter(JAC), 1: sf.SlipFilter(JAC)}
r = sf.teach_click(two, {0: np.array([20.0, 4.0]), 1: np.array([18.0, 6.0])}, 1.0)
check("both eyes 5 degrees off the same way: the headset moved", (r[0]["gate"], r[1]["gate"]), ("moved", "moved"))
check("...taken nearly whole", (near(two[0].s, (20, 4), 2.0), near(two[1].s, (18, 6), 2.0)), (True, True))
two = {0: sf.SlipFilter(JAC), 1: sf.SlipFilter(JAC)}
plain = sf.SlipFilter(JAC)
plain.click((24.0, 0.0), 1.0)
r = sf.teach_click(two, {0: np.array([24.0, 0.0]), 1: np.array([2.0, 0.0])}, 1.0)
check("one eye 6 degrees off, the other agreeing: a bad find in it", (r[0]["gate"], r[1]["gate"]), ("lone", "plain"))
check("...it counts for less than a plain click", two[0].s[0] < plain.s[0] / 2, True)
two = {0: sf.SlipFilter(JAC), 1: sf.SlipFilter(JAC)}
r = sf.teach_click(two, {0: np.array([20.0, 0.0]), 1: np.array([-20.0, 0.0])}, 1.0)
check("both off, opposite ways: not a move", (r[0]["gate"], r[1]["gate"]), ("plain", "plain"))
two = {0: sf.SlipFilter(JAC), 1: sf.SlipFilter(JAC)}
r = sf.teach_click(two, {1: np.array([20.0, 0.0])}, 1.0)
check("one eye taught: a plain click", (list(r), r[1]["gate"]), ([1], "plain"))

# --- SteamVR's looks: a bias of their own, then the slow slip ---
f = sf.SlipFilter(JAC)
B = np.array([8.0, -4.0])   # SteamVR reads 2 degrees off, through our where fit
t = 0.0
for _ in range(120):        # a minute of steady looks at 2 a second, the headset where it was calibrated
    f.update("steam", B, t)
    t += 0.5
check("SteamVR off by 2 degrees right after the calibration: its bias, not a slip", near(f.s, (0, 0), 1.0), True)
check("...learnt as its bias", near(f.x[sf.STEAM], B, 1.0), True)
slip = np.zeros(2)
for _ in range(60):         # the headset slides 6 px over 30 s
    slip += (0.1, 0.0)
    f.update("steam", slip + B, t)
    t += 0.5
for _ in range(120):
    f.update("steam", slip + B, t)
    t += 0.5
check("...a slow slide after it: followed", near(f.s, slip, 1.5), True)
s0 = f.s
r = f.update("steam", f.expect("steam") + (40.0, 0.0), t)   # 10 degrees off
check("one wild reading moves it little (Huber)", (near(f.s, s0, 2.0), r["w"] < 1), (True, True))

# --- The glint estimate: the same, with its own bias ---
f = sf.SlipFilter(JAC)
G = np.array([-6.0, 3.0])
for i in range(60):
    f.update("glint", G, float(i))
check("the glint estimate's offset is its bias", (near(f.s, (0, 0), 1.0), near(f.x[sf.GLINT], G, 1.5)), (True, True))

# --- The headset off and on ---
f = sf.SlipFilter(JAC)
t = 0.0
for _ in range(120):
    f.update("steam", B, t)
    t += 0.5
f.reseat()
check("reseat: unsure", (f.reseated, f.slip_sd() > 10), (True, True))
for _ in range(10):         # put back 30 px off: five seconds of steady looks
    f.update("steam", np.array([30.0, 0.0]) + B, t)
    t += 0.5
check("...a few steady looks find the new seating", near(f.s, (30, 0), 3.0), True)
check("...sure enough for the fit pill", f.slip_sd() <= gazecheck.PILL_SD, True)
check("...still reseated until a click", f.reseated, True)
f.click((30.0, 0.0), t)
check("a click after a reseat counts from one", (f.reseated, f.clicks), (False, 1))


# --- adjusting: follows a deliberate move faster ---
def move(lease):
    f = sf.SlipFilter(JAC)
    t = 0.0
    for _ in range(120):
        f.update("steam", B, t)
        t += 0.5
    if lease:
        f.adjusting(t, 2.0)
    for _ in range(4):      # moved 12 px, two seconds of looks
        if lease:
            f.adjusting(t, 2.0)
        f.update("steam", np.array([12.0, 0.0]) + B, t)
        t += 0.5
    return f.s[0]


check("adjusting: a deliberate move followed within two seconds", (move(True) > 9.0, move(False) < 6.0), (True, True))

# --- Kept across restarts ---
f = sf.SlipFilter(JAC)
f.click((5.0, 2.0), 1.0)
g = sf.SlipFilter.from_json(json.loads(json.dumps(f.to_json())), JAC)
check("to_json and back", (near(g.x, f.x, 1e-3), near(g.P, f.P, 1e-6), g.clicks), (True, True, 1))
g = sf.SlipFilter.from_json({"base": [3.0, 1.0], "ref": None, "meas": [[3, 1], [3, 1]]}, JAC)
check("the old shift's JSON: its slip", (g.s.tolist(), g.clicks), ([3.0, 1.0], 2))
check("a broken one: fresh", sf.SlipFilter.from_json({"x": "no"}, JAC).s.tolist(), [0.0, 0.0])

# --- ft-eyes ---
DOTS = [d[:2] for d in gazecheck.check_dots("full", True)]
CENTRE = {0: (300.0, 200.0), 1: (330.0, 205.0)}


def pupil(eye, yaw, pitch):
    x0, y0 = CENTRE[eye]
    return x0 - PX * yaw, y0 - PX * pitch


class Clock:
    t = 100.0


def hold(tracker, yaw, pitch, seen=(0, 1), slip=(0.0, 0.0), drift=0.0):
    """Half a second at (yaw, pitch), the headset slipped by `slip` px: 45 frames per eye seen.
    Returns the window's (start, end)."""
    t0 = Clock.t
    for i in range(45):
        for e in tracker.eyes:
            if e.eye in seen:
                x, y = pupil(e.eye, yaw, pitch)
                e.history.append((t0 + i / 90, x + slip[0] + i * drift, y + slip[1], float("nan"), float("nan")))
                e.last_t = t0 + i / 90
    Clock.t = t0 + 1.0
    return t0, t0 + 0.5


def calibrate():
    tracker = fe.Tracker()
    tracker.command("calib-start")
    for yaw, pitch in DOTS:
        t0, t1 = hold(tracker, yaw, pitch)
        tracker.command(f"calib-point {t0:.6f} {t1:.6f} {yaw:.4f} {pitch:.4f}")
    tracker.command("calib-fit")
    return tracker


tracker = calibrate()
right, left = tracker.eyes
check("each eye's filter reads the fit's 4 px a degree", near(np.abs(right.filter.jac), np.abs(np.array(JAC)), 0.05), True)
st = tracker.status()
check("status: the slip filter", st["slip"], "filter")
check("...each eye's slip and how sure", sorted(st["eyes"]["right"]),
      ["clicks", "glint_bias_deg", "reseat", "shift", "slip_deg", "slip_sd", "steam_bias_deg"])
check("...none yet", (st["eyes"]["right"]["slip_deg"], st["eyes"]["right"]["reseat"]), ([0.0, 0.0], False))

# A look: SteamVR says (5, 2), the eyes slipped 8 px right in the image.
r = None
for _ in range(20):
    t0, t1 = hold(tracker, 5.0, 2.0, slip=(8.0, 0.0))
    r = tracker.command(f"look {t1:.6f} 5.0 2.0")
check("look: each eye's shift against SteamVR's look", [round(float(v)) for v in r.split()[2:]], [8, 0, 8, 0])
f = right.filter
check("...ten seconds of them: right after a calibration, mostly SteamVR's bias, but the two add up",
      (0.0 < f.s[0] < 4.0, near(f.s + f.x[sf.STEAM], (8, 0), 1.0)), (True, True))
t0, t1 = hold(tracker, 40.0, 0.0)
check("look past the dots: nothing", tracker.command(f"look {t1:.6f} 40.0 0.0"), "look ok nan nan nan nan")
t0, t1 = hold(tracker, 0.0, 0.0, drift=1.0)   # 27 px over the look's window
check("look while the pupils moved: nothing", tracker.command(f"look {t1:.6f} 0.0 0.0"), "look ok nan nan nan nan")
check("adjusting", tracker.command("adjusting 2"), "adjusting ok")
check("...a lease from the newest frame", right.filter.adjust_until, tracker.frame_time() + 2.0)
check("...at most 10 s", (tracker.command("adjusting 99"), right.filter.adjust_until), ("adjusting ok", tracker.frame_time() + 10.0))

# A click there: the slip pinned, and SteamVR's share of those looks its bias.
t0, t1 = hold(tracker, 5.0, 2.0, slip=(8.0, 0.0))
tracker.command(f"click {t1:.6f} 5.0 2.0")
rec = json.loads(fe.CLICKS.read_text().splitlines()[-1])
check("the clicks log: what the click did", sorted(k for k in rec["eyes"]["0"] if k in ("gate", "innov_deg", "sd_before", "after")),
      ["after", "gate", "innov_deg", "sd_before"])
st = tracker.status()["eyes"]["right"]
check("the status' slip_deg: 8 px right in the image is 2 degrees of yaw", near(st["slip_deg"], (-2, 0), 0.6), True)

# The headset off: the slip unsure again until readings come.
right.reseat(Clock.t)
check("reseat: in the status", tracker.status()["eyes"]["right"]["reseat"], True)
check("...not 'taught by a click' in the shared memory's flags", bool(right.filter.clicks and not right.filter.reseated), False)

# state.json: kept, and the old shift's read.
tracker.save_state()
again = fe.Tracker()
check("state.json keeps each eye's slip", near(again.eyes[0].filter.s, right.filter.s, 1e-3), True)
check("...reseated at start (the headset may have been off)", again.eyes[0].filter.reseated, True)
d = json.loads(fe.STATE.read_text())
fe.STATE.write_text(json.dumps({"calibration": d["calibration"],
                                "shift": {"0": {"base": [6.0, -2.0], "ref": None, "meas": [[6, -2]]}}}))
old = fe.Tracker()
check("an old state.json's shift: its slip", (old.eyes[0].filter.s.tolist(), old.eyes[1].filter.s.tolist()),
      ([6.0, -2.0], [0.0, 0.0]))

# slip.jsonl
row = tracker.log_row()
check("slip.jsonl's row: each eye's filter state and variances", {"x", "P", "slip_deg", "slip_sd"} <= set(row["eyes"]["0"]),
      True)
tracker.log.write(row)
tracker.log.flush()
lines = [json.loads(v) for v in fe.SLIP_LOG.read_text().splitlines()]
check("...events too", {"look", "click", "reseat", "calibration", "adjusting"} <= {v.get("event") for v in lines}, True)

# The log moves aside past its size.
import sliplog  # noqa: E402

small = sliplog.SlipLog(Path(HOME) / "small.jsonl", max_bytes=200)
for i in range(10):
    small.write({"t": i, "pad": "x" * 40})
small.flush()
check("past its size the log moves to .old", ((Path(HOME) / "small.old.jsonl").exists(),
                                               (Path(HOME) / "small.jsonl").stat().st_size < 200), (True, True))

# --- The pill's reading ---
own = {"eyes": {"right": {"slip_deg": [-6.0, 1.0], "slip_sd": 1.0}, "left": {"slip_deg": [-4.0, 3.0], "slip_sd": 2.0}}}
check("pill_terms: both eyes' mean", gazecheck.pill_terms(own), {"up": 2.0, "left": -5.0})
own["eyes"]["left"]["slip_sd"] = 9.0
check("...an unsure eye left out", gazecheck.pill_terms(own), {"up": 1.0, "left": -6.0})
own["eyes"]["right"]["slip_sd"] = 9.0
check("...neither sure: no reading", gazecheck.pill_terms(own), None)
check("...nothing from ft-eyes: none", gazecheck.pill_terms({}), None)

shutil.rmtree(HOME, ignore_errors=True)
if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all ok")
