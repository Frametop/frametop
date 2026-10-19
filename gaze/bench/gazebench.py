#!/usr/bin/env python3
"""Frametop Gaze Bench: targets in hard places for gaze tracking, and a recording of every look
at them, so gaze techniques can be compared on the same looks afterwards (README.md).

The targets show on a panel fixed to the headset, our own ft-gazepanel in the eye recorder's
mode (gaze/rec/session.py): wherever the head turns, a target stays at the same direction from
it, so every run asks the same of the eyes. Pick a scenario (scenarios.json) in the window; the
panel shows one dot at a time. Look at the dot and press the button on the headset's right side:
where you looked is the dot. Two presses take the last one back, a hold ends the scenario. The
window's keys do the same: Space marks, Backspace takes back, S skips, Esc ends. As in Frametop's
gaze calibration, a left click, Enter, an index pinch (hand tracking) or Meta+J marks too, and a
right click or Meta+K ends: the bench claims the pointer helper's calibration panel while its
panel shows (pointer/helper/calpanel.h), so the helper answers those here and hides the pointer.

Each scenario played is a run, recorded by benchrec.py into BASE/<time>-<scenario>/, with the
panel's own log of what the dot did and when it reached the eyes, and the headset's pose. After
each mark the window works out how far our tracker and SteamVR were off (their median over the
LOOK before the press) for the summary; compare.py replays every technique on the recording.

Runs in Frametop's container (PySide6), launched by ft-gazebench. --base DIR, --no-images.
"""
import argparse
import json
import math
import os
import random
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

from PySide6.QtCore import Property, QObject, QSocketNotifier, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.dirname(HERE)
REPO = os.path.dirname(GAZE)
sys.path.insert(0, HERE)
sys.path.insert(0, REPO)
import benchrec  # noqa: E402
from recorder.button import ButtonReader, find_button, read_input_devices  # noqa: E402
from recorder.common import in_container  # noqa: E402

PANEL_DEG, PANEL_ASPECT = 64.0, 0.75   # ft-gazepanel's rec panel (kFullDeg, 4:3), as gaze/rec/session.py
MARGIN = 1.5            # degrees from the panel's edge to a target at 0 or 1, measured in the panel's plane
                        # from straight ahead: about 1.1 out at the edges, where the dot's ring (0.75
                        # out from its middle) still fits
PANEL_SOCK = os.environ.get("FT_BENCH_PANEL_SOCKET", "ft_benchpanel")   # (the tests' stand-in)
PANEL_OVERLAY = "frametop.benchpanel"
HELPER_SOCK = os.environ.get("FT_BENCH_HELPER_SOCKET", "ft_pointer_helper")   # (the tests' stand-in)
CLAIM_EVERY = 1.0       # s between claims of the helper's calibration panel (it lets one go after 3 s)
PANELS = [Path(p) for p in (os.environ.get("FT_GAZEPANEL"), os.path.join(GAZE, "build", "ft-gazepanel"),
                            Path.home() / ".local/share/frametop/releases/current/gaze/build/ft-gazepanel") if p]
IN_BOX = os.path.join(REPO, "scripts", "in-box")
PANEL_WAIT = 20.0       # s for a new panel to answer (SteamVR's overlays can take a while)
LOOK = (0.4, 0.1)       # s before a press: the gaze samples a mark takes. It ends a little before the
                        # press (a click's was 0.05 s): the push on the headset's side comes before it
DONE_S = 0.25           # s a dot marked from the window's keys shows done before the next (the
                        # button's shows done from its down, a double-press time before)
WAKE = 60               # s: the gaze service's wake lease while a run records (Input Settings' Gaze
                        # page does the same), so the trackers run whether gaze mode is on or not
# SteamVR's is the mean of its two eyes (set 2's left and right): on the 2026-10-17 runs it was
# nearer than its combined gaze (mmap1), and it's what ft-gazed's fusion corrects.
QUICK_SOURCES = (("own", "Our tracker"), ("steam-eyes", "SteamVR"))


def quick_gaze(src, key):
    """A sample's (yaw, pitch) for a QUICK_SOURCES key, or None."""
    if key == "steam-eyes":
        left, right = src.get("left") or {}, src.get("right") or {}
        if "hy" in left and "hy" in right:
            return (left["hy"] + right["hy"]) / 2, (left["hp"] + right["hp"]) / 2
        return None
    g = src.get(key) or {}
    return (g["hy"], g["hp"]) if "hy" in g and g.get("ok", 1) != 0 else None
