#!/usr/bin/env python3
"""Offline test of our tracker's slip filter (gaze/tracker/slipfilter.py) and ft-eyes' parts for
it: clicks that fade, both eyes judged together, SteamVR's steady looks and the glint estimate
as weaker readings with biases of their own, the headset off and on, the "adjusting" lease,
state.json (and the old shift's), the status the fit pill reads, and slip.jsonl.

Pupil positions are written into ft-eyes' eye histories as its frames would leave them: no
cameras, SteamVR or live gaze service. Needs the tracker's Python (numpy, OpenCV).

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/slip-filter-test.py
"""
import copy
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
GLINT, STEAM = slice(2, 4), slice(4, 6)   # an eye's x: its slip, glint bias, steam bias (px)


def near(v, want, tol):
    return bool(np.all(np.abs(np.asarray(v, float) - np.asarray(want, float)) <= tol))


# --- The filter on its own ---
f = sf.SlipFilter(JAC)
check("fresh: no slip", f.s.tolist(), [0.0, 0.0])
check("...sure to START_SD and OWN_START_SD", round(f.slip_sd(), 3), round(float(np.hypot(sf.START_SD, sf.OWN_START_SD)), 3))
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

# --- A glance: a free mouse's click (you most likely looked at what you clicked) ---
moved_pair, glance_pair = sf.load(None, {0: JAC, 1: JAC}), sf.load(None, {0: JAC, 1: JAC})
lesson = {0: np.array([20.0, 4.0]), 1: np.array([18.0, 6.0])}
sf.teach_click(moved_pair, lesson, 1.0)
r = sf.teach_click(glance_pair, lesson, 1.0, glance=True)
check("a glance both eyes 5 degrees off the same way: never the headset moved", (r[0]["gate"], r[1]["gate"]),
      ("glance", "glance"))
plain_one = sf.load(None, {0: JAC, 1: JAC})
sf.teach_click(plain_one, {0: lesson[0]}, 1.0)   # one eye taught: a plain click
check("...it pulls less than a plain click, and far less than the headset moved",
      glance_pair[0].s[0] < plain_one[0].s[0] - 2 and glance_pair[0].s[0] < 0.7 * moved_pair[0].s[0], True)
check("...and isn't counted as a click", (glance_pair[0].clicks, moved_pair[0].clicks), (0, 1))
g = sf.load(None, {0: JAC, 1: JAC})
g[0].reseat(), g[1].reseat()
sf.teach_click(g, {0: np.array([8.0, 0.0]), 1: np.array([8.0, 0.0])}, 1.0, glance=True)
check("after a reseat: a glance teaches, but the one-dot check is still asked for",
      (g[0].s[0] > 4.0, g[0].reseated, g[1].reseated), (True, True, True))

# --- SteamVR's looks: a bias of their own, then the slow slip ---
f = sf.SlipFilter(JAC)
B = np.array([8.0, -4.0])   # SteamVR reads 2 degrees off, through our where fit
t = 0.0
for _ in range(120):        # a minute of steady looks at 2 a second, the headset where it was calibrated
    f.update("steam", B, t)
    t += 0.5
check("SteamVR off by 2 degrees right after the calibration: its bias, not a slip", near(f.s, (0, 0), 1.0), True)
check("...learnt as its bias", near(f.x[STEAM], B, 1.0), True)
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
plain = copy.deepcopy(f)
r = f.update("steam", f.expect("steam") + (80.0, 0.0), t)   # 20 degrees off: a blink, say
plain.update("steam", plain.expect("steam") + (80.0, 0.0), t, robust=False)
moved, plain_moved = abs(f.s[0] - s0[0]), abs(plain.s[0] - s0[0])
check("one wild reading counts for less (Huber): under half what it would", (r["w"] < 0.5, moved < 0.5 * plain_moved),
      (True, True))
check("...and moves it under a degree", moved < PX, True)
try:
    f.update("steam", (float("nan"), 0.0), t)
    check("a reading that isn't finite: refused", False, True)
except ValueError:
    check("a reading that isn't finite: refused", near(f.s, f.s, 0) and bool(np.all(np.isfinite(f.x))), True)

# --- The glint estimate: the same, with its own bias ---
f = sf.SlipFilter(JAC)
G = np.array([-6.0, 3.0])
for i in range(60):
    f.update("glint", G, float(i))
