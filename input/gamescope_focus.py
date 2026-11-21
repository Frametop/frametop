"""Whether Native Desktop has gamescope's focus, for the input relay's typing (PR 110).

The window GAMESCOPE_FOCUSED_WINDOW names on gamescope's first Xwayland is Native Desktop's when
GAMESCOPE_FOCUSABLE_WINDOWS (window, app id, pid each) gives it a kwin_wayland's pid. Native
Desktop's KWin is the only one there: the Frametop desktop's runs in ft-screens.

Asked through libxcb (ctypes) without waiting on gamescope: the connection is made on a thread,
native() sends the question at most every FOCUS_POLL seconds and reads the answers on later calls,
so a stuck gamescope can't hold up the relay. No answer for FOCUS_STALE seconds counts as not
focused.
"""

import ctypes
import ctypes.util
import struct
import threading

GAMESCOPE_DISPLAY = ":0"
FOCUS_POLL = 0.5
FOCUS_STALE = 3.0
RETRY_S = 5.0  # between connection attempts
ATOMS = (b"GAMESCOPE_FOCUSED_WINDOW", b"GAMESCOPE_FOCUSABLE_WINDOWS")


class XcbCookie(ctypes.Structure):
    _fields_ = [("sequence", ctypes.c_uint)]


class XcbScreenIterator(ctypes.Structure):
    _fields_ = [("data", ctypes.POINTER(ctypes.c_uint32)), ("rem", ctypes.c_int), ("index", ctypes.c_int)]


def load_xcb():
    """libxcb with the calls GamescopeFocus makes typed, and libc's free; OSError when missing."""
    lib = ctypes.CDLL(ctypes.util.find_library("xcb") or "libxcb.so.1")
    libc = ctypes.CDLL(None)
    lib.xcb_connect.restype = ctypes.c_void_p
    lib.xcb_connect.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    lib.xcb_connection_has_error.argtypes = [ctypes.c_void_p]
    lib.xcb_disconnect.argtypes = [ctypes.c_void_p]
    lib.xcb_flush.argtypes = [ctypes.c_void_p]
    lib.xcb_get_setup.restype = ctypes.c_void_p
    lib.xcb_get_setup.argtypes = [ctypes.c_void_p]
    lib.xcb_setup_roots_iterator.restype = XcbScreenIterator
    lib.xcb_setup_roots_iterator.argtypes = [ctypes.c_void_p]
    lib.xcb_intern_atom.restype = XcbCookie
    lib.xcb_intern_atom.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_char_p]
    lib.xcb_get_property.restype = XcbCookie
    lib.xcb_get_property.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
    lib.xcb_poll_for_reply.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p),
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.xcb_get_property_value.restype = ctypes.c_void_p
    lib.xcb_get_property_value.argtypes = [ctypes.c_void_p]
    lib.xcb_get_property_value_length.argtypes = [ctypes.c_void_p]
    libc.free.argtypes = [ctypes.c_void_p]
    return lib, libc.free


def read_comm(pid):
    """/proc/PID/comm, or "" when it's gone."""
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return ""


