"""Apple Magic Mouse 2 surface scrolling for the input relay.

SteamOS leaves CONFIG_HID_MAGICMOUSE off, so hid-generic reports only buttons
and relative X/Y. The top of the mouse is a trackpad: after feature report
F1 02 01, HID report 0x12 grows from 8 bytes to 14+8N with touch records.
Decoding and the wheel step follow drivers/hid/hid-magicmouse.c. Standard
library only; nothing opens a device at import. Users: input/input-relay.py.
"""
import errno
import fcntl
import os
import struct

from ft_evdev import ioc, ior

REL_WHEEL, REL_HWHEEL = 0x08, 0x06
MOUSE2_REPORT_ID = 0x12
TOUCH_STATE_MASK, TOUCH_STATE_START, TOUCH_STATE_DRAG = 0xF0, 0x30, 0x40
SCROLL_ACCEL_DEFAULT = 7
FEATURE_MT = bytes((0xF1, 0x02, 0x01))

APPLE_VENDORS = {0x004C, 0x05AC}
MAGICMOUSE2_PRODUCTS = {0x0269, 0x0323}

HIDIOCGRAWINFO = ior("H", 0x03, 8)
HIDIOCGRAWUNIQ = ior("H", 0x08, 256)
HIDIOCSFEATURE = ioc(3, 0x06, len(FEATURE_MT), "H")
DEVINFO = struct.Struct("Ihh")


def is_magic_mouse2(vendor, product):
    return vendor & 0xFFFF in APPLE_VENDORS and product & 0xFFFF in MAGICMOUSE2_PRODUCTS


def s12(value):
    value &= 0xFFF
    return value - 0x1000 if value & 0x800 else value


def cdiv(n, d):
    """C integer division: truncate toward zero."""
    return int(n / d)


def parse_touch(tdata):
    """One 8-byte Magic Mouse 2 touch: id, x, y, size, state (hid-magicmouse.c)."""
    ident = (tdata[6] << 2 | tdata[5] >> 6) & 0xF
    x = s12((tdata[1] & 0x0F) << 8 | tdata[0])
    y = -s12((tdata[2] << 4) | (tdata[1] >> 4))
    size = tdata[5] & 0x3F
    state = tdata[7] & TOUCH_STATE_MASK
    return ident, x, y, size, state


def pack_touch(ident, x, y, size, state):
    """The 8-byte touch record parse_touch reads back (tests)."""
    y_raw = -y
    x12, y12 = x & 0xFFF, y_raw & 0xFFF
    t0 = x12 & 0xFF
    t1 = ((x12 >> 8) & 0x0F) | ((y12 & 0x0F) << 4)
    t2 = (y12 >> 4) & 0xFF
    t5 = (size & 0x3F) | ((ident & 0x3) << 6)
    t6 = ((ident >> 2) & 0x3) | (32 << 2)
    return bytes((t0, t1, t2, 0, 0, t5, t6, state & 0xFF))


class ScrollEmulator:
    """One mouse: report bytes in, REL_WHEEL / REL_HWHEEL steps out."""

    def __init__(self, scroll_speed=32, scroll_acceleration=False):
        self.scroll_speed = scroll_speed
        self.scroll_acceleration = scroll_acceleration
        self.scroll_accel = SCROLL_ACCEL_DEFAULT
        self.scroll_jiffies = 0.0
        self.touches = {}

    def feed(self, data, now=0.0):
        if not data or data[0] != MOUSE2_REPORT_ID:
            return []
        size = len(data)
        if size != 8 and (size < 14 or (size - 14) % 8 != 0):
            return []
        npoints = 0 if size == 8 else (size - 14) // 8
        wheel = hwheel = 0
        seen = set()
        for i in range(npoints):
            ident, x, y, _size, state = parse_touch(data[14 + i * 8:22 + i * 8])
            seen.add(ident)
            dw, dh = self._touch(ident, x, y, state, now)
            wheel += dw
            hwheel += dh
        for ident in [k for k in self.touches if k not in seen]:
            del self.touches[ident]
        out = []
        if wheel:
            out.append((REL_WHEEL, wheel))
        if hwheel:
            out.append((REL_HWHEEL, hwheel))
        return out

    def _touch(self, ident, x, y, state, now):
        t = self.touches.get(ident)
        if t is None:
            t = self.touches[ident] = {"x": 0, "y": 0, "scroll_x": 0, "scroll_y": 0}
        t["x"], t["y"] = x, y
        if state == TOUCH_STATE_START:
            t["scroll_x"], t["scroll_y"] = x, y
            if self.scroll_acceleration and now - self.scroll_jiffies < 0.5:
                self.scroll_accel = max(self.scroll_accel - 1, 1)
            else:
                self.scroll_accel = SCROLL_ACCEL_DEFAULT
            return 0, 0
        if state != TOUCH_STATE_DRAG:
            return 0, 0
        step = (64 - self.scroll_speed) * self.scroll_accel
        step_x = cdiv(t["scroll_x"] - x, step)
        step_y = cdiv(t["scroll_y"] - y, step)
        if step_x:
            t["scroll_x"] -= step_x * step
            self.scroll_jiffies = now
        if step_y:
            t["scroll_y"] -= step_y * step
            self.scroll_jiffies = now
        return step_y, -step_x