check("the glint estimate's offset is its bias", (near(f.s, (0, 0), 1.5), near(f.x[GLINT], G, 1.5)), (True, True))

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


def lease_sd(glint_every):
    """slip_sd's peak over a minute of the fit pill's lease, with glint readings alone (SteamVR's
    looks don't count while you look at the pill, past steamlook.RAW_DEG)."""
    f = sf.SlipFilter(JAC)
    t, peak, next_glint = 0.0, 0.0, 0.0
    while t < 60.0:
        f.adjusting(t, 2.0)
        if glint_every and t >= next_glint:
            f.update("glint", f.expect("glint"), t)
            next_glint = t + glint_every
        f.predict(t)
        if t > 10.0:
            peak = max(peak, f.slip_sd())
        t += 0.1
    return peak


check("while the pill shows, glints alone keep the slip sure enough for it to follow",
      lease_sd(1.0) <= gazecheck.PILL_FOLLOW_SD, True)
check("...but with no readings at all it doesn't", lease_sd(None) > gazecheck.PILL_FOLLOW_SD, True)
f = sf.SlipFilter(JAC)
f.predict(0.0)
f.adjusting(0.0, 1.0)
f.predict(10.0)   # frames stalled past the lease's end: only its second is fast
check("a lease counts only until it ends", round(float(f.P[0, 0]), 3),
      round((sf.START_SD ** 2 + sf.OWN_START_SD ** 2 + sf.ADJUST_RATE ** 2 * 1.0 + sf.SLIP_RATE ** 2 * 9.0
             + sf.OWN_RATE ** 2 * 10.0) * PX ** 2, 3))

# --- Glint readings judged across the eyes (eyeslip.take_glint) ---
import eyeslip  # noqa: E402


class FakeCal:
    def __init__(self):
        self.glint = {0, 1}

    def has(self, kind, eye):
        return kind != "glint" or eye in self.glint

    def inside(self, eye, gaze):
        return abs(gaze[0]) < 30


class FakeSlip:
    def __init__(self):
        self.z = None

    def update(self, t):
        z, self.z = self.z, None
        return z


class FakeEye:
    def __init__(self, eye, headset):
        self.eye, self.cal, self.slip = eye, FakeCal(), FakeSlip()
        self.filter = sf.SlipFilter(JAC, headset=headset, eye=eye)
        self.glint = self.glint_innov = None


def settled():
    hs = sf.Headset()
    eyes = [FakeEye(0, hs), FakeEye(1, hs)]
    for i in range(30):
        for e in eyes:
            e.slip.z = (0.0, 0.0)
            eyeslip.take_glint(e, eyes[1 - e.eye], float(i), (0, 0))
    return eyes


eyes = settled()
eyes[0].slip.z = (1.0, 0.0)
check("glint: none outside its dots", eyeslip.take_glint(eyes[0], eyes[1], 30.0, (40, 0)), None)
eyes[0].slip.z = (1.0, 0.0)
r = eyeslip.take_glint(eyes[0], eyes[1], 30.0, (0, 0))
check("...a reading for the log", (r["z"], r["moved"], r["kind"]), ([1.0, 0.0], False, "glint"))
KNOCK = 40.0   # px: 10 degrees


def glints(eyes, t, z, gaze=(0, 0)):
    """Both eyes' glint readings half a second apart: the readings for the log."""
    out = []
    for e, dt in ((eyes[0], 0.0), (eyes[1], 0.5)):
        if z[e.eye] is not None:
            e.slip.z = z[e.eye]
            out.append(eyeslip.take_glint(e, eyes[1 - e.eye], t + dt, gaze))
    return out


eyes = settled()
r = glints(eyes, 30.0, {0: (KNOCK, 0.0), 1: (KNOCK, 0.0)})
check("both eyes' glints jump the same way: not a knock yet (a misread pair snaps back)",
      [v["moved"] for v in r], [False, False])
r = glints(eyes, 31.0, {0: (KNOCK, 0.0), 1: (KNOCK, 0.0)})
check("...still there a second later: the headset knocked", r[-1]["moved"], True)
glints(eyes, 32.0, {0: (KNOCK, 0.0), 1: (KNOCK, 0.0)})
check("...taken nearly whole by the next readings", [float(e.filter.s[0] + e.filter.x[GLINT][0]) > 0.8 * KNOCK
                                                       for e in eyes], [True, True])
