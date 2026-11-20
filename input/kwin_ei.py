"""The Frametop desktop's wheel through KWin's EIS server (issue 108: nothing scrolled in Zed).

KWin runs inside ft-screens on its nested Wayland backend, and that backend passes a wheel on as a
bare delta, with no discrete steps and no source (KWin 6.2.5 and 6.6.2: "TODO: Send discreteDelta
and source as well"). An app that reads a wheel only from its steps misses it. Zed takes every
scroll for a wheel until a source says otherwise, skips a wheel's bare delta, and so never scrolled.
KWin's EIS server (libei's server side) passes a discrete scroll on with its steps (value120), as a
mouse wheel's on a native session. So the input relay keeps a libei sender connected to the
desktop's KWin: `connectToEIS` on org.kde.KWin.EIS.RemoteDesktop, on the desktop's own bus, with
the pointer capabilities. KWin drops the connection when the D-Bus connection that asked for it
closes, so that stays open too.

While the sender has a scroll device, the relay tells ft-screens "kwinei 1" each second, and
ft-screens sends a KWin screen's wheel here ("kwinscroll DX DY", 120ths of a notch, positive right
and down) instead of through its seat. KWin puts it where its pointer is, which ft-screens' seat
has just moved. Without word for 3 s ft-screens scrolls through its seat again, so a desktop with
no EIS, or a host without libei, scrolls as before.

libei goes through ctypes (the host's libei.so.1, which KWin's own EIS server comes with), and
D-Bus through dbus-python, both on the host's system Python. `KwinEi.fileno()` goes in the relay's
select(); `dispatch()` when it's readable, `poll(now)` once a second.
"""

import ctypes
import ctypes.util
import time

RETRY_S = 10.0  # between attempts while there's no desktop or no EIS
NAME = b"Frametop wheel"
SERVICE, PATH, IFACE = "org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop", "org.kde.KWin.EIS.RemoteDesktop"
POINTER_PORTAL = 2  # connectToEIS's capabilities: pointer, absolute pointer, buttons and scroll

# libei.h
CAP_POINTER, CAP_SCROLL = 1 << 0, 1 << 4
(EV_CONNECT, EV_DISCONNECT, EV_SEAT_ADDED, EV_SEAT_REMOVED, EV_DEVICE_ADDED, EV_DEVICE_REMOVED,
 EV_DEVICE_PAUSED, EV_DEVICE_RESUMED) = range(1, 9)


def load_libei():
    """libei.so.1 with the calls used here typed, or None."""
    name = ctypes.util.find_library("ei") or "libei.so.1"
    try:
        lib = ctypes.CDLL(name)
    except OSError:
        return None
    p, u32, i32, u64 = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint64
    for fn, res, args in (
            ("ei_new_sender", p, [p]), ("ei_unref", p, [p]), ("ei_configure_name", None, [p, ctypes.c_char_p]),
            ("ei_setup_backend_fd", ctypes.c_int, [p, ctypes.c_int]), ("ei_get_fd", ctypes.c_int, [p]),
            ("ei_dispatch", None, [p]), ("ei_get_event", p, [p]), ("ei_now", u64, [p]),
            ("ei_event_get_type", ctypes.c_int, [p]), ("ei_event_get_seat", p, [p]),
            ("ei_event_get_device", p, [p]), ("ei_event_unref", p, [p]),
            ("ei_device_ref", p, [p]), ("ei_device_unref", p, [p]),
            ("ei_device_has_capability", ctypes.c_bool, [p, ctypes.c_int]),
            ("ei_device_start_emulating", None, [p, u32]), ("ei_device_frame", None, [p, u64]),
            ("ei_device_scroll_discrete", None, [p, i32, i32])):
        f = getattr(lib, fn)
        f.restype, f.argtypes = res, args
    # Variadic, NULL-terminated: its arguments are typed at the call.
    lib.ei_seat_bind_capabilities.restype = None
    return lib


def connect_to_eis(address):
    """(fd, cookie, the D-Bus connection to keep open) from the KWin on bus `address`.
    Raises on any failure (no dbus-python, no KWin there, no EIS in it)."""
    import dbus  # the host's system Python has it (float/ft_floatd.py uses it too)
    bus = dbus.bus.BusConnection(address)
    try:
        fd, cookie = bus.get_object(SERVICE, PATH, introspect=False).connectToEIS(
            dbus.Int32(POINTER_PORTAL), dbus_interface=IFACE, timeout=1.0)
        return fd.take(), int(cookie), bus
    except Exception:
        bus.close()
        raise


