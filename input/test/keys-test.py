#!/usr/bin/env python3
"""Offline test of the input relay's key combinations and modifier taps, of which keys and
mouse buttons reach the desktop (ft-screens), of dictation (Frametop Voice): the keys that
start and stop it, and the text it has the relay type, and of Full Keyboard's typing.

Runs the relay's main() against a fake pass-through keyboard, a fake mouse, and two fake nodes
with volume keys, a keyboard's Consumer Control node and the headset's gpio-keys (pipes), with
every socket it sends to renamed, no uinput devices (they record what they're sent), no grabs,
no volume changes, and ft-steam swapped for a logger. Pausing (game_pause.py) is a stub: no
threads, state file, or services. Nothing reaches the live desktop, SteamVR, or the running
relay, so it's safe next to them. Rules come from the test, not ~/.config/frametop-input.json.

  input/test/keys-test.py [RELAY]   (default: input/input-relay.py next to this folder)
"""
import fcntl
import importlib.util
import json
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
tag = f"ft_keys_test_{os.getpid()}"
with open(relay_path) as f:
    src = f.read().replace('"\\0frametop_relay"', f'"\\0{tag}_relay"')
screens_socket = "screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)"
assert src.count(screens_socket) == 1, "relay's screens socket constructor changed"
src = src.replace(screens_socket, "screens_sock = _test_screens_sender")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))  # its game_pause and vrws
exec(compile(src, relay_path, "exec"), relay.__dict__)


class StubPause:
    """game_pause.GamePause without its threads, state file, or services."""
    paused = False

    def __init__(self, log, on_change, buttons):
        self.on_change = on_change
        stub["pause"] = self

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


stub = {}
relay.game_pause.GamePause = StubPause

for name in ("SCREENS", "HELPER", "FLOAT", "GAZED", "VOICE", "FRUNNER"):
    setattr(relay, name, f"\0{tag}_{name.lower()}")
OUT = tempfile.mkdtemp(prefix="ft-keys-test-")
steam_log, cmd_log = f"{OUT}/steam.log", f"{OUT}/cmd.log"
relay.FT_STEAM = f"{OUT}/ft-steam"
with open(relay.FT_STEAM, "w") as f:
    f.write(f'#!/bin/sh\necho "$@" >> {steam_log}\n')
os.chmod(relay.FT_STEAM, 0o755)


EMITTED = []  # (virtual device, code, value): what the virtual mouse and keyboard were sent
VOLUME = []  # (code, value): volume keys the relay handled itself (no wpctl here)


class NoDevice:
    """A virtual device that only records what it's sent."""

    def __init__(self, name, *args, **kwargs):
        self.name = name.split()[-1]  # mouse or keyboard

    def emit(self, etype, code, value):
        EMITTED.append((self.name, code, value))

    def sync(self):
        pass


relay.Virtual = NoDevice
relay.Volume.key = lambda self, fd, code, value, now: VOLUME.append((code, value))
LOG = []  # the relay's own log
relay.log = lambda *args: LOG.append(" ".join(map(str, args)))
bindings = {"now": None}  # the rules' key_bindings; None: the relay's defaults
devices = {"roles": {}, "buttons": {}}  # the rules' "devices" roles and per-device "buttons"
vr_keyboard = {"mode": None}  # the rules' vr_keyboard (Frametop's keyboard); None: the default
MOUSE_ID = "usb:0003:0004:test mouse"  # the fake mouse's id (Node.id)


def read_rules(path=None):
    rules = {"devices": {i: {"role": r} for i, r in devices["roles"].items()},
             "buttons": {i: dict(b) for i, b in devices["buttons"].items()}, "controller_buttons": {}}
    rules["key_bindings"] = dict(relay.DEFAULT_KEY_BINDINGS if bindings["now"] is None else bindings["now"])
    if vr_keyboard["mode"]:
        rules["vr_keyboard"] = vr_keyboard["mode"]
    return rules


real_read_rules = relay.read_rules  # the file reader, for the renamed-actions check
relay.read_rules = read_rules
conf = {"POINTER": "0"}  # ~/.config/frametop.conf, as the relay reads it
relay.read_config = lambda path=None: dict(conf)

# The fake devices, each a pipe: /dev/input/event900 (keyboard), event901 (mouse), event902 (the
# keyboard's Consumer Control node: media and volume keys), event903 (the headset's gpio-keys).
kb_r, kb_w = os.pipe()
ms_r, ms_w = os.pipe()
cc_r, cc_w = os.pipe()
gp_r, gp_w = os.pipe()
for fd in (kb_r, ms_r, cc_r, gp_r):
    os.set_blocking(fd, False)
FAKE = {"/dev/input/event900": kb_r, "/dev/input/event901": ms_r, "/dev/input/event902": cc_r,
        "/dev/input/event903": gp_r}
READ_END = {kb_w: kb_r, ms_w: ms_r, cc_w: cc_r, gp_w: gp_r}
HELD = {kb_r: set(), ms_r: set(), cc_r: set(), gp_r: set()}  # what EVIOCGKEY says each holds
BUS_HOST = 0x19


