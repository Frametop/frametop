"""What the tests that run ft-together for real share: one ft-together on this machine's
loopback and the questions to it, a program on its media socket, a program that hands it poses,
voices and pointers and gets the others', a program whose printed lines are read as they come,
and the checks' tally."""
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "conf"))
import ftipc  # noqa: E402

failed = 0


def check(what, ok):
    global failed
    failed += not ok
    print(("ok  " if ok else "FAIL") + "  " + what, flush=True)
    return ok


def finish():
    """The last line of a test, and its exit status."""
    print("FAILED" if failed else "all ok")
    sys.exit(1 if failed else 0)


def free_port():
    """A UDP port nothing listens on right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class End:
    """One ft-together and the questions to it."""

    def __init__(self, program, name, port=None, sock=None, env=None):
        self.name = name
        self.port = port or free_port()
        self.sock = sock or "ft_together_test_%d_%s" % (os.getpid(), name)
        self.client = ftipc.Client(timeout=2.0)
        self.proc = subprocess.Popen([program, "--loopback", "--name", name, "--port", str(self.port), "--socket", self.sock],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)

    def ask(self, text):
        return self.client.ask(self.sock, text) or "none (%s)" % self.client.why

    def state(self):
        reply = self.ask("state")
        return json.loads(reply[3:]) if reply.startswith("ok {") else {"mode": reply, "note": "", "people": [], "panels": [], "asks": [], "codes": 0}

    def hand(self):
        """From now on this ft-together is told each second that its wearer's hand moves their
        pointer, as ft-roomgaze tells it while gaze mode is off. Until it is told, a pointer
        that only moves is kept from the room (local/hush.h)."""
        def say():
            client = ftipc.Client(timeout=1.0)
            while self.proc.poll() is None:
                client.ask(self.sock, "gaze pointer hand")
                time.sleep(1.0)
        said = self.ask("gaze pointer hand")
        threading.Thread(target=say, daemon=True).start()
        return said == "ok"

    def until(self, what, seconds=5.0):
        """Whether what(state) comes true in time."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(self.state()):
                return True
            time.sleep(0.05)
        return False

    def end(self, sig=signal.SIGTERM):
        """Ends it; its exit status and what it printed."""
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            out, _ = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
            return -99, out
        return self.proc.returncode, out