class _Device:
    __slots__ = ("fd", "path", "uniq", "emu")

    def __init__(self, fd, path, uniq):
        self.fd, self.path, self.uniq = fd, path, uniq
        self.emu = ScrollEmulator()


class MagicMice:
    """hidraw nodes for Magic Mouse 2 devices the relay currently has."""

    def __init__(self, log=print):
        self.log = log
        self.by_fd = {}
        self.by_uniq = {}

    def fds(self):
        return list(self.by_fd)

    def sync(self, nodes):
        """Open hidraw for each Magic Mouse 2 mouse node, close the rest."""
        want = {n.uniq.lower() for n in nodes.values()
                if n.is_mouse and n.uniq and is_magic_mouse2(n.vendor, n.product)}
        for uniq in [u for u in self.by_uniq if u not in want]:
            self._close(self.by_uniq[uniq])
        for uniq in want:
            if uniq in self.by_uniq:
                continue
            found = _find_hidraw(uniq)
            if found is None:
                continue
            path, fd = found
            if not _enable_multitouch(fd):
                os.close(fd)
                self.log(f"Magic Mouse 2 ({uniq}): could not enable surface reports")
                continue
            dev = _Device(fd, path, uniq)
            self.by_fd[fd] = dev
            self.by_uniq[uniq] = dev
            self.log(f"Magic Mouse 2 ({uniq}): surface scrolling on {path}")

    def forget(self, node):
        if node.is_mouse and node.uniq:
            dev = self.by_uniq.get(node.uniq.lower())
            if dev is not None:
                self._close(dev)

    def read(self, fd, nodes):
        """Consume one hidraw read. Returns (mouse node or None, wheel motions)."""
        dev = self.by_fd.get(fd)
        if dev is None:
            return None, []
        try:
            data = os.read(fd, 256)
        except OSError as e:
            if e.errno == errno.EAGAIN:
                return None, []
            self._close(dev)
            return None, []
        if not data:
            self._close(dev)
            return None, []
        motions = dev.emu.feed(data)
        node = next((n for n in nodes.values()
                     if n.is_mouse and n.uniq.lower() == dev.uniq), None)
        return node, motions

    def _close(self, dev):
        self.by_fd.pop(dev.fd, None)
        self.by_uniq.pop(dev.uniq, None)
        try:
            os.close(dev.fd)
        except OSError:
            pass


def _enable_multitouch(fd):
    buf = bytearray(FEATURE_MT)
    try:
        return fcntl.ioctl(fd, HIDIOCSFEATURE, buf) >= 0
    except OSError:
        return False


def _find_hidraw(uniq):
    want = uniq.lower()
    try:
        names = os.listdir("/dev")
    except OSError:
        return None
    for name in names:
        if not name.startswith("hidraw"):
            continue
        path = f"/dev/{name}"
        if not os.access(path, os.R_OK | os.W_OK):
            continue
        try:
            fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            info = bytearray(DEVINFO.size)
            fcntl.ioctl(fd, HIDIOCGRAWINFO, info)
            _bustype, vendor, product = DEVINFO.unpack(info)
            if not is_magic_mouse2(vendor, product):
                os.close(fd)
                continue
            raw = bytearray(256)
            fcntl.ioctl(fd, HIDIOCGRAWUNIQ, raw)
            got = bytes(raw).split(b"\0", 1)[0].decode(errors="replace").lower()
            if got == want:
                return path, fd
        except OSError:
            pass
        os.close(fd)
    return None
