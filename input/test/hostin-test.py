#!/usr/bin/env python3
"""Offline test of the input relay's host devices (docs/host-input.md): a streamed computer's
keyboard and mouse, sent by its ft-stream as "hostdev", "hostev" and "hostbye" on the control
socket. A fake host here sends them. It checks the devices and their roles, the 3D mouse,
typing to the desktop and to Steam, a key binding, keys held when the host goes quiet, and
a host letting go.

Runs the relay's main() with no event nodes, every socket it sends to renamed, no uinput
devices (they record what they're sent), no grabs, and ft-steam swapped for a logger, like
keys-test.py. Nothing reaches the live desktop, SteamVR, or the running relay.

  input/test/hostin-test.py [RELAY]   (default: input/input-relay.py next to this folder)
"""
import importlib.util
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
tag = f"ft_hostin_test_{os.getpid()}"
with open(relay_path) as f:
    src = f.read().replace('"\\0frametop_relay"', f'"\\0{tag}_relay"')
screens_socket = "screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)"
assert src.count(screens_socket) == 1, "relay's screens socket constructor changed"
src = src.replace(screens_socket, "screens_sock = _test_screens_sender")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))
exec(compile(src, relay_path, "exec"), relay.__dict__)


class StubPause:
    """game_pause.GamePause without its threads, state file, or services."""
    paused = game = False
    settings = {"sound": False}

    def __init__(self, log, on_change, buttons):
        self.on_change = on_change

    def toggle(self, reason, now):
        self.paused = not self.paused
        self.on_change(self.paused)

    def set(self, on, reason, now):
        if on != self.paused:
            self.toggle(reason, now)

    def status(self):
        return {"t": "pause", "paused": self.paused}

    def timeout(self, now):
        return 3600.0

    def configure(self, *args):
        pass

    game_state = tick = helper_started = controllers_changed = set_hands = dashboard_state = screens_seen = configure
    set_pad = configure


relay.game_pause.GamePause = StubPause
for name in ("SCREENS", "HELPER", "FLOAT", "GAZED", "VOICE", "SPOTWATCH"):
    setattr(relay, name, f"\0{tag}_{name.lower()}")
OUT = tempfile.mkdtemp(prefix="ft-hostin-test-")
steam_log = f"{OUT}/steam.log"
relay.FT_STEAM = f"{OUT}/ft-steam"
with open(relay.FT_STEAM, "w") as f:
    f.write(f'#!/bin/sh\necho "$@" >> {steam_log}\n')
os.chmod(relay.FT_STEAM, 0o755)

EMITTED = []  # (virtual device, code, value)


class NoDevice:
    def __init__(self, name, *args, **kwargs):
        self.name = name.split()[-1]

    def emit(self, etype, code, value):
        EMITTED.append((self.name, code, value))

    def sync(self):
        pass


relay.Virtual = NoDevice
LOG = []
relay.log = lambda *args: LOG.append(" ".join(map(str, args)))
relay.read_rules = lambda path=None: {"devices": {}, "buttons": {}, "controller_buttons": {},
                                      "key_bindings": dict(relay.DEFAULT_KEY_BINDINGS)}
relay.read_config = lambda path=None: {"POINTER": "1"}

# No event nodes at all: the host devices are the only ones.
fake_os = type(os)("os")
fake_os.__dict__.update(os.__dict__)
fake_os.listdir = lambda path: [] if path == "/dev/input" else os.listdir(path)
relay.os = fake_os

relay._test_screens_sender, screens = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
relay._test_screens_sender.setblocking(False)
screens.bind(relay.SCREENS)
screens.settimeout(0.05)
helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
helper.bind(relay.HELPER)
helper.settimeout(0.05)


def received(sock):
    got = []
    while True:
        try:
            got.append(sock.recv(256).decode())
        except socket.timeout:
            return got


def to_relay(text, wait=0.05):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.sendto(text.encode(), f"\0{tag}_relay")
    c.close()
    time.sleep(wait)


def ask(text):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.bind("")
    c.settimeout(2)
    c.sendto(text.encode(), f"\0{tag}_relay")
    data = c.recv(65536)
    c.close()
    return json.loads(data)


def emitted():
    got = list(EMITTED)
    EMITTED.clear()
    return got


def steam_calls():
    time.sleep(0.3)
    try:
        with open(steam_log) as f:
            return f.read().split()
    except OSError:
        return []


def ev(kind, *triples):
    flat = " ".join(f"{t} {c} {v}" for t, c, v in triples)
    to_relay(f"hostev testpc {kind} {flat} 0 0 0")


def key(code, value):
    ev("keyboard", (relay.EV_KEY, code, value))