KEYS_START = "Press the button, click, Enter or pinch to start. Hold the button or right click to end."
KEYS_END = "Press the button, click, Enter or pinch to close this."


def boxed(argv):
    """A program built in the dev container: run it there (scripts/in-box from the host)."""
    return list(argv) if in_container() or not os.path.exists(IN_BOX) else [IN_BOX] + list(argv)


def margins(margin=MARGIN):
    """MARGIN as fractions of the panel across and down."""
    half = math.tan(math.radians(PANEL_DEG / 2))
    m = math.tan(math.radians(margin)) / (2 * half)
    return m, m / PANEL_ASPECT


def panel_direction(u, v, margin=MARGIN):
    """The head-relative direction (yaw +left, pitch +up, degrees) of a target u across and v down
    the panel, 0 and 1 being margin degrees in from its edges: ft-gazepanel's ToPixel backwards."""
    half = math.tan(math.radians(PANEL_DEG / 2))
    mx, my = margins(margin)
    x, y = mx + u * (1 - 2 * mx), my + v * (1 - 2 * my)
    yaw = math.atan((0.5 - x) * 2 * half)
    pitch = math.atan((0.5 - y) * 2 * half * PANEL_ASPECT * math.cos(yaw))
    return math.degrees(yaw), math.degrees(pitch)


def panel_place(yaw, pitch, margin=MARGIN):
    """panel_direction backwards: u, v for a direction."""
    half = math.tan(math.radians(PANEL_DEG / 2))
    mx, my = margins(margin)
    y_r = math.radians(yaw)
    x = 0.5 - math.tan(y_r) / (2 * half)
    y = 0.5 - math.tan(math.radians(pitch)) / math.cos(y_r) / (2 * half) / PANEL_ASPECT
    return (x - mx) / (1 - 2 * mx), (y - my) / (1 - 2 * my)


def unit(yaw, pitch):
    """ft-gaze's head-relative yaw (+left), pitch (+up), degrees, as a unit vector (-Z ahead)."""
    y, p = math.radians(yaw), math.radians(pitch)
    return (-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))


def angle(a, b):
    """Degrees between two (yaw, pitch) directions."""
    d = sum(x * y for x, y in zip(unit(*a), unit(*b)))
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


def make_trials(sc, rng):
    """The scenario's targets, in order: dicts with u, v (fractions of the panel across and down,
    panel_direction), and maybe bg (the panel's brightness, 0 to 1) and dwell (s)."""
    spec = sc["targets"]
    pts = []
    if "grid" in spec:
        cols, rows = spec["grid"]
        lo, hi = spec.get("span", (0.2, 0.8))
        at = lambda k, n: lo + (hi - lo) * k / (n - 1) if n > 1 else 0.5  # noqa: E731
        pts = [{"u": at(i, cols), "v": at(j, rows)} for j in range(rows) for i in range(cols)]
    elif "edges" in spec:
        n = spec["edges"]
        seen = []
        for k in range(n):
            t = k / (n - 1)
            for p in ((t, 0.0), (t, 1.0), (0.0, t), (1.0, t)):
                if p not in seen:
                    seen.append(p)
        pts = [{"u": u, "v": v} for u, v in seen]
    elif "rays" in spec:
        dirs = spec.get("dirs", 8)
        for f in spec["rays"]:
            for k in range(dirs):
                a = 2 * math.pi * k / dirs
                c, s = math.cos(a), math.sin(a)
                m = max(abs(c), abs(s))  # so 1 reaches the edge, or the corner on a diagonal
                pts.append({"u": 0.5 + 0.5 * f * c / m, "v": 0.5 - 0.5 * f * s / m})
    elif "random" in spec:
        for _ in range(spec["random"]):
            u, v = rng.random(), rng.random()
            if rng.random() < spec.get("edge_share", 0.0):
                if rng.random() < 0.5:
                    u = float(round(u))
                else:
                    v = float(round(v))
            pts.append({"u": u, "v": v})
    elif "steps" in spec:
        # A walk of small steps from the middle: each target lo to hi degrees from the last, within
        # the box (degrees either side of straight ahead)
        lo, hi = spec.get("size", (2.0, 4.0))
        by, bp = spec.get("box", (12.0, 9.0))
        at = (0.0, 0.0)
        for _ in range(spec["steps"]):
            for _ in range(100):
                a, d = rng.uniform(0, 2 * math.pi), rng.uniform(lo, hi)
                nxt = (at[0] + d * math.cos(a), at[1] + d * math.sin(a))
                if abs(nxt[0]) <= by and abs(nxt[1]) <= bp:
                    break
            at = nxt
            u, v = panel_place(*at)
            pts.append({"u": u, "v": v})
    if sc.get("shuffle"):
        rng.shuffle(pts)
    for i, p in enumerate(pts):
        if sc.get("backgrounds"):
            p["bg"] = sc["backgrounds"][i % len(sc["backgrounds"])]
        if sc.get("dwell"):
            p["dwell"] = sc["dwell"]
    return pts


