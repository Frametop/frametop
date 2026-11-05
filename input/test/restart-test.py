#!/usr/bin/env python3
"""Offline test of what the input relay does when it starts again: Relay.recover_from_restart
and the pause's state file, one test each for the restart fixes L1, L5 and L6 (G1)
and K3, K6 and K7 (G3) in docs/charts/README.md. Then the relay's state objects,
and its reload, which puts the whole config in place or none of it.

It makes Relay objects with fake virtual devices, a renamed helper and ft-screens, a temporary pause
state file and the pause's worker and controller reader stubbed, and never runs the loop: no
devices, grabs, services, or the live relay's sockets.

  input/test/restart-test.py [RELAY]   (default: input/input-relay.py next to this folder)
"""
import importlib.util
import json
import os
import shutil
import socket
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))  # its game_pause and vrws
with open(relay_path) as f:
    exec(compile(f.read(), relay_path, "exec"), relay.__dict__)
gp = relay.game_pause

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


class StubWorker:
    """The pause's worker without its thread or services: the jobs it was given, by name."""

    def __init__(self, log, save):
        self.jobs = []
        self.busy = False
        self.keep = ()
        self.hands_with_desktop = False
        self.stopped = {"units": [], "remote": False, "desktop": None}

    def start(self):
        pass

    def submit(self, job, *args):
        self.jobs.append(job.__name__)

    def snapshot(self):
        return dict(self.stopped)

    def restore(self, stopped):
        self.stopped = stopped

    def pause(self, desktop):
        pass

    def resume(self):
        pass

    def enforce(self):
        pass

    def defer_remote(self):
        pass


class StubWatch:
    connected = False

    def __init__(self, log):
        pass

    def start(self):
        pass

    def set_gesture(self, spec):
        pass

    def set_touch_feed(self, on):
        pass

    def set_pad(self, on):
        pass

    def helper_started(self):
        pass

    def recheck(self, delay=1.5):
        pass


class FakeVirtual:
    """The virtual mouse or keyboard: made new, or reused from the fd store."""

    def __init__(self, reused):
        self.reused = reused
        self.released = 0

    def release_all(self):
        self.released += 1

    def emit(self, etype, code, value):
        pass

    def sync(self):
        pass


tag = f"ft_restart_test_{os.getpid()}"
for name in ("SCREENS", "HELPER", "FLOAT", "GAZED", "VOICE", "FRUNNER"):
    setattr(relay, name, f"\0{tag}_{name.lower()}")
OUT = tempfile.mkdtemp(prefix="ft-restart-test-")
gp.STATE_PATH = os.path.join(OUT, "pause.json")
gp.SCREENS, gp.HELPER = relay.SCREENS, relay.HELPER
gp.PARK_PATH = os.path.join(OUT, "frametop-hands", "park")
gp.SOUNDS = {True: os.path.join(OUT, "no-pause.oga"), False: os.path.join(OUT, "no-resume.oga")}  # no pw-play
gp.Worker, gp.ControllerWatch = StubWorker, StubWatch
LOG = []
relay.log = lambda *args: LOG.append(" ".join(map(str, args)))
CONF = {}  # frametop.conf, as the relay reads it
RULES = {"bad": False, "rules": {}}  # the rules file: bad, or what it says on top of the defaults
real_read_rules = relay.read_rules


def read_rules(path=None):
    if path == "":
        return real_read_rules(os.path.join(OUT, "no-rules.json"))  # the defaults
    if RULES["bad"]:
        raise ValueError("a rules file that doesn't read")
    return {**real_read_rules(os.path.join(OUT, "no-rules.json")), **RULES["rules"]}


relay.read_rules = read_rules
relay.read_config = lambda path=None: dict(CONF)


def receiver(addr):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(addr)
    s.settimeout(0.05)
    return s


helper, screens = receiver(relay.HELPER), receiver(relay.SCREENS)


def received(sock):
    """What sock was sent since the last call."""
    got = []
    while True:
        try:
            got.append(sock.recv(256).decode())
        except socket.timeout:
            return got


control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)  # unbound: the tests send nothing to it
screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)


def start(pause_state=None, conf=None, rules=None, reused=False):
    """A relay starting with that pause state file (None: none), frametop.conf and rules."""
    if pause_state is None:
        if os.path.exists(gp.STATE_PATH):
            os.remove(gp.STATE_PATH)
    else:
        with open(gp.STATE_PATH, "w") as f:
            json.dump(pause_state, f)
    CONF.clear()
    CONF.update(conf or {})
    RULES["bad"], RULES["rules"] = False, rules or {}
    r = relay.Relay(FakeVirtual(reused), FakeVirtual(reused), control, screens_sock, can_grab=False, stored={})
    received(helper)
    received(screens)
    LOG.clear()
    return r