class KwinEi:
    """The relay's libei sender into the desktop's KWin. find_bus(): the desktop's D-Bus address,
    or None while it isn't running. lib and connect are for tests."""

    def __init__(self, log, find_bus, lib=load_libei, connect=connect_to_eis):
        self.log, self.find_bus, self.connect = log, find_bus, connect
        self.lib = lib() if callable(lib) else lib
        self.ei = self.bus = self.device = None
        self.fd = -1
        self.emulating = False
        self.sequence = 0
        self.retry_at = 0.0
        self.said = None  # the last failure logged, so a missing desktop isn't logged every 10 s
        if self.lib is None:
            self.log("desktop wheel: no libei here, scrolling through ft-screens' seat")

    def fileno(self):
        return self.fd

    def ready(self):
        """A scroll device KWin has resumed: the wheel can go this way."""
        return self.emulating

    def poll(self, now):
        """Once a second: connect when there's nothing and it's time to try again."""
        if self.lib is None or self.ei is not None or now < self.retry_at:
            return
        self.retry_at = now + RETRY_S
        address = self.find_bus()
        if not address:
            return self.fail("the desktop isn't running")
        try:
            fd, _cookie, bus = self.connect(address)
        except Exception as e:
            return self.fail(f"KWin's EIS isn't reachable ({e.__class__.__name__}: {e})")
        ei = self.lib.ei_new_sender(None)
        if not ei:
            bus.close()
            return self.fail("libei couldn't make a sender")
        self.lib.ei_configure_name(ei, NAME)
        if self.lib.ei_setup_backend_fd(ei, fd) != 0:
            self.lib.ei_unref(ei)
            bus.close()
            return self.fail("libei didn't take KWin's EIS socket")
        self.ei, self.bus, self.fd = ei, bus, self.lib.ei_get_fd(ei)
        self.said = None
        self.log("desktop wheel: connected to KWin's EIS")

    def fail(self, why):
        if why != self.said:
            self.log(f"desktop wheel: {why}; scrolling through ft-screens' seat")
            self.said = why

    def dispatch(self):
        """The EIS socket is readable: take KWin's events."""
        if self.ei is None:
            return
        lib = self.lib
        lib.ei_dispatch(self.ei)
        while self.ei is not None:
            ev = lib.ei_get_event(self.ei)
            if not ev:
                break
            kind = lib.ei_event_get_type(ev)
            if kind == EV_SEAT_ADDED:
                # Untyped (variadic): a bare int would go as 32 bits, so the seat goes as a pointer.
                lib.ei_seat_bind_capabilities(ctypes.c_void_p(lib.ei_event_get_seat(ev)), ctypes.c_int(CAP_POINTER),
                                              ctypes.c_int(CAP_SCROLL), ctypes.c_void_p(None))
            elif kind == EV_DEVICE_ADDED:
                dev = lib.ei_event_get_device(ev)
                if self.device is None and lib.ei_device_has_capability(dev, CAP_SCROLL):
                    self.device = lib.ei_device_ref(dev)
            elif kind == EV_DEVICE_RESUMED and self.device and lib.ei_event_get_device(ev) == self.device:
                self.sequence += 1
                lib.ei_device_start_emulating(self.device, self.sequence)
                self.emulating = True
                self.log("desktop wheel: KWin's EIS has the scroll device: the wheel goes that way")
            elif kind == EV_DEVICE_PAUSED and self.device and lib.ei_event_get_device(ev) == self.device:
                self.emulating = False
            elif kind == EV_DEVICE_REMOVED and self.device and lib.ei_event_get_device(ev) == self.device:
                lib.ei_device_unref(self.device)
                self.device, self.emulating = None, False
            elif kind == EV_DISCONNECT:
                lib.ei_event_unref(ev)
                self.close("KWin's EIS let go (the desktop ended?)")
                return
            lib.ei_event_unref(ev)

    def scroll(self, dx, dy):
        """A wheel, in 120ths of a notch (positive right and down). False if it can't go this way."""
        if not self.emulating:
            return False
        self.lib.ei_device_scroll_discrete(self.device, int(dx), int(dy))
        self.lib.ei_device_frame(self.device, self.lib.ei_now(self.ei))
        return True

    def close(self, why):
        if self.ei is None:
            return
        if self.device:
            self.lib.ei_device_unref(self.device)
        self.lib.ei_unref(self.ei)
        try:
            self.bus.close()
        except Exception:
            pass
        self.ei = self.bus = self.device = None
        self.fd, self.emulating = -1, False
        self.retry_at = time.monotonic() + 2.0  # a restarted desktop, soon; not a loop
        self.log(f"desktop wheel: {why}; scrolling through ft-screens' seat")
