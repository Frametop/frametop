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
EV_KEY, EV_REL = 1, 2
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL = 0, 1, 6, 8
BUTTONS = {'left': 0x110, 'right': 0x111, 'middle': 0x112, 'back': 0x113, 'forward': 0x114}
# Numeric codes: a keyboard's (1-255), but not ones that put the headset to sleep, turn it
# off or cut its radios (logind and PowerDevil act on the relay's virtual keyboard).
CODE_MAX = 255
SYSTEM_KEYS = {113, 114, 115, 116, 120, 142, 143, 152, 205, 224, 225, 238, 244, 245, 246, 247}
TYPE_STEP = 0.004   # input-relay.py: a typed key's press to its release, and on to the next key
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
    keys, so evdev codes below 10 need their names. Power, sleep, radio and display keys are
    refused by number."""
    name = name.strip().lower()
    name = ALIASES.get(name, name.removeprefix('key_'))
    if name in KEYS:
        return KEYS[name]
    if name.isdigit() and int(name) in SYSTEM_KEYS:
        raise ValueError(f'key {name} is a power, sleep, radio or display key: the testbench leaves those alone')
    if name.isdigit() and 0 < int(name) <= CODE_MAX:
        return int(name)
    raise ValueError(f'unknown key {name!r}: a name such as enter, a, f5, ctrl, or an evdev code')


def combo(text):
    """"ctrl+shift+t" -> [29, 42, 20]: held in order, let go in reverse."""
    codes = [key_code(part) for part in text.split('+')]
    if len(codes) > 6 or len(set(codes)) != len(codes):
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

    def __init__(self, kind):
        self.kind = kind
        self.link = Datagrams()
        self.prefix = f'hostev {DEVICE_ID} {kind}'
        self.said = 0.0
        self.held = set()

    def hello(self):
        self.link.send(RELAY, f'hostdev {DEVICE_ID} {self.kind} {DEVICE_NAME}')
        self.said = time.monotonic()

    def __enter__(self):
        self.hello()
        return self

    def __exit__(self, *_):
        try:
            # Anything still down comes up (an error mid-drag): in pointer mode the relay passes
            # the mouse's buttons on to the helper and doesn't let go of them when it drops us.
            if self.held:
                self.events(*[(EV_KEY, code, 0) for code in sorted(self.held, reverse=True)])
            # The relay reads a device's events after its control messages: let them through first.
            time.sleep(0.15)
            self.link.send(RELAY, f'hostbye {DEVICE_ID} {self.kind}')
        except (RuntimeError, OSError):
            pass   # the relay went away: it drops the device itself, and the first error stands
        finally:
            self.link.close()

    def events(self, *triples):
        """Events (type, code, value), then a report's end."""
        if time.monotonic() - self.said > 1:   # the relay's hello, under HOST_TIMEOUT
            self.hello()
        words = [str(v) for triple in triples for v in triple]
        self.link.send(RELAY, ' '.join([self.prefix, *words, '0 0 0']))
        for t, code, value in triples:
            if t == EV_KEY:
                (self.held.add if value else self.held.discard)(code)


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
    one for Steam); only printable characters. The relay types them a key at a time and doesn't
    answer, so this waits about as long as that takes (up to four steps a character, with a
    modifier): a key pressed next, such as enter, comes after the text."""
    if not text or len(text) > TEXT_MAX or not text.isprintable():
        raise ValueError(f'type 1 to {TEXT_MAX} printable characters (no newlines: press enter instead)')
    with Datagrams() as link:
        link.send(RELAY, 'type ' + text)
    time.sleep(len(text) * 4 * TYPE_STEP + 0.2)
    return {'ok': True, 'queued': len(text)}


def steps(dx, dy, most=40):
    """A move in mouse counts, cut into pieces a real mouse would send."""
    n = max(1, -(-max(abs(dx), abs(dy)) // most))
    done_x = done_y = 0
    for i in range(1, n + 1):
        x, y = round(dx * i / n), round(dy * i / n)
        yield x - done_x, y - done_y
        done_x, done_y = x, y


def mouse_counts(value):
    value = int(value)
    if abs(value) > 20000:
        raise ValueError('mouse moves are within 20000 counts')
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
            moved = [t for t in ((EV_REL, REL_X, x), (EV_REL, REL_Y, y)) if t[2]]
            if moved:
                mouse.events(*moved)
                time.sleep(0.008)
        if button:
            time.sleep(0.08)
            mouse.events((EV_KEY, BUTTONS[button], 0))
    return {'ok': True, 'moved': [dx, dy], 'held': button}


def mouse_click(button='left', count=1):
    """Click where the cursor is, up to a triple click."""
    count = int(count)
    if button not in BUTTONS:
        raise ValueError(f'button is one of {", ".join(BUTTONS)}')
    if not 1 <= count <= 3:
        raise ValueError('click 1 to 3 times')
    with Device('mouse') as mouse:
        for _ in range(count):
            mouse.events((EV_KEY, BUTTONS[button], 1))
            time.sleep(0.06)
            mouse.events((EV_KEY, BUTTONS[button], 0))
            time.sleep(0.1)
    return {'ok': True, 'clicked': button, 'count': count}


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


def ok_words(reply, name, least=0):
    """The words after a reply's "ok", at least `least` of them; anything else is the service's error."""
    words = reply.split()
    if not words or words[0] != 'ok':
        raise RuntimeError(f'@{name}: {reply.strip() or "empty reply"}')
    if len(words) - 1 < least:
        raise RuntimeError(f'@{name}: a short reply: {reply.strip()[:200]!r}')
    return words[1:]


def numbers(words, name, count):
    try:
        values = [float(w) for w in words]
    except ValueError:
        values = []
    if len(values) != count:
        raise RuntimeError(f'@{name}: expected {count} numbers, got {" ".join(words)[:200]!r}')
    return values


def pointer(command):
    """Show or hide the cursor, or bring it back in front. The helper doesn't answer: ok means sent."""
    if command not in POINTER_COMMANDS:
        raise ValueError(f'pointer command is one of {", ".join(POINTER_COMMANDS)}')
    with Datagrams() as link:
        link.send(POINTER, command)
    return {'ok': True, 'pointer': command}