PAUSED = {"paused": True, "reason": "gesture", "since": 1.0, "ends_with_game": False, "stopped": {}}
DRIVER = [f"btn {d} 0" for d in sorted(set(relay.Pointer.DRIVER_BUTTONS.values()))]
DESKTOP = ([f"key {c} 0" for c in sorted(relay.MODIFIERS)]
           + [f"key {c} 0" for c in range(relay.BTN_MOUSE, relay.BTN_TASK + 1)] + ["keys release"])

# ---------------------------------------------------------------- L1
r = start(PAUSED, {"POINTER": "1"})
check("L1: restarted while paused, pointer mode on: the pointer isn't in use", (r.pause.paused, r.lease.current, bool(r.lease.conf)),
      (True, None, True))
r.recover_from_restart()
check("L1: ...and stands down, so the helper lets go of a pointer the last relay woke",
      received(helper), ["hide pause", "vrbind -"] + DRIVER)
r = start(None, {"POINTER": "1"})
r.recover_from_restart()
check("L1: not paused: the pointer is in use and doesn't stand down", (r.lease.current is r.lease.conf, received(helper)),
      (True, ["vrbind -"] + DRIVER))
r = start(PAUSED, {"POINTER": "0"})
r.recover_from_restart()
check("L1: paused with POINTER=0: no pointer to stand down", received(helper), ["vrbind -"] + DRIVER)

# ---------------------------------------------------------------- L5
r = start({"paused": False, "stopped": {"units": ["frametop-gaze.service"], "remote": True, "desktop": None}})
check("L5: restarted after a resume, before its job ran: the resume runs now",
      (r.pause.paused, r.pause.worker.jobs, r.pause.worker.stopped["units"]), (False, ["resume"], ["frametop-gaze.service"]))
r = start({"paused": False, "stopped": {"units": [], "remote": False, "desktop": "closed"}})
check("L5: ...a desktop the pause closed comes back", r.pause.worker.jobs, ["resume"])
r = start({"paused": False, "stopped": {"units": [], "remote": False, "desktop": None}})
check("L5: ...nothing left over: nothing", r.pause.worker.jobs, [])

# ---------------------------------------------------------------- L6
GAME = {"paused": True, "reason": "game", "since": 1.0, "ends_with_game": True, "stopped": {}}
r = start(GAME)
check("L6: restarted during a pause that ends with its game: the game counts as running, the pause holds",
      (r.pause.paused, r.pause.game, r.pause.ends_with_game, r.pause.worker.jobs), (True, True, True, ["enforce"]))
now = time.monotonic()
r.pause.game_state(False, now)
r.pause.tick(now + gp.RESUME_DELAY + 0.1)
check("L6: ...the helper says the game ended: resumed", r.pause.paused, False)
r = start(GAME)
now = time.monotonic()
r.pause.tick(now + gp.GAME_STALE + 0.1)
r.pause.tick(now + gp.GAME_STALE + gp.RESUME_DELAY + 0.2)
check("L6: ...no word from the helper (SteamVR went too): resumed", r.pause.paused, False)
r = start(GAME, rules={"pause_auto": False})
check("L6: with automatic pausing off, the rules (applied after the state file) end the tie to the game",
      (r.pause.paused, r.pause.ends_with_game), (True, False))

# ---------------------------------------------------------------- K3
r = start()
r.held.desktop.update({30, 42})  # A and Shift down on the desktop, as the relay knows it
r.recover_from_restart()
check("K3: the desktop lets go of the modifiers and mouse buttons, then of every key (keys release)",
      received(screens), DESKTOP)
# A plain key goes up through ft-screens' "keys release"; this relay's record of it clears at the
# next scan (reconcile_desktop_keys: no device holds it).
check("K3: ...a modifier counts as up here at once, a plain key until the next scan", r.held.desktop, {30})

# ---------------------------------------------------------------- K6
r = start(reused=True)
r.recover_from_restart()
check("K6: a virtual mouse and keyboard reused from the fd store let go of every key",
      (r.mouse.released, r.keyboard.released), (1, 1))
r = start(reused=False)
r.recover_from_restart()
check("K6: ...new ones have nothing down", (r.mouse.released, r.keyboard.released), (0, 0))

# ---------------------------------------------------------------- K7
r = start(None, {"POINTER": "1"})
r.recover_from_restart()
got = received(helper)
check("K7: the helper gets a release for every driver button, after the button map",
      got[got.index("vrbind -"):] if "vrbind -" in got else got, ["vrbind -"] + DRIVER)