check("...and as the slip, not the glints' bias", [abs(float(e.filter.x[GLINT][0])) < 0.2 * KNOCK for e in eyes],
      [True, True])
eyes = settled()
moved = []
for t in (30.0, 31.0):   # a push on purpose, as 2026-11-22's: both eyes' glints 18-20 degrees off
    moved += [v["moved"] for v in glints(eyes, t, {0: (-10.0, 76.0), 1: (3.0, 72.0)})]
check("a 19 degree push: a knock at the second readings", moved, [False, False, False, True])
eyes = settled()
for t in (30.0, 31.0, 32.0, 33.0):   # one eye's glints alone: a misread pair, likelier
    r = glints(eyes, t, {0: (KNOCK, 0.0), 1: (0.0, 0.0)})
check("one eye's glints alone: not a knock", (r[0]["moved"], eyes[0].filter.s[0] < 0.3 * KNOCK), (False, True))
eyes = settled()
moved = []
for t in (30.0, 31.0, 32.0):
    moved += [v["moved"] for v in glints(eyes, t, {0: (KNOCK, 0.0), 1: (KNOCK, 0.0)}, gaze=(25, 0))]
check("looking far aside (the glint fit's error shared by both eyes): not a knock", any(moved), False)
eyes = settled()
moved = []
for t in (30.0, 31.0, 32.0):
    moved += [v["moved"] for v in glints(eyes, t, {0: (140.0, 0.0), 1: (140.0, 0.0)})]
check("past KNOCK_MAX_DEG: a bad pair, not a knock", any(moved), False)
eyes = settled()
eyes[1].filter = None   # a one-eye calibration
for t in (30.0, 31.0):
    r = glints(eyes, t, {0: (KNOCK, 0.0), 1: None})
check("one eye mapped: two readings aren't enough", r[0]["moved"], False)
r = glints(eyes, 32.0, {0: (KNOCK, 0.0), 1: None})
check("...three are a knock", r[0]["moved"], True)
eyes = settled()
for e in eyes:
    e.cal.glint = {0}   # the other eye mapped by its pupil alone (too few glint dots)
for t in (30.0, 31.0):
    r = glints(eyes, t, {0: (KNOCK, 0.0), 1: None})
check("other eye without a glint fit: two readings aren't enough", r[0]["moved"], False)
r = glints(eyes, 32.0, {0: (KNOCK, 0.0), 1: None})
check("...three are a knock", r[0]["moved"], True)

rng = np.random.default_rng(7)
eyes, knocks = settled(), 0
for i in range(600):   # ten minutes of steady glints at GLINT_SD's noise, half of it shared by the eyes
    common = rng.normal(0, sf.GLINT_SD * PX, 2)
    z = {e: tuple(np.sqrt(0.5) * common + np.sqrt(0.5) * rng.normal(0, sf.GLINT_SD * PX, 2)) for e in (0, 1)}
    knocks += sum(v["moved"] for v in glints(eyes, 30.0 + i, z))
check("noisy steady glints: no knocks", knocks, 0)

# --- The headset's slip and each eye's own ---
def pair():
    fs = sf.load(None, {0: JAC, 1: JAC})
    return fs[0], fs[1], fs[0].headset


r0, l0, hs = pair()
t = 0.0
for _ in range(10):   # clicks keep finding the left eye 3 degrees off, the right where it was
    sf.teach_click({0: r0, 1: l0}, {0: np.zeros(2), 1: np.array([-12.0, 0.0])}, t)
    t += 2.0
check("one eye off at every click: the eyes' own parts hold the difference, the headset their mean",
      (near(l0.own_deg() - r0.own_deg(), (3, 0), 0.5), near(sf.headset_status(hs)["slip_deg"], (1.5, 0), 0.5),
       near(r0.s, (0, 0), 1.5), near(l0.s, (-12, 0), 1.5)), (True, True, True, True))
before = (r0.s.copy(), l0.s.copy())
hs.adjusting(t, 10.0)   # then the headset is pushed 2 degrees: both eyes' glints say so
for _ in range(10):
    for e in (r0, l0):
        e.update("glint", e.x[GLINT] + before[e.eye] + (-8.0, 0.0), t)
    t += 1.0
moved0, moved1 = r0.s - before[0], l0.s - before[1]
check("...both eyes' glints moved the same way: both slips move together, each eye's own kept",
      (near(moved0, moved1, 0.3), moved0[0] < -4.0, near(l0.own_deg() - r0.own_deg(), (3, 0), 0.5)), (True, True, True))