def git_rev():
    try:
        return subprocess.run(["git", "-C", HERE, "describe", "--always", "--dirty"], capture_output=True,
                              text=True, timeout=5).stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


class Panel:
    """Our own ft-gazepanel in its rec mode (as gaze/rec/session.py runs one): 64 degrees across,
    fixed to the headset, a solid background and one dot. Commands go to its control socket
    without waiting; it answers each one, in order, and the answers are read later (errors
    logged), or waited for by ask()."""

    def __init__(self, prog=None, log=print, no_vr=False):
        self.prog = Path(prog) if prog else next((p for p in PANELS if p.exists()), PANELS[0])
        self.log, self.no_vr = log, no_vr
        self.proc = None
        self.ready = False
        self.error = None
        self.sent = {}
        self.pending = 0     # commands sent and not answered yet
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind("")
        self.sock.setblocking(False)

    def start(self):
        """Start it and wait for its answer: call from a thread (SteamVR can take a while)."""
        self.ready, self.error = False, None
        if not self.prog.exists():
            self.error = f"{self.prog} isn't there: run gaze/build.sh in the dev container"
            return False
        argv = [str(self.prog), "--watch-stdin", "--socket", PANEL_SOCK, "--overlay", PANEL_OVERLAY]
        try:
            self.proc = subprocess.Popen(boxed(argv + (["--no-vr"] if self.no_vr else [])), stdin=subprocess.PIPE,
                                         stdout=subprocess.DEVNULL, start_new_session=True)
        except OSError as e:
            self.error = f"can't start {self.prog.name}: {e}"
            return False
        self.log(f"started {self.prog} (pid {self.proc.pid})")
        end = time.monotonic() + PANEL_WAIT
        while time.monotonic() < end:
            if self.ask("ping", 0.3):
                self.ready = True
                return True
            if self.proc.poll() is not None:
                self.error = f"{self.prog.name} exited ({self.proc.returncode}): is SteamVR running?"
                return False
        self.error = f"{self.prog.name} doesn't answer"
        self.stop()
        return False

    def alive(self):
        return self.ready and self.proc is not None and self.proc.poll() is None

    def _got(self, r):
        self.pending = max(0, self.pending - 1)
        if r.startswith("error"):
            self.log(f"panel: {r}")

    def _drain(self):
        while True:
            try:
                r = self.sock.recv(65536).decode(errors="replace")
            except OSError:
                return
            self._got(r)

    def cmd(self, text):
        with self.lock:
            self._drain()
            try:
                self.sock.sendto(text.encode(), "\0" + PANEL_SOCK)
                self.pending += 1
            except OSError:
                pass

    def ask(self, text, timeout=0.5):
        """Send, and its answer (or None): the one after those of the commands still unanswered."""
        with self.lock:
            self._drain()
            try:
                self.sock.sendto(text.encode(), "\0" + PANEL_SOCK)
            except OSError:
                return None
            self.pending += 1
            end = time.monotonic() + timeout
            while (left := end - time.monotonic()) > 0:
                try:
                    self.sock.settimeout(left)
                    r = self.sock.recv(65536).decode(errors="replace")
                except OSError:
                    break
                finally:
                    self.sock.setblocking(False)
                last = self.pending == 1
                self._got(r)
                if last:
                    return r
            self.pending = 0   # a lost answer: count afresh
            return None

    def set(self, key, text):
        """Send only if it changes that part of the panel."""
        if self.sent.get(key) != text:
            self.sent[key] = text
            self.cmd(text)

    def show(self, bg):
        self.cmd("show rec")   # an empty panel
        self.sent = {}
        self.set("bg", f"bg {bg:.3f}")

    def lines(self, title="", text="", keys="", step=""):
        for key, value in (("title", title), ("text", text), ("keys", keys), ("step", step)):
            self.set(key, f"{key} {value}" if value else key)

    def dot(self, yaw, pitch, state, progress=None):
        self.set("dot", f"dot {yaw:.4f} {pitch:.4f} {state}" + ("" if progress is None else f" {progress:.2f}"))

    def close(self):
        self.sock.close()

    def stop(self):
        p, self.proc, self.ready = self.proc, None, False
        if not p:
            return
        try:
            p.stdin.close()   # --watch-stdin: it quits
            p.wait(5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                os.killpg(p.pid, signal.SIGTERM)
                p.wait(3)
            except (OSError, subprocess.TimeoutExpired):
                p.kill()
        self.log(f"panel stopped (exit {p.returncode})")


def text_line(text):
    """One line for the panel's socket: its own line breaks are "|"."""
    return " ".join(text.replace("\n", "|").split())


class Bench(QObject):
    changed = Signal()
    statusChanged = Signal()
    # The headset button's events, from its reader's thread: "down", "press", "double", "hold",
    # and the raw time of the press they're about (its down)
    button = Signal(str, float)

    def __init__(self, scenarios, base, images, gaze_prog=None, panel_prog=None, panel_no_vr=False, button=None):
        super().__init__()
        self.all = scenarios
        self.base = base
        self._images = images
        self._page = "menu"
        self._status = "Looking for the gaze service..."
        self._progress = self._message = self._summary = self._last = self._notice = ""
        self.bg = float(scenarios.get("background", 0.15))
        self.gaze = benchrec.GazeStream(prog=gaze_prog, log=self.log).start()
        self.gazed = benchrec.Sock(benchrec.GAZED_SOCK)
        self.panel = Panel(panel_prog, self.log, panel_no_vr)
        self.panel_starting = None
        self.run = self.sc = None
        self.stopping = None       # the last run's stop (it copies the logs), for shutdown to wait for
        self.trials, self.i, self.results = [], 0, []
        # A run's phase: intro (its message, until a press), target (a dot to look at), marked (its
        # done shows), gap (no dot, a scenario's gap), end (the summary on the panel, until a press)
        self.phase = None
        self.target = None         # the dot showing: (yaw, pitch)
        self.shown_t = self.ready_t = None
        self.flashed = False       # the button's down showed this dot done before its press came
        self.next_at = None        # monotonic: when the marked or gap phase ends
        self.rev = git_rev()
        self.button.connect(self.on_button)
        self.down_t = None         # the reader's thread: its last down's raw time
        self.reader = None
        path = find_button(read_input_devices()) if button is None else button
        if path:
            self.reader = ButtonReader(path, self._gesture, self.log, on_down=self._down).start()
        self.button_path = path or None
        # The pointer helper's calibration panel (see the top): claimed as @ft_gazebench.PID while
        # a run or its summary shows; its answers ("calaccept", "calquit") come back there.
        self.claim_name = f"ft_gazebench.{os.getpid()}"
        self.claim_at = None       # monotonic: the last claim; None while not claiming
        self.helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC)
        self.helper_note = None
        try:
            self.helper.bind("\0" + self.claim_name)
            self.helper_note = QSocketNotifier(self.helper.fileno(), QSocketNotifier.Type.Read, self)
            self.helper_note.activated.connect(self.from_helper)
        except OSError as e:
            self.log(f"pointer helper socket: {e} (clicks, Enter and pinches won't mark)")
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(50)
        self.status_at = 0.0
        self.wake_at = 0.0
        self.status_busy = False
        self.status_thread = None
        self.start_panel()

    def log(self, line):
        print("ft-gazebench: " + line, file=sys.stderr, flush=True)

    def start_panel(self):
        if self.panel_starting and self.panel_starting.is_alive():
            return
        self.panel_starting = threading.Thread(target=self._start_panel, daemon=True)
        self.panel_starting.start()

    def _start_panel(self):
        if not self.panel.start():
            self.log(f"panel: {self.panel.error}")
        self.refresh_status()

    # --- properties for bench.qml

    def _get(name):  # noqa: N805
        return lambda self: getattr(self, name)

    page = Property(str, _get("_page"), notify=changed)
    progress = Property(str, _get("_progress"), notify=changed)
    message = Property(str, _get("_message"), notify=changed)
    summary = Property(str, _get("_summary"), notify=changed)
    last = Property(str, _get("_last"), notify=changed)
    notice = Property(str, _get("_notice"), notify=changed)
    status = Property(str, _get("_status"), notify=statusChanged)

    def _set_images(self, v):
        self._images = bool(v)
        self.changed.emit()

    images = Property(bool, _get("_images"), _set_images, notify=changed)

    @Property("QVariantList", constant=True)
    def scenarios(self):
        out = []
        for sc in self.all["scenarios"]:
            n = len(make_trials(sc, random.Random(0)))
            out.append({"id": sc["id"], "name": sc["name"], "about": sc["about"], "count": n})
        return out

    # --- status (in a thread: the sockets wait for replies)

    def refresh_status(self):
        if self.status_busy:
            return
        self.status_busy = True
        self.status_thread = threading.Thread(target=self._status_thread, daemon=True)
        self.status_thread.start()

    def _status_thread(self):
        try:
            self._status = self.status_text()
        finally:
            self.status_busy = False
        self.statusChanged.emit()

    def status_text(self):
        lines = []
        if self.panel.alive():
            lines.append("Headset panel: ready")
        elif self.panel_starting and self.panel_starting.is_alive():
            lines.append("Headset panel: starting...")
        else:
            lines.append(f"Headset panel: not running ({self.panel.error or 'it stopped'}). Starting a scenario tries again.")
        lines.append(f"Headset button: {self.button_path}" if self.button_path else
                     "Headset button: not found. Click, press Enter or pinch, or use this window's keys (Space marks).")
        g = self.gazed.json("status")
        if g is None:
            lines.append("The gaze service isn't answering (frametop-gaze.service).")
        else:
            tracker = "our tracker" if g.get("tracker") == "own" else "SteamVR's tracker"
            checks = g.get("checks") or {}
            lines.append(f"Gaze service: {tracker}" + ("; gaze mode is on (the bench doesn't need it)"
                                                        if checks.get("gaze_mode") else ""))
            if checks.get("headset_worn") is False:
                lines.append("Nobody is wearing the headset.")
            if g.get("tracker") == "own":
                made = g.get("calibration_made")
                lines.append(f"Our tracker: calibrated {made}" if made else "Our tracker: not calibrated yet")
        s = self.gaze.latest()
        if self.gaze.error:
            lines.append(f"ft-gaze: {self.gaze.error}")
        elif s is None or benchrec.raw_now() - s.get("t", 0) > 2:
            lines.append("ft-gaze: no samples yet (is SteamVR running?)")
        else:
            src = s.get("src", {})
            ok = [n for k, n in QUICK_SOURCES if quick_gaze(src, k)]
            lines.append("Gaze now from: " + (", ".join(ok) or "nothing (eyes closed, or the headset off?)"))
        lines.append("Eye images: " + ("recorded (they stay on this headset)" if self._images else "off"))
        lines.append(f"Runs go to {self.base}")
        return "\n".join(lines)

    # --- the headset button (its thread: to the Qt thread through the signal)

    def _down(self, t):
        self.down_t = benchrec.raw_now() - (time.monotonic() - t)
        self.button.emit("down", self.down_t)

    def _gesture(self, gesture):
        self.button.emit(gesture, self.down_t or benchrec.raw_now())

    @Slot(str, float)
    def on_button(self, what, t):
        if not self.run and self.phase != "end":
            return
        if what == "down":
            # Done at once, so the press feels taken; it counts when "press" comes (a double is undo)
            if self.phase == "target" and self.ready_t is not None and t >= self.ready_t:
                self.panel.dot(*self.target, "done")
                self.flashed = True
        elif what == "press":
            if self.phase == "end":
                self.close_panel()
            elif self.phase == "intro":
                self.begin()
            else:
                self.mark(t, "button")
        elif what == "double":
            self.undo()
        elif what == "hold" and self.run:
            self.log("headset button: hold (end)")
            self.finish(early=True)

    # --- the pointer helper's calibration panel: clicks, Enter and pinches

    def claim(self):
        """Claim the helper's panel while a run or its summary shows on ours, and let it go after."""
        want = (self.run is not None or self.phase == "end") and self.panel.alive()
        now = time.monotonic()
        if want and (self.claim_at is None or now - self.claim_at >= CLAIM_EVERY):
            self.claim_at = now
            self.to_helper(f"calpanel 1 {self.claim_name}")
        elif not want and self.claim_at is not None:
            self.claim_at = None
            self.to_helper(f"calpanel 0 {self.claim_name}")

    def to_helper(self, text):
        try:
            self.helper.sendto(text.encode(), "\0" + HELPER_SOCK)
        except OSError:
            pass  # the helper isn't running (SteamVR isn't)

    @Slot()
    def from_helper(self):
        while True:
            try:
                data = self.helper.recv(256)
            except OSError:
                return
            t = benchrec.raw_now()
            what = data.decode(errors="replace").strip()
            if self.claim_at is None:
                continue  # a late answer: the panel is gone
            if what == "calaccept":     # a press: as the button's, at its arrival
                if self.phase == "end":
                    self.close_panel()
                elif self.phase == "intro":
                    self.begin()
                elif self.run:
                    self.mark(t, "helper")
            elif what == "calquit":     # as holding the button
                if self.run:
                    self.log("pointer helper: quit (end)")
                    self.finish(early=True)
                else:
                    self.close_panel()

    # --- a run

    @Slot(str)
    def start(self, sid):
        if self.run:
            return
        if not self.panel.alive():
            self._notice = ("The headset panel isn't running" + (f": {self.panel.error}" if self.panel.error else "")
                            + ". Trying again; start the scenario once it's ready.")
            self.start_panel()
            self.refresh_status()
            self.changed.emit()
            return
        self.sc = next(s for s in self.all["scenarios"] if s["id"] == sid)
        seed = self.sc.get("seed", random.randrange(1 << 30))
        self.trials = make_trials(self.sc, random.Random(seed))
        self.i, self.results = 0, []
        info = {"display": "panel", "panel": {"deg": PANEL_DEG, "aspect": PANEL_ASPECT, "margin": MARGIN,
                                              "prog": str(self.panel.prog)},
                "seed": seed, "look": LOOK, "app": {"rev": self.rev}, "trials_planned": len(self.trials),
                "button": self.button_path}
        self.run = benchrec.Run(self.sc, self.gaze, images=self._images, base=self.base, log=self.log, info=info).start()
        r = self.panel.ask(f"log start {self.run.dir}")
        if not (r or "").startswith("ok"):
            self.log(f"panel log: {r or 'no answer'}")
            self.run.info["panel"]["log_error"] = r or "no answer"
            self.run.write_info()
        self._page, self._summary, self._last, self._notice = "run", "", "", ""
        prompt = self.sc.get("prompt", self.all.get("prompt", "Look at the dot and press the button (or click, Enter or pinch)."))
        intro = [self.sc["message"]] if self.sc.get("message") else []
        self._message = "\n".join(intro + [prompt])
        self.phase, self.target, self.next_at = "intro", None, None
        self.panel.show(self.bg)
        self.panel.dot(0, 0, "off")
        self.panel.lines(self.sc["name"], text_line(self._message), KEYS_START, f"{len(self.trials)} targets")
        self._progress = f"{self.sc['name']}: {len(self.trials)} targets. Press the headset button (or Space) to start."
        self.run.trial({"kind": "message", "text": self._message, "t": benchrec.raw_now()})
        self.wake_at = 0.0
        self.claim()
        self.changed.emit()

    def begin(self):
        """The intro read: the first target."""
        self.run.trial({"kind": "message-done", "t": benchrec.raw_now()})
        self.panel.lines()
        self.show(self.i)

    @Slot()
    def again(self):
        if self._page == "done" and self.sc:
            self.start(self.sc["id"])

    def show(self, i):
        self.i = i
        if i >= len(self.trials):
            return self.finish()
        p = self.trials[i]
        self.target = panel_direction(p["u"], p["v"])
        bg = float(p.get("bg", self.bg))
        self.panel.set("bg", f"bg {bg:.3f}")
        self.shown_t = benchrec.raw_now()
        dwell = p.get("dwell")
        self.ready_t = None if dwell else self.shown_t
        self.panel.dot(*self.target, "capture" if dwell else "look", 0.0 if dwell else None)
        self.phase, self.flashed, self.next_at = "target", False, None
        self._progress = f"{self.sc['name']}: {i + 1} of {len(self.trials)}"
        self.changed.emit()

    def quick(self, t, target):
        """How far our tracker and SteamVR were from the target: their median direction over the
        LOOK before t, {source: {"yaw", "pitch", "n", "deg"}}."""
        samples = self.gaze.window(t - LOOK[0], t - LOOK[1])
        out = {}
        for key, _ in QUICK_SOURCES:
            rows = [g for g in (quick_gaze(s.get("src") or {}, key) for s in samples) if g]
            if len(rows) < 3:
                continue
            g = (statistics.median(r[0] for r in rows), statistics.median(r[1] for r in rows))
            out[key] = {"yaw": round(g[0], 2), "pitch": round(g[1], 2), "n": len(rows), "deg": round(angle(g, target), 2)}
        return out

    @Slot()
    def markKey(self):
        """Space in the window: a mark now."""
        if self.phase == "intro":
            self.begin()
        elif self.phase == "end":
            self.close_panel()
        elif self.run:
            self.mark(benchrec.raw_now(), "key")

    def mark(self, t, by):
        """You were looking at the dot at t (a press's down)."""
        if self.phase != "target" or t < self.shown_t:
            self.run.trial({"kind": "stray", "t": t, "by": by, "phase": self.phase})
            return
        if self.ready_t is None or t < self.ready_t:
            self.run.trial({"kind": "early", "i": self.i, "t": t, "by": by})
            return
        p = self.trials[self.i]
        yaw, pitch = self.target
        rec = {"kind": "trial", "i": self.i, "u": p["u"], "v": p["v"], "yaw": round(yaw, 4), "pitch": round(pitch, 4),
               "bg": float(p.get("bg", self.bg)), "shown_t": self.shown_t, "ready_t": self.ready_t,
               "click": {"t": t, "by": by}, "truth": {"yaw": round(yaw, 4), "pitch": round(pitch, 4), "from": "target"}}
        rec["quick"] = self.quick(t, self.target)
        self.results.append(rec)
        self.run.trial(rec)
        self._last = "Last: " + ", ".join(f"{label} {rec['quick'][k]['deg']:.1f}° off" if k in rec["quick"] else
                                          f"{label} no gaze" for k, label in QUICK_SOURCES)
        self.panel.dot(yaw, pitch, "done")
        self.after(0.0 if self.flashed else DONE_S)

    def after(self, wait):
        """The next target, after wait (and the scenario's gap)."""
        self.i += 1
        self.phase, self.next_at = "marked", time.monotonic() + wait
        self.changed.emit()

    @Slot()
    def skip(self):
        if self.phase == "target":
            p = self.trials[self.i]
            self.run.trial({"kind": "trial", "i": self.i, "u": p["u"], "v": p["v"], "yaw": round(self.target[0], 4),
                            "pitch": round(self.target[1], 4), "shown_t": self.shown_t,
                            "click": {"t": benchrec.raw_now(), "by": "key"}, "skipped": True})
            self.panel.dot(*self.target, "off")
            self.after(0.0)

    @Slot()
    def undo(self):
        """Back to the last target (self.i is the one showing, or the next one while marked or in
        a gap)."""
        if not self.run or self.phase in ("intro", "end"):
            return
        back = self.i - 1
        if back < 0:
            if self.phase == "target" and self.flashed:   # the first dot's down: show it again
                self.show(self.i)
            return
        if self.results and self.results[-1]["i"] == back:
            self.results.pop()
        self.run.trial({"kind": "undo", "i": back, "t": benchrec.raw_now()})
        self._last = "Took the last one back"
        self.show(back)

    @Slot()
    def stop(self):
        if self.run:
            self.finish(early=True)
        elif self._page == "done":
            self.close_panel()
            self._page = "menu"
            self.changed.emit()
        else:
            QGuiApplication.quit()

    def finish(self, early=False, why=None):
        summary = self.summarize(early, why)
        self.phase, self.next_at = "end", None
        self._page = "done"
        self._summary = summary["text"]
        self.changed.emit()
        run, self.run = self.run, None
        run.info["panel"]["log_rows"] = self.panel.ask("log stop") if self.panel.alive() else None
        if self.panel.alive():
            self.panel.dot(0, 0, "off")
            self.panel.set("bg", f"bg {self.bg:.3f}")
            self.panel.lines("Ended early" if early else "Done", text_line(summary["panel"]), KEYS_END)
        self.stopping = threading.Thread(target=lambda: run.stop(summary), daemon=True)
        self.stopping.start()

    def close_panel(self):
        if self.phase == "end":
            self.phase = None
            self.panel.cmd("hide")
            self.claim()

    def summarize(self, early, why=None):
        marks = [r for r in self.results if not r.get("skipped")]
        lines = [f"{self.sc['name']}: {len(marks)} marked" + (" (ended early)" if early else "")]
        if why:
            lines.append(why)
        out = {"marks": len(marks), "early": early}
        for key, label in QUICK_SOURCES:
            errs = sorted(r["quick"][key]["deg"] for r in marks if key in r.get("quick", {}))
            if errs:
                med, worst = statistics.median(errs), errs[-1]
                p90 = errs[min(len(errs) - 1, int(0.9 * len(errs)))]
                out[key] = {"median": med, "p90": p90, "worst": worst, "n": len(errs)}
                lines.append(f"{label}: {med:.1f}° off at the median, {p90:.1f}° at the 90th percentile, worst {worst:.1f}°")
            else:
                lines.append(f"{label}: no gaze at the marks")
        out["panel"] = "\n".join(lines)
        if self.run:
            lines.append(f"Saved in {self.run.dir}")
        out["text"] = "\n".join(lines)
        return out

    # --- timers

    def tick(self):
        now = time.monotonic()
        if self._page != "run" and now - self.status_at > 2:
            self.status_at = now
            self.refresh_status()
        self.claim()
        if not self.run:
            return
        if now - self.wake_at > WAKE / 2:
            self.wake_at = now
            threading.Thread(target=lambda: self.gazed.ask(f"wake {WAKE}"), daemon=True).start()
        self.run.keepalive()
        if not self.panel.alive():
            self.log("the panel stopped")
            return self.finish(early=True, why="The headset panel stopped.")
        if self.phase == "target" and self.ready_t is None:
            dwell = self.trials[self.i]["dwell"]
            done = min(1.0, (benchrec.raw_now() - self.shown_t) / dwell)
            self.panel.dot(*self.target, "capture", round(done, 2))
            if done >= 1.0:
                self.ready_t = benchrec.raw_now()
        if self.next_at is not None and now >= self.next_at:
            gap = self.sc.get("gap")
            if self.phase == "marked" and gap and self.i < len(self.trials):
                self.panel.dot(*self.target, "off")
                self.phase, self.next_at = "gap", now + random.uniform(*gap)
            else:
                self.show(self.i)

    def shutdown(self):
        self.timer.stop()
        if self.run:
            self.finish(early=True)
        if self.stopping:
            self.stopping.join(10)
        if self.reader:
            self.reader.stop()
        if self.panel.alive():
            self.panel.cmd("hide")
        if self.claim_at is not None:
            self.to_helper(f"calpanel 0 {self.claim_name}")
            self.claim_at = None
        if self.helper_note:
            self.helper_note.setEnabled(False)
        self.helper.close()
        for t in (self.panel_starting, self.status_thread):
            if t:
                t.join(PANEL_WAIT + 1)
        self.panel.stop()
        self.panel.close()
        self.gaze.stop()
        self.gazed.close()


