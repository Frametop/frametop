#!/usr/bin/env python3
"""Offline test of the fit pill's words and timing (gaze/fitpill.py) and the fit coach's parts
for it (gaze/fitcoach.py): a calibration's seating from its dots, the reference read with it,
and readings moved to where the eyes look.

  gaze/test/fit-pill-test.py
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import fitcoach  # noqa: E402
import fitpill  # noqa: E402
from fitpill import Pill  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


# --- The words ---
check("within GOOD_DEG: looks good", fitpill.words({"up": 2.0, "left": -4.0}), fitpill.GOOD)
check("eyes high: nudge it up, slightly", fitpill.words({"up": 12.0, "left": 2.0}), ("Nudge slightly up", "up"))
check("eyes low, far: nudge it down", fitpill.words({"up": -28.0, "left": 6.0}), ("Nudge down", "down"))
check("eyes left: nudge it left", fitpill.words({"up": 4.0, "left": 14.0}), ("Nudge slightly left", "left"))
check("eyes right, the bigger term: right", fitpill.words({"up": 8.0, "left": -18.0}), ("Nudge slightly right", "right"))
check("no terms: looks good", fitpill.words({}), fitpill.GOOD)
check("two terms nearly the same: the one said stays",
      fitpill.words({"up": 12.0, "left": -13.0}, ("Nudge slightly up", "up")), ("Nudge slightly up", "up"))
check("  until the other is SWITCH_DEG bigger",
      fitpill.words({"up": 12.0, "left": -17.0}, ("Nudge slightly up", "up")), ("Nudge slightly right", "right"))
check("  not if the one said turned round",
      fitpill.words({"up": -12.0, "left": -13.0}, ("Nudge slightly up", "up")), ("Nudge slightly right", "right"))
check("just under BIG_DEG after past it: still big", fitpill.words({"left": -18.0}, ("Nudge right", "right")),
      ("Nudge right", "right"))
check("  under BIG_DEG - SWITCH_DEG: slightly", fitpill.words({"left": -15.0}, ("Nudge right", "right")),
      ("Nudge slightly right", "right"))


def run(pill, steps):
    """steps: (seconds, terms) held, read every 0.25 s; the changes as (time, change)."""
    out, t = [], run.t
    for seconds, terms in steps:
        end = t + seconds
        while t < end - 1e-9:
            ch = pill.step(t, terms)
            if ch:
                out.append((round(t - run.t, 2), ch))
            t += 0.25
    run.t = t
    return out


run.t = 1000.0
OFF = {"up": 0.0, "left": -14.0}     # sits 14 degrees off: nudge slightly right
NEAR = {"up": 0.0, "left": -7.0}     # under SHOW_DEG, over GOOD_DEG
GOOD = {"up": 1.0, "left": -2.0}

# --- Opening ---
p = Pill()
check("a brief slip: nothing", run(p, [(fitpill.SHOW_HOLD - 0.5, OFF), (1.0, GOOD)]), [])
check("under SHOW_DEG: nothing", run(p, [(5.0, NEAR)]), [])
ch = run(p, [(fitpill.SHOW_HOLD + fitpill.SMOOTH, OFF)])
check("off for SHOW_HOLD: it opens once", [c for _, c in ch], [("show", "Nudge slightly right", "right")])
check("  once the median has been past for SHOW_HOLD", fitpill.SHOW_HOLD <= ch[0][0] < fitpill.SHOW_HOLD + fitpill.SMOOTH / 2 + 0.25,
      True)

big = Pill()
ch = run(big, [(fitpill.BIG_HOLD + fitpill.SMOOTH + 0.5, {"up": 0.0, "left": -28.0})])
check("a big push: open within BIG_HOLD plus the median's lag", (len(ch), ch[0][1] if ch else None,
      ch[0][0] <= fitpill.BIG_HOLD + fitpill.SMOOTH / 2 + 0.25 if ch else None),
      (1, ("show", "Nudge right", "right"), True))

# --- A gap in the readings starts SHOW_HOLD over ---
q = Pill()
check("off, then no readings past STALE, then off again: not yet",
      run(q, [(fitpill.SHOW_HOLD - 0.5, OFF), (fitpill.STALE + 1.0, None), (fitpill.SHOW_HOLD - 0.5, OFF)]), [])

# --- Following, then good ---
ch = run(p, [(1.0, {"up": 0.0, "left": -24.0})])
check("further off: says nudge right", [c for _, c in ch], [("say", "Nudge right", "right")])
ch = run(p, [(1.0, NEAR)])
check("under SHOW_DEG but past GOOD_DEG: still guides", [c for _, c in ch], [("say", "Nudge slightly right", "right")])
ch = run(p, [(fitpill.GOOD_HOLD + fitpill.SMOOTH, GOOD)])
check("within GOOD_DEG for GOOD_HOLD: looks good", [c for _, c in ch], [("say", "Looks good", "good")])
start = run.t
ch = run(p, [(fitpill.GOOD_SECONDS + 0.5, OFF)])
check("  then it closes, whatever comes", [c for _, c in ch], [("hide",)])
left = start + ch[0][0] + fitpill.AGAIN - run.t   # seconds of AGAIN still to go
check("not again within AGAIN", run(p, [(left - 0.25, OFF)]), [])
check("  but after it", [c for _, c in run(p, [(1.0, OFF)])][:1], [("show", "Nudge slightly right", "right")])

# --- No readings while open ---
ch = run(p, [(fitpill.STALE + 0.5, None)])
check("no readings for STALE: look straight ahead", [c for _, c in ch], [("say", "Look straight ahead", "")])
ch = run(p, [(0.5, OFF)])
check("  a reading: guides again", [c for _, c in ch], [("say", "Nudge slightly right", "right")])

# --- Readings that swing both ways while the headset moves (2026-11-22) ---
p = Pill()
run(p, [(fitpill.SHOW_HOLD + fitpill.SMOOTH, {"up": 14.0, "left": 0.0})])
swing = [(0.5, {"up": 18.0, "left": 0.0}), (0.5, {"up": 12.0, "left": 0.0}), (0.25, {"up": -8.0, "left": 0.0})] * 4
check("single readings the other way: it keeps saying up", [c for _, c in run(p, swing)], [])
check("a raw small slip doesn't open it", [c for _, c in run(Pill(), [(10.0, {"up": 7.0, "left": -6.0})])], [])

# --- Giving up ---
p = Pill()
ch = run(p, [(fitpill.SHOW_HOLD + fitpill.PILL_MAX + 0.5, OFF)])
check("never good: closes after PILL_MAX", [c for _, c in ch], [("show", "Nudge slightly right", "right"), ("hide",)])
check("  at the last reading", ch[-1][0], fitpill.SHOW_HOLD + fitpill.PILL_MAX + 0.25)
check("  and waits COOLDOWN", run(p, [(fitpill.COOLDOWN - 1.0, OFF)]), [])
check("  then opens again", [c for _, c in run(p, [(fitpill.SHOW_HOLD + 1.0, OFF)])][:1],
      [("show", "Nudge slightly right", "right")])

# --- Closed from outside ---
p = Pill()
run(p, [(fitpill.SHOW_HOLD + 0.5, OFF)])
check("open", p.shown, True)
check("close() hides it", p.close(run.t), ("hide",))
check("close() when hidden: nothing", p.close(run.t), None)
check("not again within AGAIN", run(p, [(fitpill.AGAIN - 1.0, OFF)]), [])

# --- A calibration's seating ---
REF = fitcoach.Reference.load()
check("the shipped reference loads", REF is not None and sorted(REF.eyes), [0, 1])
JAC = {0: [[1.2, 0.1], [-0.05, -1.3]], 1: [[-1.25, 0.0], [0.1, 1.35]]}
CENTRE = {0: [330.0, 215.0], 1: [320.0, 180.0]}
DOTS = [(0.0, 0.0), (15.0, 0.0), (-15.0, 0.0), (0.0, 9.0), (0.0, -9.0), (10.6, 6.4), (-10.6, -6.4), (10.6, -6.4)]


def pupil(cam, yaw, pitch):
    (a, b), (c, d) = JAC[cam]
    return [CENTRE[cam][0] + a * yaw + b * pitch, CENTRE[cam][1] + c * yaw + d * pitch]


old = [{"made": "2026-10-01 10:00:00", "truth": [y, p], "eye": {"0": {"pupil": [p_ + 30 for p_ in pupil(0, y, p)]}}}
       for y, p in DOTS]
new = [{"made": "2026-11-22 09:05:53", "truth": [y, p], "eye": {str(c): {"pupil": pupil(c, y, p), "mid": None} for c in (0, 1)}}
       for y, p in DOTS]
with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "calibration-dots.jsonl")
    with open(path, "w") as f:
        for r in old + new:
            f.write(json.dumps(r) + "\n")
        f.write('{"made": "2026-11-22 09:05:53", "tru')   # cut short
    rows = fitcoach.read_dots(path)
check("read_dots skips a line cut short", len(rows), len(old) + len(new))
seat = fitcoach.seating_from_dots(rows, ipd=0.064)
check("the seating is the newest calibration's", seat["made"], "2026-11-22 09:05:53")
check("  from a calibration", fitcoach.from_calibration(seat), True)
check("  both eyes", sorted(seat["eyes"]), ["0", "1"])
check("  each eye's straight-ahead pupil",
      all(near(seat["eyes"][str(c)]["centre"][i], CENTRE[c][i], 0.01) for c in (0, 1) for i in (0, 1)), True)
check("  and its own Jacobian",
      all(near(seat["eyes"][str(c)]["jacobian"][i][j], JAC[c][i][j], 1e-3) for c in (0, 1) for i in (0, 1) for j in (0, 1)), True)
check("  the IPD", seat["ipd"], 0.064)
check("no dots: no seating", fitcoach.seating_from_dots([]), None)
check("the last written is the newest, whatever its made says",
      fitcoach.seating_from_dots(new + [dict(r, made="2026-11-22 01:30:00") for r in new])["made"], "2026-11-22 01:30:00")
check("a fit check's baseline isn't a calibration's", fitcoach.from_calibration({"eyes": {}}), False)

own = fitcoach.own_reference(seat, REF)
check("own_reference uses the seating's Jacobians", own.eyes[1]["jacobian"], seat["eyes"]["1"]["jacobian"])
check("a baseline without Jacobians reads with the shipped reference", fitcoach.own_reference({"eyes": {"0": {"centre": [1, 2]}}}, REF), REF)


def summary(yaw, pitch, slip=(0.0, 0.0)):
    """Both eyes' finds looking at (yaw, pitch), the headset slipped as if the eyes turned by slip."""
    out = {}
    for c in (0, 1):
        x, y = pupil(c, yaw + slip[0], pitch + slip[1])
        out[c] = {"n": 90, "finds": 90, "found": 1.0, "x": x, "y": y, "a": 28.0, "spacing": None, "extra": 0.0}
    return out


r = fitcoach.assess(summary(0, 0), own, baseline=seat)
check("straight ahead at the seating: ok", r["state"], "ok")
one = {**seat, "eyes": {"0": seat["eyes"]["0"]}}
r = fitcoach.assess(summary(0, 0), fitcoach.own_reference(one, REF), baseline=one)
check("a one-eye seating reads only its eye", (r["state"], sorted(e for e in r["eyes"] if "offset_deg" in r["eyes"][e])),
      ("ok", [0]))
r = fitcoach.assess(summary(12, -6), own, baseline=seat)
check("looking aside: the coach reads it as off (the pill reads the slip filter instead)", r["state"], "adjust")
check("the pill's words for a slip up", fitpill.words({"up": 14.0, "left": 0.0}), ("Nudge slightly up", "up"))

print("all passed" if not failures else f"{len(failures)} failed: {failures}", flush=True)
sys.exit(1 if failures else 0)