check("the pill reads the headset's slip, not one eye's own",
      gazecheck.pill_terms({"headset": sf.headset_status(hs)})["left"] < l0.slip_deg()[0], True)
r0, l0, hs = pair()
for _ in range(10):
    r0.update("steam", r0.expect("steam") + (8.0, 0.0), float(_))
check("one eye's readings alone move the other's slip too (it's the same headset)", l0.s[0] > 1.0, True)
r0, l0, hs = pair()
r0.reseat()
sd = r0.slip_sd()
l0.reseat()
check("both eyes' reseats count once", (round(l0.slip_sd(), 6) == round(sd, 6), l0.reseated), (True, True))

# --- Kept across restarts ---
r0, l0, hs = pair()
sf.teach_click({0: r0, 1: l0}, {0: np.array([5.0, 2.0]), 1: np.array([9.0, 2.0])}, 1.0)
g = sf.load(json.loads(json.dumps(sf.save({0: r0, 1: l0}))), {0: JAC, 1: JAC})
check("save and load", (near(g[0].x, r0.x, 1e-3), near(g[1].P, l0.P, 1e-6), g[0].clicks, g[1].headset is g[0].headset),
      (True, True, 1, True))
g = sf.load({"0": {"base": [3.0, 1.0], "ref": None, "meas": [[3, 1], [3, 1]]}}, {0: JAC, 1: JAC})
check("the old shift's JSON: its slip", (g[0].s.tolist(), g[0].clicks), ([3.0, 1.0], 2))
old6 = {"0": {"x": [4.0, 0.0, 1.0, 0.0, 2.0, 0.0], "P": np.eye(6).tolist(), "clicks": 3},
        "1": {"x": [8.0, 0.0, 0.0, 0.0, 0.0, 0.0], "P": np.eye(6).tolist(), "clicks": 2}}
g = sf.load(old6, {0: JAC, 1: JAC})
check("the per-eye filters' JSON (before 2026-11-23): each eye's slip, split into the headset's and its own",
      (near(g[0].s, (4, 0), 1e-9), near(g[1].s, (8, 0), 1e-9), near(g[0].x[STEAM], (2, 0), 1e-9),
       near(sf.headset_status(g[0].headset)["slip_deg"], (-1.5, 0), 1e-3), g[1].clicks), (True, True, True, True, 2))
check("a broken one: fresh", sf.load({"x": "no"}, {0: JAC})[0].s.tolist(), [0.0, 0.0])

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
check("...the headset's slip", sorted(st["headset"]), ["slip_deg", "slip_sd"])
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
      (0.0 < f.s[0] < 4.0, near(f.s + f.x[STEAM], (8, 0), 1.0)), (True, True))
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
t0, t1 = hold(tracker, 5.0, 2.0, slip=(8.0, 0.0))
check("a glance's command", tracker.command(f"click {t1:.6f} 5.0 2.0 glance").startswith("ok "), True)
rec = json.loads(fe.CLICKS.read_text().splitlines()[-1])
check("...logged as one, judged as one", (rec.get("glance"), rec["eyes"]["0"]["gate"]), (True, "glance"))
check("...a word that isn't 'glance': refused", tracker.command(f"click {t1:.6f} 5.0 2.0 nudge").startswith("ok "), False)

# The headset off: the slip unsure again until readings come.
check("taught by the click (the shared memory's flags bit)", right.filter.taught, True)
right.slip.add(Clock.t, (300.0, 200.0), (310.0, 210.0))
right.tick(Clock.t)
right.tick(Clock.t + fe.GAP + 1.0)   # frames stopped: the headset was off
check("frames stopping for GAP: reseated", tracker.status()["eyes"]["right"]["reseat"], True)
check("...not 'taught by a click' in the shared memory's flags", right.filter.taught, False)
check("...and the glint sightings from before forgotten", (len(right.slip.rows), right.glint_innov), (0, None))
Clock.t += fe.GAP + 2.0

# Glint sightings older than their window give no estimate (they were fed again and again).
import eyes_model  # noqa: E402

class SlipCal:
    def slip(self, eye, rows):
        return np.zeros(2) if rows else None


