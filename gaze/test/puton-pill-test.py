#!/usr/bin/env python3
"""Offline test of the put-on pill (gaze/putonpill.py): what the fit pill says just after the
headset goes on, from the fit coach's readings against your saved fit.

  python3 gaze/test/puton-pill-test.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fitpill  # noqa: E402
import putonpill  # noqa: E402

failures = []


def check(name, got, want):
    ok = got == want
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": got {got!r}, want {want!r}"))
    if not ok:
        failures.append(name)


def reading(up=0.0, left=0.0, hints=()):
    return {"state": "ok", "against": "baseline", "terms": {"up": up, "left": left}, "hints": list(hints)}


p = putonpill.PutOnPill(0.0)
check("no reading yet: nothing at first", p.step(0.5, None), [])
check("  then: look straight ahead", p.step(putonpill.LOOK_AFTER, None), [("show", "Look straight ahead", "")])
check("sits as saved: looks good", p.step(2.0, reading(1.0, -2.0)), [("say", "Looks good", "good")])
check("  then it goes", (p.step(2.0 + fitpill.GOOD_SECONDS, None), p.done), ([("hide",)], True))

p = putonpill.PutOnPill(0.0)
check("put on low: nudge up", p.step(0.5, reading(12.0)), [("show", "Nudge slightly up", "up")])
check("  the same: nothing new", p.step(1.0, reading(11.0)), [])
check("  not looking ahead meanwhile: keeps saying it", p.step(3.0, None), [])
check("  way low: nudge up", p.step(3.5, reading(25.0)), [("say", "Nudge up", "up")])
check("  back: looks good", p.step(4.0, reading(2.0)), [("say", "Looks good", "good")])
check("  then it goes", p.step(4.0 + fitpill.GOOD_SECONDS, None), [("hide",)])

p = putonpill.PutOnPill(0.0)
ipd = {"key": "ipd-changed", "text": "The IPD setting changed", "amount": 2.0}
check("only the IPD changed: says so", p.step(0.5, reading(1.0, hints=[ipd])), [("show", "The IPD setting changed", "")])

p = putonpill.PutOnPill(0.0)
p.step(0.5, reading(-14.0))
check("never back: it gives up", (p.step(putonpill.TIMEOUT + 1, reading(-14.0)), p.done), ([("hide",)], True))
p = putonpill.PutOnPill(0.0)
check("closed before it said anything: nothing to hide", p.close(), [])

if failures:
    print("FAILED: " + ", ".join(failures))
    sys.exit(1)
print("all ok")
