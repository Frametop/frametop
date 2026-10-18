#!/usr/bin/env python3
"""Frametop Gaze Bench: targets in hard places for the gaze pointer, and a recording of every
click on them, so gaze techniques can be compared on the same looks afterwards (README.md).

A full-screen window on a Frametop screen. Pick a scenario (scenarios.json); it shows one
target at a time. With gaze mode on, look at the target: the pointer goes where the tracker
thinks you look. Then click it, nudging the pointer onto it first if it's off (gaze mode's
left button: press, steer with the mouse, let go). Where you looked is the target's middle
when the click lands on the target (within HIT of it), else the click: if you were really
looking somewhere else, click there. A right click (or S) skips a target, Backspace takes
the last one back, Esc ends the scenario.

Each scenario played is a run, recorded by benchrec.py into BASE/<time>-<scenario>/. After
each click the window works out how far our tracker and SteamVR were off (from ft-gaze's
screen hits just before it) for the summary; compare.py replays every technique on the
recording.

Runs in Frametop's container (PySide6), launched by ft-gazebench. --base DIR, --no-images.
"""
import argparse
import json
import math
import os
import random
import signal
import statistics
import subprocess
import sys
import threading
import time

from PySide6.QtCore import Property, QObject, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import benchrec  # noqa: E402

RADIUS = 14             # px (the window's): a target ring's radius
INSET = RADIUS + 6      # px from the screen's edge to a target at 0 or 1
HIT = 2 * RADIUS        # px: a click this close to the target's middle was a look at the target
LOOK = (0.35, 0.05)     # s before a click: the gaze samples a quick score takes (the eyes are on the
                        # target while the pointer arrives)
WAKE = 60               # s: the gaze service's wake lease while a run records (Input Settings' Gaze
                        # page does the same), so the trackers keep running if gaze mode goes off
QUICK_SOURCES = (("own", "Our tracker"), ("mmap1", "SteamVR"))


def make_trials(sc, rng):
    """The scenario's targets, in order: dicts with u, v (fractions of the screen), and maybe bg,
    dwell, decoy (dx, dy in px from the target)."""
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
    elif "pairs" in spec:
        gap = spec.get("gap", 40)
        for _ in range(spec["pairs"]):
            side = rng.choice((-1, 1))
            across = rng.random() < 0.5
            pts.append({"u": rng.uniform(0.15, 0.85), "v": rng.uniform(0.15, 0.85),
                        "decoy": {"dx": side * gap if across else 0, "dy": 0 if across else side * gap}})
    if sc.get("shuffle"):
        rng.shuffle(pts)
    for i, p in enumerate(pts):
        if sc.get("backgrounds"):
            p["bg"] = sc["backgrounds"][i % len(sc["backgrounds"])]
        if sc.get("dwell"):
            p["dwell"] = sc["dwell"]
    return pts


def lightness(color):
    """#rrggbb's HSL lightness, 0 to 1."""
    try:
        rgb = [int(color.lstrip("#")[k:k + 2], 16) / 255 for k in (0, 2, 4)]
    except ValueError:
        return 0.0
    return (max(rgb) + min(rgb)) / 2


def git_rev():
    try:
        return subprocess.run(["git", "-C", HERE, "describe", "--always", "--dirty"], capture_output=True,
                              text=True, timeout=5).stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


