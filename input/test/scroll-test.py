#!/usr/bin/env python3
"""Offline test of the relay's scrolling through the virtual joystick: each wheel notch (or
scroll_up / scroll_down press) deflects it for SCROLL_STEP, notches that come faster add their
time to the push under way up to SCROLL_BACKLOG, and the other direction starts again. The
pointer's socket is a recorder, so nothing reaches the helper, SteamVR, or the running relay.

  input/test/scroll-test.py [RELAY]
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
tag = f"ft_scroll_test_{os.getpid()}"
src = open(relay_path).read().replace('"\\0frametop_relay"', f'"\\0{tag}_relay"')
# macOS has no SOCK_NONBLOCK; the pointer's socket is replaced below anyway.
src = src.replace("socket.SOCK_DGRAM | socket.SOCK_NONBLOCK", "socket.SOCK_DGRAM")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))
exec(compile(src, relay_path, "exec"), relay.__dict__)

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


class Recorder:
    """Stands in for the helper's socket: remembers every command, in order."""

    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append(data.decode())


def pointer():
    p = relay.Pointer(0.02, 3600.0)
    p.sock = Recorder()
    p.action("pointer_toggle", 1, 10.0)  # awake, so the wheel isn't taken for waking it
    p.sock.sent.clear()
    return p


def scrolls(p):
    return [c for c in p.sock.sent if c.startswith("scroll")]


P = relay.Pointer
STEP, BACKLOG = P.SCROLL_STEP, P.SCROLL_BACKLOG

# ---------------------------------------------------------------- one notch, one step
p = pointer()
p.motion(relay.REL_WHEEL, 1, 10.0)
check("a notch up deflects the joystick up", scrolls(p), ["scroll 0 1"])
p.tick(10.0 + STEP - 0.001)
check("the push lasts SCROLL_STEP", scrolls(p), ["scroll 0 1"])
p.tick(10.0 + STEP)
check("then the joystick centres", scrolls(p), ["scroll 0 1", "scroll 0 0"])

# ---------------------------------------------------------------- fast notches add up
p = pointer()
for i in range(3):
    p.motion(relay.REL_WHEEL, -1, 10.0 + i * 0.01)
check("three fast notches down: one push", scrolls(p), ["scroll 0 -1"])
check("it lasts three steps", round(p.scroll_until - 10.0, 6), round(3 * STEP, 6))
p.tick(10.0 + 3 * STEP + 1e-9)
check("then the joystick centres", scrolls(p)[-1], "scroll 0 0")

# A wheel report of 2 counts as two notches.
p = pointer()
p.motion(relay.REL_WHEEL, 2, 10.0)
check("a report of 2 notches lasts two steps", round(p.scroll_until - 10.0, 6), round(2 * STEP, 6))

# ---------------------------------------------------------------- a fast spin is capped
p = pointer()
for i in range(40):
    p.motion(relay.REL_WHEEL, -1, 10.0 + i * 0.02)
check("40 notches in 0.8 s: still one push", scrolls(p), ["scroll 0 -1"])
check("it ends at most SCROLL_BACKLOG after the last notch", round(p.scroll_until - (10.0 + 39 * 0.02), 6),
      round(BACKLOG, 6))

# ---------------------------------------------------------------- the other direction
p = pointer()
p.motion(relay.REL_WHEEL, 1, 10.0)
p.motion(relay.REL_WHEEL, 1, 10.01)
p.motion(relay.REL_WHEEL, -1, 10.02)
check("a notch the other way turns the joystick round", scrolls(p), ["scroll 0 1", "scroll 0 -1"])
check("and buys only its own step", round(p.scroll_until - 10.02, 6), round(STEP, 6))

# ---------------------------------------------------------------- a notch after the push ended
p = pointer()
p.motion(relay.REL_WHEEL, 1, 10.0)
p.tick(10.0 + STEP)
p.motion(relay.REL_WHEEL, 1, 11.0)
check("a later notch starts a new push", scrolls(p), ["scroll 0 1", "scroll 0 0", "scroll 0 1"])
check("of one step", round(p.scroll_until - 11.0, 6), round(STEP, 6))

# ---------------------------------------------------------------- buttons mapped to scrolling
p = pointer()
p.action("scroll_down", 1, 10.0)
p.action("scroll_down", 0, 10.01)
p.action("scroll_down", 1, 10.02)
check("scroll_down presses: one push, releases ignored", scrolls(p), ["scroll 0 -1"])
check("two steps long", round(p.scroll_until - 10.0, 6), round(2 * STEP, 6))

# ---------------------------------------------------------------- the loop wakes on time
p = pointer()
p.motion(relay.REL_WHEEL, 1, 10.0)
check("the loop waits no longer than the push", p.timeout() <= 0.02, True)

# ---------------------------------------------------------------- pausing
p = pointer()
p.motion(relay.REL_WHEEL, 1, 10.0)
p.stand_down()
check("pausing centres the joystick", scrolls(p), ["scroll 0 1", "scroll 0 0"])
check("and forgets the push", p.scroll_until, None)

# ---------------------------------------------------------------- on a Frametop panel
p = pointer()
taken = []
p.wheel_to = lambda n: taken.append(n) or True  # ft-screens said the laser is on a panel
for i in range(40):
    p.motion(relay.REL_WHEEL, -1, 10.0 + i * 0.02)
check("on a panel, every notch goes to ft-screens", (len(taken), sum(taken)), (40, -40))
check("and the joystick stays still", scrolls(p), [])
p.wheel_to = lambda n: False  # off the panels again
p.motion(relay.REL_WHEEL, 1, 11.0)
check("off the panels, the joystick again", scrolls(p), ["scroll 0 1"])

print("FAILED: " + ", ".join(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