def focused_pid(focused, focusable):
    """The pid GAMESCOPE_FOCUSABLE_WINDOWS gives the window GAMESCOPE_FOCUSED_WINDOW names (both
    raw property values, native-endian CARD32s), or 0."""
    window = struct.unpack("=I", focused[:4])[0] if len(focused) >= 4 else 0
    if not window:
        return 0
    triples = struct.unpack(f"={len(focusable) // 4}I", focusable[:len(focusable) // 4 * 4])
    return next((triples[i + 2] for i in range(0, len(triples) - 2, 3) if triples[i] == window), 0)


class GamescopeFocus:
    def __init__(self, log, load=load_xcb, comm=read_comm):
        self.log, self.load_lib, self.comm = log, load, comm
        self.lib = None  # libxcb, once loaded; False: there's none
        self.conn, self.root, self.atoms = None, 0, None
        self.connecting = None  # the thread making a connection, while it runs
        self.made = None  # (conn, root) the thread made, for native() to take
        self.asked, self.got = [], {}  # (what, sequence) of the questions out; the answers so far
        self.retry_at, self.ask_at, self.asked_at, self.answered = 0.0, 0.0, 0.0, -1e9
        self.focused = False

    def load(self):
        if self.lib is None:
            try:
                self.lib, self.free = self.load_lib()
            except OSError as e:
                self.log(f"Native Desktop's focus: no libxcb ({e}); its typing goes as Steam's")
                self.lib = False
        return bool(self.lib)

    def drop(self, now):
        if self.conn:
            self.lib.xcb_disconnect(self.conn)
        self.conn, self.atoms, self.asked, self.got = None, None, [], {}
        self.retry_at = now + RETRY_S

    def connect_in_thread(self):
        """xcb_connect blocks while the X server doesn't answer its setup: on a thread, so the
        relay doesn't. native() takes what it made."""
        made = (None, 0)  # also when a call below raises: native() must never wait on it
        try:
            conn = self.lib.xcb_connect(GAMESCOPE_DISPLAY.encode(), None)
            if conn and not self.lib.xcb_connection_has_error(conn):
                made = (conn, self.lib.xcb_setup_roots_iterator(self.lib.xcb_get_setup(conn)).data[0])  # .root
            elif conn:
                self.lib.xcb_disconnect(conn)
        finally:
            self.made = made

    def connected(self, now):
        """Starts a connection when it's time, and takes one the thread made. Whether there is one."""
        if self.conn is None and self.connecting is None and now >= self.retry_at:
            self.made = None
            self.connecting = threading.Thread(target=self.connect_in_thread, daemon=True)
            self.connecting.start()
        if self.connecting is not None and self.made is not None:
            self.connecting.join()
            (conn, root), self.connecting = self.made, None
            if conn is None:
                self.retry_at = now + RETRY_S
            else:
                self.conn, self.root = conn, root
                self.ask(now, [(name, self.lib.xcb_intern_atom(conn, 1, len(name), name).sequence) for name in ATOMS])
        return self.conn is not None

    def ask(self, now, asked):
        self.asked, self.got = asked, {}
        self.lib.xcb_flush(self.conn)
        self.asked_at = now

    def answers(self, now):
        """The replies to every question asked, {what: bytes (an atom's id, or a property's value)},
        once all have come; None while some haven't (the ones that came are kept). Drops the
        connection when it broke."""
        while self.asked:
            what, sequence = self.asked[0]
            reply, error = ctypes.c_void_p(), ctypes.c_void_p()
            if not self.lib.xcb_poll_for_reply(self.conn, sequence, ctypes.byref(reply), ctypes.byref(error)):
                return None
            if error.value:
                self.free(error)
            if not reply.value:
                if self.lib.xcb_connection_has_error(self.conn):
                    self.drop(now)
                    return None
                self.got[what] = b""
            elif what in ATOMS:  # xcb_intern_atom_reply_t: its atom after 8 bytes
                self.got[what] = ctypes.string_at(reply.value + 8, 4)
            else:
                n = self.lib.xcb_get_property_value_length(reply)
                self.got[what] = ctypes.string_at(self.lib.xcb_get_property_value(reply), n) if n > 0 else b""
            if reply.value:
                self.free(reply)
            self.asked.pop(0)
        got, self.got = self.got, {}
        return got

    def understand(self, got, now):
        if self.atoms is None:
            self.atoms = [struct.unpack("=I", got.get(name, b""))[0] if len(got.get(name, b"")) == 4 else 0
                          for name in ATOMS]
            if not all(self.atoms):
                self.drop(now)  # not gamescope's Xwayland (yet)
            return
        pid = focused_pid(got.get(b"focused", b""), got.get(b"focusable", b""))
        focused_now = bool(pid) and self.comm(pid) == "kwin_wayland"
        if focused_now != self.focused:
            self.log("Native Desktop has gamescope's focus" if focused_now else "Native Desktop lost gamescope's focus")
        self.focused, self.answered = focused_now, now

    def native(self, now):
        """Native Desktop had gamescope's focus at the last answer, under FOCUS_STALE ago."""
        if not self.load():
            return False
        if self.connected(now):
            if self.asked:
                got = self.answers(now)
                if got is not None:
                    self.understand(got, now)
            if self.conn is not None and not self.asked and self.atoms and now >= self.ask_at:
                self.ask(now, [(what, self.lib.xcb_get_property(self.conn, 0, self.root, atom, 0, 0, length).sequence)
                               for what, atom, length in ((b"focused", self.atoms[0], 1),
                                                          (b"focusable", self.atoms[1], 3 * 64))])
                self.ask_at = now + FOCUS_POLL
        if now - self.answered > FOCUS_STALE:
            if self.focused:
                self.log("Native Desktop's focus: gamescope stopped answering")
            self.focused = False
            if self.asked and self.conn is not None and now - self.asked_at > FOCUS_STALE:
                self.drop(now)  # asked long ago and never heard: start over
        return self.focused