def pointer_state():
    """What the pointer helper knows: the head (standing universe), gaze mode, worn or not."""
    with Datagrams() as link:
        gaze = ok_words(link.ask(POINTER, 'gaze ? headset'), POINTER, 1)
        try:
            x, y, z, yaw, pitch = numbers(ok_words(link.ask(POINTER, 'head'), POINTER), POINTER, 5)
            head = {'position': [x, y, z], 'yaw': yaw, 'pitch': pitch}
        except RuntimeError as error:
            head = {'error': str(error)}
    return {'ok': True, 'gaze_mode': gaze[0] == 'on', 'worn': gaze[1] == 'worn' if len(gaze) > 1 else None,
            'head': head}


def overlays():
    """SteamVR's overlays as the helper lists them: {"t": "overlays", "list": [{key, name, visible}]}."""
    with Datagrams(timeout=8) as link:   # the helper asks SteamVR afresh: seconds, at times
        reply = link.ask(POINTER, 'overlays')
    try:
        return {'ok': True, 'overlays': json.loads(reply)}
    except ValueError:
        raise RuntimeError(f'@{POINTER}: {reply.strip()[:200]}') from None


def measure(overlay):
    """A SteamVR overlay's centre, size and axes in the standing universe. The helper finds only
    an overlay in view of the head, so this needs a head pose."""
    if not overlay or len(overlay.encode()) > 127 or any(c.isspace() or not c.isprintable() for c in overlay):
        raise ValueError('an overlay key: up to 127 characters, no spaces')
    with Datagrams() as link:
        v = numbers(ok_words(link.ask(POINTER, 'measure ' + overlay), POINTER), POINTER, 14)
    return {'ok': True, 'overlay': overlay, 'center': v[0:3], 'width': v[3], 'height': v[4],
            'right': v[5:8], 'up': v[8:11], 'front': v[11:14]}


def power(command):
    """ft-powerd's status (displays on, off or away: SteamVR's standby), or the displays on or off."""
    if command not in POWER_COMMANDS:
        raise ValueError(f'power command is one of {", ".join(POWER_COMMANDS)}')
    with Datagrams() as link:
        words = ok_words(link.ask(POWERD, command), POWERD, 3 if command == 'status' else 0)
    if command != 'status':
        return {'ok': True, 'power': command}
    state, (idle, timeout) = words[0], numbers(words[1:3], POWERD, 2)
    return {'ok': True, 'displays': state, 'idle_seconds': idle, 'off_after_seconds': timeout or None}
