#!/usr/bin/env python3
"""Offline test of pausing for VR games (input/game_pause.py): the controller gesture, and when
Frametop pauses and resumes by itself. The worker (services, remote desktop, the desktop) and
the controller reader are stubs, the state file is a temporary one, and ft-screens' socket is
renamed, so nothing reaches the live Frametop, SteamVR, or the running relay.

  input/test/pause-test.py
"""
import os
import sys
import shutil
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_pause as gp  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


# ---------------------------------------------------------------- the gesture
L, R = "left/thumbstick", "right/thumbstick"


def presses(g, events):
    """events: (time, button, down); how many times the gesture went off."""
    return sum(g.feed(b, d, t) for t, b, d in events)


def both(t, hold=0.1, gap=0.02):
    """Both sticks clicked at t (the right one gap later), held for hold."""
    return [(t, L, True), (t + gap, R, True), (t + hold, L, False), (t + hold + gap, R, False)]


g = gp.Gesture((L, R), 2)
check("both sticks twice: goes off", presses(g, both(0) + both(0.3)), 1)
g = gp.Gesture((L, R), 2)
check("both sticks once: nothing", presses(g, both(0)), 0)
g = gp.Gesture((L, R), 2)
check("both sticks twice, 0.9 s apart: nothing", presses(g, both(0) + both(0.9)), 0)
g = gp.Gesture((L, R), 2)
check("left held (sprint), right clicked twice: nothing",
      presses(g, [(0, L, True), (1.0, R, True), (1.1, R, False), (1.3, R, True), (1.4, R, False)]), 0)
g = gp.Gesture((L, R), 2)
check("sticks 0.5 s apart, twice: nothing", presses(g, both(0, gap=0.5, hold=0.6) + both(1.2, gap=0.5, hold=0.6)), 0)
g = gp.Gesture((L, R), 2)
check("three double presses in a row: once", presses(g, both(0) + both(0.3) + both(0.6)), 1)
g = gp.Gesture((L, R), 2)
check("again after a second: twice in all", presses(g, both(0) + both(0.3) + both(2.0) + both(2.3)), 2)
g = gp.Gesture((L, R), 2)
check("repeated down states count once",
      presses(g, [(0, L, True), (0.01, L, True), (0.02, R, True), (0.03, R, True), (0.1, L, False), (0.1, R, False)]
              + both(0.3)), 1)
g = gp.Gesture((L, R), 1)
check("both sticks once (presses 1): goes off", presses(g, both(0)), 1)
g = gp.Gesture(("right/b",), 2)
check("one button double-clicked: goes off",
      presses(g, [(0, "right/b", True), (0.1, "right/b", False), (0.3, "right/b", True)]), 1)
g = gp.Gesture(("right/b",), 2)
check("other buttons don't count", presses(g, [(0, "right/a", True), (0.2, "right/a", True)]), 0)

VR = ("left/thumbstick", "right/thumbstick", "right/b", "right/a")
check("no pause_gesture: both sticks twice", gp.gesture_of({}, VR), ((L, R), 2))
check("pause_gesture null: none", gp.gesture_of({"pause_gesture": None}, VR), None)
check("one button, presses 1: still twice", gp.gesture_of({"pause_gesture": {"buttons": ["right/b"], "presses": 1}}, VR),
      (("right/b",), 2))
check("unknown buttons dropped", gp.gesture_of({"pause_gesture": {"buttons": ["right/zz", "right/a"]}}, VR),
      (("right/a",), 2))
check("settings: defaults", gp.settings_of({}, VR)["auto"], True)
check("settings: unknown desktop mode is hide", gp.settings_of({"pause_desktop": "x"}, VR)["desktop"], "hide")


# ---------------------------------------------------------------- pausing by itself
class StubWorker:
    def __init__(self, log, save):
        self.jobs = []
        self.busy = False

    def start(self):
        pass

    def submit(self, job, *args):
        self.jobs.append(job.__name__)

    def snapshot(self):
        return {"units": [], "remote": False, "desktop": None}

    def restore(self, stopped):
        self.restored = stopped

    def pause(self, desktop):
        pass

    def resume(self):
        pass

    def enforce(self):
        pass


class StubWatch:
    connected = False

    def __init__(self, log):
        pass

    def start(self):
        pass

    def set_gesture(self, spec):
        self.spec = spec

    def set_touch_feed(self, on):
        self.touch = on

    def helper_started(self):
        pass


tmp = tempfile.mkdtemp(prefix="ft-pause-test-")
gp.STATE_PATH = os.path.join(tmp, "pause.json")
gp.SCREENS = f"\0ft_pause_test_{os.getpid()}_screens"
gp.HELPER = f"\0ft_pause_test_{os.getpid()}_helper"  # "paused 1|0" mustn't reach the live pointer helper
os.makedirs(os.path.join(tmp, "frametop-hands"))
gp.PARK_PATH = os.path.join(tmp, "frametop-hands", "park")
gp.Worker, gp.ControllerWatch = StubWorker, StubWatch
changes = []


