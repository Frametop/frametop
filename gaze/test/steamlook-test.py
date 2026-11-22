#!/usr/bin/env python3
"""Offline test of gaze/steamlook.py: when SteamVR's look counts as held still, which eyes it
is, and when it goes through fusion.json's affine (only the same calibration's).

  gaze/test/steamlook-test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import steamlook  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok   " if ok else "FAIL ") + f"{label}: got {got!r}, want {want!r}", flush=True)
    if not ok:
        failures.append(label)


MADE = "2026-11-22 09:05:53"


class Fusion:   # SteamVR's affine: raw, it reads looks out from the centre too far
    def __init__(self, made=MADE, how="calibration"):
        self.info = {"how": how, "made": made}

    def steam_gaze(self, left, right, clicks=True):
        return (left[0] * 0.6, left[1] * 0.6)


def run(looks, t0=0.0, seconds=0.6, one_eye=None, fusion=None, made=MADE, sl=None, rate=90):
    """Feeds `looks(i)` -> (left, right) at `rate` a second; the looks it sent."""
    sl = sl or steamlook.SteamLooks()
    out = []
    for i in range(int(seconds * rate)):
        left, right = looks(i)
        r = sl.step(t0 + i / rate, left, right, one_eye, fusion, made)
        if r:
            out.append(tuple(round(v, 2) for v in r[1:]))
    return out


def held(yaw, pitch):
    return lambda i: ((yaw, pitch), (yaw, pitch))


got = run(held(3.0, 1.0))
check("held still: a look", got[:1], [(3.0, 1.0)])
check("  not more than every EVERY", len(got), 1)
check("  not before WINDOW", run(held(3.0, 1.0), seconds=0.2), [])
check("a saccade: none", run(lambda i: ((3.0 + (i % 2) * 4, 1.0),) * 2), [])
check("a blink starts it over", run(lambda i: (None, None) if i == 20 else ((3.0, 1.0), (3.0, 1.0)), seconds=0.4), [])
check("both eyes: their mean", run(lambda i: ((2.0, 0.0), (4.0, 2.0)))[:1], [(3.0, 1.0)])
check("one eye lost (both tracked): none", run(lambda i: (None, (3.0, 1.0))), [])
check("one eye tracked: its look alone", run(lambda i: ((0.0, 0.0), (6.0, 2.0)), one_eye=1)[:1], [(6.0, 2.0)])
check("raw: only near straight ahead", run(held(10.0, 0.0)), [])
check("the same calibration's affine: corrected", run(held(20.0, 0.0), fusion=Fusion())[:1], [(12.0, 0.0)])
check("  out to CENTRE_DEG", run(held(30.0, 0.0), fusion=Fusion()), [])
check("an older calibration's affine: raw", run(held(5.0, 0.0), fusion=Fusion("2026-10-01 10:00:00"))[:1], [(5.0, 0.0)])
check("a clicks-only affine: raw", run(held(5.0, 0.0), fusion=Fusion(how="clicks"))[:1], [(5.0, 0.0)])
check("one eye tracked: never the affine (fitted on the mean)",
      run(held(5.0, 0.0), fusion=Fusion(), one_eye=0)[:1], [(5.0, 0.0)])
check("no calibration made: raw", run(held(5.0, 0.0), fusion=Fusion(), made=None)[:1], [(5.0, 0.0)])
sl = steamlook.SteamLooks()
first = run(held(3.0, 1.0), sl=sl)
again = run(held(3.0, 1.0), t0=0.6, seconds=0.6, sl=sl)
check("still held: again after EVERY", (len(first), len(again)), (1, 1))

print("all passed" if not failures else f"{len(failures)} failed: {failures}", flush=True)
sys.exit(1 if failures else 0)