stale = eyes_model.SlipTracker(SlipCal(), 0, every=1.0, window=2.0)
stale.add(10.0, (0, 0), (0, 0))
check("a glint estimate from fresh sightings", stale.update(11.0) is not None, True)
check("...none once they're older than the window", stale.update(20.0), None)

# Commands that don't read.
newest = tracker.frame_time()
check("look with a number that isn't: refused", tracker.command("look nan 0 0").startswith("look fail"), True)
check("look from the future: refused", tracker.command(f"look {newest + 100:.3f} 0 0").startswith("look fail"), True)
check("...the filters' clock where it was", right.filter.t <= newest + 1.0, True)
check("adjusting nan: refused", tracker.command("adjusting nan"), "adjusting fail not a number")
try:
    tracker.command(f"click {newest:.3f} inf 0")
    check("click with a number that isn't: refused", False, True)
except ValueError:
    check("click with a number that isn't: refused", bool(np.all(np.isfinite(right.filter.x))), True)

# state.json: kept, and the old shift's read.
was = fe.ft_store.write_json


def full(*a, **k):
    raise OSError(28, "No space left on device")


fe.ft_store.write_json = full
try:
    tracker.save_state()
    check("a save that fails (a full disk) doesn't stop the tracker", True, True)
except OSError:
    check("a save that fails (a full disk) doesn't stop the tracker", False, True)
fe.ft_store.write_json = was
tracker.save_state()
again = fe.Tracker()
check("state.json keeps each eye's slip", (near(again.eyes[0].filter.s, right.filter.s, 1e-3),
                                           again.eyes[0].filter.headset is again.eyes[1].filter.headset), (True, True))
check("...reseated at start (the headset may have been off)", again.eyes[0].filter.reseated, True)
d = json.loads(fe.STATE.read_text())
fe.STATE.write_text(json.dumps({"calibration": d["calibration"],
                                "shift": {"0": {"base": [6.0, -2.0], "ref": None, "meas": [[6, -2]]}}}))
old = fe.Tracker()
check("an old state.json's shift: its slip", ([round(float(v), 6) for v in old.eyes[0].filter.s], [round(float(v), 6) for v in old.eyes[1].filter.s]),
      ([6.0, -2.0], [0.0, 0.0]))

# slip.jsonl
row = tracker.log_row()
check("slip.jsonl's row: each eye's filter state and variances, and the headset's",
      ({"x", "P", "slip_deg", "slip_sd", "own_deg"} <= set(row["eyes"]["0"]), sorted(row["headset"])),
      (True, ["slip_deg", "slip_sd"]))
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
flush_every, sliplog.FLUSH_EVERY = sliplog.FLUSH_EVERY, 0.0
timed = sliplog.SlipLog(Path(HOME) / "timed.jsonl")
timed.write({"t": 1})
sliplog.FLUSH_EVERY = flush_every
check("rows reach the file without a flush (ft-eyes is stopped with os._exit)",
      len((Path(HOME) / "timed.jsonl").read_text().splitlines()), 1)
timed.event(2.0, "glint", eye=0, **{"z": [1.0, 0.0], "moved": False, "streak": 0, "kind": "glint"})  # take_glint's reading
timed.flush()
check("an event row keeps the reading's kind", json.loads((Path(HOME) / "timed.jsonl").read_text().splitlines()[-1])
      ["kind"], "glint")
check("past its size the log moves to .old", ((Path(HOME) / "small.old.jsonl").exists(),
                                               (Path(HOME) / "small.jsonl").stat().st_size < 200), (True, True))

# --- The pill's reading ---
own = {"headset": {"slip_deg": [-6.0, 1.0], "slip_sd": 1.0},
       "eyes": {"right": {"slip_deg": [-9.0, 1.0], "slip_sd": 1.0}}}
check("pill_terms: the headset's slip", gazecheck.pill_terms(own), {"up": 1.0, "left": -6.0})
own["headset"]["slip_sd"] = 9.0
check("...unsure: no reading", gazecheck.pill_terms(own), None)
check("...nothing from ft-eyes: none", gazecheck.pill_terms({}), None)
own["headset"]["slip_sd"] = 3.5
check("...while the pill shows, a looser limit", (gazecheck.pill_terms(own), gazecheck.pill_terms(own, gazecheck.PILL_FOLLOW_SD)),
      (None, {"up": 1.0, "left": -6.0}))

shutil.rmtree(HOME, ignore_errors=True)
if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all ok")
