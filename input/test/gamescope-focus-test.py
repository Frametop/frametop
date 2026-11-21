#!/usr/bin/env python3
"""input/gamescope_focus.py against a fake libxcb: no X server or gamescope.

  python3 input/test/gamescope-focus-test.py
"""
import ctypes
import os
import struct
import sys
import time
import types

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gamescope_focus as gf  # noqa: E402

ROOT, FOCUSED_ATOM, FOCUSABLE_ATOM = 0x100, 41, 42
NATIVE_WINDOW, NATIVE_PID, GAME_WINDOW, GAME_PID = 0x500, 222, 0x600, 333


class FakeXcb:
    """The calls GamescopeFocus makes. A question's reply is ready unless its sequence is held."""

    def __init__(self):
        self.seq, self.answers, self.held, self.keep = 0, {}, set(), {}
        self.focused_window, self.connects, self.disconnects = NATIVE_WINDOW, 0, 0

    def cookie(self, value):
        self.seq += 1
        self.answers[self.seq] = value
        return types.SimpleNamespace(sequence=self.seq)

    def xcb_connect(self, display, screen):
        self.connects += 1
        return 1234

    def xcb_connection_has_error(self, conn):
        return 0

    def xcb_disconnect(self, conn):
        self.disconnects += 1

    def xcb_flush(self, conn):
        pass

    def xcb_get_setup(self, conn):
        return 1

    def xcb_setup_roots_iterator(self, setup):
        return types.SimpleNamespace(data=[ROOT])

    def xcb_intern_atom(self, conn, only_if_exists, n, name):
        atom = {b"GAMESCOPE_FOCUSED_WINDOW": FOCUSED_ATOM, b"GAMESCOPE_FOCUSABLE_WINDOWS": FOCUSABLE_ATOM}[name]
        return self.cookie(("atom", atom))

    def xcb_get_property(self, conn, delete, window, atom, kind, offset, length):
        assert window == ROOT
        if atom == FOCUSED_ATOM:
            return self.cookie(("prop", struct.pack("=I", self.focused_window)))
        return self.cookie(("prop", struct.pack("=6I", GAME_WINDOW, 7, GAME_PID, NATIVE_WINDOW, 769, NATIVE_PID)))

    def xcb_poll_for_reply(self, conn, sequence, reply, error):
        if sequence in self.held:
            return 0
        kind, value = self.answers.pop(sequence)
        raw = b"\0" * 8 + struct.pack("=I", value) if kind == "atom" else value
        buf = ctypes.create_string_buffer(raw, len(raw))
        self.keep[ctypes.addressof(buf)] = (buf, len(raw))
        reply._obj.value = ctypes.addressof(buf)
        return 1

    def xcb_get_property_value_length(self, reply):
        return self.keep[reply.value][1]

    def xcb_get_property_value(self, reply):
        return reply.value


def make(fake):
    said = []
    focus = gf.GamescopeFocus(said.append, load=lambda: (fake, lambda p: None),
                              comm=lambda pid: {NATIVE_PID: "kwin_wayland", GAME_PID: "game"}.get(pid, ""))
    return focus, said


def settle(focus, now):
    """native() until the connection thread has been taken."""
    for _ in range(200):
        focus.native(now)
        if focus.conn is not None:
            return
        time.sleep(0.005)
    raise AssertionError("never connected")


def test_focused_pid():
    focusable = struct.pack("=6I", GAME_WINDOW, 7, GAME_PID, NATIVE_WINDOW, 769, NATIVE_PID)
    assert gf.focused_pid(struct.pack("=I", NATIVE_WINDOW), focusable) == NATIVE_PID
    assert gf.focused_pid(struct.pack("=I", GAME_WINDOW), focusable) == GAME_PID
    assert gf.focused_pid(struct.pack("=I", 0x999), focusable) == 0
    assert gf.focused_pid(b"", focusable) == 0
    assert gf.focused_pid(struct.pack("=I", NATIVE_WINDOW), focusable[:-4]) == 0  # its pid cut off
    assert gf.focused_pid(struct.pack("=I", NATIVE_WINDOW), focusable + b"\1\2") == NATIVE_PID


def test_focus_follows_gamescope():
    fake = FakeXcb()
    focus, said = make(fake)
    now = 100.0
    settle(focus, now)  # the atoms came at once; the properties are asked now
    assert focus.atoms == [FOCUSED_ATOM, FOCUSABLE_ATOM]
    assert focus.native(now + 0.1) is True
    assert said[-1] == "Native Desktop has gamescope's focus"
    fake.focused_window = GAME_WINDOW
    focus.native(now + 0.7)  # asks again (FOCUS_POLL)
    assert focus.native(now + 0.8) is False
    assert said[-1] == "Native Desktop lost gamescope's focus"


def test_partial_answers_are_kept():
    fake = FakeXcb()
    focus, _ = make(fake)
    now = 100.0
    settle(focus, now)
    fake.held = {fake.seq}  # the second property's reply hasn't come
    assert focus.native(now + 0.1) is False
    assert [w for w, _ in focus.asked] == [b"focusable"] and b"focused" in focus.got
    fake.held = set()
    assert focus.native(now + 0.2) is True


def test_silent_gamescope_goes_stale_and_reconnects():
    fake = FakeXcb()
    focus, said = make(fake)
    now = 100.0
    settle(focus, now)
    assert focus.native(now + 0.1) is True
    focus.native(now + 0.7)
    fake.held = set(fake.answers)  # asked, never answered
    assert focus.native(now + 2.0) is True  # still fresh
    assert focus.native(now + 3.9) is False
    assert said[-1] == "Native Desktop's focus: gamescope stopped answering"
    assert focus.conn is None and fake.disconnects == 1
    focus.native(now + 5.0)
    assert fake.connects == 1  # waits RETRY_S before trying again
    settle(focus, now + 9.0)
    assert fake.connects == 2


def test_no_libxcb():
    said = []

    def missing():
        raise OSError("libxcb.so.1: cannot open shared object file")

    focus = gf.GamescopeFocus(said.append, load=missing)
    assert focus.native(1.0) is False and focus.native(2.0) is False
    assert len(said) == 1 and "no libxcb" in said[0]


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
