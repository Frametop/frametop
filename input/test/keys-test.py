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
import select
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
os.environ["FT_KWIN_EI"] = "0"  # the relay's wheel sender stays off the live desktop's KWin

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["FT_STEAM_PIN_LOCK"] = "0"   # Steam's PIN lock off, whatever this machine's setting
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
    game = False
    settings = {"sound": False}

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

    def set_pad(self, on):
        stub.setdefault("pad", []).append(on)

    game_state = tick = helper_started = controllers_changed = set_hands = dashboard_state = screens_seen = configure


stub = {}
relay.game_pause.GamePause = StubPause

for name in ("SCREENS", "HELPER", "FLOAT", "GAZED", "VOICE", "FRUNNER"):
    setattr(relay, name, f"\0{tag}_{name.lower()}")
OUT = tempfile.mkdtemp(prefix="ft-keys-test-")
# Native Desktop's focus is the test's, not the live gamescope's, and its layout a file of its own.
FOCUS = {"native": False}


class FakeFocus:
    def __init__(self, log):
        pass

    def native(self, now):
        return FOCUS["native"]


relay.GamescopeFocus = FakeFocus
relay.NATIVE_KXKBRC = os.path.join(OUT, "native-kxkbrc")
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

    reused = False  # made new, not from the fd store

    def emit(self, etype, code, value):
        EMITTED.append((self.name, code, value))

    def sync(self):
        pass


RealVirtual = relay.Virtual
relay.Virtual = NoDevice
relay.Volume.key = lambda self, fd, code, value, now: VOLUME.append((code, value))
LOG = []  # the relay's own log
relay.log = lambda *args: LOG.append(" ".join(map(str, args)))
bindings = {"now": None}  # the rules' key_bindings; None: the relay's defaults
devices = {"roles": {}, "buttons": {}, "controller": {}}  # the rules' roles, per-device "buttons", controller_buttons
vr_keyboard = {"mode": None, "steam": None}  # the rules' vr_keyboard and steam_fields_keyboard; None: the default
meta_tap = {"on": None}  # the rules' meta_tap (Meta taps do their binding); None: not in the file
remote_keys = {"now": None}  # the rules' remote_key_bindings (a PC's combinations); None: not in the file
MOUSE_ID = "usb:0003:0004:test mouse"  # the fake mouse's id (Node.id)


def read_rules(path=None):
    rules = {"devices": {i: {"role": r} for i, r in devices["roles"].items()},
             "buttons": {i: dict(b) for i, b in devices["buttons"].items()},
             "controller_buttons": dict(devices["controller"])}
    rules["key_bindings"] = dict(relay.DEFAULT_KEY_BINDINGS if bindings["now"] is None else bindings["now"])
    if vr_keyboard["mode"]:
        rules["vr_keyboard"] = vr_keyboard["mode"]
    if vr_keyboard["steam"]:
        rules["steam_fields_keyboard"] = vr_keyboard["steam"]
    if meta_tap["on"] is not None:
        rules["meta_tap"] = meta_tap["on"]
    if remote_keys["now"] is not None:
        rules["remote_key_bindings"] = dict(remote_keys["now"])
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

