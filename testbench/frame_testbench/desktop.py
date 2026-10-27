"""Frametop's own control sockets, for driving the desktop the way its devices and tools do.

- A keyboard and a mouse plugged into the input relay (input/input-relay.py, "Host devices"):
  "hostdev testbench KIND NAME", then evdev events as "hostev testbench KIND T C V ...", then
  "hostbye". The relay treats them as one more keyboard or mouse, with the same roles, mapped
  buttons and key bindings: in pointer mode the mouse drives the 3D mouse, and keys go where
  typing goes. Text goes through the relay's "type", as Frametop Voice's does.
- The pointer helper (@ft_pointer_helper, pointer/helper/ft-pointer.cpp): show, hide, recenter,
  and what it knows: the head, gaze mode and whether the headset is worn, the overlays, a
  panel's place and size.
- ft-powerd (@ft_powerd, power/ft-powerd.cpp): status, and the displays on or off.

All are abstract datagram sockets: nothing listens on the network, and the relay takes keys,
clicks and text only from this user.
"""
import json
import socket
import time

RELAY, POINTER, POWERD = 'frametop_relay', 'ft_pointer_helper', 'ft_powerd'
DEVICE_ID = 'testbench'
DEVICE_NAME = 'Frame testbench'

# Linux input codes (include/uapi/linux/input-event-codes.h).
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL = 0, 1, 6, 8
BUTTONS = {'left': 0x110, 'right': 0x111, 'middle': 0x112, 'back': 0x113, 'forward': 0x114}
KEY_MAX = 0x2ff
KEYS = {'esc': 1, 'minus': 12, 'equal': 13, 'backspace': 14, 'tab': 15, 'leftbrace': 26,
        'rightbrace': 27, 'enter': 28, 'leftctrl': 29, 'semicolon': 39, 'apostrophe': 40, 'grave': 41,
        'leftshift': 42, 'backslash': 43, 'comma': 51, 'dot': 52, 'slash': 53, 'rightshift': 54,
        'leftalt': 56, 'space': 57, 'capslock': 58, 'f11': 87, 'f12': 88, 'rightctrl': 97,
        'rightalt': 100, 'home': 102, 'up': 103, 'pageup': 104, 'left': 105, 'right': 106, 'end': 107,
        'down': 108, 'pagedown': 109, 'insert': 110, 'delete': 111, 'mute': 113, 'volumedown': 114,
        'volumeup': 115, 'leftmeta': 125, 'rightmeta': 126, 'menu': 127}
KEYS.update({str(d): 2 + (d - 1) % 10 for d in range(10)})
for _row, _first in (('qwertyuiop', 16), ('asdfghjkl', 30), ('zxcvbnm', 44)):
    KEYS.update({c: _first + i for i, c in enumerate(_row)})
KEYS.update({f'f{n}': 58 + n for n in range(1, 11)})
ALIASES = {'ctrl': 'leftctrl', 'control': 'leftctrl', 'shift': 'leftshift', 'alt': 'leftalt',
           'altgr': 'rightalt', 'meta': 'leftmeta', 'super': 'leftmeta', 'win': 'leftmeta',
           'escape': 'esc', 'return': 'enter', 'del': 'delete', 'pgup': 'pageup', 'pgdn': 'pagedown',
           'period': 'dot', 'ins': 'insert'}
TEXT_MAX = 2000
POINTER_COMMANDS = ('show', 'hide', 'recenter')
POWER_COMMANDS = ('status', 'on', 'off')


def key_code(name):
    """A key's evdev code from its name (enter, a, f5, ctrl, ...) or number. 0-9 are the digit
    keys, so evdev codes below 10 need their names."""
    name = name.strip().lower()
    name = ALIASES.get(name, name.removeprefix('key_'))
    if name in KEYS:
        return KEYS[name]
    if name.isdigit() and 0 < int(name) <= KEY_MAX:
        return int(name)
    raise ValueError(f'unknown key {name!r}: a name such as enter, a, f5, ctrl, or an evdev code')