def hello():
    to_relay("hostdev testpc mouse Test PC", wait=0)
    to_relay("hostdev testpc keyboard Test PC")


failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


A, SHIFT, META = 30, 42, 125


def tests():
    time.sleep(0.5)
    received(screens)
    received(helper)

    ev("mouse", (relay.EV_REL, relay.REL_X, 50))
    check("events before a hello: no device", [d["id"] for d in ask("devices")["nodes"]], [])

    hello()
    devs = {d["id"]: (d["role"], d["bus"], d["kinds"], d["name"]) for d in ask("devices")["nodes"]}
    check("a hello makes a mouse (pointer) and a keyboard (pass-through)", devs,
          {"host:testpc:mouse": ("pointer", "network", ["mouse"], "Test PC Mouse"),
           "host:testpc:keyboard": ("passthrough", "network", ["keyboard"], "Test PC Keyboard")})

    ev("mouse", (relay.EV_REL, relay.REL_X, 30), (relay.EV_REL, relay.REL_Y, 20))
    ev("mouse", (relay.EV_REL, relay.REL_X, 30))
    time.sleep(0.1)
    got = received(helper)
    check("the host's mouse wakes the 3D mouse", got[:2], ["show", "recenter"])
    check("...and moves it", any(m.startswith("move ") for m in got), True)

    ev("mouse", (relay.EV_KEY, relay.BTN_LEFT, 1))
    ev("mouse", (relay.EV_KEY, relay.BTN_LEFT, 0))
    time.sleep(0.1)
    clicks = [m for m in received(helper) if m.startswith("btn trigger")]
    check("its left button is the 3D mouse's trigger", clicks, ["btn trigger 1", "btn trigger 0"])

    to_relay("keyboard desktop 1")
    received(screens)
    key(A, 1)
    key(A, 0)
    check("typing to the desktop: the keys go to ft-screens", received(screens), ["key 30 1", "key 30 0"])
    check("...and not out on the virtual keyboard", emitted(), [])

    to_relay("keyboard steam 1")
    key(A, 1)
    key(A, 0)
    received(screens)
    check("typing to Steam: the keys go out on the virtual keyboard", emitted(),
          [("keyboard", A, 1), ("keyboard", A, 0)])

    to_relay("keyboard desktop 1")
    key(META, 1)
    key(META, 0)
    check("a Meta tap on the host's keyboard: the Steam menu (default binding)", steam_calls(), ["menu"])
    received(screens)

    # Held keys when the host goes quiet: Shift on the desktop, A on Steam.
    key(SHIFT, 1)
    to_relay("keyboard steam 1")
    key(A, 1)
    received(screens)
    emitted()
    check("Shift held on the desktop counts as down", relay_down_note(), True)
    deadline = time.monotonic() + relay.HOST_TIMEOUT + 2.5
    while time.monotonic() < deadline and any(d["id"].startswith("host:") for d in ask("devices")["nodes"]):
        time.sleep(0.2)
    check("no word for HOST_TIMEOUT: both devices go away",
          [d["id"] for d in ask("devices")["nodes"]], [])
    check("...the key it held on Steam comes up", emitted(), [("keyboard", A, 0)])
    time.sleep(1.2)  # the next scan's reconcile
    check("...and the one it held on the desktop", "key 42 0" in received(screens), True)
    check("the log says why", any("its host went quiet" in line for line in LOG), True)

    hello()
    to_relay("hostbye testpc mouse")
    check("hostbye: that device goes, the other stays",
          [d["id"] for d in ask("devices")["nodes"]], ["host:testpc:keyboard"])
    to_relay("hostdev ../evil keyboard x")
    to_relay("hostdev testpc tablet x")
    check("a bad id or kind is ignored", [d["id"] for d in ask("devices")["nodes"]], ["host:testpc:keyboard"])


def relay_down_note():
    # The relay's reconcile must not release a key a host holds: a second scan passes
    # with Shift still down on the desktop.
    time.sleep(1.2)
    return "key 42 0" not in received(screens)


def run():
    try:
        tests()
    except Exception as e:
        check(f"no exception ({e!r})", False, True)
    finally:
        shutil.rmtree(OUT, ignore_errors=True)
        print("\n".join(f"  log: {line}" for line in LOG if "host" in line))
        print(f"{len(failures)} failed" if failures else "all ok", flush=True)
        os._exit(1 if failures else 0)


relay.signal.signal = lambda *a: None  # main() runs in this thread; it may set handlers
threading.Thread(target=run, daemon=True).start()
sys.argv = [relay_path, "--no-grab"]
relay.main()
