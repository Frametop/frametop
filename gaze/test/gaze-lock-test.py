#!/usr/bin/env python3
"""Offline test of the fixation lock on the gaze's direction in the room (gazecal.GazeLock) and
of ft-gazed's blink settle for our tracker (BLINK_SETTLE). Made-up samples only: ft-gazed's
Service is built without its sockets, so nothing reaches the live gaze service.

  gaze/test/gaze-lock-test.py
"""
import importlib.machinery
import importlib.util
import math
import os
import random
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from gazecal import EyeWeights, Fixation, GazeLock, angles, direction, rotate  # noqa: E402

loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(HERE, "..", "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)

failures = []


def check(what, ok, detail=""):
    if not ok:
        failures.append(what)
        print(f"FAIL {what} {detail}", flush=True)


def head(yaw, pitch=0.0, roll=0.0):
    """A head pose's quaternion [w, x, y, z]: yaw about +Y (left), then pitch about +X (up), then
    roll about -Z, as a headset's pose matrix would give."""
    def q(axis, deg):
        h = math.radians(deg) / 2
        return [math.cos(h)] + [math.sin(h) * a for a in axis]

    def mul(a, b):
        w1, x1, y1, z1 = a
        w2, x2, y2, z2 = b
        return [w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2]
    return mul(mul(q((0, 1, 0), yaw), q((1, 0, 0), pitch)), q((0, 0, -1), roll))


def room(q, yaw, pitch):
    """Where a head-relative gaze points in the room, as (yaw, pitch)."""
    return angles(rotate(q, direction(yaw, pitch)))


def head_rel(q, ryaw, rpitch):
    return angles(rotate(q, direction(ryaw, rpitch), inverse=True))


def off(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(direction(*a), direction(*b)))))))


# --- The conventions: ft-gaze's Direction and Angles, and the quaternion as a pose rotation
check("straight ahead is -Z", direction(0, 0) == (-0.0, 0.0, -1.0), direction(0, 0))
check("yaw turns left (-X), pitch looks up", direction(90, 0)[0] < -0.99 and direction(0, 90)[1] > 0.99)
rng = random.Random(1)
worst = 0.0
for _ in range(500):
    q = head(rng.uniform(-180, 180), rng.uniform(-60, 60), rng.uniform(-30, 30))
    y, p = rng.uniform(-50, 50), rng.uniform(-40, 40)
    back = head_rel(q, *room(q, y, p))
    worst = max(worst, abs(back[0] - y), abs(back[1] - p))
check("into the room and back", worst < 1e-9, worst)
check("a head turned 30 deg left, eyes ahead: the room's 30 left", max(map(abs, (room(head(30), 0, 0)[0] - 30,
                                                                              room(head(30), 0, 0)[1]))) < 1e-9)
check("a head pitched 10 up, eyes 5 up: the room's 15 up", abs(room(head(0, 10), 0, 5)[1] - 15) < 1e-9)

# --- A look at a spot in the room while the head turns (the eyes turn the other way)
def turning(speed, lock, poses=True, noise=0.05, seconds=2.0):
    """The pointer's place in the room (drawn with the newest head, as the helper does) while the
    eyes hold room yaw 10, pitch 5 and the head turns at `speed` deg/s: the worst miss, and the
    jumps over 0.5 deg between samples."""
    rng = random.Random(2)
    worst, jumps, last = 0.0, 0, None
    for i in range(int(90 * seconds)):
        t = i / 90
        q = head(-20 + speed * t, 3.0, 2.0)
        hy, hp = head_rel(q, 10.0 + rng.gauss(0, noise), 5.0 + rng.gauss(0, noise))
        if isinstance(lock, GazeLock):
            out = lock(hy, hp, t, q if poses else None, q if poses else None)
        else:
            out = lock(hy, hp, t, 1.0)
        at = room(q, *out)
        if i >= 3:
            worst = max(worst, off(at, (10.0, 5.0)))
            jumps += last is not None and off(at, last) > 0.5
        last = at
    return worst, jumps


w, j = turning(20, GazeLock())
check("room lock, head turning 20 deg/s: the pointer stays on the spot", w < 0.3 and j == 0, (w, j))
w, j = turning(60, GazeLock())
check("room lock, head turning 60 deg/s: still on it", w < 0.3 and j == 0, (w, j))
w, j = turning(20, Fixation(1.0))
check("the head-relative lock rides along and snaps back (why the room lock)", w > 0.8 and j >= 10, (w, j))
w, j = turning(20, GazeLock(), poses=False)
check("no poses (an older ft-gaze): the head-relative lock", w > 0.8 and j >= 10, (w, j))