def main():
    ap = argparse.ArgumentParser(description="Frametop Gaze Bench")
    ap.add_argument("--base", default=str(benchrec.BASE), help="where runs are recorded")
    ap.add_argument("--no-images", action="store_true", help="don't record the eye cameras")
    ap.add_argument("--scenarios", default=os.path.join(HERE, "scenarios.json"))
    ap.add_argument("--ft-gaze", help="the ft-gaze to run (default: this tree's build, else the installed release's)")
    ap.add_argument("--ft-gazepanel", help="the ft-gazepanel to run (default: this tree's build, else the release's)")
    ap.add_argument("--panel-no-vr", action="store_true", help="the panel without SteamVR (its --no-vr)")
    ap.add_argument("--no-button", action="store_true", help="don't read the headset button")
    ap.add_argument("--style", default="Fusion", help="the Qt Quick Controls style")
    a, qt_args = ap.parse_known_args()
    app = QGuiApplication([sys.argv[0]] + qt_args)
    app.setApplicationName("ft-gazebench")
    app.setApplicationDisplayName("Frametop Gaze Bench")
    app.setDesktopFileName("ft-gazebench")
    QQuickStyle.setStyle(a.style)
    with open(a.scenarios) as f:
        scenarios = json.load(f)
    bench = Bench(scenarios, a.base, not a.no_images, gaze_prog=a.ft_gaze, panel_prog=a.ft_gazepanel,
                  panel_no_vr=a.panel_no_vr, button=False if a.no_button else None)
    app.aboutToQuit.connect(bench.shutdown)
    # A stop (the launcher's container doesn't pass one on, but kill does) ends a run properly
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: QGuiApplication.quit())
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("bench", bench)
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, "bench.qml")))
    if not engine.rootObjects():
        sys.exit(1)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
