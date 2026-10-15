"""One recorder window at a time (hands/rec/ft_handrec.py; Frametop's and the standalone Hand
Recorder share the name). Two copies would share one headset panel (ft-handpanel's @ft_handpanel:
the second copy's panel can't start, and its pings reach the first's), the same camera and
tracking units (whichever closes first stops them under the other) and Frametop's paused pointer.
It happens easily: the window opened from SteamVR's dashboard stays open in the dashboard's
session when the person goes to Frametop's desktop and opens it there.

The window that's open listens on an abstract unix socket (the dev container shares the host's
network, so Frametop's and the standalone window see each other). A second one asks it:
  "where": a JSON line: pid, where (to tell the person) and busy ("" or what it's doing: a
           session, an export, an upload)
  "show":  raise the window
  "quit":  close it as its window's close button would, unless it's busy ("busy" then)
take() does that: the newest launch wins unless the open one is busy. An idle one closes rather
than being raised: raising does nothing visible across sessions (dashboard and desktop), often not
on Wayland either, and one left on a placeholder screen when its compositor restarted (Qt's
QT_WAYLAND_RECONNECT, which Plasma sets) can't be seen at all.

Standard library only, apart from the QSocketNotifier the listener needs."""
import json
import os
import signal
import socket
import time

TAKE_S = 30.0    # how long a closing window may take (Backend.shutdown stops a session: up to 20 s)
ASK_S = 10.0     # a window that doesn't answer in this long is stuck (one still loading its QML
                 # answers once its event loop runs)


def where():
    """Where the window shows, in words: on Frametop's desktop (its Wayland socket is in
    /run/user/UID/frametop), on a Plasma desktop, or in SteamVR's dashboard (Launch a program
    runs it in Steam's gamescope session, through Xwayland)."""
    wl = os.environ.get("WAYLAND_DISPLAY", "")
    if "/frametop/" in wl:
        return "on Frametop's desktop"
    if wl and os.environ.get("KDE_FULL_SESSION") == "true":
        return "on the desktop"
    return "in SteamVR's dashboard"


class Taken(Exception):
    """take() couldn't have the window: .text says why, for the person."""
    def __init__(self, text):
        super().__init__(text)
        self.text = text


def _bind(name):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
    try:
        s.bind("\0" + name)
    except OSError:
        s.close()
        return None
    s.listen(4)
    return s


def ask(name, command, timeout=ASK_S):
    """(the open window's answer or None, its pid or 0). None: nobody listens, or it's stuck."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
    s.settimeout(timeout)
    pid = 0
    try:
        s.connect("\0" + name)
        try:
            cred = s.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
            pid = int.from_bytes(cred[:4], "little")
        except OSError:
            pass
        s.sendall(command.encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return (data.decode(errors="replace").strip() or None), pid
    except OSError:
        return None, pid
    finally:
        s.close()


def _is_recorder(pid, script):
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return any(os.path.basename(a.decode(errors="replace")) == script for a in f.read().split(b"\0"))
    except OSError:
        return False


def take(name, script, log=lambda line: None, wait=TAKE_S):
    """The listening socket, once no other window holds name: an idle one (or one that doesn't
    answer: SIGTERM, if it's a script process) is asked to close. Raises Taken if it's busy or
    won't close; a busy one is shown, so the person finds it."""
    s = _bind(name)
    if s:
        return s
    answer, pid = ask(name, "where")
    info = {}
    if answer:
        try:
            info = json.loads(answer)
        except ValueError:
            pass
    there = info.get("where") or "somewhere else"
    if info.get("busy"):
        ask(name, "show")
        raise Taken("The Hand Recorder is already open %s, %s. Finish or stop that there first "
                    "(holding the headset button for 1.5 s stops a round), then open it here again."
                    % (there, info["busy"]))
    if answer is None:
        if not pid or not _is_recorder(pid, script):
            raise Taken("Another Hand Recorder (pid %d) holds its window and doesn't answer." % pid)
        log("one window: the open one (pid %d) doesn't answer: SIGTERM" % pid)
        os.kill(pid, signal.SIGTERM)
    else:
        log("one window: closing the one open %s (pid %d)" % (there, pid))
        reply, _ = ask(name, "quit")
        if reply == "busy":
            ask(name, "show")
            raise Taken("The Hand Recorder open %s just started something. Finish it there first." % there)
    end = time.monotonic() + wait
    while time.monotonic() < end:
        s = _bind(name)
        if s:
            return s
        time.sleep(0.2)
    raise Taken("The Hand Recorder open %s (pid %d) didn't close. Close it there, then open it here again."
                % (there, pid))


class Listener:
    """Answers other launches on the socket take() returned. busy() gives what the window is
    doing ("" when idle); show() and quit() act on the window (quit returns False if it's busy)."""

    def __init__(self, sock, busy, show, quit):
        from PySide6.QtCore import QSocketNotifier
        self.sock, self.busy, self.show, self.quit = sock, busy, show, quit
        sock.setblocking(False)
        self.notifier = QSocketNotifier(sock.fileno(), QSocketNotifier.Type.Read)
        self.notifier.activated.connect(self._accept)

    def _accept(self, *_):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        try:
            conn.settimeout(1.0)
            data = b""
            while b"\n" not in data and len(data) < 256:
                chunk = conn.recv(256)
                if not chunk:
                    break
                data += chunk
            cmd = data.decode(errors="replace").strip()
            if cmd == "where":
                reply = json.dumps({"pid": os.getpid(), "where": where(), "busy": self.busy()})
            elif cmd == "show":
                self.show()
                reply = "ok"
            elif cmd == "quit":
                reply = "ok" if self.quit() else "busy"
            else:
                reply = "error unknown command"
            conn.sendall(reply.encode() + b"\n")
        except OSError:
            pass
        finally:
            conn.close()