class Bench(QObject):
    changed = Signal()
    statusChanged = Signal()

    def __init__(self, scenarios, base, images, gaze_prog=None):
        super().__init__()
        self.all = scenarios
        self.base = base
        self._images = images
        self._page = "menu"
        self._status = "Looking for the gaze service..."
        self._prompt = self._progress = self._message = self._summary = ""
        self._bg = scenarios.get("background", "#262626")
        self._tx = self._ty = self._dx = self._dy = 0.0
        self._target = self._ready = self._decoy = self._mark = False
        self._hx = self._hy = 0.0
        self._head = False
        self.screen = {}
        self.gaze = benchrec.GazeStream(prog=gaze_prog, log=self.log).start()
        self.gazed = benchrec.Sock(benchrec.GAZED_SOCK)
        self.run = self.sc = None
        self.stopping = None       # the last run's stop (it copies the logs), for shutdown to wait for
        self.trials, self.i, self.results = [], 0, []
        self.shown_t = self.ready_t = None
        self.next_at = None        # monotonic: when the next target shows (a scenario's gap)
        self.waiting_message = False
        self.screen_votes = {}     # ft-screens' screen: samples whose head ray hit it, this run
        self.rev = git_rev()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(50)
        self.status_at = 0.0
        self.wake_at = 0.0
        self.status_busy = False

    def log(self, line):
        print("ft-gazebench: " + line, file=sys.stderr, flush=True)

    # --- properties for bench.qml

    def _get(name):  # noqa: N805
        return lambda self: getattr(self, name)

    page = Property(str, _get("_page"), notify=changed)
    prompt = Property(str, _get("_prompt"), notify=changed)
    progress = Property(str, _get("_progress"), notify=changed)
    message = Property(str, _get("_message"), notify=changed)
    summary = Property(str, _get("_summary"), notify=changed)
    background = Property(str, _get("_bg"), notify=changed)
    bright = Property(bool, lambda self: self._page == "run" and lightness(self._bg) > 0.6, notify=changed)
    tx = Property(float, _get("_tx"), notify=changed)
    ty = Property(float, _get("_ty"), notify=changed)
    dx = Property(float, _get("_dx"), notify=changed)
    dy = Property(float, _get("_dy"), notify=changed)
    targetOn = Property(bool, _get("_target"), notify=changed)
    ready = Property(bool, _get("_ready"), notify=changed)
    decoyOn = Property(bool, _get("_decoy"), notify=changed)
    markOn = Property(bool, _get("_mark"), notify=changed)
    status = Property(str, _get("_status"), notify=statusChanged)
    headChanged = Signal()
    hx = Property(float, _get("_hx"), notify=headChanged)
    hy = Property(float, _get("_hy"), notify=headChanged)
    headOn = Property(bool, _get("_head"), notify=headChanged)
    radius = Property(int, lambda self: RADIUS, constant=True)

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

    # --- the screen

    @Slot(float, float, float, str, int, int)
    def setScreen(self, w, h, dpr, name, x, y):
        self.screen = {"w": w, "h": h, "dpr": dpr, "name": name, "x": x, "y": y}

    def place(self, u, v):
        w, h = self.screen.get("w", 1920), self.screen.get("h", 1080)
        return INSET + u * (w - 2 * INSET), INSET + v * (h - 2 * INSET)

    # --- status (in a thread: the sockets wait for replies)

    def refresh_status(self):
        if self.status_busy:
            return
        self.status_busy = True
        threading.Thread(target=self._status_thread, daemon=True).start()

    def _status_thread(self):
        try:
            self._status = self.status_text()
        finally:
            self.status_busy = False
        self.statusChanged.emit()

    def status_text(self):
        lines = []
        g = self.gazed.json("status")
        if g is None:
            lines.append("The gaze service isn't answering (frametop-gaze.service).")
        else:
            tracker = "our tracker" if g.get("tracker") == "own" else "SteamVR's tracker"
            checks = g.get("checks") or {}
            lines.append(f"Gaze service: {tracker}; gaze mode " + ("on" if checks.get("gaze_mode") else
                         "off: turn it on to point with your eyes"))
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
            ok = [n for k, n in QUICK_SOURCES if src.get(k, {}).get("ok", 1) != 0 and "hy" in src.get(k, {})]
            lines.append("Gaze now from: " + (", ".join(ok) or "nothing (eyes closed, or the headset off?)"))
        lines.append("Eye images: " + ("recorded (they stay on this headset)" if self._images else "off"))
        lines.append(f"Runs go to {self.base}")
        return "\n".join(lines)

    # --- a run

    @Slot(str)
    def start(self, sid):
        self.sc = next(s for s in self.all["scenarios"] if s["id"] == sid)
        seed = self.sc.get("seed", random.randrange(1 << 30))
        self.trials = make_trials(self.sc, random.Random(seed))
        self.i, self.results, self.screen_votes = 0, [], {}
        info = {"screen": self.screen, "seed": seed, "radius": RADIUS, "inset": INSET, "look": LOOK,
                "app": {"rev": self.rev}, "trials_planned": len(self.trials)}
        self.run = benchrec.Run(self.sc, self.gaze, images=self._images, base=self.base, log=self.log, info=info).start()
        self._page, self._summary = "run", ""
        self._mark = bool(self.sc.get("head_mark"))
        self._prompt = self.sc.get("prompt", self.all.get("prompt", "Look at the target, then click it."))
        # Every run opens with what to do (and gives ft-gaze and our tracker's log a moment)
        intro = [self.sc["message"]] if self.sc.get("message") else []
        self._message = "\n".join(intro + [self._prompt, "", "Click anywhere to start. Esc ends it early."])
        self.waiting_message = True
        self._target = self._decoy = False
        self._bg = self.all.get("background", "#262626")
        self._progress = f"{self.sc['name']}: {len(self.trials)} targets"
        self.run.trial({"kind": "message", "text": self._message, "t": benchrec.raw_now()})
        self.wake_at = 0.0
        self.changed.emit()

    @Slot()
    def again(self):
        if self._page == "done" and self.sc:
            self.start(self.sc["id"])

    def show(self, i):
        self.i = i
        if i >= len(self.trials):
            return self.finish()
        p = self.trials[i]
        self._tx, self._ty = self.place(p["u"], p["v"])
        d = p.get("decoy")
        self._decoy = bool(d)
        if d:
            w, h = self.screen.get("w", 1920), self.screen.get("h", 1080)
            self._dx = min(max(self._tx + d["dx"], INSET), w - INSET)
            self._dy = min(max(self._ty + d["dy"], INSET), h - INSET)
        self._bg = p.get("bg", self.all.get("background", "#262626"))
        self._target = True
        self.shown_t = benchrec.raw_now()
        self._ready = not p.get("dwell")
        self.ready_t = self.shown_t if self._ready else None
        self._progress = f"{self.sc['name']}: {i + 1} of {len(self.trials)}"
        self.next_at = None
        self.changed.emit()

    def bench_screen(self):
        """ft-screens' number for the screen the window is on: the one the head faced most."""
        return max(self.screen_votes, key=self.screen_votes.get) if self.screen_votes else None

    def vote(self, samples):
        for s in samples:
            hit = (s.get("head") or {}).get("hit")
            if hit:
                self.screen_votes[hit["s"]] = self.screen_votes.get(hit["s"], 0) + 1

    def quick(self, t, cx, cy):
        """How far our tracker and SteamVR were from where you looked (cx, cy: the window's
        pixels), from ft-gaze's screen hits in the LOOK before the click: {"screen": N, source:
        {"x", "y", "n", "deg"}}. Screen pixels are ft-screens' (the window's times its pixel
        ratio)."""
        samples = self.gaze.window(t - LOOK[0], t - LOOK[1])
        self.vote(samples)
        screen = self.bench_screen()
        if screen is None:
            return {}
        dpr = self.screen.get("dpr", 1.0) or 1.0
        out = {"screen": screen}
        for key, _ in QUICK_SOURCES:
            hits = [s["src"][key]["hit"] for s in samples
                    if (s.get("src", {}).get(key) or {}).get("hit") and s["src"][key]["hit"]["s"] == screen]
            if len(hits) < 3:
                continue
            x = statistics.median(h["x"] for h in hits)
            y = statistics.median(h["y"] for h in hits)
            dpp = statistics.median(h["dpp"] for h in hits)
            out[key] = {"x": round(x, 1), "y": round(y, 1), "n": len(hits),
                        "deg": round(math.hypot(x - cx * dpr, y - cy * dpr) * dpp, 2)}
        return out

    @Slot(float, float, str)
    def click(self, x, y, button):
        if self._page != "run":
            return
        t = benchrec.raw_now()
        if self.waiting_message:
            self.waiting_message, self._message = False, ""
            self.run.trial({"kind": "message-done", "t": t})
            self.show(self.i)
            return
        if not self._target:
            self.run.trial({"kind": "stray", "t": t, "x": x, "y": y, "button": button})
            return
        if not self._ready:
            self.run.trial({"kind": "early", "i": self.i, "t": t, "x": x, "y": y})
            return
        p = self.trials[self.i]
        rec = {"kind": "trial", "i": self.i, "u": p["u"], "v": p["v"], "x": round(self._tx, 1), "y": round(self._ty, 1),
               "bg": self._bg, "shown_t": self.shown_t, "ready_t": self.ready_t,
               "click": {"x": round(x, 1), "y": round(y, 1), "t": t, "button": button}}
        if self._decoy:
            rec["decoy"] = {"x": round(self._dx, 1), "y": round(self._dy, 1)}
        if button == "right":
            rec["skipped"] = True
        else:
            off = math.hypot(x - self._tx, y - self._ty)
            on = off <= HIT
            rec["off_px"] = round(off, 1)
            rec["truth"] = {"x": rec["x"] if on else rec["click"]["x"], "y": rec["y"] if on else rec["click"]["y"],
                            "from": "target" if on else "click"}
            rec["quick"] = self.quick(t, rec["truth"]["x"], rec["truth"]["y"])
            self.results.append(rec)
        self.run.trial(rec)
        self.advance()

    def advance(self):
        gap = self.sc.get("gap")
        if gap and self.i + 1 < len(self.trials):
            self._target = self._decoy = False
            self.next_at = time.monotonic() + random.uniform(*gap)
            self.i += 1
            self.changed.emit()
        else:
            self.show(self.i + 1)

    @Slot()
    def skip(self):
        if self._page == "run" and self._target:
            self.click(self._tx, self._ty, "right")

    @Slot()
    def undo(self):
        """Back to the last target (self.i is the one showing, or next after a gap)."""
        if self._page != "run" or self.waiting_message:
            return
        back = self.i - 1
        if back < 0:
            return
        if self.results and self.results[-1]["i"] == back:
            self.results.pop()
        self.run.trial({"kind": "undo", "i": back, "t": benchrec.raw_now()})
        self.show(back)

    @Slot()
    def stop(self):
        if self._page == "run":
            self.finish(early=True)
        elif self._page == "done":
            self._page = "menu"
            self.changed.emit()
        else:
            QGuiApplication.quit()

    def finish(self, early=False):
        summary = self.summarize(early)
        self._target = self._decoy = self._mark = self._head = False
        self.headChanged.emit()
        self._message, self.next_at = "", None
        self._bg = self.all.get("background", "#262626")
        self._page = "done"
        self._summary = summary["text"]
        self.changed.emit()
        run, self.run = self.run, None
        self.stopping = threading.Thread(target=lambda: run.stop(summary), daemon=True)
        self.stopping.start()

    def summarize(self, early):
        clicks = [r for r in self.results if not r.get("skipped")]
        lines = [f"{self.sc['name']}: {len(clicks)} clicks" + (" (ended early)" if early else "")]
        out = {"clicks": len(clicks), "early": early}
        for key, label in QUICK_SOURCES:
            errs = sorted(r["quick"][key]["deg"] for r in clicks if key in r.get("quick", {}))
            if errs:
                med, worst = statistics.median(errs), errs[-1]
                p90 = errs[min(len(errs) - 1, int(0.9 * len(errs)))]
                out[key] = {"median": med, "p90": p90, "worst": worst, "n": len(errs)}
                lines.append(f"{label}: {med:.1f}° off at the median, {p90:.1f}° at the 90th percentile, worst {worst:.1f}°")
            else:
                lines.append(f"{label}: no gaze on this screen at the clicks")
        if self.run:
            lines.append(f"Saved in {self.run.dir}")
        out["text"] = "\n".join(lines)
        return out

    # --- timers

    def tick(self):
        now = time.monotonic()
        if self._page == "menu" and now - self.status_at > 2:
            self.status_at = now
            self.refresh_status()
        if self._page != "run":
            return
        if now - self.wake_at > WAKE / 2:
            self.wake_at = now
            threading.Thread(target=lambda: self.gazed.ask(f"wake {WAKE}"), daemon=True).start()
        if self.run:
            self.run.keepalive()
        s = self.gaze.latest()
        if s:
            self.vote([s])
        if self._mark:
            hit = (s or {}).get("head", {}).get("hit") if s else None
            dpr = self.screen.get("dpr", 1.0) or 1.0
            on = bool(hit) and hit["s"] == self.bench_screen() and benchrec.raw_now() - s.get("t", 0) < 0.5
            if on:
                self._hx, self._hy = hit["x"] / dpr, hit["y"] / dpr
            if on or self._head:
                self._head = on
                self.headChanged.emit()
        if self.next_at is not None and now >= self.next_at:
            self.show(self.i)
        if self._target and not self._ready:
            p = self.trials[self.i]
            if benchrec.raw_now() - self.shown_t >= p.get("dwell", 0):
                self._ready, self.ready_t = True, benchrec.raw_now()
                self.changed.emit()

    def shutdown(self):
        if self.run:
            self.finish(early=True)
        if self.stopping:
            self.stopping.join(10)
        self.gaze.stop()
        self.gazed.close()


def main():
    ap = argparse.ArgumentParser(description="Frametop Gaze Bench")
    ap.add_argument("--base", default=str(benchrec.BASE), help="where runs are recorded")
    ap.add_argument("--no-images", action="store_true", help="don't record the eye cameras")
    ap.add_argument("--scenarios", default=os.path.join(HERE, "scenarios.json"))
    ap.add_argument("--ft-gaze", help="the ft-gaze to run (default: this tree's build, else the installed release's)")
    ap.add_argument("--style", default="Fusion", help="the Qt Quick Controls style")
    a, qt_args = ap.parse_known_args()
    app = QGuiApplication([sys.argv[0]] + qt_args)
    app.setApplicationName("ft-gazebench")
    app.setApplicationDisplayName("Frametop Gaze Bench")
    app.setDesktopFileName("ft-gazebench")
    QQuickStyle.setStyle(a.style)
    with open(a.scenarios) as f:
        scenarios = json.load(f)
    bench = Bench(scenarios, a.base, not a.no_images, gaze_prog=a.ft_gaze)
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
