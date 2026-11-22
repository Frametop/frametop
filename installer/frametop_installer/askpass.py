"""The window's end of the askpass socket: host/helper.py serve, running on the host, answers
the install's sudo with the password the window gives it (installer/README.md, The password)."""
import os

from gi.repository import GLib

from . import host, job


class Helper:
    """The host helper that answers the install's sudo (installer/host/helper.py serve)."""

    def __init__(self, on_ask):
        self.on_ask = on_ask
        self.proc = host.popen(["python3", str(host.BIN / "helper.py"), "serve", "--socket", job.SOCKET,
                                "--unit", job.UNIT])
        self.buf = b""
        self.watch = GLib.io_add_watch(self.proc.stdout.fileno(), GLib.PRIORITY_DEFAULT,
                                       GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.readable)

    def readable(self, fd, cond):
        try:
            data = os.read(fd, 1024)
        except OSError:
            data = b""
        if not data:
            self.watch = None
            return False
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if line == b"ask":
                self.on_ask()
        return True

    def send(self, line):
        try:
            self.proc.stdin.write(line + b"\n")
            self.proc.stdin.flush()
            return True
        except (OSError, ValueError):
            return False

    def give(self, pw):
        hexed = bytearray(bytes(pw).hex().encode())
        ok = self.send(b"pw " + bytes(hexed))
        hexed[:] = bytes(len(hexed))
        return ok

    def refuse(self):
        self.send(b"refuse")

    def close(self):
        if self.watch:
            GLib.source_remove(self.watch)
            self.watch = None
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