# A connected pair holds all 17 startup messages even when the kernel limits unconnected
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
    check("a relay that starts releases the modifiers and mouse buttons on the desktop, then every key there",
          typed(),
          [f"key {c} 0" for c in sorted(relay.MODIFIERS)] + [f"key {c} 0" for c in range(0x110, 0x118)] + ["keys release"])

    # a virtual device from the fd store lets go of every key it has (the kernel drops
    # the ones that aren't down), and so does the gamepad, its sticks centred.
    def written(fd):
        data = os.read(fd, 65536)
        return [EVENT.unpack_from(data, i)[2:] for i in range(0, len(data), EVENT.size)]
    r, w = os.pipe()
    v = RealVirtual.__new__(RealVirtual)
    v.fd, v.keys, v.dirty = w, range(1, 4), False
    v.release_all()
    check("a reused virtual device lets go of every key, then syncs", written(r),
          [(relay.EV_KEY, 1, 0), (relay.EV_KEY, 2, 0), (relay.EV_KEY, 3, 0), (0, 0, 0)])
    vrpad = relay.vrpad
    vrpad.VirtualPad(lambda m: None, {vrpad.STORE_NAME: w}, lambda *a: None)
    check("a reused gamepad lets go of its buttons and centres its sticks", written(r),
          [(vrpad.EV_KEY, c, 0) for c in vrpad.KEYS.values()] + [(vrpad.EV_ABS, a[0], 0) for a in vrpad.AXES]
          + [(0, 0, 0)])
    os.close(r)
    os.close(w)
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

    # A Meta tap fires on the release only, and only with nothing else going down or up meanwhile.
    menus = len(lines(steam_log))
    key(META, 1)
    check("Meta pressed and still held: no Steam menu yet", len(lines(steam_log)), menus)
    key(META, 0)
    check("...its release, with nothing in between: the Steam menu", len(lines(steam_log)), menus + 1)
    typed()
    key(A, 1); key(META, 1); key(A, 0); key(META, 0)
    check("a key down before Meta, let go while it's held: no tap", len(lines(steam_log)), menus + 1)
    key(relay.BTN_LEFT, 1, ms_w); key(META, 1); key(relay.BTN_LEFT, 0, ms_w); key(META, 0)
    check("a mouse button down before Meta, let go while it's held: no tap", len(lines(steam_log)), menus + 1)
    key(A, 1); key(META, 1); key(A, 2); key(META, 0); key(A, 0)
    check("a key held from before Meta, repeating while it's held: no tap", len(lines(steam_log)), menus + 1)
    key(relay.BTN_LEFT, 1, ms_w); key(META, 1); key(META, 0)
    check("a mouse button held right through Meta's hold (a drag): no tap", len(lines(steam_log)), menus + 1)
    key(relay.BTN_LEFT, 0, ms_w)
    key(A, 1); key(META, 1); key(META, 0)
    check("a key held right through Meta's hold, no repeats: no tap", len(lines(steam_log)), menus + 1)
    key(A, 0)
    typed()
    key(META, 1); to_relay("notap"); time.sleep(0.1); key(META, 0)
    check("a laser click while Meta's held (ft-screens' notap, Super+drag): no tap", len(lines(steam_log)), menus + 1)
    check("...and the desktop gets F24 before Meta's release (no Plasma launcher)", typed(), ["key 125 1"] + F24 + ["key 125 0"])
    key(SHIFT, 1); key(META, 1); to_relay("notap"); time.sleep(0.1); key(META, 0); key(SHIFT, 0)
    check("...not with Shift held there too (a terminal would type it)", typed(),
          [f"key {SHIFT} 1", "key 125 1", "key 125 0", f"key {SHIFT} 0"])
    for _ in range(3):
        key(META, 1); key(META, 0)
    check("three Meta taps in a row: three Steam menus (a cancelled hold doesn't stop the next)",
          len(lines(steam_log)), menus + 4)
    typed()

    # "meta_tap": false (Input Settings) turns the tap's binding off, either side; combinations stay.
    meta_tap["on"] = False
    use(None)
    key(META, 1); key(META, 0); key(RMETA, 1); key(RMETA, 0)
    check("meta_tap off: Meta taps open nothing", len(lines(steam_log)), menus + 4)
    check("meta_tap off: Meta reaches the desktop as an unbound one (no F24)", typed(),
          ["key 125 1", "key 125 0", "key 126 1", "key 126 0"])
    LOG.clear()
    key(META, 1); key(SHIFT, 1); key(35, 1); key(35, 0); key(SHIFT, 0); key(META, 0)  # H
    check("meta_tap off: Meta+Shift+H still toggles the screens",
          [m for m in LOG if m.startswith("key combination")], ["key combination 42+125+35: screens_toggle"])
    typed()
    meta_tap["on"] = True
    use(None)
    key(META, 1); key(META, 0)
    check("meta_tap back on: a Meta tap opens the Steam menu again", len(lines(steam_log)), menus + 5)
    typed()

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
    check("Meta tap bound to none: nothing", len(lines(steam_log)), menus + 5)
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
    # Your commands (run:NAME): a plain process here (FT_COMMANDS_NO_SYSTEMD), the list in OUT.
    relay.ft_commands.COMMANDS_PATH = f"{OUT}/commands.json"
    os.environ["FT_COMMANDS_NO_SYSTEMD"] = "1"
    relay.ft_commands.save([{"name": "note-it", "run": f"echo named >> {cmd_log}"}], aliases=False)
    use({"125+36": "gaze_left", "29": "run:note-it", "42+125+25": "pause_toggle"})
    key(RCTRL, 1); key(RCTRL, 0)
    for _ in range(40):
        if lines(cmd_log)[-1:] == ["named"]:
            break
        time.sleep(0.05)
    check("paused: your command (run:NAME) runs too, in the background", lines(cmd_log), ["combo", "4", "paused", "named"])
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

    # a click pressed while paused went to the virtual mouse, and comes up there after
    # the resume, not on the helper.
    pause("on")
    clicks()
    emitted()
    key(relay.BTN_LEFT, 1, ms_w)
    pause("off")
    key(relay.BTN_LEFT, 0, ms_w)
    check("a click held through the resume comes up on the virtual mouse, not the helper",
          (emitted(), clicks(), typed()), ([("mouse", relay.BTN_LEFT, 1), ("mouse", relay.BTN_LEFT, 0)], [], []))
    # POINTER=0 with a click held lets go of it on the helper, and its release goes to
    # the virtual mouse (which never had it down: the kernel drops it).
    key(relay.BTN_LEFT, 1, ms_w)
    check("pointer mode: the click reaches the helper", clicks(), ["btn trigger 1"])
    conf["POINTER"] = "0"
    use({})
    check("POINTER=0 with the left button held: it comes up on the helper, then the pointer hides", clicks(),
          ["btn trigger 0", "hide pause"])
    key(relay.BTN_LEFT, 0, ms_w)
    check("...and its release goes to the virtual mouse only", (clicks(), emitted()), ([], [("mouse", relay.BTN_LEFT, 0)]))
    conf["POINTER"] = "1"
    use({})

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

    # Keyboard + 3D pointer ("combo") with its keys on its mouse node (one node for both, as a
    # keyboard with a mouse-key layer can be): key combinations work there. A plain 3D pointer's
    # keys and a key its button map has are left as they were.
    devices["roles"] = {MOUSE_ID: "combo"}
    use(None)
    emitted()
    menus = len(lines(steam_log))
    key(META, 1, ms_w); key(META, 0, ms_w)
    check("combo, keys on its mouse node: a Meta tap opens Steam's menu", len(lines(steam_log)), menus + 1)
    check("...and Meta still reaches the desktop, F24 first, and the virtual keyboard", (typed(), emitted()),
          (["key 125 1"] + F24 + ["key 125 0"], [("keyboard", META, 1), ("keyboard", META, 0)]))
    key(META, 1, ms_w); key(J, 1, ms_w); key(J, 0, ms_w); key(META, 0, ms_w)
    check("combo: Meta+J isn't typed, and Meta comes up on the desktop and the virtual keyboard",
          (typed(), emitted()), (["key 125 1"] + F24 + ["key 125 0"], [("keyboard", META, 1), ("keyboard", META, 0)]))
    conf["POINTER"] = "1"  # as a combo device is used: its mouse node drives the 3D pointer
    use(None)
    key(META, 1, ms_w); key(META, 0, ms_w)
    check("combo, pointer mode: a Meta tap opens Steam's menu", len(lines(steam_log)), menus + 2)
    check("...and Meta reaches the desktop and the virtual keyboard", (typed(), emitted()),
          (["key 125 1"] + F24 + ["key 125 0"], [("keyboard", META, 1), ("keyboard", META, 0)]))
    conf["POINTER"] = "0"
    key(META, 1, ms_w); key(J, 1, ms_w)
    devices["buttons"] = {MOUSE_ID: {str(J): "none"}}
    use(None)  # J mapped while Meta+J is held
    key(J, 0, ms_w); key(META, 0, ms_w)
    emitted()
    key(META, 1, ms_w); key(META, 0, ms_w)
    check("combo: a key mapped while held in a combination still lets the next Meta tap work",
          len(lines(steam_log)), menus + 3)
    typed(), emitted()
    devices["buttons"] = {MOUSE_ID: {str(META): "none"}}
    use(None)
    key(META, 1, ms_w); key(META, 0, ms_w)
    check("combo: a key its button map has keeps its map (none: no tap, nothing sent)",
          (len(lines(steam_log)), typed(), emitted()), (menus + 3, [], []))
    devices["roles"], devices["buttons"] = {}, {}
    use(None)
    emitted()
    key(META, 1, ms_w); key(META, 0, ms_w)
    check("a plain 3D pointer's Meta tap does nothing (it's typed)", (len(lines(steam_log)), emitted()),
          (menus + 3, [("keyboard", META, 1), ("keyboard", META, 0)]))
    typed()

    # "keys:CODES" taps a key combination where typing goes, from a controller button or a key
    # combination: all down in order, then up, on the press; the release sends nothing.
    D = 32
    check("keys: specs", [relay.keys_codes(a) for a in ("keys:125+32", "keys:", "keys:115", "keys:256",
                                                        "keys:1+2+3+4+5+6+7", "keys:a", "keys:１", "run:x", "keys:116")],
          [[125, 32], None, None, None, None, None, None, None, None])
    devices["controller"] = {"right/b": "keys:125+32"}
    use({"29+56+32": "keys:125+32", "29+56+33": "keys:115", "29+56+20": "keys:29+20"})
    to_relay("keyboard desktop")
    emitted()
    to_relay("vrbtn right/b 1")
    time.sleep(0.1)
    meta_d = ["key 125 1", "key 32 1", "key 32 0", "key 125 0"]
    check("keys: on a controller button: Meta+D tapped on the desktop", (typed(), emitted()), (meta_d, []))
    to_relay("vrbtn right/b 0")
    time.sleep(0.1)
    check("...and its release sends nothing more", (typed(), emitted()), ([], []))
    to_relay("keyboard steam")
    key(CTRL, 1); key(ALT, 1); key(D, 1); key(D, 0); key(ALT, 0); key(CTRL, 0)
    check("keys: on a key combination, typing going to Steam: tapped on the virtual keyboard", emitted(),
          [("keyboard", META, 1), ("keyboard", D, 1), ("keyboard", D, 0), ("keyboard", META, 0)])
    typed()
    to_relay("keyboard desktop")
    key(CTRL, 1); key(ALT, 1); key(D, 1); key(D, 0); key(ALT, 0); key(CTRL, 0)
    check("keys: on a key combination on the desktop: its held modifiers come up around the tap", typed(),
          ["key 29 1", "key 56 1", "key 29 0", "key 56 0"] + meta_d + ["key 29 1", "key 56 1", "key 56 0", "key 29 0"])
    T = 20
    key(CTRL, 1); key(ALT, 1); key(T, 1); key(T, 0); key(ALT, 0); key(CTRL, 0)
    check("...one the action has too stays down: Ctrl+T, Ctrl held throughout", typed(),
          ["key 29 1", "key 56 1", "key 56 0", "key 20 1", "key 20 0", "key 56 1", "key 56 0", "key 29 0"])
    key(CTRL, 1); key(ALT, 1); key(F, 1); key(F, 0); key(ALT, 0); key(CTRL, 0)
    check("a volume key in keys: makes it no action: the combination is typed", typed(),
          ["key 29 1", "key 56 1", "key 33 1", "key 33 0", "key 56 0", "key 29 0"])
    to_relay("keyboard steam")
    devices["controller"] = {}
    typed()

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
    check("carry_toggle goes to ft-screens", (relay.needs_pointer("carry_toggle"), relay.SCREENS_ACTIONS["carry_toggle"]),
          (False, b"carry toggle"))

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
    # os_menu (DEE-172; no default): frunner opens with Frametop's own actions listed, its OS menu.
    use({"42+125+57": "os_menu"})
    key(META, 1); key(SHIFT, 1); key(SPACE, 1); key(SPACE, 0); key(SHIFT, 0); key(META, 0)
    check("os_menu bound to Meta+Shift+Space: frunner toggles as the OS menu", frunner_got(), ["toggle keyboard actions"])
    use(None)
    check("os_menu works without pointer mode, has no default, and isn't paused-safe",
          (relay.known_action("os_menu"), relay.needs_pointer("os_menu"), "os_menu" in relay.DEFAULT_KEY_BINDINGS.values(),
           relay.works_paused("os_menu")), (True, False, False, False))
    check("every menu action is an action that works without pointer mode",
          [a for a in sys.modules["ft_input_actions"].MENU_ACTION_NAMES if not relay.known_action(a) or relay.needs_pointer(a)], [])

    # The dashboard tiling keys (defaults): Meta+Shift+Left goes to ft-screens as "tile left" only
    # while it says an anchored app's tile has the focus ("tilefocus 1", renewed every 2 s);
    # otherwise (a screen's or a remote display's tile too: those are dragged, "tilefocus 2"), and
    # once that lapses, it's the desktop's. Meta+Alt+Up/Down nudge any tile, a display's too. Meta+arrows alone move the app's tile too, unless the rules bind
    # them: on the desktop they'd quick-tile the window, which halves it. Meta+/ lists the key
    # combinations in frunner.
    LEFT, RIGHT, SLASH = 105, 106, 53
    to_relay("keyboard desktop")
    time.sleep(0.05)
    typed()

    def meta(*codes):
        for c in (META, *codes):
            key(c, 1)
        for c in reversed((META, *codes)):
            key(c, 0)
        return typed()

    got = meta(SHIFT, LEFT)
    check("Meta+Shift+Left, no tile focused: the desktop gets Left", ([m for m in got if m.startswith("tile ")],
                                                                      f"key {LEFT} 1" in got), ([], True))
    to_relay("tilefocus 1")
    time.sleep(0.05)
    got = meta(SHIFT, LEFT)
    check("Meta+Shift+Left, a tile focused: ft-screens moves it, the desktop doesn't get Left",
          ([m for m in got if m.startswith("tile ")], f"key {LEFT} 1" in got), (["tile left"], False))
    UP, DOWN = 103, 108
    got = [meta(c) for c in (LEFT, RIGHT, UP, DOWN)]
    check("Meta+arrows alone, a tile focused: ft-screens moves it, the desktop gets no arrow",
          ([m for g in got for m in g if m.startswith("tile ")],
           [c for c, g in zip((LEFT, RIGHT, UP, DOWN), got) if f"key {c} 1" in g]),
          (["tile left", "tile right", "tile up", "tile down"], []))
    check("Meta+Ctrl+Right: wider", [m for m in meta(CTRL, RIGHT) if m.startswith("tile ")], ["tile wider"])
    check("Meta+Alt+Up/Down: nudged", [m for c in (UP, DOWN) for m in meta(ALT, c) if m.startswith("tile ")],
          ["tile nudgeup", "tile nudgedown"])
    # A screen's or a remote display's tile ("tilefocus 2"): it's dragged, but nudged by the keys.
    to_relay("tilefocus 2")
    time.sleep(0.05)
    got = [meta(SHIFT, LEFT), meta(LEFT), meta(ALT, UP)]
    check("tilefocus 2: only the nudge is ft-screens', the moves the desktop's",
          ([m for g in got for m in g if m.startswith("tile ")], [f"key {LEFT} 1" in g for g in got[:2]]),
          (["tile nudgeup"], [True, True]))
    to_relay("tilefocus 0")
    time.sleep(0.05)
    check("tilefocus 0: Meta+Shift+Left is the desktop's again", [m for m in meta(SHIFT, LEFT) if m.startswith("tile ")], [])
    check("tilefocus 0: Meta+Alt+Up too", [m for m in meta(ALT, UP) if m.startswith("tile ")], [])
    lapse, relay.TILE_FOCUS_LAPSE = relay.TILE_FOCUS_LAPSE, 0.2
    to_relay("tilefocus 1")
    time.sleep(0.35)
    check("tilefocus lapses without renewal", [m for m in meta(SHIFT, LEFT) if m.startswith("tile ")], [])
    relay.TILE_FOCUS_LAPSE = lapse
    got = meta(LEFT)
    check("no tile focused: Meta+Left is the desktop's (KWin's snap, or a PC's)",
          ([m for m in got if m.startswith("tile ")], f"key {LEFT} 1" in got), ([], True))
    use({**relay.DEFAULT_KEY_BINDINGS, "125+105": "key"})
    to_relay("tilefocus 1")
    time.sleep(0.05)
    got = meta(LEFT)
    check("Meta+Left bound in the rules (pass through), a tile focused: the rules' own",
          ([m for m in got if m.startswith("tile ")], f"key {LEFT} 1" in got), ([], True))
    use(None)
    # A PC's app window's tile: its desktop has the keys (the sixth "keyboard" word), and the
    # tiling keys are still ft-screens'.
    to_relay("keyboard desktop 1 0 1 1")
    to_relay("tilefocus 1")
    time.sleep(0.05)
    got = [meta(SHIFT, LEFT), meta(LEFT)]
    check("a PC has the keys, its app's tile focused: Meta+Shift+Left and Meta+Left move the tile",
          ([m for g in got for m in g if m.startswith("tile ")], [f"key {LEFT} 1" in g for g in got]),
          (["tile left", "tile left"], [False, False]))
    to_relay("keyboard desktop 1 0 1 0")
    to_relay("tilefocus 0")
    time.sleep(0.05)
    meta(SLASH)
    check("Meta+/ (default): frunner lists the key combinations", frunner_got(), ["toggle keyboard keys"])
    check("the tiling actions and keys_list work without pointer mode",
          [a for a in (*relay.TILE_ACTIONS, "keys_list") if not relay.known_action(a) or relay.needs_pointer(a)], [])

    # Meta+W (default) closes the app typing goes to: Alt+F4 tapped there, and W isn't typed.
    W_KEY, F4 = 17, 62
    to_relay("keyboard desktop")  # (ft-screens renews it; the checks above took a while)
    time.sleep(0.05)
    typed()
    got = meta(W_KEY)
    check("Meta+W (default): Alt+F4 tapped on the desktop, W not typed",
          ([m for m in got if m.split()[:2] in (["key", str(ALT)], ["key", str(F4)])], f"key {W_KEY} 1" in got),
          ([f"key {ALT} 1", f"key {F4} 1", f"key {F4} 0", f"key {ALT} 0"], False))
    check("close_app works without pointer mode, and not while paused",
          (relay.known_action("close_app"), relay.needs_pointer("close_app"), relay.works_paused("close_app")),
          (True, False, False))

    # A PC's desktop has the keys (ft-screens' sixth "keyboard" word: 1 the 3D mouse on it, 2 on
    # no panel since and asleep): Meta+Space and a Meta tap are the PC's, typed through, not
    # frunner's or Steam's menu. 0 (the 3D mouse moved off it) gives them back.
    SPACE = 57
    menus = len(lines(steam_log))
    for word in ("1", "2"):
        to_relay(f"keyboard desktop 1 0 1 {word}")
        time.sleep(0.05)
        got = meta(SPACE)
        check(f"PC has the keys ({word}): Meta+Space is typed there, frunner hears nothing",
              (frunner_got(), got), ([], ["key 125 1", f"key {SPACE} 1", f"key {SPACE} 0", "key 125 0"]))
        check(f"PC has the keys ({word}): a Meta tap is the PC's, no F24, no Steam menu",
              (meta(), len(lines(steam_log))), (["key 125 1", "key 125 0"], menus))
    # Pointer mode on (the Frame's usual): "2" goes by the 3D mouse being asleep, not by pointer mode.
    conf["POINTER"] = "1"
    use(None)
    to_relay("keyboard desktop 1 0 1 2")
    time.sleep(0.05)
    got = meta(SPACE)
    check("pointer mode, the 3D mouse asleep (2): Meta+Space is the PC's",
          (frunner_got(), got), ([], ["key 125 1", f"key {SPACE} 1", f"key {SPACE} 0", "key 125 0"]))
    RELAYS[0].lease.current.active, RELAYS[0].lease.current.last_used = True, time.monotonic()
    meta(SPACE)
    check("pointer mode, the 3D mouse awake off the PC (2): Meta+Space is frunner's", frunner_got(), ["toggle keyboard"])
    RELAYS[0].lease.current.active = False
    conf["POINTER"] = "0"
    use(None)
    # The way out of stuck keys still works with Meta held while the PC has them (hazards.md).
    to_relay("keyboard desktop 1 0 1 1")
    time.sleep(0.05)
    typed()
    key(META, 1); key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(1, 1); key(1, 0)
    key(SHIFT, 0); key(ALT, 0); key(CTRL, 0); key(META, 0)
    check("PC has the keys: Ctrl+Alt+Shift+Esc with Meta held still lets every key go",
          [m for m in typed() if m == "keys release"], ["keys release"])
    # Which combinations are the PC's is the rules' "remote_key_bindings" (ft_remote_keys). A file
    # without it gets Frametop's default Meta combinations but the gaze clicks (above: Meta+Space
    # and a Meta tap), so Meta+J still clicks where you look on a remote display.
    check("PC has the keys: Meta+J is still Frametop's gaze click, J isn't typed there",
          meta(J), ["key 125 1"] + F24 + ["key 125 0"])

    def pc_table(table, key_bindings=None):
        remote_keys["now"] = table
        use(key_bindings)
        to_relay("keyboard desktop 1 0 1 1")
        time.sleep(0.05)
        frunner_got()
        typed()

    # A table of its own: only what it names is the PC's. One sent as another combination is
    # tapped there on the press, with the modifiers the PC holds up meanwhile.
    pc_table({"42+125+57": "125+57"})
    got = meta(SPACE)
    check("own table without Meta+Space: it's frunner's on the PC too",
          (frunner_got(), f"key {SPACE} 1" in got), (["toggle keyboard"], False))
    check("Meta+Shift+Space sent as Meta+Space: the PC gets Meta+Space alone, Shift back after",
          meta(SHIFT, SPACE),
          ["key 125 1", f"key {SHIFT} 1", "key 125 0", f"key {SHIFT} 0", "key 125 1", f"key {SPACE} 1",
           f"key {SPACE} 0", "key 125 0", f"key {SHIFT} 1", f"key {SHIFT} 0"])
    check("it isn't frunner's, and Meta+J stays the gaze click", (frunner_got(), meta(J)),
          ([], ["key 125 1"] + F24 + ["key 125 0"]))
    to_relay("keyboard desktop 1 0 1 1")  # (ft-screens renews it; the checks above took a while)
    menus = len(lines(steam_log))
    check("own table without the Meta tap: it's the Steam menu on the PC too",
          (meta(), len(lines(steam_log))), (["key 125 1"] + F24 + ["key 125 0"], menus + 1))
    to_relay("keyboard desktop 1 0 1 0")
    time.sleep(0.05)
    check("off the PC: Meta+Shift+Space is typed as it is (no binding of Frametop's)",
          meta(SHIFT, SPACE), ["key 125 1", f"key {SHIFT} 1", f"key {SPACE} 1", f"key {SPACE} 0",
                               f"key {SHIFT} 0", "key 125 0"])
    # A tap sent as another combination: the modifier is down on the PC, so an F24 either side.
    menus = len(lines(steam_log))
    pc_table({"125": "29+57"})
    check("a Meta tap sent as Ctrl+Space: tapped there, no Meta tap of the PC's, no Steam menu",
          (meta(), len(lines(steam_log))),
          (["key 125 1"] + F24 + ["key 125 0", f"key {CTRL} 1", f"key {SPACE} 1", f"key {SPACE} 0",
                                  f"key {CTRL} 0", "key 125 1"] + F24 + ["key 125 0"], menus))
    # Entries the relay can't use are left out; the way out of stuck keys is never the PC's.
    pc_table({"125+57": "115", "57+125": "125+57", "29+42+56+1": "29+42+56+1"})
    meta(SPACE)
    check("a sent side with a volume key, and a pressed side out of order: not entries", frunner_got(), ["toggle keyboard"])
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(1, 1); key(1, 0); key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    check("release_keys in the table: it still lets every key go, Esc isn't typed there",
          [m for m in typed() if m in ("keys release", "key 1 1")], ["keys release"])
    # No key combinations of Frametop's at all: the PC's table still works.
    remote_keys["now"] = {"42+125+57": "125+57"}
    use({})
    to_relay("keyboard desktop 1 0 1 1")
    time.sleep(0.05)
    typed()
    check("no key_bindings: Meta+Shift+Space still sends the PC Meta+Space, Shift up meanwhile",
          meta(SHIFT, SPACE),
          ["key 125 1", f"key {SHIFT} 1", "key 125 0", f"key {SHIFT} 0", "key 125 1", f"key {SPACE} 1",
           f"key {SPACE} 0", "key 125 0", f"key {SHIFT} 1", f"key {SHIFT} 0"])
    # A combination of the user's own that the PC gets as it is: held there as long as it's held.
    pc_table({}, {"29+125+30": "frunner"})
    got = meta(CTRL, A)
    check("Ctrl+Meta+A bound to frunner, not listed: frunner's on the PC, A isn't typed there",
          (frunner_got(), f"key {A} 1" in got), (["toggle keyboard"], False))
    pc_table({"29+125+30": "29+125+30"}, {"29+125+30": "frunner"})
    key(META, 1); key(CTRL, 1); key(A, 1); key(A, 2)
    check("...listed as itself: down on the PC while held (its repeats are the PC's own), no frunner",
          (typed(), frunner_got()), (["key 125 1", f"key {CTRL} 1", f"key {A} 1"], []))
    key(A, 0); key(CTRL, 0); key(META, 0)
    check("...and up when let go", (typed(), frunner_got()), ([f"key {A} 0", f"key {CTRL} 0", "key 125 0"], []))
    # An entry comes before a tile's keys (the rules name it), which are ft-screens' otherwise.
    pc_table({"42+125+105": "42+125+105"})
    to_relay("tilefocus 1")
    time.sleep(0.05)
    got = [meta(SHIFT, LEFT), meta(LEFT)]
    check("a tile focused on the PC: the listed Meta+Shift+Left is the PC's, Meta+Left still moves the tile",
          ([m for g in got for m in g if m.startswith("tile ")], [f"key {LEFT} 1" in g for g in got]),
          (["tile left"], [True, False]))
    to_relay("tilefocus 0")
    # Paused, typing isn't the desktop's (a "keyboard desktop" doesn't count then), so no PC has
    # the keys and the table rests: nothing is tapped for a combination, it's typed as it is.
    pc_table({"42+125+57": "125+57"}, {"125+57": "frunner"})
    pause("on")
    to_relay("keyboard desktop 1 0 1 1")
    time.sleep(0.05)
    typed()
    check("paused: Meta+Shift+Space is typed as it is, nothing sent in its place",
          (meta(SHIFT, SPACE), frunner_got()),
          (["key 125 1", f"key {SHIFT} 1", f"key {SPACE} 1", f"key {SPACE} 0", f"key {SHIFT} 0", "key 125 0"], []))
    pause("off")
    # Hold to talk on a modifier: the PC's key if the table lists its tap, Frametop's otherwise.
    pc_table({"125": "125"}, {"125": "dictate_hold"})
    dictation()
    check("Meta bound to dictate_hold, its tap listed for the PC: no dictation, the PC's Meta tap",
          (meta(), dictation()), (["key 125 1", "key 125 0"], []))
    pc_table({}, {"125": "dictate_hold"})
    key(META, 1)
    check("...not listed: dictation starts on the press on the PC too", dictation(), ["start keyboard"])
    key(META, 0)
    check("...and stops on the release", dictation(), ["stop keyboard"])
    # A right-hand modifier's tap: its own entry comes before the folded one, and so does its own
    # binding of Frametop's (an entry for a Ctrl tap leaves Right Ctrl's dictation).
    pc_table({"97": "97"}, {"29": "dictate"})
    key(RCTRL, 1); key(RCTRL, 0)
    check("Right Ctrl's own entry: its tap is the PC's, though a Ctrl tap is dictation",
          (dictation(), typed()), ([], [f"key {RCTRL} 1", f"key {RCTRL} 0"]))
    key(CTRL, 1); key(CTRL, 0)
    check("...and Left Ctrl's still is dictation", dictation(), ["toggle keyboard"])
    pc_table({"29": "29"}, {"97": "dictate"})
    key(RCTRL, 1); key(RCTRL, 0)
    check("a Ctrl tap listed, Right Ctrl bound on its own: Right Ctrl is still dictation", dictation(), ["toggle keyboard"])
    pc_table({"29": "29"}, {"29": "dictate"})
    key(RCTRL, 1); key(RCTRL, 0); key(CTRL, 1); key(CTRL, 0)
    check("a Ctrl tap listed, bound by its left code: either side's tap is the PC's",
          (dictation(), typed()), ([], [f"key {RCTRL} 1", f"key {RCTRL} 0", f"key {CTRL} 1", f"key {CTRL} 0"]))
    remote_keys["now"] = None
    use(None)
    to_relay("keyboard desktop 1 0 1 0")
    time.sleep(0.05)
    meta(SPACE)
    check("the 3D mouse off the PC: Meta+Space opens frunner again", frunner_got(), ["toggle keyboard"])
    to_relay("keyboard steam 1 0 1 1")
    time.sleep(0.05)
    meta(SPACE)
    check("typing to Steam: the word means nothing, Meta+Space is frunner's", frunner_got(), ["toggle keyboard"])
    to_relay("keyboard desktop")
    time.sleep(0.05)
    typed()
    to_relay("keyboard steam")
    time.sleep(0.05)
    typed()

    # Meta+Shift+H (a default): ft-screens hides or shows the screens. Hidden, they give typing to
    # Steam and the keyboards aren't grabbed, but the relay still reads them, so a second press
    # brings the screens back; a repeat isn't one.
    H = 35
    logged = len(LOG)
    to_relay("keyboard desktop")
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 2); key(H, 0); key(SHIFT, 0); key(META, 0)
    got = typed()
    check("Meta+Shift+H (default): the screens toggle once", [m for m in got if m == "toggle"], ["toggle"])
    check("...H isn't typed", [m for m in got if m.startswith(f"key {H} ")], [])
    to_relay("keyboard steam")
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 0); key(SHIFT, 0); key(META, 0)
    check("Meta+Shift+H again, typing going to Steam: they toggle back", [m for m in typed() if m == "toggle"],
          ["toggle"])
    check("...and the relay did nothing else", [m for m in LOG[logged:] if m.startswith("key combination")],
          ["key combination 42+125+35: screens_toggle"] * 2)
    # Meta+Shift held, H twice (DEE-325, DEE-326): the desktop gets Meta and Shift, then Meta's
    # release, with no F24 (Shift held already keeps Meta's release from opening the launcher,
    # and a terminal typed Shift+F24), and nothing more for the second H: no Meta down again.
    typed()
    to_relay("keyboard desktop")
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 0)
    check("Meta+Shift+H: Meta up on the desktop, no F24", typed(),
          ["key 125 1", "key 42 1", "key 125 0", "toggle"])
    to_relay("keyboard steam")
    key(H, 1); key(H, 0)
    check("...H again, Meta+Shift still held: the toggle alone", typed(), ["toggle"])
    key(H, 1); key(H, 0)
    check("...and a third time", typed(), ["toggle"])
    key(SHIFT, 0); key(META, 0)
    check("...letting go: Shift comes up, Meta's real release is dropped", typed(), ["key 42 0"])
    to_relay("keyboard desktop")
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 0); key(SHIFT, 0)
    key(30, 1); key(30, 0); key(META, 0)  # A, no combination
    check("Meta+Shift+H, Shift let go, then A: Meta back first, and Meta+A typed", typed(),
          ["key 125 1", "key 42 1", "key 125 0", "toggle", "key 42 0", "key 125 1", "key 30 1", "key 30 0",
           "key 125 0"])
    pause("on")
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 0); key(SHIFT, 0); key(META, 0)
    check("paused: Meta+Shift+H doesn't toggle", [m for m in typed() if m == "toggle"], [])
    pause("off")
    typed()
    check("screens_toggle works without pointer mode", relay.needs_pointer("screens_toggle"), False)
    use({"42+125+33": "float_toggle"})
    key(META, 1); key(SHIFT, 1); key(H, 1); key(H, 0); key(SHIFT, 0); key(META, 0)
    check("a key_bindings list of its own without it: Meta+Shift+H is typed",
          [m for m in typed() if m == "toggle" or m.startswith(f"key {H} ")], [f"key {H} 1", f"key {H} 0"])
    use(None)

    # Ctrl+Alt+Shift+Esc (a default): ft-screens lets go of every key, for a modifier stuck in KWin.
    # The relay reads it with the screens hidden too, with more modifiers held (a stuck
    # one), and paused; Esc isn't typed, and nothing else happens.
    ESC = 1
    to_relay("keyboard desktop")
    logged = len(LOG)
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 2); key(ESC, 0)
    key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    got = typed()
    check("Ctrl+Alt+Shift+Esc (default): ft-screens releases every key, once",
          [m for m in got if m == "keys release"], ["keys release"])
    check("...Esc isn't typed", [m for m in got if m.startswith(f"key {ESC} ")], [])
    check("...the modifiers still are, down and up",
          [m for m in got if m.startswith(("key 29 ", "key 56 ", "key 42 "))],
          ["key 29 1", "key 56 1", "key 42 1", "key 42 0", "key 56 0", "key 29 0"])
    check("...the relay logs it", [m for m in LOG[logged:] if "release" in m],
          ["release_keys: every key let go", "key combination 29+42+56+1: release_keys"])
    key(META, 1); key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0)
    key(SHIFT, 0); key(ALT, 0); key(CTRL, 0); key(META, 0)
    got = typed()
    check("with Meta held as well: it still fires", [m for m in got if m == "keys release"], ["keys release"])
    check("...and Esc still isn't typed", [m for m in got if m.startswith(f"key {ESC} ")], [])
    to_relay("keyboard steam")
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0); key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    check("typing going to Steam (the screens hidden): it fires", [m for m in typed() if m == "keys release"],
          ["keys release"])
    pause("on")
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0); key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    check("paused: it fires", [m for m in typed() if m == "keys release"], ["keys release"])
    pause("off")
    typed()
    to_relay("keyboard desktop")
    key(CTRL, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0); key(SHIFT, 0); key(CTRL, 0)
    got = typed()
    check("Ctrl+Shift+Esc (fewer modifiers) is typed, not a release",
          ([m for m in got if m == "keys release"], [m for m in got if m.startswith(f"key {ESC} ")]),
          ([], [f"key {ESC} 1", f"key {ESC} 0"]))
    check("release_keys: no pointer mode needed, works paused, a known action",
          (relay.needs_pointer("release_keys"), relay.works_paused("release_keys"), relay.known_action("release_keys")),
          (False, True, True))
    use({"42+125+33": "float_toggle"})
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0); key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    check("a key_bindings list of its own without it: Ctrl+Alt+Shift+Esc is typed",
          [m for m in typed() if m == "keys release" or m.startswith(f"key {ESC} ")], [f"key {ESC} 1", f"key {ESC} 0"])
    use({"29+x+1": "release_keys"})  # a hand-edited combination with a part that isn't a key code
    key(CTRL, 1); key(ESC, 1); key(ESC, 0); key(CTRL, 0)
    check("a combination with a part that isn't a key code never matches: Ctrl+Esc is typed",
          [m for m in typed() if m == "keys release" or m.startswith(f"key {ESC} ")], [f"key {ESC} 1", f"key {ESC} 0"])
    use(None)
    key(META, 1); key(SPACE, 1); key(META, 0)
    HELD[kb_r].discard(SPACE)  # Space gone with its keyboard, its release never read
    frunner_got(); typed()
    menus = len(lines(steam_log))
    key(META, 1); key(META, 0)
    check("a combination left down keeps Meta taps from working", len(lines(steam_log)), menus)
    key(CTRL, 1); key(ALT, 1); key(SHIFT, 1); key(ESC, 1); key(ESC, 0); key(SHIFT, 0); key(ALT, 0); key(CTRL, 0)
    key(META, 1); key(META, 0)
    check("...until Ctrl+Alt+Shift+Esc forgets it", len(lines(steam_log)), menus + 1)
    typed()
    use({"29+56+22": "release_keys"})  # Ctrl+Alt+U, one of the user's own
    key(CTRL, 1); key(ALT, 1); key(META, 1); key(22, 1); key(22, 0); key(META, 0); key(ALT, 0); key(CTRL, 0)
    check("a binding of the user's own: it fires with Meta held as well",
          [m for m in typed() if m == "keys release" or m.startswith("key 22 ")], ["keys release"])
    use(None)
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
        check("...an accent its dead key types: the dead key (\u00b4), then the letter", typed(),
              ["key 30 1", "key 30 0", "key 13 1", "key 13 0", "key 18 1", "key 18 0", "key 48 1", "key 48 0"])
        to_relay("type a\u0436b")
        time.sleep(0.3)
        check("...a character it has no key for is dropped", typed(),
              [f"key {c} {v}" for c, v in keys_for("ab", de)])
        check("...and the log says which", [m for m in LOG if m.startswith("typing: 1 characters")][-1:],
              ["typing: 1 characters the desktop's layout (de), with no text field taking text from the input "
               "method, has no key for: \u0436"])
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
    check("run:NAME: an action that needs a name, no pointer mode, and works paused",
          (relay.known_action("run:  "), relay.known_action("run:x"), relay.needs_pointer("run:x"), relay.works_paused("run:x")),
          (False, True, False, True))
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

    # A character the desktop's layout (US here) has no key for goes to ft-textinput, the input
    # method, as "commit CHARACTER" while a text field is focused, between the keys around it.
    to_relay("keyboard desktop")
    im = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    im.bind("")  # autobound, as ft-textinput's
    im.sendto(b"textfield 1", f"\0{tag}_relay")
    time.sleep(0.1)
    typed()
    to_relay("type a\u0105b")
    got, end = [], time.monotonic() + 0.5
    while time.monotonic() < end:  # both ways, in the order they arrive
        for sock in select.select([screens, im], [], [], 0.05)[0]:
            data, addr = sock.recvfrom(256)
            got.append(data.decode())
            if sock is im:
                check("...commit comes from the relay's own name", addr, f"\0{tag}_relay".encode())
    check("type a\u0105b on a US desktop: a and b as keys, \u0105 committed between them",
          [m for m in got if not m.startswith("vrkeyboard")],
          [f"key {c} {v}" for c, v in keys_for("a")] + ["commit \u0105"] + [f"key {c} {v}" for c, v in keys_for("b")])
    to_relay("keyboard steam")
    to_relay("type \u0105")
    time.sleep(0.3)
    im.settimeout(0.1)
    try:
        sent = im.recv(256)
    except socket.timeout:
        sent = None
    check("...typing going to Steam: dropped, not committed", (sent, emitted()), (None, []))
    to_relay("keyboard desktop")
    im.sendto(b"textfield 0", f"\0{tag}_relay")
    time.sleep(0.1)
    LOG.clear()
    to_relay("type \u0119")
    time.sleep(0.3)
    try:
        sent = im.recv(256)
    except socket.timeout:
        sent = None
    check("no text field focused: dropped, not committed", (sent, typed()), (None, []))
    check("...and the log says why", [m for m in LOG if m.startswith("typing: ")],
          ["typing: 1 characters the desktop's layout (us), with no text field taking text from the input "
           "method, has no key for: \u0119"])

    def commits(socks, wait=0.3):
        """What each socket got as "commit ...", after `wait`."""
        time.sleep(wait)
        got = []
        for sock in socks:
            sock.setblocking(False)
            mine = []
            while True:
                try:
                    mine.append(sock.recv(256).decode())
                except BlockingIOError:
                    break
            got.append(mine)
        return got

    # A relay that restarted knows no text field: ft-textinput's renewal finds it again, without a
    # focus change, and doesn't open the keyboard again. (After "textfield 0" the relay is as a
    # fresh one.)
    vr_keyboard["mode"] = "always"
    use(None)
    typed()
    im.sendto(b"textfield 1 renew", f"\0{tag}_relay")
    time.sleep(0.1)
    check("renewal: no keyboard opened", [m for m in typed() if m.startswith("vrkeyboard")], [])
    to_relay("type \u0119")
    check("...but its address is known again: \u0119 committed", commits([im]), [["commit \u0119"]])
    # A crashed ft-textinput, started again by KWin, renews from a new address: the relay follows it
    # and stops sending to the old one.
    im2 = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    im2.bind("")
    im2.sendto(b"textfield 1 renew", f"\0{tag}_relay")
    time.sleep(0.1)
    to_relay("type \u0142")
    check("a restarted ft-textinput's new address: \u0142 goes there, not to the old one",
          commits([im, im2]), [[], ["commit \u0142"]])
    im2.sendto(b"textfield 0 renew", f"\0{tag}_relay")
    im2.sendto(b"textfield 1 again", f"\0{tag}_relay")
    time.sleep(0.1)
    check("...other textfield words: ignored", [m for m in typed() if m.startswith("vrkeyboard")], [])
    im2.sendto(b"textfield 0", f"\0{tag}_relay")
    time.sleep(0.1)
    vr_keyboard["mode"] = None
    use(None)
    typed()
    im2.close()
    im.close()

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

    # Full Keyboard (keyboard/host/ft-kbhost): it says hello, hears where typing goes, whether
    # the screens show and whether Steam's fields may open it, and types on the desktop through
    # the relay.
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

    # No Full Keyboard running (quit from its panel): Open/close keyboard starts it, and it opens.
    typed(), emitted()
    LOG.clear()
    to_relay("action keyboard_toggle")
    to_relay("action keyboard_toggle")
    time.sleep(0.1)
    check("keyboard_toggle with no Full Keyboard: its unit is started, once for two quick presses",
          (len(KB_STARTS), [m for m in typed() if m.startswith("vrkeyboard")]), (1, []))
    check("...logged", [m for m in LOG if m.startswith("keyboard_toggle")],
          ["keyboard_toggle: Full Keyboard isn't running (quit from its panel?): starting it"])
    from_kb("kbhello 0")
    check("...and its hello opens it, after ft-screens heard it's there",
          [m for m in typed() if m.startswith(("fullkb", "vrkeyboard"))], ["fullkb 0", "vrkeyboard show"])
    from_kb("kbhello 0")
    check("...once", [m for m in typed() if m.startswith("vrkeyboard")], [])
    to_relay("action keyboard_toggle")
    time.sleep(0.1)
    check("keyboard_toggle with Full Keyboard there: ft-screens' toggle, no start",
          (len(KB_STARTS), [m for m in typed() if m.startswith("vrkeyboard")]), (1, ["vrkeyboard toggle"]))
    heard()
    from_kb("kbhello 0")
    check("Full Keyboard's hello: the word back (Steam, nothing shown)", heard(), ["kb 0 0 0"])
    check("...and ft-screens hears it's there", [m for m in typed() if m.startswith("fullkb")], ["fullkb 0"])
    check("...logged", [m for m in LOG if m.startswith("Full Keyboard")][-1:], ["Full Keyboard connected"])
    to_relay("keyboard desktop 1")
    time.sleep(0.1)
    check("typing goes to the desktop, the screens show: it hears at once", heard(), ["kb 1 1 0"])
    from_kb("kbhello 1")
    check("a hello always gets the word", heard(), ["kb 1 1 0"])
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
          [m for m in LOG if m.startswith("typing")], ["typing: 1 characters the desktop's layout (us), with no text field taking text from the input method, "
           "has no key for"])
    from_kb(f"kbkey {SHIFT} 1")
    to_relay("keyboard steam 1")
    time.sleep(0.1)
    check("typing goes to Steam: it hears", heard(), ["kb 0 1 0"])
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
    # Its key right after a committed character waits the switch gap: the two go by different
    # ways, so Enter mustn't overtake the text it ends.
    im = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    im.bind("")
    im.sendto(b"textfield 1", f"\0{tag}_relay")
    time.sleep(0.1)
    typed()
    for text in ("kbtext \u0105", f"kbkey {ENTER} 1", f"kbkey {ENTER} 0"):
        kb.sendto(text.encode(), f"\0{tag}_relay")
    got, end = [], time.monotonic() + 0.5
    while time.monotonic() < end:
        for sock in select.select([screens, im], [], [], 0.05)[0]:
            got.append((sock.recv(256).decode(), time.monotonic()))
    got = [(m, t) for m, t in got if not m.startswith(("vrkeyboard", "fullkb"))]
    check("Full Keyboard: \u0105 committed, then Enter as keys",
          [m for m, _ in got], ["commit \u0105", "key 28 1", "key 28 0"])
    check("...Enter the switch gap after the commit",
          len(got) == 3 and got[1][1] - got[0][1] >= relay.TYPE_SWITCH * 0.8, True)
    im.sendto(b"textfield 0", f"\0{tag}_relay")
    im.close()
    heard()
    to_relay("kbpanel show")
    check("ft-screens' kbpanel show: Full Keyboard opens", heard(), ["kb show"])
    to_relay("kbpanel hide")
    check("...and hide", heard(), ["kb hide"])
    pose = " ".join(["1.0000", "0.0000", "0.0000", "0.1000"] + ["0.0000", "1.0000", "0.0000", "1.6000"]
                    + ["0.0000", "0.0000", "1.0000", "-0.2000"])
    to_relay(f"kbpanel show head {pose}")
    to_relay(f"kbpanel show dash {pose}")
    check("...and where: the head's pose, or the dashboard's spot", heard(),
          [f"kb show head {pose}", f"kb show dash {pose}"])
    to_relay("kbpanel show head 1 2 3")
    to_relay(f"kbpanel show knee {pose}")
    to_relay("kbpanel show head " + " ".join(["1e9"] * 12))
    to_relay("kbpanel toggle")
    check("...malformed ones don't go on", heard(), [])
    typed()
    from_kb("kbsteam")
    check("a Steam field, a keyboard connected (default setting): kb nosteam, nothing opens",
          (heard(), [m for m in typed() if m.startswith("vrkeyboard")]), (["kb nosteam"], []))
    keyboards = relay.Relay.physical_keyboards
    relay.Relay.physical_keyboards = lambda self: []
    from_kb("kbhello 0")
    check("...with no keyboard connected: the word says Steam's fields may take it",
          [m.split()[-1] for m in heard()[-1:]], ["1"])
    from_kb("kbsteam")
    check("...and a Steam field opens it", [m for m in typed() if m.startswith("vrkeyboard")], ["vrkeyboard show steam"])
    relay.Relay.physical_keyboards = keyboards
    from_kb("kbhello 0")
    heard()
    vr_keyboard["mode"] = "always"
    use({})
    from_kb("kbhello 0")
    check("the rule always: the word says Steam's fields may take it", heard()[-1:], ["kb 1 1 1"])
    from_kb("kbsteam")
    check("...and a Steam field has ft-screens open it, where it says",
          (heard(), [m for m in typed() if m.startswith("vrkeyboard")]), ([], ["vrkeyboard show steam"]))
    other = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    other.bind("")
    from_kb("kbsteam", other)
    check("...only from the address that said hello", [m for m in typed() if m.startswith("vrkeyboard")], [])
    other.close()
    pin_allowed = relay.ft_pinlock.allowed
    relay.ft_pinlock.allowed = lambda: False
    from_kb("kbsteam")
    check("...but not while Steam's PIN lock holds: kb nosteam",
          (heard(), [m for m in typed() if m.startswith("vrkeyboard")]), (["kb nosteam"], []))
    relay.ft_pinlock.allowed = pin_allowed
    screens_up = relay.Relay.vr_keyboard
    relay.Relay.vr_keyboard = lambda self, command: False
    from_kb("kbsteam")
    check("...and without ft-screens it opens where it is: kb show", heard(), ["kb show"])
    relay.Relay.vr_keyboard = screens_up
    vr_keyboard["steam"] = "steam"  # issue 102: Steam's own keyboard in Steam's fields
    use({})
    from_kb("kbhello 0")
    check("Steam's fields set to Steam's keyboard: the word says they don't take it", heard()[-1:], ["kb 1 1 0"])
    from_kb("kbsteam")
    check("...and a Steam field gets kb nosteam, nothing opens",
          (heard()[-1:], [m for m in typed() if m.startswith("vrkeyboard")]), (["kb nosteam"], []))
    vr_keyboard["steam"] = None
    use({})
    from_kb("kbhello 0")
    heard()
    vr_keyboard["mode"] = "never"
    use({})
    from_kb("kbsteam")
    check("the rule never: kb nosteam", heard()[-1:], ["kb nosteam"])
    vr_keyboard["mode"] = None
    use({})
    to_relay("keyboard desktop 1")   # ft-screens' word lasts 3 s: say it again for what follows
    time.sleep(0.1)
    heard(), typed()
    to_relay("kbpoke right press 0.1200 1.0500 -0.6500")
    to_relay("kbpoke left end")
    check("ft-screens' kbpoke: a fingertip's poke goes on to Full Keyboard", heard(),
          ["kb poke right press 0.1200 1.0500 -0.6500", "kb poke left end"])
    to_relay("kbpoke right press 1e9 0 0")
    to_relay("kbpoke middle hover 0 0 0")
    to_relay("kbpoke right type 0 0 0")
    check("...malformed ones don't", heard(), [])
    pause("on")
    time.sleep(0.1)
    check("paused: typing to Steam, and not over the screens", heard(), ["kb 0 0 0"])
    pause("off")
    # Native Desktop (SteamOS's own, in gamescope), focused, with a French layout: Full Keyboard's
    # typing comes through the relay and goes on the virtual keyboard, as that layout's keys.
    with open(relay.NATIVE_KXKBRC, "w") as f:
        f.write("[Layout]\nLayoutList=fr\nUse=true\n")
    try:
        fr = relay.layout_keys("fr")
    except OSError as e:
        fr = None
        print(f"skip  Native Desktop typing: {e}")
    if fr:
        to_relay("keyboard steam 1")
        time.sleep(0.1)
        heard()
        FOCUS["native"] = True
        from_kb("kbhello 1")
        check("Native Desktop focused: its typing goes through the relay", heard()[-1:], ["kb 1 1 0"])
        typed(), emitted()
        from_kb("kbtext aéê")
        from_kb(f"kbkey {ENTER} 1")
        from_kb(f"kbkey {ENTER} 0")
        time.sleep(0.3)
        check("...AZERTY keys (a on Q's key, é the 2 key, ê ^ then e), then its key, on the virtual keyboard",
              (emitted(), [m for m in typed() if not m.startswith("fullkb")]),
              ([("keyboard", c, v) for c in (16, 3, 26, 18, ENTER) for v in (1, 0)], []))
        to_relay("type ça")
        time.sleep(0.3)
        check("...dictation too", emitted(), [("keyboard", c, v) for c in (10, 16) for v in (1, 0)])
        # Its shortcuts name their letter (Ctrl+KeyA is Ctrl+A): AZERTY's A and Z keys. Z let go
        # after Ctrl comes up where it went down; a key with no shortcut modifier keeps its place.
        for text in ("kbkey 29 1", "kbkey 30 1", "kbkey 30 0", "kbkey 44 1", "kbkey 29 0", "kbkey 44 0",
                     "kbkey 30 1", "kbkey 30 0"):
            from_kb(text)
        time.sleep(0.2)
        check("...Ctrl+A and Ctrl+Z as AZERTY's A and Z keys", emitted(),
              [("keyboard", c, v) for c, v in ((29, 1), (16, 1), (16, 0), (17, 1), (29, 0), (17, 0), (30, 1), (30, 0))])
        from_kb(f"kbkey {SHIFT} 1")
        FOCUS["native"] = False
        from_kb("kbhello 1")
        check("focus elsewhere: Steam again", heard()[-1:], ["kb 0 1 0"])
        from_kb(f"kbkey {SHIFT} 0")
        time.sleep(0.1)
        check("...a key held from before comes up on the virtual keyboard", emitted(),
              [("keyboard", SHIFT, 1), ("keyboard", SHIFT, 0)])
        typed()
        # The Frametop desktop, its layout just made French and no text typed since: Ctrl+A goes
        # as AZERTY's A key, and one still held when typing moves to Steam comes up there.
        relay.KXKBRC = os.path.join(OUT, "kxkbrc-fr")
        with open(relay.KXKBRC, "w") as f:
            f.write("[Layout]\nLayoutList=fr\nUse=true\n")
        to_relay("keyboard desktop 1")
        from_kb("kbhello 1")
        heard(), typed()
        for text in ("kbkey 29 1", "kbkey 30 1", "kbkey 30 0", "kbkey 29 0", "kbkey 29 1", "kbkey 30 1"):
            from_kb(text)
        to_relay("keyboard steam 1")
        time.sleep(0.1)
        from_kb("kbkey 30 0")
        from_kb("kbkey 29 0")
        check("Frametop desktop, French, no text since: Ctrl+A as AZERTY's A key, let go there after a move to Steam",
              ([m for m in typed() if m.startswith("key")], emitted()),
              ([f"key {c} {v}" for c, v in ((29, 1), (16, 1), (16, 0), (29, 0), (29, 1), (16, 1), (16, 0), (29, 0))], []))
        relay.KXKBRC = os.path.join(OUT, "no-kxkbrc")
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

    # Gamepad mode (vrpad.py): ft-screens says "gamepad desktop|remote" while SteamVR's Gamepad mode
    # is Frametop's (the gamepad pill), and the controller reader's pad goes to that kind of panel.
    pads = []

    class FakePad:
        def __init__(self, *args):
            pads.append([])

        def send(self, pad_state):
            pads[-1].append(pad_state)

    relay.vrpad.VirtualPad = FakePad

    def reader():
        """What the controller reader was told since the last look (set_pad)."""
        time.sleep(0.05)
        got = stub.get("pad", [])
        stub["pad"] = []
        return got

    def pad_log():
        return [m for m in LOG if m.startswith("gamepad mode")][-1:]

    reader()
    typed()
    to_relay("vrpad 1001 0 0 0 0 0 0")
    time.sleep(0.05)
    check("gamepad mode off: the reader's pad reaches nothing", (pads, typed()), ([], []))
    to_relay("gamepad desktop")
    check("ft-screens: on for a desktop panel, the reader follows the controllers", (reader(), pad_log()),
          ([True], ["gamepad mode on: the controllers are a gamepad for a Frametop desktop panel"]))
    to_relay("vrpad 1001 0 255 0 32767 0 -100")
    to_relay("vrpad zz 0 0 0 0 0 0")
    to_relay("vrpad 1 0 999 0 0 0 0")
    time.sleep(0.05)
    check("the reader's state goes to the virtual pad (buttons held from before it came on wait for "
          "their release), malformed ones don't", pads, [[(0, 0, 255, 0, 32767, 0, -100)]])
    to_relay("vrpad 1000 0 0 0 0 0 0")
    to_relay("vrpad 1001 0 0 0 0 0 0")
    time.sleep(0.05)
    check("...and once let go, a press goes through", pads[-1][-2:], [(0, 0, 0, 0, 0, 0, 0), (0x0001, 0, 0, 0, 0, 0, 0)])
    to_relay("vrdash 1")
    to_relay("vrpad 1000 0 0 0 0 0 0")
    time.sleep(0.05)
    check("the dashboard is open: the pad lets go and takes nothing", pads[-1][-1:], [relay.vrpad.NEUTRAL])
    to_relay("vrdash 0")
    to_relay("vrpad 1000 0 0 0 0 0 0")
    to_relay("vrpad 0 0 0 0 0 0 0")
    to_relay("vrpad 1000 0 0 0 0 0 0")
    time.sleep(0.05)
    check("...and as it closes, what's held waits for its release, then the pad goes on", pads[-1][-1:],
          [(0x1000, 0, 0, 0, 0, 0, 0)])
    typed()
    to_relay("gamepad remote")
    time.sleep(0.05)
    check("now for a remote display: the desktop's pad lets go", (pads[-1][-1:], pad_log()),
          ([relay.vrpad.NEUTRAL], ["gamepad mode: now for a remote display"]))
    to_relay("vrpad 0 0 0 0 0 0 0")
    to_relay("vrpad 2000 0 0 0 0 0 0")
    time.sleep(0.05)
    check("...and ft-screens gets the pad", typed()[-1:], ["pad 2000 0 0 0 0 0 0"])
    time.sleep(1.2)
    check("no word from the reader for a second: it lets go", typed(), ["pad off"])
    to_relay("gamepad off")
    check("SteamVR's Gamepad mode ended: off", (reader(), pad_log()),
          ([False], ["gamepad mode off (SteamVR's Gamepad mode ended)"]))
    # Issue 105: on SteamOS 0.4.5 the helper still calls the dashboard open after the pill closed it.
    to_relay("vrdash 1")
    to_relay("gamepad remote")
    to_relay("vrpad 0 0 0 0 0 0 0")
    to_relay("vrpad 2000 0 0 0 0 0 0")
    time.sleep(0.05)
    check("a dashboard called open before gamepad mode came on doesn't hold the pad", typed()[-1:],
          ["pad 2000 0 0 0 0 0 0"])
    to_relay("vrdash 0")
    to_relay("vrdash 1")
    to_relay("vrpad 2000 0 0 0 0 0 0")
    time.sleep(0.05)
    check("...one that opens during it does", typed(), ["pad off"])
    to_relay("gamepad off")
    to_relay("vrdash 0")
    time.sleep(0.05)
    typed(), reader()
    to_relay("gamepad desktop")
    reader()
    time.sleep(3.2)
    check("ft-screens quiet for 3 s: off", (reader(), pad_log()), ([False], ["gamepad mode off (no word from ft-screens for 3 s)"]))
    for why, end in (("Frametop paused", lambda: pause("on")), ("a VR game started", lambda: to_relay("vrgame 1"))):
        to_relay("gamepad desktop")
        check(f"on again ({why} next)", reader(), [True])
        end()
        time.sleep(0.05)
        check(f"{why}: it ends", (reader(), pad_log()), ([False], [f"gamepad mode off ({why})"]))
        pause("off")
    stub["pause"].game = True
    to_relay("gamepad desktop")
    check("a VR game runs: ft-screens' word doesn't start it", (reader(), pad_log()), ([], ["gamepad mode: not on (a VR game runs)"]))
    stub["pause"].game = False
    to_relay("keyboard steam")
    use(None)
    print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
    shutil.rmtree(OUT, ignore_errors=True)
    os._exit(1 if failures else 0)


RELAYS = []  # the relay main() makes, for the tests that set its state
KB_STARTS = []  # the times the relay started the keyboard's unit (kb_revive): never the real one here
_relay_init = relay.Relay.__init__


def _keep_relay(self, *a, **kw):
    RELAYS.append(self)
    _relay_init(self, *a, **kw)
    self.kb_revive = relay.kb_revive.KeyboardRevive(start=lambda: KB_STARTS.append(1))


relay.Relay.__init__ = _keep_relay
threading.Thread(target=tests, daemon=True).start()
sys.argv = [relay_path, "--no-grab"]
relay.KXKBRC = os.path.join(OUT, "no-kxkbrc")  # a US desktop until the layout tests
relay.main()