r = start(None, {"POINTER": "0"})
r.recover_from_restart()
check("K7: ...with pointer mode off too", received(helper), ["vrbind -"] + DRIVER)

# ---------------------------------------------------------------- the state objects
lease = relay.PointerLease()
lease.configure(True, 0.05, 12.0, 25, paused=False)
p = lease.conf
check("lease: pointer mode on: in use, with the conf's settings",
      (lease.current is p, p.sensitivity, p.idle, p.wake_counts), (True, 0.05, 12.0, 25))
lease.configure(True, 0.04, 10.0, 20, paused=False)
check("lease: a reload keeps the same Pointer", (lease.conf is p, p.sensitivity), (True, 0.04))
lease.pause(True)
check("lease: paused: the pointer isn't in use", (lease.current, lease.conf is p), (None, True))
lease.configure(True, 0.04, 10.0, 20, paused=True)
check("lease: a reload while paused leaves it out of use", lease.current, None)
lease.pause(False)
check("lease: resumed: in use again", lease.current is p, True)
p.sock = type("Recorder", (), {"sent": [], "sendto": lambda self, data, addr: self.sent.append(data.decode())})()
p.action("left", 1, 10.0)
p.sock.sent.clear()
lease.configure(False, 0.03, 30.0, 40, paused=False)
check("lease: pointer mode off with a click held: the helper lets go",
      (lease.conf, lease.current, p.sock.sent), (None, None, ["btn trigger 0", "hide pause"]))
check("lease: holding: no change while there's no pointer", lease.holding_changed(), False)
lease.configure(True, 0.03, 30.0, 40, paused=False)
lease.conf.active = True
check("lease: holding: the pointer came on", (lease.holding_changed(), lease.holding_changed()), (True, False))

held = relay.HeldKeys()
held.meta, held.meta_hidden = {relay.KEY_LEFTMETA, relay.KEY_RIGHTMETA}, {relay.KEY_LEFTMETA}
held.modifiers = {relay.MODIFIERS[relay.KEY_LEFTMETA], relay.MODIFIERS[29]}  # Meta and Left Ctrl
held.forget_up({relay.KEY_RIGHTMETA})
check("held keys: what no device holds is forgotten",
      (held.meta, held.meta_hidden, held.modifiers),
      ({relay.KEY_RIGHTMETA}, set(), {relay.MODIFIERS[relay.KEY_RIGHTMETA]}))

typing = relay.Typing()
typing.queue.extend(["a", ("quiet", "b"), ("key", 30, 1), "c", ("key", 30, 0)])
typing.kb_held = {31: True, 30: False}
check("typing: Full Keyboard goes: its keys to come are dropped, the ones it held come back to let go",
      (typing.drop_full_keyboard(), list(typing.queue), typing.kb_held), ([(30, False), (31, True)], ["a", ("quiet", "b"), "c"], {}))

# ---------------------------------------------------------------- reload: all or nothing
r = start(None, {"POINTER": "1", "POINTER_SENSITIVITY": "0.05"},
          {"pause_sound": False, "controller_buttons": {"right/a": "left"}})
rules, pointer, settings = r.rules, r.lease.conf, dict(r.pause.settings)
CONF.update({"POINTER": "0"})
RULES["bad"] = True
r.load_config()
check("reload, the rules don't read: nothing changes, not even frametop.conf's part",
      (r.rules is rules, r.lease.conf is pointer, r.lease.current is pointer, pointer.sensitivity, r.pause.settings),
      (True, True, True, 0.05, settings))
check("...and says so", [line for line in LOG if "not applied" in line][:1],
      ["config not applied, the last one kept: ValueError(\"a rules file that doesn't read\")"])
RULES["bad"] = False
RULES["rules"] = {"pause_sound": True}
r.load_config()
check("reload, both read: all of it applies",
      (r.lease.conf, r.lease.current, r.rules["controller_buttons"].get("right/a"), r.pause.settings["sound"]),
      (None, None, None, True))
CONF.clear()
CONF.update({"POINTER": "1"})
RULES["bad"] = True
r = relay.Relay(FakeVirtual(False), FakeVirtual(False), control, screens_sock, can_grab=False, stored={})
check("at start, the rules don't read: the default rules, with frametop.conf's settings",
      (r.rules == read_rules(""), bool(r.lease.conf)), (True, True))

shutil.rmtree(OUT, ignore_errors=True)
print(f"\n{len(failures)} failed" if failures else "\nall ok")
sys.exit(1 if failures else 0)
