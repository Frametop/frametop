#!/usr/bin/env python3
"""Offline test of input/kwin_ei.py: the desktop's wheel through KWin's EIS (issue 108). libei and
the D-Bus call are fakes, so nothing reaches a running KWin.

  input/test/kwinei-test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import kwin_ei  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


class Bus:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class Lib:
    """libei's calls the module makes, recorded; events are (type, device) pairs queued in `events`."""

    def __init__(self):
        self.calls, self.events, self.backend_ok = [], [], True

    def ei_new_sender(self, _ctx):
        return 100

    def ei_configure_name(self, ei, name):
        self.calls.append(("name", name))

    def ei_setup_backend_fd(self, ei, fd):
        self.calls.append(("fd", fd))
        return 0 if self.backend_ok else -1

    def ei_get_fd(self, ei):
        return 33

    def ei_unref(self, ei):
        self.calls.append(("unref", ei))

    def ei_dispatch(self, ei):
        pass

    def ei_get_event(self, ei):
        return self.events.pop(0) if self.events else None

    def ei_event_get_type(self, ev):
        return ev[0]

    def ei_event_get_seat(self, ev):
        return 7

    def ei_event_get_device(self, ev):
        return ev[1]

    def ei_event_unref(self, ev):
        pass

    def ei_seat_bind_capabilities(self, seat, *caps):
        self.calls.append(("bind", seat.value, tuple(c.value for c in caps)))

    def ei_device_has_capability(self, dev, cap):
        return dev == 55 and cap == kwin_ei.CAP_SCROLL

    def ei_device_ref(self, dev):
        return dev

    def ei_device_unref(self, dev):
        self.calls.append(("devunref", dev))

    def ei_device_start_emulating(self, dev, seq):
        self.calls.append(("start", dev, seq))

    def ei_device_scroll_discrete(self, dev, dx, dy):
        self.calls.append(("scroll", dev, dx, dy))

    def ei_device_frame(self, dev, t):
        self.calls.append(("frame", dev))

    def ei_now(self, ei):
        return 1


logs = []
buses = []
connects = []
bus_address = ["unix:path=/tmp/dbus-test"]


def connect(address):
    connects.append(address)
    if address == "broken":
        raise RuntimeError("no EIS")
    bus = Bus()
    buses.append(bus)
    return 9, 1, bus


# No libei: nothing connects, ft-screens keeps its seat.
none = kwin_ei.KwinEi(logs.append, lambda: "x", lib=None, connect=connect)
none.poll(0.0)
check("no libei: never connects", (connects, none.ready(), none.fileno()), ([], False, -1))
check("no libei: said once", [m for m in logs if "no libei" in m] != [], True)

lib = Lib()
ei = kwin_ei.KwinEi(logs.append, lambda: bus_address[0], lib=lib, connect=connect)

# No desktop: a failure, logged once, tried again after RETRY_S.
bus_address[0] = None
logs.clear()
ei.poll(0.0)
ei.poll(kwin_ei.RETRY_S + 1)
check("no desktop: logged once", len([m for m in logs if "isn't running" in m]), 1)
check("no desktop: not ready", ei.ready(), False)

# A KWin without EIS: logged, nothing kept.
bus_address[0] = "broken"
ei.poll(2 * kwin_ei.RETRY_S + 2)
check("no EIS: nothing kept", (ei.fileno(), ei.ei), (-1, None))
ei.poll(2 * kwin_ei.RETRY_S + 3)
check("no EIS: waits before the next try", connects.count("broken"), 1)

# The desktop: connects with libei's fd, then waits for the seat and the device.
bus_address[0] = "unix:path=/tmp/dbus-test"
ei.poll(4 * kwin_ei.RETRY_S)
check("connected: libei's fd in select", ei.fileno(), 33)
check("connected: KWin's socket given to libei", ("fd", 9) in lib.calls, True)
check("connected: not ready before the device", ei.ready(), False)
check("connected: a scroll before then doesn't go", ei.scroll(0, 120), False)

lib.events = [(kwin_ei.EV_CONNECT, None), (kwin_ei.EV_SEAT_ADDED, None), (kwin_ei.EV_DEVICE_ADDED, 44),
              (kwin_ei.EV_DEVICE_ADDED, 55), (kwin_ei.EV_DEVICE_RESUMED, 55)]
ei.dispatch()
check("seat: binds pointer and scroll", ("bind", 7, (kwin_ei.CAP_POINTER, kwin_ei.CAP_SCROLL, None)) in lib.calls, True)
check("device: the scroll one is kept", ei.device, 55)
check("resumed: emulating", (ei.ready(), ("start", 55, 1) in lib.calls), (True, True))

lib.calls.clear()
check("scroll: goes", ei.scroll(-120, 240), True)
check("scroll: discrete steps, then a frame", lib.calls, [("scroll", 55, -120, 240), ("frame", 55)])

lib.events = [(kwin_ei.EV_DEVICE_PAUSED, 55)]
ei.dispatch()
check("paused: not ready", ei.ready(), False)
lib.events = [(kwin_ei.EV_DEVICE_RESUMED, 55)]
ei.dispatch()
check("resumed again: a new sequence", (ei.ready(), ("start", 55, 2) in lib.calls), (True, True))

# The desktop ends: everything goes, and the next try waits a little.
lib.events = [(kwin_ei.EV_DISCONNECT, None)]
ei.dispatch()
check("disconnect: closed", (ei.ready(), ei.fileno(), ei.ei, buses[-1].closed), (False, -1, None, True))
check("disconnect: the device let go", ("devunref", 55) in lib.calls, True)
check("disconnect: retries soon, not at once", ei.retry_at > 0, True)

# libei refusing the socket: the bus closes too.
lib2 = Lib()
lib2.backend_ok = False
ei2 = kwin_ei.KwinEi(logs.append, lambda: "unix:path=/tmp/dbus-test", lib=lib2, connect=connect)
ei2.poll(0.0)
check("backend refused: nothing kept, bus closed", (ei2.fileno(), buses[-1].closed), (-1, True))

print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
