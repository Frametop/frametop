"""A stand-in for ft-screens, as far as ft-roompanels asks it (test_roompanels.py asks it
directly; roompanels-test.py serves it on a socket): the desktop's screens and where they stand,
the remote screens, the share tap, the head, and "shift", which moves all that stands in the
room as ft-screens does (screens/roomshift.h): its sum is kept, and "shift back" undoes it."""
import math
import socket
import threading


def _turned(yaw, v):
    c, s = math.cos(yaw), math.sin(yaw)
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


class Screens:
    """ft-screens, as far as the keeper asks it."""

    def __init__(self):
        self.kwin = {1: (3440, 1440), 2: (1920, 1080)}
        self.at = {1: (0.0, 1.5, -2.0), 2: (1.5, 1.5, -1.5)}
        self.yaw = {}         # number -> how far a desktop screen is turned to the left (radians)
        self.shifted = []     # every "shift" taken: (dx, dz, yaw) or "back"
        self.remotes = {}     # number -> [client, size]
        self.shares = {}      # number -> (slot, size)
        self.panels = {}      # a remote screen's number -> {"pose", "width", "curve"}
        self.head = "ok 0.5000 1.6000 0.2500 90.00"
        self.asked = []
        self.there = True

    def __call__(self, text):
        self.asked.append(text)
        if not self.there:
            return None
        w = text.split()
        if text == "head":
            return self.head
        if text == "screens":
            return f"ok {len(self.kwin)}" + "".join(f" {n}:{s[0]}x{s[1]}:1.200" for n, s in sorted(self.kwin.items()))
        if text == "remotes":
            return f"ok {len(self.remotes)}" + "".join(f" {n}:{r[0]}:live:{r[1]}" for n, r in sorted(self.remotes.items()))
        if text == "shares":
            return f"ok {len(self.shares)}" + "".join(f" {n}:{s[0]}:live:{s[1]}" for n, s in sorted(self.shares.items()))
        if w[0] == "get":
            if int(w[1]) not in self.at:
                return f"error no screen {w[1]}"
            x, y, z = self.at[int(w[1])]
            ax, az = _turned(self.yaw.get(int(w[1]), 0.0), (1, 0, 0)), _turned(self.yaw.get(int(w[1]), 0.0), (0, 0, 1))
            return (f"ok {x:.4f} {y:.4f} {z:.4f}  {ax[0] + 0.0:.5f} 0 {ax[2] + 0.0:.5f}  0 1 0  {az[0] + 0.0:.5f} 0 {az[2] + 0.0:.5f}"
                    "  1.2000 0.5023 0.000 none")   # (+ 0.0: no "-0.00000")
        if w[0] == "remote" and w[2] == "peer":
            if int(w[1]) in self.remotes:
                return "error an old one is still ending"
            self.remotes[int(w[1])] = [f"peer-{w[3]}", w[4]]
            self.panels[int(w[1])] = {"label": " ".join(w[6:])}
            return "ok"
        if w[0] == "remote" and w[2] == "stop":
            self.panels.pop(int(w[1]), None)
            return "ok" if self.remotes.pop(int(w[1]), None) else f"error no remote screen {w[1]}"
        if w[0] in ("pose", "width", "curve"):
            if int(w[1]) not in self.panels:
                return f"error no screen {w[1]}"
            self.panels[int(w[1])][w[0]] = [float(v) for v in w[2:]]
            return "ok"
        if text == "shift back":
            for dx, dz, yaw in [s for s in reversed(self.shifted) if s != "back"]:
                c, s = math.cos(-yaw), math.sin(-yaw)
                self._shift(-(c * dx + s * dz), -(-s * dx + c * dz), -yaw)
            self.shifted = ["back"]
            return f"ok {len(self.at) + len(self.panels)}"
        if w[0] == "shift":
            dx, dz, yaw = float(w[1]), float(w[2]), math.radians(float(w[3]))
            self._shift(dx, dz, yaw)
            self.shifted.append((dx, dz, yaw))
            return f"ok {len(self.at) + len(self.panels)}"
        if w[0] == "share" and w[2] == "start":
            self.shares[int(w[1])] = (int(w[3]), w[4])
            return "ok"
        if w[0] == "share" and w[2] == "stop":
            return "ok" if self.shares.pop(int(w[1]), None) else f"error screen {w[1]} isn't shared"
        raise AssertionError(f"ft-screens was asked {text!r}")

    def _shift(self, dx, dz, yaw):
        """Every screen and every placed panel, turned `yaw` about the floor's middle and moved."""
        def moved(v):
            t = _turned(yaw, v)
            return (t[0] + dx, t[1], t[2] + dz)
        for n in self.at:
            self.at[n], self.yaw[n] = moved(self.at[n]), self.yaw.get(n, 0.0) + yaw
        for panel in self.panels.values():
            p = panel.get("pose")
            if p:
                cols = [_turned(yaw, p[k::4]) for k in range(3)] + [moved(p[3::4])]
                panel["pose"] = [cols[k][row] for row in range(3) for k in range(4)]

    def count(self, start):
        return sum(1 for a in self.asked if a.startswith(start))


def serve(screens, name):
    """Answers on the datagram socket @name from a thread of its own, for as long as the test runs."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
    sock.bind("\0" + name)
    lock = threading.Lock()

    def run():
        while True:
            data, who = sock.recvfrom(4096)
            with lock:
                reply = screens(data.decode())
            if reply is not None:
                try:
                    sock.sendto(reply.encode(), who)
                except OSError:
                    pass

    threading.Thread(target=run, daemon=True).start()
    return lock
