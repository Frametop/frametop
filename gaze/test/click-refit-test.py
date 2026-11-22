#!/usr/bin/env python3
"""Offline test of our tracker's click refit (gaze/tracker/clickrefit.py): it finds a scale and
turn the shift can't, refuses clicks bunched in one place and a refit that doesn't help, leaves
out a click far from where you looked, and an eye without a glint fit gets one from clicks.

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/click-refit-test.py
"""
import sys
from pathlib import Path

GAZE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAZE / "tracker"))

import numpy as np  # noqa: E402

import clickrefit as cr  # noqa: E402
import eyes_model  # noqa: E402

failures = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not ok:
        failures.append(name)


rng = np.random.default_rng(7)
A_TRUE = 1.12 * np.array([[np.cos(0.1), -np.sin(0.1)], [np.sin(0.1), np.cos(0.1)]])


def clicks(n, spread=12.0, noise=0.4, A=A_TRUE):
    """Clicks whose calibration gaze is the truth bent by A's inverse: the refit should find A."""
    out = []
    for _ in range(n):
        t = rng.uniform(-spread, spread, 2)
        g = np.linalg.solve(A, t) + rng.normal(0, noise, 2)
        out.append((g, t, False))
    return out


r, why = cr.fit(clicks(30), np.zeros(2))
check("recovers a scale and turn", r is not None and np.abs(r.A - A_TRUE).max() < 0.05,
      why if r is None else np.round(r.A, 3).tolist())
if r is not None:
    t = np.array([7.0, -4.0])
    check("undo then apply gives the truth back", np.allclose(r.apply(r.undo(t)), t))

r, why = cr.fit(clicks(30, spread=1.5), np.zeros(2))
check("refuses clicks bunched in one place", r is None and "spread" in why, why)

r, why = cr.fit(clicks(30, A=np.eye(2), noise=1.0), np.zeros(2))
check("refuses a refit that doesn't beat none", r is None, why)

r, why = cr.fit(clicks(6), np.zeros(2))
check("refuses too few clicks", r is None and "clicks" in why, why)

ks = clicks(30)
ks[-8] = (ks[-8][0] + np.array([15.0, 0.0]), ks[-8][1], False)
check("trim leaves out a click far off", len(cr.trim(ks)) == 29)

w = cr.weights([False] * 15 + [True])
check("newest counts most, a glance half", abs(w[-1] - cr.GLANCE) < 1e-9 and abs(w[0] - 0.5) < 1e-9)


class Cal:
    """Enough of eyes_model.Calibration for a Learner."""
    def __init__(self):
        self.fits, self.box = {}, {1: [-15, 15, -10, 10]}

    def has(self, name, eye):
        return (name, eye) in self.fits


cal = Cal()
learn = cr.Learner(cal, 1)
events = []
for g, t, _ in clicks(20):
    mid = np.array([300.0, 200.0])
    pupil = mid + t * 2.0 + rng.normal(0, 0.3, 2)   # pupil less glint mid: about the gaze
    events += learn.add(g, t, False, pupil, mid)
check("an eye without a glint fit gets one from clicks", cal.has("glint", 1)
      and any("glint_fit" in e for e in events), str(events[:2]))
if cal.has("glint", 1):
    got = cal.fits["glint", 1].one(*(np.array([10.0, -6.0]) * 2.0))
    check("...and it maps", np.hypot(*(got - [10.0, -6.0])) < 0.5, np.round(got, 2).tolist())
check("its refit came on", learn.refit is not None and any(e.get("refit") == "on" for e in events))

back = cr.Learner(cal, 1, learn.to_json())
check("saved and loaded", back.refit is not None and np.allclose(back.refit.A, learn.refit.A, atol=1e-3)
      and len(back.clicks) == len(learn.clicks))

cal2 = Cal()
cal2.fits["glint", 1] = eyes_model.Quad(rng.normal(0, 1, (12, 2)), rng.normal(0, 1, (12, 2)))
kept = cal2.fits["glint", 1]
learn2 = cr.Learner(cal2, 1)
for g, t, _ in clicks(15):
    learn2.add(g, t, False, t * 2.0 + 300, np.array([300.0, 300.0]))
check("a calibration's own glint fit is kept", cal2.fits["glint", 1] is kept)

if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all ok")
