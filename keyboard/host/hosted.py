"""Full Keyboard in hosted mode: its program, launched on one end of a socket pair, and the
line-delimited JSON on the other (protocol 1, in Full Keyboard's integration guide:
https://github.com/TaiKeid/steam-frame-full-keyboard/blob/main/docs/integration.md).

Hosted connects the transport only: launch, hello, the lease, requests and their answers, and
events. What to ask for is ft-kbhost's (host.py).
"""
import json
import os
import select
import socket
import subprocess
import time

PROTOCOL = 1
LEASE_MS = 2000
PING_EVERY = LEASE_MS / 3000   # seconds; any message renews the lease
MAX_LINE = 1 << 20
SEND_TIMEOUT = 1.0
EVENTS = ["visibility", "geometry", "target"]
# Requests that get no answer (integration.md): no id for them.
UNANSWERED = ("pointer", "ack", "reject", "quiesced")


class Hosted:
    """One running keyboard. on_event(message) gets each event and each failed answer."""

    def __init__(self, program, data_dir, config_dir, on_event, client_version="0",
                 popen=subprocess.Popen):
        self.on_event = on_event
        self.client_version = client_version
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock = ours
        command = [str(program), "--vr", "--hosted", str(theirs.fileno()), "--input", "ei",
                   "--start-enabled", "--data-dir", str(data_dir), "--config-dir", str(config_dir)]
        try:
            self.process = popen(command, pass_fds=(theirs.fileno(),), stdin=subprocess.DEVNULL)
        finally:
            theirs.close()
        self.sock.setblocking(False)
        self.buffer = b""
        self.next_id = 0
        self.greeted = False
        self.closed = False
        self.sent_at = time.monotonic()
        self.version = None

    def fileno(self):
        return self.sock.fileno()

    def send(self, op, **fields):
        """Sends one request; returns its id (None for the unanswered kinds, or when closed)."""
        if self.closed:
            return None
        message = {"op": op, **fields}
        if op not in UNANSWERED:
            self.next_id += 1
            message["id"] = self.next_id
        data = json.dumps(message, separators=(",", ":")).encode() + b"\n"
        try:
            _send_whole(self.sock, data)
        except OSError:
            # Gone, or stuck for SEND_TIMEOUT (it reads every frame): either way this keyboard is
            # done. Shutting the socket down makes it quit, and the next receive() say so.
            self.closed = True
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return None
        self.sent_at = time.monotonic()
        return message.get("id")

    def tick(self, now):
        if self.greeted and now - self.sent_at >= PING_EVERY:
            self.send("ping")

    def receive(self):
        """Reads what arrived; False once the keyboard closed its end."""
        while True:
            try:
                data = self.sock.recv(65536)
            except (BlockingIOError, InterruptedError):
                return True
            except OSError:
                data = b""
            if not data:
                self.closed = True
                return False
            self.buffer += data
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                self._line(line)
            if len(self.buffer) > MAX_LINE:
                self.closed = True
                return False

    def _line(self, line):
        try:
            message = json.loads(line)
        except ValueError:
            return
        if not isinstance(message, dict):
            return
        if message.get("event") == "ready":
            # Each keyboard process on this connection says it: the first, and a successor an
            # update or a rollback starts (integration.md, Handover), which wants hello again.
            if PROTOCOL not in (message.get("protocol") or []):
                self.closed = True
                self.sock.shutdown(socket.SHUT_RDWR)
                return
            self.version = message.get("version")
            self.greeted = True
            self.send("hello", protocol=PROTOCOL, client="Frametop", client_version=self.client_version,
                      lease_ms=LEASE_MS)
            self.send("subscribe", events=EVENTS)
        if "event" in message or message.get("ok") is False:
            self.on_event(message)

    def close(self):
        """Closing our end makes the keyboard quit as Settings -> Quit does."""
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass

    def wait(self, timeout=10):
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                return self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return self.process.wait()


def _send_whole(sock, data):
    """Writes all of data to a non-blocking socket, waiting up to SEND_TIMEOUT for room: a line
    is never half sent."""
    deadline = time.monotonic() + SEND_TIMEOUT
    view = memoryview(data)
    while view:
        try:
            view = view[sock.send(view):]
        except (BlockingIOError, InterruptedError):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([], [sock], [], left)[1]:
                raise TimeoutError("the keyboard isn't reading") from None


def config_dir():
    return os.path.join(os.path.expanduser("~"), ".config", "frametop-keyboard")