class Program:
    """One of a Frame's media programs on ft-together's stream socket (local/mediawire.h): one
    that encodes the wearer's panel (hello b"S", its number) or shows someone's (b"V", its id)."""

    def __init__(self, end, hello=None, which=0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
        self.sock.connect(ftipc.address(end.sock + "_media"))
        self.buf = b""
        self.ignore = b""   # kinds of message messages() leaves out
        if hello:
            self.send(hello, bytes([which]))

    def send(self, kind, body=b""):
        self.sock.sendall(struct.pack("<I", 1 + len(body)) + kind + body)

    def unit(self, data, key=False):
        self.send(b"U", bytes([1 if key else 0]) + data)

    def messages(self, seconds=0.5, count=None):
        """What came in that time, or until `count` of them did: [(kind, body)], with None as
        the last when ft-together closed the socket."""
        got = []
        deadline = time.monotonic() + seconds
        while True:
            while len(self.buf) >= 4:
                n = struct.unpack_from("<I", self.buf)[0]
                if len(self.buf) < 4 + n:
                    break
                if self.buf[4:5] not in self.ignore:
                    got.append((self.buf[4:5], self.buf[5:4 + n]))
                self.buf = self.buf[4 + n:]
                if count is not None and len(got) >= count:
                    return got
            left = deadline - time.monotonic()
            if left <= 0:
                return got
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(1 << 20)
            except socket.timeout:
                return got
            except OSError:
                data = b""
            if not data:
                return got + [None]
            self.buf += data

    def let_go(self, seconds=3.0):
        """Whether ft-together closes the socket in that time."""
        return self.messages(seconds)[-1:] == [None]

    def close(self):
        self.sock.close()


def pose(x, turn=0, eye_height=1.6, z=0.0):
    """A pose datagram (proto/posewire.h): a head at (x, 1.6, z) looking ahead, no hands."""
    return struct.pack("<BH3f4hHB5s", 1, turn, x, 1.6, z, 32767, 0, 0, 0, round(eye_height * 1000), 0, bytes(5))


def voice(count, size=60):
    """A voice datagram (proto/voicewire.h): its count, and a packet of that many bytes."""
    return struct.pack("<BH", 2, count) + bytes([count & 255]) * size


def point(panel, x, y, pressed=False, count=0):
    """A pointer datagram (proto/pointwire.h): at (x, y) of 65535 across and down a panel."""
    return struct.pack("<BHBHHB", 3, count, panel, x, y, 1 if pressed else 0)


def act(panel, what, a, b):
    """An act's body (proto/control.h): 1 a move, 2 a button, 3 a scroll, 4 a key; 0 lets go."""
    return struct.pack("<BBii", panel, what, a, b)


class Watcher:
    """One of a Frame's own programs: it hands ft-together the wearer's pose, voice or pointer, and gets what
    the others send, or the input they may give the desktop."""

    def __init__(self, end):
        self.to = ftipc.address(end.sock)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind("")

    def send(self, data):
        self.sock.sendto(data, self.to)

    def ask(self, text):
        self.sock.settimeout(2.0)
        self.send(text.encode())
        try:
            while True:
                data = self.sock.recv(4096)
                if data[:1] < b"\x80":   # an answer is text; what comes unasked begins 0x81 or above (local/datagrams.h)
                    return data.decode()
        except OSError:
            return "none"

    def poses(self, seconds=0.5):
        """What came in that time: [(person, pose bytes)]."""
        return self.got(b"\x81", seconds)

    def voices(self, seconds=0.5):
        return self.got(b"\x82", seconds)

    def words(self, seconds=0.5):
        """The text that came unasked in that time (a floor that moved: local/watchers.h, Tell)."""
        got = []
        deadline = time.monotonic() + seconds
        while (left := deadline - time.monotonic()) > 0:
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except OSError:
                break
            if data[:1] < b"\x80":
                got.append(data.decode())
        return got

    def got(self, kind, seconds):
        got = []
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return got
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except OSError:
                return got
            if data[:1] == kind:
                got.append((data[1], data[2:]))

    def close(self):
        self.sock.close()


class Tool:
    """A program whose printed lines are read as they come."""

    def __init__(self, argv, env=None):
        self.lines = []
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))

    def until(self, what, seconds=10.0):
        """Whether what(lines) comes true in time."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(list(self.lines)):
                return True
            time.sleep(0.05)
        return what(list(self.lines))

    def said(self, words, seconds=10.0, since=0):
        """Whether a line with `words` in it comes in time, among those from line `since` on."""
        return self.until(lambda lines: any(words in line for line in lines[since:]), seconds)

    def wait(self, seconds=30.0):
        """Its exit status once it ends by itself, or None when it doesn't in time."""
        try:
            status = self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            return None
        self.reader.join(2)
        return status

    def end(self, sig=signal.SIGTERM):
        """Ends it; its exit status."""
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        status = self.wait(10)
        if status is None:
            self.proc.kill()
            self.wait(5)
            return -99
        return status


def model(name, may_be_sent, filler=b""):
    """A model's file (avatar/modelfile.h) with nothing to draw in it: what it is called, whether
    its owner lets peers have it, and `filler` in a chunk no reader knows."""
    def chunk(tag, body):
        return tag + struct.pack("<I", len(body)) + body + bytes(-len(body) % 4)

    info = b"".join(struct.pack("<BH", key, len(text)) + text for key, text in ((1, name.encode()), (2, b"CC0"), (5, bytes([may_be_sent]))))
    return b"FTMD" + struct.pack("<I", 1) + chunk(b"INFO", info) + chunk(b"JUNK", filler)


def names(state):
    return sorted(p["name"] for p in state["people"])


def linked(state):
    return all(p["linked"] for p in state["people"])


def let_in(owner, guest, host="127.0.0.1"):
    """`guest` joins `owner`'s room with a new invite and is accepted."""
    code = owner.ask("invite")
    if not (code.startswith("ok ") and guest.ask("join %s %d %s" % (host, owner.port, code[3:])) == "ok"):
        return False
    if not owner.until(lambda s: any(a["name"] == guest.name for a in s["asks"])):
        return False
    ask = [a for a in owner.state()["asks"] if a["name"] == guest.name][0]
    if ask["login"] != "someone@loopback" or ask["address"] != "127.0.0.1":
        return False
    return owner.ask("accept %d" % ask["id"]) == "ok" and guest.until(lambda s: s["mode"] == "joined")