class Inode:
    st_ino = 1


fake_os = type(os)("os")
fake_os.__dict__.update(os.__dict__)
fake_os.listdir = lambda path: [p.split("/")[-1] for p in FAKE] if path == "/dev/input" else os.listdir(path)
fake_os.stat = lambda path, *a, **k: Inode() if path in FAKE else os.stat(path, *a, **k)
fake_os.access = lambda path, mode, *a, **k: path in FAKE or os.access(path, mode, *a, **k)
relay.os = fake_os


def ioctl(fd, request, arg=0, *rest):
    if fd in HELD and request == relay.EVIOCGKEY:
        for c in HELD[fd]:
            arg[c // 8] |= 1 << (c % 8)
        return 0
    return fcntl.ioctl(fd, request, arg, *rest)


fake_fcntl = type(fcntl)("fcntl")
fake_fcntl.__dict__.update(fcntl.__dict__)
fake_fcntl.ioctl = ioctl
relay.fcntl = fake_fcntl


def probe(path):
    if FAKE[path] == kb_r:
        return relay.Node(path, kb_r, "test keyboard", relay.BUS_USB, 1, 2, "", False, True)
    if FAKE[path] == ms_r:
        return relay.Node(path, ms_r, "test mouse", relay.BUS_USB, 3, 4, "", True, False)
    # Nodes with volume keys (role "volume"). take_volume does nothing with --no-grab, so they
    # come as if it had remapped their volume keys to the stand-ins.
    if FAKE[path] == cc_r:
        node = relay.Node(path, cc_r, "test keyboard Consumer Control", relay.BUS_USB, 1, 2, "", False, False,
                          candidate=False)
    else:
        node = relay.Node(path, gp_r, "gpio-keys", BUS_HOST, 0, 0, "", False, False, candidate=False)
    node.remapped = True
    return node


relay.probe = probe

# A connected pair holds all 16 startup releases even when the kernel limits unconnected
# datagram queues to 10 messages. The relay still uses real sendto(), including other peers.
relay._test_screens_sender, screens = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
relay._test_screens_sender.setblocking(False)
screens.bind(relay.SCREENS)
screens.settimeout(0.05)
voice = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)  # Frametop Voice's socket
voice.bind(relay.VOICE)
voice.settimeout(0.05)
frunner = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)  # frunner's socket (frunner/ft-frunner)
frunner.bind(relay.FRUNNER)
frunner.settimeout(0.05)
EVENT = struct.Struct("llHHi")


def send(fd, etype, code, value):
    os.write(fd, EVENT.pack(0, 0, etype, code, value) + EVENT.pack(0, 0, 0, 0, 0))
    time.sleep(0.03)


def key(code, value, fd=kb_w):
    held = HELD[READ_END[fd]]
    (held.add if value else held.discard)(code)
    send(fd, relay.EV_KEY, code, value)


def typed():
    """What the desktop was sent since the last call."""
    got = []
    while True:
        try:
            got.append(screens.recv(256).decode())
        except socket.timeout:
            return got


def frunner_got():
    """What frunner was sent since the last call."""
    got = []
    while True:
        try:
            got.append(frunner.recv(256).decode())
        except socket.timeout:
            return got


def dictation():
    """What Frametop Voice was sent since the last call."""
    got = []
    while True:
        try:
            got.append(voice.recv(256).decode())
        except socket.timeout:
            return got


def to_relay(text):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.sendto(text.encode(), f"\0{tag}_relay")
    c.close()


def keys_for(text, keys=None):
    """The presses and releases that type text, as (code, value), modifiers included: on a US
    layout, or keys ({character: (code, modifiers)})."""
    out = []
    for ch in text:
        code, mods = (keys or relay.US_KEYS)[ch]
        out += [(m, 1) for m in mods] + [(code, 1), (code, 0)] + [(m, 0) for m in reversed(mods)]
    return out


def emitted():
    """What the virtual mouse and keyboard were sent since the last call."""
    got = []
    while EMITTED:
        got.append(EMITTED.pop(0))
    return got


def use(key_bindings):
    bindings["now"] = key_bindings
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.bind("")
    c.settimeout(2)
    c.sendto(b"reload", f"\0{tag}_relay")
    c.recv(4096)
    time.sleep(0.05)
    typed()


def pause(cmd):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    c.bind("")
    c.settimeout(2)
    c.sendto(f"pause {cmd} test".encode(), f"\0{tag}_relay")
    c.recv(4096)
    time.sleep(0.05)


def lines(path, wait=0.3):
    time.sleep(wait)
    try:
        with open(path) as f:
            return f.read().split()
    except OSError:
        return []


failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


META, RMETA, SHIFT, CTRL, RCTRL, ALT, A, C, F, J, P, TAB = 125, 126, 42, 29, 97, 56, 30, 46, 33, 36, 25, 15
F24 = ["key 194 1", "key 194 0"]