# A look elsewhere moves it at once (two samples), and a still head is as before
lock = GazeLock()
q = head(15, -5, 1)
for i in range(30):
    out = lock(2.0, 1.0, i / 90, q, q)
check("still: the look", off(out, (2.0, 1.0)) < 1e-6, out)
lock(20.0, 1.0, 30 / 90, q, q)
out = lock(20.0, 1.0, 31 / 90, q, q)
check("a saccade: there after two samples", off(out, (20.0, 1.0)) < 1e-6, out)
one = lock(2.0, 1.0, 32 / 90, q, q)
check("one stray sample is held off", off(one, (20.0, 1.0)) < 1e-6, one)
fix, lock2 = Fixation(1.0), GazeLock()
same = 0.0
for i in range(200):
    y, p = 3 + 0.3 * math.sin(i / 7), -2 + 0.3 * math.cos(i / 5)
    a, b = fix(y, p, i / 90, 1.0), lock2(y, p, i / 90, q, q)
    same = max(same, off(a, b))
check("a still head: as the head-relative lock", same < 0.02, same)
lock2(3, -2, 3.0, q, q)
lock2(3, -2, 3.1, None, None)
check("switching to no poses starts over", lock2.count == 0 and lock2.head.count == 1)
# Newest pose differs from the sample's: the room direction is kept, re-expressed in the newest head
lock3 = GazeLock()
then, now = head(0), head(4)
out = lock3(10.0, 0.0, 0.0, then, now)
check("drawn with the newest head", abs(out[0] - 6.0) < 1e-9 and abs(out[1]) < 1e-9, out)


# --- ft-gazed's own path: an eye stays out for BLINK_SETTLE after its lids came down
def service():
    svc = gazed.Service.__new__(gazed.Service)
    svc.override, svc.tracker, svc.source, svc.one_eye = None, "own", "mmap1", None
    svc.counts = dict.fromkeys(("samples", "sent", "blinks", "one_eye", "one_eye_used", "lost_left", "lost_right",
                                "looking_down", "dropped"), 0)
    svc.opens, svc.lost = (deque(maxlen=90), deque(maxlen=90)), [False, False]
    svc.fix, svc.last_sample, svc.low_until = GazeLock(radius=1.0), 0.0, [0.0, 0.0]
    svc.weights = {"own": EyeWeights()}
    svc.sent = []
    svc.send = lambda t, hy, hp, rhy, rhp, eyes: svc.sent.append((t, hy, hp))
    return svc


def blink(settle):
    """The right eye's lids down for 3 samples, and its pupil found wrong (30, -20) for 3 more
    after they're up: the gaze sent from 60 on."""
    saved = gazed.BLINK_SETTLE
    gazed.BLINK_SETTLE = settle
    try:
        svc = service()
        q = head(0)
        for i in range(120):
            closing = 60 <= i < 63
            right = [30.0, -20.0] if 60 <= i < 66 else [6.0, 2.0]
            svc.on_eyes_sample({"t": i / 90, "head": {"q": q}, "src": {
                "mmap1": {"unc": [0.001, 0.001], "open": [0.8, 0.0 if closing else 0.8]},
                "own": {"hy": 5.0, "hp": 2.0, "hq": q, "eyes": [[4.0, 2.0], right]}}}, True)
    finally:
        gazed.BLINK_SETTLE = saved
    return svc, [s for s in svc.sent if s[0] >= 60 / 90]


svc, sent = blink(gazed.BLINK_SETTLE)
worst = max(off(s[1:], (5.0, 2.0)) for s in sent)
check("settle: the bad finds after the lids came up don't reach the pointer", worst < 1.1, worst)
check("settle: then both eyes again", off(sent[-1][1:], (5.0, 2.0)) < 0.2, sent[-1])
check("settle: counted as one-eye samples", svc.counts["one_eye"] >= 6, svc.counts)
_, sent = blink(0.0)
worst = max(off(s[1:], (5.0, 2.0)) for s in sent)
check("without it, they do (the test's control)", worst > 5, worst)

print("FAILED: " + ", ".join(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
