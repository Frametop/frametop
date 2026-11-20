"""ft-kbhost's side of the input relay's control socket, @frametop_relay (input/input-relay.py).

The relay protocol Frametop's keyboard has spoken since 0.8.1 (keyboard/FRAMETOP.md), plus
the Steam word:

  to the relay:   kbhello 0|1          every second, and at once when the panel opens or closes
                  kbkey CODE 0|1       a key for the desktop (evdev), while the relay says so
                  kbtext TEXT          text for the desktop, the same
                  kbsteam              a Steam text field asks for the keyboard
  from the relay: kb DESKTOP SURFACE STEAM
                                       typing goes to the desktop; the screens show, not paused;
                                       Steam's fields may take the keyboard (each 0|1)
                  kb show [head|dash M]   open (where: twelve numbers, a pose; placement.spot)
                  kb hide
                  kb poke SIDE VERB [X Y Z]   a fingertip on the panel

Word counts only from the relay's own name and this user. Desktop typing goes only while the
relay's word is under STALE seconds old, so a stopped relay never leaves typing addressed to a
name another process could take.
"""
import os
import socket
import struct

RELAY = b"\0frametop_relay"
STALE = 3.0
CREDS = struct.Struct("3i")  # struct ucred: pid, uid, gid


class RelayLink:
    def __init__(self, address=RELAY):
        self.address = address
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        self.sock.bind("")  # an autobound abstract name, which the relay answers
        self.heard_at = None   # monotonic time of the relay's last word
        self.owed = []         # key releases the relay's full socket didn't take yet

    def fileno(self):
        return self.sock.fileno()

    def fresh(self, now):
        return self.heard_at is not None and now - self.heard_at < STALE

    def send(self, text):
        try:
            self.sock.sendto(text.encode(), self.address)
            return True
        except OSError:
            return False  # the relay isn't running, or is behind

    def hello(self, shown):
        self.send(f"kbhello {int(bool(shown))}")

    def steam(self):
        self.send("kbsteam")

    def text(self, now, text):
        return self.fresh(now) and self.send(f"kbtext {text}")

    def key(self, now, code, down):
        """A key for the desktop. A release that doesn't go is owed and sent again (flush), before
        the next key; a press that doesn't go is dropped, since a late press would type."""
        if not self.fresh(now):
            return False
        self.flush()
        if self.owed or not self.send(f"kbkey {code} {int(down)}"):
            if not down:
                self.owed.append(code)
            return False
        return True

    def flush(self):
        while self.owed and self.send(f"kbkey {self.owed[0]} 0"):
            self.owed.pop(0)

    def receive(self, now):
        """The relay's words since the last call, each split into words."""
        out = []
        while True:
            try:
                data, anc, _, addr = self.sock.recvmsg(4096, socket.CMSG_SPACE(CREDS.size))
            except (BlockingIOError, InterruptedError):
                return out
            uid = None
            for level, kind, payload in anc:
                if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS and len(payload) >= CREDS.size:
                    uid = CREDS.unpack(payload[:CREDS.size])[1]
            addr = addr.encode() if isinstance(addr, str) else addr
            if addr != self.address or uid != os.getuid():
                continue
            words = data.decode(errors="replace").split()
            if words[:1] == ["kb"]:
                self.heard_at = now
                out.append(words[1:])

    def close(self):
        self.sock.close()
