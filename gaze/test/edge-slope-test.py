#!/usr/bin/env python3
"""Offline test of the calibration's edge (gazecal.Correction, DEE-188): past the range of view
its dots covered, the correction goes on in a straight line with the fit's slope at that edge,
capped at EDGE_SLOPE, where it used to hold still. A made-up tracker that understates how far
the eyes turn (0.8 of the true angle, as with eyes close to the lenses) is calibrated on a
20-degree ring, then read at 35 degrees: it must land at least half a degree closer than the old
held-still correction did. Inside the ring and its margin nothing changes.

  gaze/test/edge-slope-test.py
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from gazecal import Correction  # noqa: E402

GAIN = 0.8
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + (f" ({detail})" if detail else ""))
    fails += 0 if ok else 1


def ring(radius, n=8, turn=0.0):
    return [(radius * math.cos(turn + 2 * math.pi * k / n), radius * math.sin(turn + 2 * math.pi * k / n))
            for k in range(n)]


def held(C, mode, hy, hp):
    """The correction as it was before: the curve's value at the edge, held still past it."""
    f = C.features(hy, hp, mode)
    cy = sum(a * b for a, b in zip(C.a[0], f))
    cp = sum(a * b for a, b in zip(C.a[1], f))
    if mode.endswith("+grid"):
        for (i, k), w in C.weights(hy, hp):
            cy += w * C.grid[i][k][0]
            cp += w * C.grid[i][k][1]
    return cy, cp


def miss(correction, ty, tp):
    """How far the corrected gaze lands from a true look at (ty, tp), degrees."""
    ry, rp = GAIN * ty, GAIN * tp
    cy, cp = correction(ry, rp)
    return math.hypot(ry + cy - ty, rp + cp - tp)


# The calibration's dots: the centre, a 10-degree ring and a 20-degree ring, as raw readings
# with the whole error at each (true - raw).
truths = [(0.0, 0.0)] + ring(10, 6) + ring(20, 6, math.pi / 6)
points = [(GAIN * ty, GAIN * tp, ty - GAIN * ty, tp - GAIN * tp) for ty, tp in truths]

for mode in ("affine", "quadratic", "quadratic+grid"):
    C = Correction()
    C.fit(points, mode)
    new = lambda hy, hp: C.get(hy, hp, mode)  # noqa: E731
    old = lambda hy, hp: held(C, mode, hy, hp)  # noqa: E731
    same = all(new(GAIN * ty, GAIN * tp) == old(GAIN * ty, GAIN * tp) for ty, tp in truths + ring(21, 12))
    check(f"{mode}: inside the ring and its margin, as before", same)
    for ty, tp in ((35, 0), (-35, 0), (0, 32), (27, -27)):
        m, h = miss(new, ty, tp), miss(old, ty, tp)
        check(f"{mode}: a look at ({ty}, {tp}) past the ring", m < h - 0.5, f"{m:.2f} deg, held still {h:.2f}")
    # Past the edge, the pointer still moves outward as the eyes do.
    step = (new(GAIN * 40, 0)[0] + GAIN * 40) - (new(GAIN * 30, 0)[0] + GAIN * 30)
    check(f"{mode}: keeps going outward past the ring", step > GAIN * 10, f"{step:.2f} deg for 10")

# A wild fit's edge can't run away: its slope is capped.
C = Correction()
C.fit(points, "affine")
C.a[0][1] = 300.0  # 10 degrees of correction per degree of yaw
edge = C.range[1] + C.MARGIN
step = C.get(edge + 10, 0, "affine")[0] - C.get(edge, 0, "affine")[0]
check("a wild slope is capped at EDGE_SLOPE", abs(step - 10 * C.EDGE_SLOPE) < 1e-9, f"{step:.2f} for 10 deg")

# Offset and none have no slope; nothing is fitted yet: no range, no change.
C = Correction()
C.fit(points, "offset")
check("offset: the same everywhere", C.get(0, 0, "offset") == C.get(40, 30, "offset"))
check("none: no correction", Correction().get(40, 0, "none") == (0.0, 0.0))
check("unfitted: no correction", Correction().get(40, 0, "quadratic") == (0.0, 0.0))

print("all ok" if not fails else f"{fails} failed")
sys.exit(1 if fails else 0)
