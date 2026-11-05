#!/usr/bin/env python3
"""Offline test of the controller touch feed for the pointer helper's handoff arbiter
(input/game_pause.py TouchTracker): which "ctl ..." datagrams go out for made-up web socket
messages. Nothing connects to SteamVR or sends anything.

  input/test/touch-test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_pause as gp  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


LEFT, RIGHT, BENCH = "/user/hand/left", "/user/hand/right", "/devices/cv/frame_testbench_right"
FULL = {"/input/trigger/click": False, "/input/trigger/touch": False, "/input/thumbrest/touch": False,
        "/input/handgrip1/touch": False, "/input/grip/value": 0}

t = gp.TouchTracker()
check("first lookup: nothing yet", t.set_sides({LEFT: "left", RIGHT: "right"}), [])
check("the first (full) message: the touch state", t.feed(LEFT, FULL), ["ctl left touch 0"])
check("a held click in the first message isn't a press",
      t.feed(RIGHT, dict(FULL, **{"/input/trigger/click": True})), ["ctl right touch 0"])
check("thumbrest touched", t.feed(LEFT, {"/input/thumbrest/touch": True}), ["ctl left touch 1"])
check("a second sensor: no change to send", t.feed(LEFT, {"/input/handgrip1/touch": True}), [])
check("one let go, one still touched: nothing", t.feed(LEFT, {"/input/thumbrest/touch": False}), [])
check("both let go", t.feed(LEFT, {"/input/handgrip1/touch": False}), ["ctl left touch 0"])
check("a value change alone: nothing", t.feed(LEFT, {"/input/grip/value": 0.3}), [])
check("a click going down: press (and its touch)",
      t.feed(LEFT, {"/input/trigger/click": True, "/input/trigger/touch": True}), ["ctl left press", "ctl left touch 1"])
check("still down: no second press", t.feed(LEFT, {"/input/trigger/click": True}), [])
check("up, then down again: press", [t.feed(LEFT, {"/input/trigger/click": False}),
                                     t.feed(LEFT, {"/input/trigger/click": True})], [[], ["ctl left press"]])
check("an unknown device: nothing", t.feed("/devices/cv/other", {"/input/a/touch": True}), [])
# a second controller on the right (the testbench's), touched while the real one isn't
check("another right controller appears", t.set_sides({LEFT: "left", RIGHT: "right", BENCH: "right"}), [])
check("its first message", t.feed(BENCH, FULL), [])
check("it's touched: right touch 1", t.feed(BENCH, {"/input/thumbrest/touch": True}), ["ctl right touch 1"])
check("snapshot", t.snapshot(), ["ctl left touch 1", "ctl right touch 1"])
check("it goes: right still has one, nothing gone; its touch went with it",
      t.set_sides({LEFT: "left", RIGHT: "right"}), ["ctl right touch 0"])
check("the next right message: nothing new", t.feed(RIGHT, {"/input/grip/value": 0.1}), [])
check("the left controller goes: gone", t.set_sides({RIGHT: "right"}), ["ctl left gone"])
check("it comes back: its first message is a baseline",
      [t.set_sides({LEFT: "left", RIGHT: "right"}), t.feed(LEFT, dict(FULL, **{"/input/trigger/click": True}))],
      [[], ["ctl left touch 0"]])

if failures:
    print(f"{len(failures)} failed")
    sys.exit(1)
print("all passed")
