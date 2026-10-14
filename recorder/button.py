"""The headset's button, and mice (/proc/bus/input/devices, linux/input.h).

Both recorders step with the button on the headset's right side: press = next or pause, two
presses = redo, hold = stop (ButtonGestures). mouse_connected() decides which hints to show."""
import os
import re
import select
import struct
import threading
import time


# The Frame's click button on its right side is KEY_SELECT on the "gpio-keys" device. It's read
# as any other reader does, never grabbed (EVIOCGRAB): Frametop's input relay, ft-powerd, SteamVR
# and gamescope read the same device, and the relay remaps its volume keys.

INPUT_DEVICES = "/proc/bus/input/devices"
INPUT_EVENT = struct.Struct("@llHHi")   # struct input_event: a timeval, type, code, value (24 bytes on 64-bit)
EV_KEY, EV_REL = 0x01, 0x02
REL_X, REL_Y = 0x00, 0x01
KEY_SELECT = 353
BUTTON_DEVICE = "gpio-keys"
BUTTON_DOUBLE_S = 0.45    # a second press this soon after the first one's release: a double press
BUTTON_HOLD_S = 1.5       # held down this long: a hold
BUTTON_BOUNCE_S = 0.03    # a release and press closer than this are one press (gpio-keys debounces too)


def parse_input_devices(text):
    """/proc/bus/input/devices as [{"name", "phys", "sysfs", "bus", "handlers": [...], "bits": {"EV": int, ...}}]."""
    out, dev = [], None
    for line in text.splitlines() + [""]:
        line = line.strip()
        if not line:
            if dev:
                out.append(dev)
            dev = None
            continue
        if dev is None:
            dev = {"name": "", "phys": "", "sysfs": "", "bus": 0, "handlers": [], "bits": {}}
        tag, _, rest = line.partition(": ")
        if tag == "I":
            m = re.search(r"Bus=([0-9a-fA-F]+)", rest)
            dev["bus"] = int(m.group(1), 16) if m else 0
        elif tag == "N":
            dev["name"] = rest.partition("=")[2].strip('"')
        elif tag == "P":
            dev["phys"] = rest.partition("=")[2]
        elif tag == "S":
            dev["sysfs"] = rest.partition("=")[2]
        elif tag == "H":
            dev["handlers"] = rest.partition("=")[2].split()
        elif tag == "B":
            key, _, words = rest.partition("=")
            value = 0
            for w in words.split():   # the highest long first
                try:
                    value = (value << 64) | int(w, 16)
                except ValueError:
                    value = 0
                    break
            dev["bits"][key] = value
    return out


def _event_node(dev):
    for h in dev["handlers"]:
        if re.fullmatch(r"event\d+", h):
            return "/dev/input/" + h
    return None


def find_button(devices):
    """The headset button's event device: "gpio-keys" with KEY_SELECT, or None."""
    for dev in devices:
        if dev["name"] == BUTTON_DEVICE and dev["bits"].get("KEY", 0) >> KEY_SELECT & 1:
            return _event_node(dev)
    return None


def real_mouse(dev):
    """A pointing device a person holds: relative X and Y, not made in software. uinput devices
    (Frametop's virtual mouse, frame-voice's keyboard) sit under /devices/virtual/input or on the
    virtual bus (6); Bluetooth mice come through uhid, also under /devices/virtual, so those count."""
    bits = dev["bits"]
    if not (bits.get("EV", 0) >> EV_REL & 1) or (bits.get("REL", 0) & 3) != 3:   # REL_X and REL_Y
        return False
    sysfs = dev["sysfs"]
    if dev["bus"] == 0x06 or "virtual" in dev["name"].lower() or "uinput" in dev["phys"]:
        return False
    return not sysfs.startswith("/devices/virtual/") or sysfs.startswith("/devices/virtual/misc/uhid/")


def read_input_devices(path=INPUT_DEVICES):
    try:
        with open(path) as f:
            return parse_input_devices(f.read())
    except OSError:
        return []


def mouse_connected(path=INPUT_DEVICES):
    return any(real_mouse(d) for d in read_input_devices(path))


def button_events(data):
    """KEY_SELECT downs and ups in a run of input_event structs: [(True for a down or False for
    an up, the event's time in seconds), ...] (autorepeat, value 2, is left out). Returns (them,
    the bytes left over after the last whole event)."""
    out, usable = [], len(data) - len(data) % INPUT_EVENT.size
    for off in range(0, usable, INPUT_EVENT.size):
        sec, usec, etype, code, value = INPUT_EVENT.unpack_from(data, off)
        if etype == EV_KEY and code == KEY_SELECT and value in (0, 1):
            out.append((value == 1, sec + usec / 1e6))
    return out, data[usable:]