def tests():
    time.sleep(1.5)  # the relay's first device scan
    check("a relay that starts releases the modifiers and mouse buttons on the desktop", typed(),
          [f"key {c} 0" for c in sorted(relay.MODIFIERS)] + [f"key {c} 0" for c in range(0x110, 0x118)])
    key(META, 1); key(META, 0)
    check("Meta tap (default): Steam menu", lines(steam_log), ["menu"])
    check("Meta tap: the desktop gets F24 before Meta's release", typed(), ["key 125 1"] + F24 + ["key 125 0"])

    key(RMETA, 1); key(RMETA, 2); key(RMETA, 2); key(RMETA, 0)
    check("right Meta held (autorepeat), then released: a tap", lines(steam_log), ["menu", "menu"])
    typed()

    key(META, 1); key(A, 1); key(A, 0); key(META, 0)
    check("Meta+A: no tap", lines(steam_log), ["menu", "menu"])
    check("Meta+A (unbound) is typed as is", typed(), ["key 125 1", "key 30 1", "key 30 0", "key 125 0"])

    key(META, 1); key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w); key(META, 0)
    check("Meta+click: no tap", lines(steam_log), ["menu", "menu"])
    key(META, 1); send(ms_w, relay.EV_REL, relay.REL_WHEEL, 1); key(META, 0)
    check("Meta+scroll: no tap", lines(steam_log), ["menu", "menu"])
    key(META, 1); key(SHIFT, 1); key(SHIFT, 0); key(META, 0)
    check("Meta+Shift: no tap", lines(steam_log), ["menu", "menu"])
    typed()

    key(META, 1); key(J, 1); key(J, 0); key(META, 0)
    check("Meta+J (default gaze click): no tap", lines(steam_log), ["menu", "menu"])
    check("Meta+J: F24, Meta up early, its real release dropped", typed(), ["key 125 1"] + F24 + ["key 125 0"])

    # Mouse buttons reach the desktop only when pointer mode passes them through as keys.
    emitted()
    key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w); key(relay.BTN_SIDE, 1, ms_w); key(relay.BTN_SIDE, 0, ms_w)
    check("POINTER=0: clicks don't reach the desktop", typed(), [])
    check("POINTER=0: clicks go to the virtual mouse", emitted(),
          [("mouse", relay.BTN_LEFT, 1), ("mouse", relay.BTN_LEFT, 0), ("mouse", relay.BTN_SIDE, 1),
           ("mouse", relay.BTN_SIDE, 0)])

    # A keyboard's Consumer Control node isn't grabbed, so gamescope has its keys already: its
    # media keys go to the desktop and nowhere else, its volume keys to the relay's volume
    # handling only. The headset's gpio-keys (BUS_HOST) send nothing on.
    PLAYPAUSE, SELECT, VOLUP = 164, 353, relay.KEY_VOLUMEUP
    key(PLAYPAUSE, 1, cc_w); key(PLAYPAUSE, 0, cc_w)
    check("media key: reaches the desktop", typed(), ["key 164 1", "key 164 0"])
    check("media key: no virtual device", emitted(), [])
    standin = relay.VOLUME_STANDIN[VOLUP]
    key(standin, 1, cc_w); key(standin, 0, cc_w)
    check("volume key (its stand-in): the relay's volume handling only", (typed(), emitted(), VOLUME[:]),
          ([], [], [(standin, 1), (standin, 0)]))
    VOLUME.clear()
    key(VOLUP, 1, cc_w); key(VOLUP, 0, cc_w)
    check("a volume key a remap missed reaches nothing", (typed(), emitted(), VOLUME[:]), ([], [], []))
    key(SELECT, 1, gp_w); key(SELECT, 0, gp_w)
    check("the headset's click button (gpio-keys, KEY_SELECT) reaches nothing", (typed(), emitted()), ([], []))
    key(PLAYPAUSE, 1, cc_w)
    time.sleep(1.2)  # the relay's once-a-second check (reconcile_desktop_keys)
    check("a media key held for over a second stays down on the desktop", typed(), ["key 164 1"])
    HELD[cc_r].discard(PLAYPAUSE)  # its node lets go, and the release never comes
    time.sleep(1.2)
    check("...and comes up there once its node doesn't hold it", typed(), ["key 164 0"])

    use({"29+42+33": f"command:echo combo >> {cmd_log}", "125": "none"})
    key(CTRL, 1); key(SHIFT, 1); key(F, 1); key(F, 0); key(SHIFT, 0); key(CTRL, 0)
    check("Ctrl+Shift+F runs its command", lines(cmd_log), ["combo"])
    check("the combination's last key isn't typed", [k for k in typed() if k.startswith("key 33 ")], [])
    key(META, 1); key(META, 0)
    check("Meta tap bound to none: nothing", lines(steam_log), ["menu", "menu"])
    check("Meta tap bound to none: no F24", typed(), ["key 125 1", "key 125 0"])

    use({"29": f"command:command -v ft-steam ft-layout ft-float ft-voice | wc -l >> {cmd_log}"})
    key(RCTRL, 1); key(RCTRL, 0)
    check("right Ctrl tap runs its command, which finds Frametop's tools", lines(cmd_log), ["combo", "4"])
    check("Ctrl tap: F24 before Ctrl's release", typed(), ["key 97 1"] + F24 + ["key 97 0"])

    use({"125+36": "gaze_left", "29": f"command:echo paused >> {cmd_log}", "42+125+25": "pause_toggle"})
    pause("on")
    check("pause on: paused", stub["pause"].paused, True)
    key(META, 1); key(J, 1); key(J, 0); key(META, 0)
    check("paused: Meta+J (gaze click) does nothing and is typed as is", typed(),
          ["key 125 1", "key 36 1", "key 36 0", "key 125 0"])
    key(RCTRL, 1); key(RCTRL, 0)
    check("paused: a command still runs", lines(cmd_log), ["combo", "4", "paused"])
    typed()
    key(SHIFT, 1); key(META, 1); key(P, 1); key(P, 0); key(META, 0); key(SHIFT, 0)
    check("paused: Meta+Shift+P (pause_toggle) resumes", stub["pause"].paused, False)
    check("Meta+Shift+P isn't typed", [k for k in typed() if k.startswith("key 25 ")], [])
    key(META, 1); key(J, 1); key(J, 0); key(META, 0)
    check("resumed: Meta+J is a combination again", typed(), ["key 125 1"] + F24 + ["key 125 0"])

    # Pointer mode: a click held into a pause comes up as it starts, then the pointer hides
    # (stand_down); the release that comes during the pause reaches no one.
    helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    helper.bind(relay.HELPER)
    helper.settimeout(0.05)

    def clicks():
        """The helper's button commands and hides since the last call (not the claim pulse)."""
        got = []
        while True:
            try:
                m = helper.recv(256).decode()
            except socket.timeout:
                return got
            if m.startswith("hide") or (m.startswith("btn ") and not m.startswith("btn a ")):
                got.append(m)

    conf["POINTER"] = "1"
    use({})
    key(relay.BTN_LEFT, 1, ms_w)
    time.sleep(0.5)  # the claim pulse, 0.3 s after the pointer wakes
    check("pointer mode: a held left button reaches the helper", clicks(), ["btn trigger 1"])
    pause("on")
    check("pause with the left button held: it comes up, then the pointer hides", clicks(),
          ["btn trigger 0", "hide pause"])
    key(relay.BTN_LEFT, 0, ms_w)
    check("paused: its release reaches no one", clicks(), [])
    pause("off")
    key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w)
    check("resumed: a click goes to the helper again", clicks(), ["btn trigger 1", "btn trigger 0"])

    # Pointer mode: a click is the helper's alone. A side button mapped to "key" (Back on the
    # desktop) goes to the desktop too, and its release still does after a pause starts; paused,
    # clicks go to the virtual mouse only.
    check("pointer mode: a click doesn't reach the desktop", typed(), [])
    emitted()
    devices["buttons"] = {MOUSE_ID: {str(relay.BTN_SIDE): "key"}}
    use({})
    key(relay.BTN_SIDE, 1, ms_w)
    check("pointer mode: a side button mapped to key reaches the desktop", typed(), [f"key {relay.BTN_SIDE} 1"])
    check("...and the virtual mouse, not the helper", (emitted(), clicks()), ([("mouse", relay.BTN_SIDE, 1)], []))
    pause("on")
    clicks()
    key(relay.BTN_SIDE, 0, ms_w)
    check("paused: its release still reaches the desktop", typed(), [f"key {relay.BTN_SIDE} 0"])
    emitted()
    key(relay.BTN_SIDE, 1, ms_w); key(relay.BTN_SIDE, 0, ms_w); key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w)
    check("paused: clicks don't reach the desktop", typed(), [])
    check("paused: clicks go to the virtual mouse", emitted(),
          [("mouse", relay.BTN_SIDE, 1), ("mouse", relay.BTN_SIDE, 0), ("mouse", relay.BTN_LEFT, 1),
           ("mouse", relay.BTN_LEFT, 0)])
    pause("off")
    clicks()
    devices["roles"] = {MOUSE_ID: "passthrough"}
    use({})
    key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w); key(relay.BTN_SIDE, 1, ms_w); key(relay.BTN_SIDE, 0, ms_w)
    check("a pass-through mouse's clicks don't reach the desktop", typed(), [])
    check("...nor a virtual device or the helper", (emitted(), clicks()), ([], []))
    devices["roles"], devices["buttons"] = {}, {}
    conf["POINTER"] = "0"

    use(None)  # the defaults: Meta+Alt+Tab and Meta+Alt+Shift+Tab spin the panels (ft-screens)
    key(META, 1); key(ALT, 1); key(TAB, 1); key(TAB, 0); key(ALT, 0); key(META, 0)
    got = typed()
    check("Meta+Alt+Tab (default): spin next", [m for m in got if m.startswith("spin")], ["spin next"])
    check("Meta+Alt+Tab: Tab isn't typed", [m for m in got if m.startswith("key 15 ")], [])
    key(META, 1); key(ALT, 1); key(SHIFT, 1); key(TAB, 1); key(TAB, 0); key(SHIFT, 0); key(ALT, 0); key(META, 0)
    check("Meta+Alt+Shift+Tab (default): spin prev", [m for m in typed() if m.startswith("spin")], ["spin prev"])
    pause("on")
    key(META, 1); key(ALT, 1); key(TAB, 1); key(TAB, 0); key(ALT, 0); key(META, 0)
    check("paused: Meta+Alt+Tab doesn't spin", [m for m in typed() if m.startswith("spin")], [])
    pause("off")
    check("spinning works without pointer mode", (relay.needs_pointer("spin_next"), relay.needs_pointer("spin_prev")),
          (False, False))

    # Meta+Space (a default): frunner's search opens or closes, told it came from a keyboard.
    SPACE = 57
    key(META, 1); key(SPACE, 1); key(SPACE, 0); key(META, 0)
    check("Meta+Space (default): frunner toggles", frunner_got(), ["toggle keyboard"])
    check("...Space isn't typed, and the desktop gets F24 and Meta's release first", typed(),
          ["key 125 1"] + F24 + ["key 125 0"])
    pause("on")
    key(META, 1); key(SPACE, 1); key(SPACE, 0); key(META, 0)
    check("paused: Meta+Space doesn't open frunner", frunner_got(), [])
    pause("off")
    typed()
    check("frunner works without pointer mode", relay.needs_pointer("frunner"), False)
    with tempfile.NamedTemporaryFile("w", suffix=".json") as old:
        json.dump({"buttons": {"mouse": {"275": "spotwatch"}}, "controller_buttons": {"left/view": "spotwatch"},
                   "key_bindings": {"125+57": "spotwatch", "125": "steam_menu"}}, old)
        old.flush()
        r = real_read_rules(old.name)
    check("a rules file from before the rename: spotwatch means frunner",
          (r["buttons"], r["controller_buttons"], r["key_bindings"]),
          ({"mouse": {"275": "frunner"}}, {"left/view": "frunner"}, {"125+57": "frunner", "125": "steam_menu"}))

    # Dictation: Right Ctrl alone (the default with Frametop Voice installed) starts and stops it.
    use({"97": "dictate"})
    dictation()
    key(RCTRL, 1); key(RCTRL, 0)
    check("Right Ctrl tap (dictate): Frametop Voice toggles", dictation(), ["toggle keyboard"])
    check("...and the desktop gets F24 before Ctrl's release", typed(), ["key 97 1"] + F24 + ["key 97 0"])
    key(CTRL, 1); key(CTRL, 0)
    check("a Left Ctrl tap isn't Right Ctrl's", dictation(), [])
    key(SHIFT, 1); key(RCTRL, 1); key(RCTRL, 0); key(SHIFT, 0)
    check("Shift held, then a Right Ctrl tap: not dictation", dictation(), [])
    key(RCTRL, 1); key(SHIFT, 1); key(SHIFT, 0); key(RCTRL, 0)
    check("Right Ctrl held, Shift pressed and released: not dictation", dictation(), [])
    key(RCTRL, 1); key(C, 1); key(C, 0); key(RCTRL, 0)
    check("Right Ctrl+C: not dictation", dictation(), [])
    key(RCTRL, 1); key(relay.BTN_LEFT, 1, ms_w); key(relay.BTN_LEFT, 0, ms_w); key(RCTRL, 0)
    check("Right Ctrl+click: not dictation", dictation(), [])
    use({"29": "dictate"})
    key(CTRL, 1); key(CTRL, 0); key(RCTRL, 1); key(RCTRL, 0)
    check("a Ctrl tap bound by its left code: either side", dictation(), ["toggle keyboard", "toggle keyboard"])
    use({"97": "dictate_hold"})
    key(RCTRL, 1)
    check("Right Ctrl bound to dictate_hold: starts on the press", dictation(), ["start keyboard"])
    key(RCTRL, 0)
    check("...and stops on the release", dictation(), ["stop keyboard"])
    key(CTRL, 1); key(CTRL, 0)
    check("...Left Ctrl doesn't", dictation(), [])
    key(RCTRL, 1); key(F, 1)
    check("Right Ctrl+F while held: a shortcut, so that dictation is cancelled quietly", dictation(),
          ["start keyboard", "cancel keyboard quiet"])
    key(F, 0); key(RCTRL, 0)
    check("...and its release does nothing more", dictation(), [])
    key(SHIFT, 1); key(RCTRL, 1); key(RCTRL, 0); key(SHIFT, 0)
    check("Right Ctrl with Shift held: not on its own, no dictation", dictation(), [])
    use({"29+42+33": "dictate_hold"})
    key(CTRL, 1); key(SHIFT, 1); key(F, 1)
    check("Ctrl+Shift+F held (dictate_hold): starts", dictation(), ["start keyboard"])
    key(F, 0); key(SHIFT, 0); key(CTRL, 0)
    check("...and its release stops", dictation(), ["stop keyboard"])
    use({"97": "dictate"})
    pause("on")
    key(RCTRL, 1); key(RCTRL, 0)
    check("paused: dictation still toggles", dictation(), ["toggle keyboard"])
    pause("off")
    check("dictation works without pointer mode",
          (relay.needs_pointer("dictate"), relay.needs_pointer("dictate_hold")), (False, False))
    unit = os.path.join(OUT, "frametop-voice.service")
    relay.VOICE_UNIT = unit
    check("defaults without Frametop Voice: no dictation key", "97" in relay.default_key_bindings(), False)
    open(unit, "w").close()
    check("defaults with Frametop Voice installed: Right Ctrl tap", relay.default_key_bindings().get("97"), "dictate")

    # Typing dictation's text: on the virtual keyboard while typing goes to Steam, to the desktop
    # while it goes there; held back while a modifier is held.
    use({})
    typed(), emitted()
    to_relay("type Hi, you!")
    time.sleep(0.3)
    check("type: to the virtual keyboard while typing goes to Steam", emitted(),
          [("keyboard", c, v) for c, v in keys_for("Hi, you!")])
    check("...and not to the desktop", typed(), [])
    to_relay("type caf\u00e9\tok")
    time.sleep(0.3)
    check("type: what a US layout can't type is dropped", emitted(), [("keyboard", c, v) for c, v in keys_for("cafok")])
    key(CTRL, 1)
    to_relay("type ab")
    time.sleep(0.3)
    check("type: waits while Ctrl is held", emitted(), [])
    key(CTRL, 0)
    time.sleep(0.3)
    check("...and types once it's released", emitted(), [("keyboard", c, v) for c, v in keys_for("ab")])
    typed()
    to_relay("keyboard desktop")
    to_relay("type Ok.")
    time.sleep(0.3)
    check("type: to the desktop while typing goes there", typed(), [f"key {c} {v}" for c, v in keys_for("Ok.")])
    check("...and not to the virtual keyboard", emitted(), [])
    use({"97": "dictate_hold"})
    dictation()
    key(RCTRL, 1)
    to_relay("type ok")
    time.sleep(0.3)
    check("held to talk: live text reaches the desktop while Right Ctrl is held, without it",
          typed(), [f"key {c} {v}" for c, v in keys_for("ok")])
    key(RCTRL, 2); key(RCTRL, 0)
    check("...nor its repeats or release", typed(), [])
    check("...and dictation ran from press to release", dictation(), ["start keyboard", "stop keyboard"])
    key(RCTRL, 1); key(C, 1); key(C, 0); key(RCTRL, 0)
    check("held to talk, then C: the desktop gets Right Ctrl before C (a shortcut after all)", typed(),
          ["key 97 1", f"key {C} 1", f"key {C} 0", "key 97 0"])
    check("...and that dictation is cancelled quietly", dictation(), ["start keyboard", "cancel keyboard quiet"])
    key(RCTRL, 1); key(SHIFT, 1)
    check("held to talk, then Shift: cancelled, and the desktop gets both", (dictation(), typed()),
          (["start keyboard", "cancel keyboard quiet"], ["key 97 1", f"key {SHIFT} 1"]))
    key(SHIFT, 0); key(RCTRL, 0)
    typed()
    to_relay("keyboard steam")
    key(RCTRL, 1)
    to_relay("type ab")
    time.sleep(0.3)
    check("held to talk while typing goes to Steam (it sees the key itself): the text waits", emitted(), [])
    key(RCTRL, 0)
    time.sleep(0.3)
    check("...until the release", emitted(), [("keyboard", c, v) for c, v in keys_for("ab")])
    dictation()
    to_relay("keyboard desktop")
    use({})

    # The desktop's own layout (kxkbrc): German QWERTZ, through libxkbcommon.
    rc = os.path.join(OUT, "kxkbrc")
    with open(rc, "w") as f:
        f.write("[Layout]\nLayoutList=de,us\nUse=true\n")
    relay.KXKBRC = rc
    try:
        de = relay.layout_keys("de")
    except OSError as e:
        de = None
        print(f"skip  German layout typing: {e}")
    if de:
        check("German layout: z is the US Y key, y the US Z key, @ is AltGr+Q, < the ISO key",
              (de["z"], de["y"], de["@"], de["<"], de["\u00e4"]),
              ((21, ()), (44, ()), (16, (100,)), (86, ()), (40, ())))
        to_relay("type way @ f\u00fcr")
        time.sleep(0.3)
        check("type on a German desktop: its keys, AltGr and umlauts included", typed(),
              [f"key {c} {v}" for c, v in keys_for("way @ f\u00fcr", de)])
        to_relay("type a\u00e9b")
        time.sleep(0.3)
        check("...a character it has no key for (a dead key's accent) is dropped", typed(),
              [f"key {c} {v}" for c, v in keys_for("ab", de)])
        check("...and the log says which", [m for m in LOG if m.startswith("typing: 1 characters")][-1:],
              ["typing: 1 characters the desktop's layout (de) has no key for: \u00e9"])
        to_relay("keyboard steam")
        to_relay("type way")
        time.sleep(0.3)
        check("...Steam and games still get a US layout", emitted(), [("keyboard", c, v) for c, v in keys_for("way")])
        to_relay("keyboard desktop")
    with open(rc, "w") as f:
        f.write("[Layout]\nLayoutList=de\nUse=false\n")
    to_relay("type way")
    time.sleep(0.3)
    check("layouts switched off in kxkbrc (Use=false): a US layout", typed(),
          [f"key {c} {v}" for c, v in keys_for("way")])
    relay.KXKBRC = os.path.join(OUT, "no-kxkbrc")
    to_relay("keyboard steam")

    check("an empty command isn't an action", relay.known_action("command:  "), False)
    check("steam_menu, pause_toggle, hands_toggle and commands work without pointer mode",
          (relay.needs_pointer("steam_menu"), relay.needs_pointer("pause_toggle"), relay.needs_pointer("hands_toggle"),
           relay.needs_pointer("command:ls")),
          (False, False, False, False))
    check("Meta+H toggles hand tracking by default", (relay.known_action("hands_toggle"),
                                                      relay.DEFAULT_KEY_BINDINGS.get("125+35")), (True, "hands_toggle"))

    # A text field on the desktop got focus (ft-textinput): Frametop's keyboard by the Keyboard
    # setting, and a log line saying why, once per decision (scripts/report.sh reads them).
    def text_field():
        c = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        c.sendto(b"textfield 1", f"\0{tag}_relay")
        time.sleep(0.1)
        return [m for m in typed() if m.startswith("vrkeyboard")]

    def said():
        got = [m for m in LOG if m.startswith("text field focused")]
        LOG.clear()
        return got

    typed(), said()
    check("text field, a keyboard connected (default setting): no keyboard", text_field(), [])
    check("...and the log says why", said(), [("text field focused: not opening Frametop's keyboard: a keyboard is "
                                               "connected (test keyboard), and the Keyboard setting opens it only "
                                               "without one")])
    check("the same again: not logged twice", (text_field(), said()), ([], []))
    vr_keyboard["mode"] = "always"
    use(None)
    check("text field, setting always: it opens", text_field(), ["vrkeyboard show"])
    check("...logged", said(), ["text field focused: asking ft-screens to open Frametop's keyboard"])
    vr_keyboard["mode"] = "never"
    use(None)
    check("text field, setting never: no keyboard, logged",
          (text_field(), said()), ([], ["text field focused: not opening Frametop's keyboard (Keyboard setting: never)"]))
    vr_keyboard["mode"] = None
    use(None)

    # The gaze calibration panel (the helper's "calpanel 1"): Enter takes its dot ("calkey 1" to
    # the helper) and isn't typed, its press, repeats and release; with a modifier held, or once
    # the panel is gone or lapsed, Enter is typed again.
    ENTER, KPENTER = relay.KEY_ENTER, relay.KEY_KPENTER

    def cal_keys():
        got = []
        while True:
            try:
                m = helper.recv(256).decode()
            except socket.timeout:
                return got
            if m.startswith("calkey"):
                got.append(m)

    def panel(on):
        to_relay(f"calpanel {on}")
        time.sleep(0.1)

    use({})
    typed(), cal_keys()
    key(ENTER, 1); key(ENTER, 0)
    check("Enter without the panel: typed", (cal_keys(), typed()), ([], ["key 28 1", "key 28 0"]))
    panel(1)
    key(ENTER, 1); key(ENTER, 2); key(ENTER, 2); key(ENTER, 0)
    check("panel up: Enter takes the dot, once", cal_keys(), ["calkey 1"])
    check("...and its press, repeats and release aren't typed", typed(), [])
    key(KPENTER, 1); key(KPENTER, 0)
    check("panel up: keypad Enter too", (cal_keys(), typed()), (["calkey 1"], []))
    key(SHIFT, 1); key(ENTER, 1); key(ENTER, 0); key(SHIFT, 0)
    check("panel up, Shift held: Enter is typed", (cal_keys(), typed()),
          ([], ["key 42 1", "key 28 1", "key 28 0", "key 42 0"]))
    key(A, 1); key(A, 0)
    check("panel up: other keys are typed", (cal_keys(), typed()), ([], ["key 30 1", "key 30 0"]))
    key(ENTER, 1); panel(0); key(ENTER, 0)
    check("panel gone while Enter is held: it took the dot, and its release isn't typed",
          (cal_keys(), typed()), (["calkey 1"], []))
    key(ENTER, 1); key(ENTER, 0)
    check("panel gone: Enter is typed", (cal_keys(), typed()), ([], ["key 28 1", "key 28 0"]))
    panel(1)
    time.sleep(3.1)  # no renewal: the claim lapses after 3 s
    key(ENTER, 1); key(ENTER, 0)
    check("panel claim lapsed: Enter is typed", (cal_keys(), typed()), ([], ["key 28 1", "key 28 0"]))

    # Full Keyboard (keyboard/, --frametop): it says hello, hears where typing goes and whether
    # the screens show, and types on the desktop through the relay.
    use({})
    kb = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    kb.bind("")  # autobound, as Full Keyboard's
    kb.settimeout(0.05)

    def heard():
        """What Full Keyboard was sent since the last call."""
        got = []
        while True:
            try:
                got.append(kb.recv(256).decode())
            except socket.timeout:
                return got

    def from_kb(text, sock=kb):
        sock.sendto(text.encode(), f"\0{tag}_relay")
        time.sleep(0.1)

    typed(), emitted()
    from_kb("kbhello 0")
    check("Full Keyboard's hello: the word back (Steam, nothing shown)", heard(), ["kb 0 0"])
    check("...and ft-screens hears it's there", [m for m in typed() if m.startswith("fullkb")], ["fullkb 0"])
    check("...logged", [m for m in LOG if m.startswith("Full Keyboard")][-1:], ["Full Keyboard connected"])
    to_relay("keyboard desktop 1")
    time.sleep(0.1)
    check("typing goes to the desktop, the screens show: it hears at once", heard(), ["kb 1 1"])
    from_kb("kbhello 1")
    check("a hello always gets the word", heard(), ["kb 1 1"])
    check("...and ft-screens hears it's up", [m for m in typed() if m.startswith("fullkb")], ["fullkb 1"])
    from_kb("kbkey 30 1")
    from_kb("kbkey 30 0")
    check("its keys reach the desktop", (typed(), emitted()), (["key 30 1", "key 30 0"], []))
    LOG.clear()
    from_kb("kbtext a\u00e9b")
    from_kb(f"kbkey {ENTER} 1")
    from_kb(f"kbkey {ENTER} 0")
    check("its text through the desktop's layout, then its key, in order", typed(),
          [f"key {c} {v}" for c, v in keys_for("ab")] + ["key 28 1", "key 28 0"])
    check("...what the layout can't type isn't logged by name",
          [m for m in LOG if m.startswith("typing")], ["typing: 1 characters the desktop's layout (us) has no key for"])
    from_kb(f"kbkey {SHIFT} 1")
    to_relay("keyboard steam 1")
    time.sleep(0.1)
    check("typing goes to Steam: it hears", heard(), ["kb 0 1"])
    from_kb(f"kbkey {SHIFT} 0")
    from_kb("kbkey 30 1")
    from_kb("kbkey 30 0")
    check("a key held from before comes up on the desktop", typed(), [f"key {SHIFT} 1", f"key {SHIFT} 0"])
    check("...and new ones go to the virtual keyboard", emitted(), [("keyboard", 30, 1), ("keyboard", 30, 0)])
    other = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    other.bind("")
    to_relay("keyboard desktop 1")
    from_kb("kbkey 30 1", other)
    from_kb("kbtext x", other)
    check("keys and text from another address are ignored", (typed(), emitted()), ([], []))
    other.close()
    heard()
    to_relay("kbpanel show")
    check("ft-screens' kbpanel show: Full Keyboard opens", heard(), ["kb show"])
    to_relay("kbpanel hide")
    check("...and hide", heard(), ["kb hide"])
    pause("on")
    time.sleep(0.1)
    check("paused: typing to Steam, and not over the screens", heard(), ["kb 0 0"])
    pause("off")
    to_relay("keyboard desktop 1")
    from_kb("kbhello 1")
    heard()
    from_kb("kbkey 30 1")
    time.sleep(1.2)  # the relay's once-a-second check (reconcile_desktop_keys)
    check("its held key stays down on the desktop past a second", typed(), ["fullkb 1", "key 30 1"])
    time.sleep(2.2)  # no hello for 3 s
    check("it stops saying hello: its held key comes up", typed(), ["key 30 0"])
    check("...logged", [m for m in LOG if m.startswith("Full Keyboard")][-1:],
          ["Full Keyboard stopped (no word for 3 seconds)"])
    from_kb("kbkey 30 1")
    check("gone: its keys reach nothing until it says hello again", (typed(), emitted()), ([], []))
    kb.close()
    to_relay("keyboard steam")
    use(None)
    print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
    shutil.rmtree(OUT, ignore_errors=True)
    os._exit(1 if failures else 0)


threading.Thread(target=tests, daemon=True).start()
sys.argv = [relay_path, "--no-grab"]
relay.KXKBRC = os.path.join(OUT, "no-kxkbrc")  # a US desktop until the layout tests
relay.main()
