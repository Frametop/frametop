#!/usr/bin/env python3
"""Offline test of the relay's pointer standing down when Frametop pauses: a click still held
when the pause starts (a mouse button, a mapped controller button, or a key combination mapped
to left, right, middle or back) is released by stand_down, and "hide" follows the release, even
when the pointer was off already. Gaze holds (gazekey, gazedrag, precision) are the helper's
to end, so stand_down sends nothing for them. The pointer's socket is a recorder, so nothing
reaches the helper, SteamVR, or the running relay.

  input/test/pause-buttons-test.py [RELAY]
"""
import importlib.util
import os
import sys
os.environ["FT_KWIN_EI"] = "0"  # the relay's wheel sender stays off the live desktop's KWin

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
tag = f"ft_pause_buttons_test_{os.getpid()}"
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
    return p


def since(p, mark):
    """What the pointer sent the helper after mark (a length of p.sock.sent)."""
    return p.sock.sent[mark:]


# ---------------------------------------------------------------- the fix
p = pointer()
p.action("left", 1, 10.0)  # a mouse click (or drag) held: btn trigger 1
check("press reaches the helper", p.sock.sent, ["show", "recenter", "btn trigger 1"])
mark = len(p.sock.sent)
p.stand_down()  # Frametop pauses while it's held
check("stand_down releases the held button, then hides", since(p, mark), ["btn trigger 0", "hide pause"])

# The release that pausing drops later is already covered: the button is no longer down,
# so a stray second stand_down sends only "hide" again (it always goes).
mark = len(p.sock.sent)
p.stand_down()
check("a second stand_down sends no release, only hide", since(p, mark), ["hide pause"])

# Two buttons down at once: a left drag tilted with the right button
p = pointer()
p.action("left", 1, 10.0)
p.action("right", 1, 10.5)
mark = len(p.sock.sent)
p.stand_down()
check("both held buttons are released, then it hides", since(p, mark), ["btn b 0", "btn trigger 0", "hide pause"])

# A mapped controller button and a key combination take the same path as the mouse.
p = pointer()
p.action("middle", 1, 10.0, "right")  # a controller button mapped to middle
p.action("back", 1, 10.1, "keyboard")  # a key combination mapped to back
mark = len(p.sock.sent)
p.stand_down()
check("controller and key combination clicks are released too", since(p, mark),
      ["btn joystick 0", "btn x 0", "hide pause"])

# ---------------------------------------------------------------- the pointer already off
# A release with no "hide" after it would wake a helper that wakes on any btn, connecting the
# virtual controller during the game.
p = pointer()
p.idle = 30.0
p.action("left", 1, 10.0)
for t in (10.3, 10.4, 41.0):  # the claim pulse, then 30 s with no mouse input
    p.tick(t)
check("held 30 s with no mouse input: the pointer goes off", (p.active, p.sock.sent[-1]), (False, "hide idle"))
mark = len(p.sock.sent)
p.stand_down()
check("idle with a button held: the release, then hide", since(p, mark), ["btn trigger 0", "hide pause"])

p = pointer()
p.action("left", 1, 10.0)
p.action("pointer_toggle", 1, 10.5)
check("pointer_toggle says why it hides (gaze mode leaves it off)", p.sock.sent[-1], "hide toggle")
mark = len(p.sock.sent)
p.stand_down()
check("pointer toggled off with a button held: the release, then hide", since(p, mark),
      ["btn trigger 0", "hide pause"])

# ---------------------------------------------------------------- the ordinary path
p = pointer()
p.action("left", 1, 10.0)
p.action("left", 0, 11.0)  # released before the pause
mark = len(p.sock.sent)
p.stand_down()
check("a button released before the pause: stand_down only hides", since(p, mark), ["hide pause"])

p = pointer()
p.idle = 30.0
p.action("left", 1, 10.0)
p.action("left", 0, 11.0)
for t in (10.3, 10.4, 42.0):  # off by itself
    p.tick(t)
mark = len(p.sock.sent)
p.stand_down()
# A relay that restarted can't tell whether the helper still holds the pointer.
check("nothing held and the pointer off: stand_down still hides", since(p, mark), ["hide pause"])

# Gaze holds go to the helper as their own commands, and it ends them when the pointer hides.
p = pointer()
p.action("gaze_left", 1, 10.0, "keyboard")  # Meta+J held
p.action("gaze_drag", 1, 10.1, "mouse")
mark = len(p.sock.sent)
p.stand_down()
check("gaze holds: stand_down sends only hide (the helper ends them)", since(p, mark), ["hide pause"])

# A tiling key steps the pointer aside (ft-screens keeps the moved tile focused) until the mouse
# moves on purpose again; not with a button held.
p = pointer()
p.motion(relay.REL_X, 50, 10.0)
mark = len(p.sock.sent)
check("tiling key: the pointer steps aside", (p.step_aside("tilekey"), since(p, mark), p.active), (True, ["hide tilekey"], False))
check("...already off: nothing", (p.step_aside("tilekey"), since(p, mark)), (False, ["hide tilekey"]))
p.motion(relay.REL_X, 5, 10.5)
check("...a twitch doesn't bring it back", p.active, False)
p.motion(relay.REL_X, 60, 10.6)
check("...a nudge does, recentred", (p.active, "show" in since(p, mark), "recenter" in since(p, mark)), (True, True, True))
p = pointer()
p.action("left", 1, 10.0)
check("...a drag held: it stays", (p.step_aside("tilekey"), p.active), (False, True))
p = pointer()
p.motion(relay.REL_X, 50, 10.0)
p.wheel_to = lambda notches, hwheel=0: False  # (off a Frametop panel: the joystick push)
p.scroll(3, 0, 10.1)
mark = len(p.sock.sent)
p.step_aside("tilekey")
for t in (10.2, 10.5, 12.0):
    p.tick(t)
check("...a scroll under way ends, and nothing more goes to the hidden pointer (council, 2026-11-20)",
      since(p, mark), ["scroll 0 0", "hide tilekey"])
p = pointer()
p.motion(relay.REL_X, 50, 10.0)
p.step_aside("tilekey")
p.dashboard(20.0)  # (woken: its press 0.4 s later)
p.motion(relay.REL_X, 60, 20.1)
mark = len(p.sock.sent)
aside = p.step_aside("tilekey")  # a tiling key right after the dashboard key
for t in (20.5, 20.6, 21.0):
    p.tick(t)
sent = since(p, mark)
check("...a dashboard key just before it: it stays on, and the dashboard still toggles (council, 2026-11-20)",
      (aside, p.active, "btn system 1" in sent, any(c.startswith("hide") for c in sent)), (False, True, True, False))
check("...and once the press is let go, the next tiling key steps aside", (p.step_aside("tilekey"), p.active), (True, False))

print()
if failures:
    print(f"{len(failures)} failed: {', '.join(failures)}")
    sys.exit(1)
print("all ok")