def new(rules=None):
    """A relay starting afresh: not paused before."""
    if os.path.exists(gp.STATE_PATH):
        os.remove(gp.STATE_PATH)
    changes.clear()
    p = gp.GamePause(lambda *a: None, changes.append, VR)
    p.configure(dict({"pause_sound": False}, **(rules or {})))
    return p


p = new()
p.game_state(True, 0)
check("a game starts: paused", (p.paused, p.reason, p.ends_with_game), (True, "game", True))
check("the relay heard", changes, [True])
p.game_state(True, 5)
p.tick(5)
check("the helper repeats it: still paused, nothing new", (p.paused, changes), (True, [True]))
p.game_state(False, 10)
p.tick(12)
check("the game ends: not yet resumed", p.paused, True)
p.tick(15.1)
check("5 s later: resumed", (p.paused, changes), (False, [True, False]))
check("the worker paused and resumed", p.worker.jobs, ["pause", "resume"])

p = new()
p.game_state(True, 0)
p.game_state(False, 10)
p.game_state(True, 12)
p.tick(16)
check("the next game starts within 5 s: stays paused", p.paused, True)
p.game_state(False, 20)
p.tick(25.1)
check("and resumes 5 s after that one ends", p.paused, False)

p = new()
p.game_state(True, 0)
p.toggle("gesture", 1)
check("resumed during a game: running", p.paused, False)
p.game_state(True, 6)
p.tick(30)
check("and stays running while that game runs", p.paused, False)
p.game_state(False, 40)
p.game_state(True, 50)
check("the next game pauses again", p.paused, True)

p = new()
p.toggle("command", 0)
check("paused outside a game: not tied to one", (p.paused, p.ends_with_game), (True, False))
p.game_state(True, 1)
p.game_state(False, 10)
p.tick(20)
check("a game comes and goes: still paused", p.paused, True)
p.toggle("command", 21)
check("toggled: running", p.paused, False)

p = new({"pause_auto": False})
p.game_state(True, 0)
check("auto off: a game doesn't pause", p.paused, False)
p.toggle("gesture", 1)
p.game_state(False, 10)
p.tick(20)
check("auto off: a pause during a game outlasts it", p.paused, True)

p = new()
p.game_state(True, 0)
p.tick(0 + gp.GAME_STALE + 0.1)
check("the helper goes quiet: the game counts as ended", p.game, False)
p.tick(0 + gp.GAME_STALE + gp.RESUME_DELAY + 0.2)
check("and the pause ends", p.paused, False)

p = new()
p.toggle("gesture", 0)
q = gp.GamePause(lambda *a: None, changes.append, VR)
check("a relay restarted while paused stays paused", (q.paused, q.reason), (True, "gesture"))
check("and stops the services again", q.worker.jobs, ["enforce"])

# ---------------------------------------------------------------- hand tracking parked for games
parked = lambda: os.path.exists(gp.PARK_PATH)  # noqa: E731
p = new()
p.set_hands(True)
check("hand interaction on: the worker leaves hand tracking running", p.worker.keep, gp.HAND_UNITS)
check("not paused: not parked", parked(), False)
p.game_state(True, 0)
check("a game pauses Frametop: hand tracking parked", parked(), True)
p.dashboard_state(True)
check("the dashboard opens: unparked", parked(), False)
p.dashboard_state(False)
check("it closes: parked again", parked(), True)
p.toggle("gesture", 1)
check("Frametop toggled back on during the game: unparked", parked(), False)
p.game_state(False, 10)
p.game_state(True, 20)
check("the next game: parked", parked(), True)
p.set_hands(False)
check("hand interaction off: unparked (the worker stops it as before)", (parked(), p.worker.keep), (False, ()))
p.set_hands(True, always=True)
check("HANDS_MODE=always: the worker leaves hand tracking running", p.worker.keep, gp.HAND_UNITS)
check("and it isn't parked in the game", parked(), False)
p.set_hands(False, always=True)
check("always without hand interaction: still left running", (parked(), p.worker.keep), (False, gp.HAND_UNITS))
p.set_hands(True)
check("back to the other modes: parked again", parked(), True)
p.set_hands(False)
q.toggle("gesture", 1)
r = gp.GamePause(lambda *a: None, changes.append, VR)
check("resumed: the next relay starts running", r.paused, False)

p = new({"pause_gesture": None})
check("pause_gesture null: the reader gets none", p.watch.spec, None)
check("status", sorted(p.status()), sorted(["t", "paused", "reason", "since", "ends_with_game", "game", "busy",
                                            "stopped", "gesture_reader", "auto", "desktop"]))

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
shutil.rmtree(tmp)
sys.exit(1 if failures else 0)