class ButtonGestures:
    """The headset button's downs and ups, with their times, as gestures:
      "press": one short press, reported once the double-press time has passed without a second;
      "double": two presses, the second down within double_s of the first one's release, reported
        at the second down;
      "hold": held down for hold_s, reported while still down.
    A release and a press closer than bounce_s are one press. Each call returns the gestures that
    are due, in order; tick() is called now and then to report the ones that are due by time."""

    def __init__(self, double_s=BUTTON_DOUBLE_S, hold_s=BUTTON_HOLD_S, bounce_s=BUTTON_BOUNCE_S):
        self.double_s, self.hold_s, self.bounce_s = double_s, hold_s, bounce_s
        self.down_at = None    # when the button went down, while it's down
        self.done = False      # the press that's down was already reported (a double, a hold)
        self.released = None   # when a short press was let go, until it's reported or doubled
        self._up = None        # the last release, for the bounce: (time, down_at, done, released before it)

    def down(self, t):
        if self.down_at is not None:   # a missed release
            return []
        if self._up and t - self._up[0] < self.bounce_s:   # a bounce: still the same press
            _, self.down_at, self.done, self.released = self._up
            self._up = None
            return []
        out = self.tick(t)
        self.down_at, self.done = t, False
        if self.released is not None:   # within the double-press time (else tick reported it)
            self.released, self.done = None, True
            out.append("double")
        return out

    def up(self, t):
        if self.down_at is None:
            return []
        out = self.tick(t)
        self._up = (t, self.down_at, self.done, self.released)
        if not self.done:
            self.released = t
        self.down_at, self.done = None, False
        return out

    def tick(self, t):
        if self.down_at is not None and not self.done and t - self.down_at >= self.hold_s:
            self.done = True
            return ["hold"]
        if self.released is not None and t - self.released > self.double_s:
            self.released = None
            return ["press"]
        return []

    def waiting(self):
        """Something is due by time alone: a hold, or a press that may still become a double."""
        return (self.down_at is not None and not self.done) or self.released is not None


class ButtonReader:
    """Reads the headset button on a thread and calls on_gesture("press" | "double" | "hold")
    (ButtonGestures). path: an event device or, for testing, a FIFO carrying input_event
    structs. It's opened read-only and shared; if it can't be opened (no device, no permission,
    /dev/input not reachable in a container) it says so in the log and tries again now and then."""

    def __init__(self, path, on_gesture, log=None, double_s=BUTTON_DOUBLE_S, hold_s=BUTTON_HOLD_S,
                 bounce_s=BUTTON_BOUNCE_S):
        self.path, self.on_gesture, self.log = path, on_gesture, log or (lambda s: None)
        self.gestures = ButtonGestures(double_s, hold_s, bounce_s)
        self.ok = False      # opened at least once
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="rec-button", daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(2)

    def _report(self, gestures):
        for g in gestures:
            self.on_gesture(g)

    def _run(self):
        failed = False
        while not self._stop.is_set():
            try:
                fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError as e:
                if not failed:
                    self.log("headset button: can't open %s (%s): the window's keys still work" % (self.path, e.strerror))
                failed = True
                self._stop.wait(5)
                continue
            if failed or not self.ok:
                self.log("headset button: reading %s" % self.path)
            self.ok, failed = True, False
            rest = b""
            try:
                while not self._stop.is_set():
                    r, _, _ = select.select([fd], [], [], 0.02 if self.gestures.waiting() else 0.2)
                    if not r:
                        self._report(self.gestures.tick(time.monotonic()))
                        continue
                    try:
                        data = os.read(fd, INPUT_EVENT.size * 64)
                    except BlockingIOError:
                        continue
                    if not data:   # a FIFO's writer left: open it again
                        self._stop.wait(0.2)
                        break
                    events, rest = button_events(rest + data)
                    # The kernel's times (the realtime clock) keep the gaps between events that
                    # came in one read; the last one is taken as now.
                    now = time.monotonic()
                    for is_down, t in events:
                        at = now - min(max(events[-1][1] - t, 0.0), 2.0)
                        self._report(self.gestures.down(at) if is_down else self.gestures.up(at))
            except OSError as e:   # the device went away
                self.log("headset button: %s: %s" % (self.path, e.strerror))
                self._stop.wait(2)
            finally:
                os.close(fd)