def combo(text):
    """"ctrl+shift+t" -> [29, 42, 20]: held in order, let go in reverse."""
    codes = [key_code(part) for part in text.split('+')]
    if not codes or len(codes) > 6 or len(set(codes)) != len(codes):
        raise ValueError(f'{text!r}: up to six different keys joined with +')
    return codes


class Datagrams:
    """One socket bound to an autobound abstract address, so the services can reply to it."""

    def __init__(self, timeout=2.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('')   # Linux picks an unused abstract name
        self.sock.settimeout(timeout)

    def send(self, name, message):
        try:
            self.sock.sendto(message.encode(), '\0' + name)
        except (ConnectionRefusedError, FileNotFoundError):
            raise RuntimeError(f"@{name} isn't listening: is its Frametop service running?") from None

    def ask(self, name, message):
        self.send(name, message)
        try:
            return self.sock.recv(65536).decode(errors='replace')
        except socket.timeout:
            raise RuntimeError(f'@{name} did not answer {message.split()[0]!r}') from None

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Device:
    """A keyboard or mouse plugged into the input relay for the length of a with block. The relay
    lets it go HOST_TIMEOUT (3 s) after its last word, its keys coming up, if this stops early."""

    def __init__(self, kind, link=None):
        self.kind = kind
        self.link = link or Datagrams()
        self.prefix = f'hostev {DEVICE_ID} {kind}'

    def __enter__(self):
        self.link.send(RELAY, f'hostdev {DEVICE_ID} {self.kind} {DEVICE_NAME}')
        self.said = time.monotonic()
        return self

    def __exit__(self, *_):
        # The relay reads a device's events after its control messages: let them through first.
        time.sleep(0.15)
        try:
            self.link.send(RELAY, f'hostbye {DEVICE_ID} {self.kind}')
        finally:
            self.link.close()

    def events(self, *triples):
        """Events (type, code, value), then a report's end."""
        if time.monotonic() - self.said > 1:   # the relay's hello, under HOST_TIMEOUT
            self.link.send(RELAY, f'hostdev {DEVICE_ID} {self.kind} {DEVICE_NAME}')
            self.said = time.monotonic()
        words = [str(v) for triple in triples for v in triple]
        self.link.send(RELAY, ' '.join([self.prefix, *words, '0 0 0']))


def press_keys(combos, gap=0.03):
    """Each combo pressed and let go, one after another."""
    parsed = [combo(c) for c in combos]
    if not parsed or len(parsed) > 100:
        raise ValueError('between 1 and 100 key combinations')
    with Device('keyboard') as kb:
        for codes in parsed:
            for code in codes:
                kb.events((EV_KEY, code, 1))
                time.sleep(gap)
            for code in reversed(codes):
                kb.events((EV_KEY, code, 0))
                time.sleep(gap)
    return {'ok': True, 'keys': combos}


def type_text(text):
    """Text typed where typing goes, as Frametop Voice types it (the desktop's layout, or a US
    one for Steam); only printable characters."""
    if not text or len(text) > TEXT_MAX or not text.isprintable():
        raise ValueError(f'type 1 to {TEXT_MAX} printable characters (no newlines: press enter instead)')
    with Datagrams() as link:
        link.send(RELAY, 'type ' + text)
    return {'ok': True, 'typed': len(text)}


def steps(dx, dy, most=40):
    """A move in mouse counts, cut into pieces a real mouse would send."""
    n = max(1, -(-max(abs(dx), abs(dy)) // most))
    done_x = done_y = 0
    for i in range(1, n + 1):
        x, y = round(dx * i / n), round(dy * i / n)
        yield x - done_x, y - done_y
        done_x, done_y = x, y


def mouse_counts(value, limit=20000):
    value = int(value)
    if abs(value) > limit:
        raise ValueError(f'mouse moves are within {limit} counts')
    return value


def mouse_move(dx, dy, button=None):
    """Move (mouse counts, +x right, +y down), with button held through it for a drag."""
    dx, dy = mouse_counts(dx), mouse_counts(dy)
    if button is not None and button not in BUTTONS:
        raise ValueError(f'button is one of {", ".join(BUTTONS)}')
    with Device('mouse') as mouse:
        if button:
            mouse.events((EV_KEY, BUTTONS[button], 1))
            time.sleep(0.08)
        for x, y in steps(dx, dy):
            mouse.events(*[t for t in ((EV_REL, REL_X, x), (EV_REL, REL_Y, y)) if t[2]])
            time.sleep(0.008)
        if button:
            time.sleep(0.08)
            mouse.events((EV_KEY, BUTTONS[button], 0))
    return {'ok': True, 'moved': [dx, dy], 'held': button}


def mouse_click(button='left', count=1):
    if button not in BUTTONS:
        raise ValueError(f'button is one of {", ".join(BUTTONS)}')
    if not 1 <= int(count) <= 3:
        raise ValueError('click 1 to 3 times')
    with Device('mouse') as mouse:
        for i in range(int(count)):
            mouse.events((EV_KEY, BUTTONS[button], 1))
            time.sleep(0.06)
            mouse.events((EV_KEY, BUTTONS[button], 0))
            time.sleep(0.1)
    return {'ok': True, 'clicked': button, 'count': int(count)}


def mouse_scroll(notches, horizontal=False):
    """Wheel notches: + up (or right with horizontal)."""
    notches = int(notches)
    if not notches or abs(notches) > 50:
        raise ValueError('scroll 1 to 50 notches either way')
    with Device('mouse') as mouse:
        for _ in range(abs(notches)):
            mouse.events((EV_REL, REL_HWHEEL if horizontal else REL_WHEEL, 1 if notches > 0 else -1))
            time.sleep(0.03)
    return {'ok': True, 'scrolled': notches, 'horizontal': bool(horizontal)}


def ok_words(reply, name):
    words = reply.split()
    if not words or words[0] != 'ok':
        raise RuntimeError(f'@{name}: {reply.strip() or "empty reply"}')
    return words[1:]


def pointer(command):
    if command not in POINTER_COMMANDS:
        raise ValueError(f'pointer command is one of {", ".join(POINTER_COMMANDS)}')
    with Datagrams() as link:
        link.send(POINTER, command)
    return {'ok': True, 'pointer': command}


def pointer_state():
    """What the pointer helper knows: the head (standing universe), gaze mode, worn or not."""
    with Datagrams() as link:
        gaze = ok_words(link.ask(POINTER, 'gaze ? headset'), POINTER)
        try:
            x, y, z, yaw, pitch = map(float, ok_words(link.ask(POINTER, 'head'), POINTER))
            head = {'position': [x, y, z], 'yaw': yaw, 'pitch': pitch}
        except RuntimeError as error:
            head = {'error': str(error)}
    return {'ok': True, 'gaze_mode': gaze[0] == 'on', 'worn': gaze[1] == 'worn' if len(gaze) > 1 else None,
            'head': head}


def overlays():
    with Datagrams() as link:
        reply = link.ask(POINTER, 'overlays')
    try:
        return {'ok': True, 'overlays': json.loads(reply)}
    except ValueError:
        raise RuntimeError(f'@ft_pointer_helper: {reply.strip()[:200]}') from None


def measure(overlay):
    """A SteamVR overlay's centre, size and axes in the standing universe."""
    if not overlay or len(overlay) > 127 or any(c.isspace() or not c.isprintable() for c in overlay):
        raise ValueError('an overlay key: up to 127 characters, no spaces')
    with Datagrams() as link:
        v = list(map(float, ok_words(link.ask(POINTER, 'measure ' + overlay), POINTER)))
    return {'ok': True, 'overlay': overlay, 'center': v[0:3], 'width': v[3], 'height': v[4],
            'right': v[5:8], 'up': v[8:11], 'front': v[11:14]}


def power(command):
    if command not in POWER_COMMANDS:
        raise ValueError(f'power command is one of {", ".join(POWER_COMMANDS)}')
    with Datagrams() as link:
        words = ok_words(link.ask(POWERD, command), POWERD)
    if command != 'status':
        return {'ok': True, 'power': command}
    state, idle, timeout = words[0], float(words[1]), float(words[2])
    return {'ok': True, 'displays': state, 'idle_seconds': idle, 'off_after_seconds': timeout or None}
