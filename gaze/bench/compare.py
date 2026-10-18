#!/usr/bin/env python3
"""Compare gaze techniques on gaze bench runs (README.md): how far each was from where you looked,
at every click.

  compare.py RUN [RUN...] [--per-click] [--json OUT] [--only a,b]

RUN is a run's folder (benchrec.py), or a folder of them (all runs in it). Where you looked at a
click is the target's middle, or the click if it landed off the target (gazebench.py's "truth"),
turned into a head-relative direction through ft-gaze's screen hits just before the click: the
hit nearest that point, and its pixels per degree there (ft-gaze.cpp's HIT "j"). Each technique's
gaze is its median over the same LOOK window (run.json), and its error the angle between the two.

Techniques: what ran live, from gaze.jsonl:
  own-live        our tracker's published gaze (ft-eyes, with its calibration and the shifts the
                  nudges taught it)
  steamvr         SteamVR's combined gaze (ft-gaze's mmap1, its own calibration)
  steamvr-eyes    the mean of SteamVR's two eyes (left, right: set 2, no calibration of ours)
  pointer         where the pointer was: a nudge's raw gaze (lessons.jsonl), else the click
  fuse            70 % our tracker, 30 % SteamVR's (both live)
  steamvr+clicks  SteamVR's, less the median error of the last SHIFT_KEEP clicks before
Replays of our tracker on its finds (own.jsonl), from the calibration and the shift it had when
the run started (start/), with ft-eyes' own rules (eyes_model.py), and nudges teaching it when
they did live (lessons.jsonl):
  own-replay      the live rules: check this against own-live (the "replay vs live" line)
  no-jump         no glint jump: only clicks move the shift
  both-jump       a glint jump counts only while both eyes see one
  edge-guard      nudges past the calibration's dots (by EDGE_MARGIN) teach nothing
  every-click     every click teaches it, not only the nudges
  bridge          one eye lost: the other, plus how far it usually reads from the two together
Each click is scored before anything it teaches. The breakdowns: by scenario, by where the target
was (edge, corner or inside), by how far from straight ahead you looked, and by the time since
the last nudge (run.json's last_lesson for the first).

Needs numpy and (for the glint pairs) OpenCV: the tracker's Python, e.g.
  scripts/in-box /opt/frametop/venv/bin/python gaze/bench/compare.py ~/.local/share/frametop/gaze/bench
"""
import argparse
import bisect
import json
import math
import statistics
import sys
from collections import deque
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tracker"))
import eyes_model  # noqa: E402

LOOK = (0.35, 0.05)       # s before a click: as gazebench.py, for runs that don't say
CLICK_BEFORE = 0.3        # ft-eyes: a click's frames are the 300 ms before it
CLICK_MIN = 5             # ...at least this many
FRESH = 0.03              # ft-eyes: an eye counts toward the output this long after its frame
GAP = 3.0                 # ft-eyes: frames stopping this long is the headset off (a re-seat)
HISTORY = 12.0            # ft-eyes: seconds of pupil positions kept per eye
LESSON_MATCH = 1.5        # s between a click and its nudge's lesson
EDGE_MARGIN = 2.0         # degrees past the calibration's outermost dots for edge-guard
BRIDGE_TAU = 2.0          # s: bridge's average of each eye against the two together
FUSE = 0.7                # fuse: our tracker's share
SHIFT_KEEP = eyes_model.SHIFT_KEEP
LIVE = ("own-live", "steamvr", "steamvr-eyes", "pointer", "fuse", "steamvr+clicks")
REPLAYS = {  # name: Replay options
    "own-replay": {},
    "no-jump": {"jump": None},
    "both-jump": {"jump": "both"},
    "edge-guard": {"guard": True},
    "every-click": {"teach": "every"},
    "bridge": {"bridge": True},
}
ECC_BINS = ((0, 10, "<10°"), (10, 20, "10-20°"), (20, 30, "20-30°"), (30, 999, "30°+"))
SINCE_BINS = ((0, 30, "<30 s"), (30, 120, "30-120 s"), (120, math.inf, "120 s+"))


def jsonl(path):
    try:
        with open(path) as f:
            for line in f:
                try:
                    yield json.loads(line)
                except ValueError:
                    continue
    except FileNotFoundError:
        return


def unit(hy, hp):
    """ft-gaze's head-relative yaw (+left), pitch (+up), degrees, as a unit vector (-Z ahead)."""
    y, p = math.radians(hy), math.radians(hp)
    return np.array([-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p)])


def angle(a, b):
    if a is None or b is None:
        return None
    return math.degrees(math.acos(max(-1.0, min(1.0, float(unit(*a) @ unit(*b))))))


def median2(rows):
    rows = [r for r in rows if r is not None]
    if not rows:
        return None
    return (statistics.median(r[0] for r in rows), statistics.median(r[1] for r in rows))


def rays(s):
    """A sample's rays with screen hits: (yaw, pitch, hit), head-relative."""
    head = s.get("head") or {}
    if head.get("hit"):
        yield 0.0, 0.0, head["hit"]
    for name, src in (s.get("src") or {}).items():
        if src.get("hit") and "hy" in src:
            yield src["hy"], src["hp"], src["hit"]
        for e, h in zip(src.get("eyes") or (), src.get("ehit") or ()):
            if e and h:
                yield e[0], e[1], h


class Run:
    def __init__(self, d):
        self.dir = Path(d)
        self.info = json.loads((self.dir / "run.json").read_text())
        st = self.info["start"]
        self.wall = st["time"] - st["t_raw"]          # wall clock = raw + this
        self.look = tuple(self.info.get("look") or LOOK)
        self.scenario = self.info["scenario"]["id"]
        self.samples = sorted(jsonl(self.dir / "gaze.jsonl"), key=lambda s: s.get("t", 0))
        self.ts = [s.get("t", 0) for s in self.samples]
        votes = {}
        for s in self.samples:
            hit = (s.get("head") or {}).get("hit")
            if hit:
                votes[hit["s"]] = votes.get(hit["s"], 0) + 1
        self.screen = max(votes, key=votes.get) if votes else None
        self.notes = []
        # ft-screens' pixels per window pixel: its size over the window's (gazebench.Bench.scale)
        win, size = self.info.get("screen") or {}, (self.info.get("screens") or {}).get(str(self.screen))
        if size and win.get("w") and win.get("h"):
            self.sx, self.sy = size[0] / win["w"], size[1] / win["h"]
        else:
            self.sx = self.sy = win.get("dpr") or 1.0
            self.notes.append("no screen sizes in run.json (a run before 2026-10-17): window pixels at Qt's "
                              "pixel ratio, wrong where it isn't the desktop's scale")
        self.lessons = sorted((r for r in jsonl(self.dir / "lessons.jsonl") if "refused" not in r),
                              key=lambda r: r["time"])
        self.trials = self.load_trials()

    def load_trials(self):
        """The trials that stand: the last click on each target, unless taken back or skipped."""
        by_i = {}
        for r in jsonl(self.dir / "trials.jsonl"):
            if r.get("kind") == "trial":
                by_i[r["i"]] = r
            elif r.get("kind") == "undo":
                by_i.pop(r["i"], None)
        out = []
        used = set()
        for r in sorted(by_i.values(), key=lambda r: r["click"]["t"]):
            if r.get("skipped") or "truth" not in r:
                continue
            r = dict(r)
            wall = r["click"]["t"] + self.wall
            best = None
            for k, ls in enumerate(self.lessons):
                if k not in used and abs(ls["time"] - wall) <= LESSON_MATCH and (
                        best is None or abs(ls["time"] - wall) < abs(self.lessons[best]["time"] - wall)):
                    best = k
            if best is not None:
                used.add(best)
                r["lesson"] = self.lessons[best]
            out.append(r)
        return out

    def window(self, t0, t1):
        return self.samples[bisect.bisect_left(self.ts, t0):bisect.bisect_right(self.ts, t1)]

    def look_window(self, r):
        """The click's look: before the press for a nudge (the lesson's look_t), else before the click."""
        ls = r.get("lesson")
        if ls and ls.get("look_t") is not None:
            return ls["look_t"] - (self.look[0] - self.look[1]), ls["look_t"]
        t = r["click"]["t"]
        return t - self.look[0], t - self.look[1]

    def direction(self, samples, px, py):
        """Head-relative (yaw, pitch) of a point on the bench's screen (ft-screens' pixels): per
        sample, from the hit nearest it, through that hit's pixels per degree; the median."""
        out = []
        for s in samples:
            best = None
            for hy, hp, h in rays(s):
                if h.get("s") != self.screen or not h.get("j"):
                    continue
                d = math.hypot(h["x"] - px, h["y"] - py)
                if best is None or d < best[0]:
                    best = (d, hy, hp, h)
            if best is None:
                continue
            _, hy, hp, h = best
            j = h["j"]  # [dx/dhy, dy/dhy, dx/dhp, dy/dhp]
            det = j[0] * j[3] - j[2] * j[1]
            if abs(det) < 1e-9:
                continue
            ex, ey = px - h["x"], py - h["y"]
            out.append((hy + (j[3] * ex - j[2] * ey) / det, hp + (-j[1] * ex + j[0] * ey) / det))
        return median2(out)

    def truth(self, r):
        t0, t1 = self.look_window(r)
        return self.direction(self.window(t0, t1), r["truth"]["x"] * self.sx, r["truth"]["y"] * self.sy)

    def source(self, name, t0, t1):
        rows = []
        for s in self.window(t0, t1):
            src = (s.get("src") or {}).get(name) or {}
            if "hy" in src:
                rows.append((src["hy"], src["hp"]))
        return median2(rows)

    def frames(self):
        """Our tracker's finds, in time order: (t, cam, x, y, glint pair midpoint or None)."""
        import eyes_pupil  # OpenCV
        out = []
        for r in jsonl(self.dir / "own.jsonl"):
            if r.get("lost") or "x" not in r:
                continue
            pair = eyes_pupil.glint_pair({"x": r["x"], "y": r["y"], "glints": r.get("glints") or []})
            out.append((r["t"], r["cam"], r["x"], r["y"], eyes_model.pair_mid(pair) if pair else None, r.get("gaze")))
        out.sort(key=lambda f: (f[0], f[1]))
        return out


class Replay:
    """ft-eyes' classic tracker (Eye.feed, Tracker.command "click", the output loop) over a run's
    finds, with variations."""

    def __init__(self, run, frames, cal, shifts, status, clicks, jump="each", guard=False, bridge=False, box=None):
        self.run, self.frames, self.cal = run, frames, cal
        self.clicks = sorted(clicks, key=lambda c: c[0])  # (apply at, look t, (yaw, pitch))
        self.jump, self.guard, self.bridge, self.box = jump, guard, bridge, box
        self.shift = {}
        for e in (0, 1):
            sh = eyes_model.Shift.from_json(shifts[e]) if e in shifts else eyes_model.Shift()
            st = ((status or {}).get("eyes") or {}).get(("right", "left")[e]) or {}
            sh.reseated = bool(st.get("reseat", e not in shifts))
            if st.get("jump") and jump is not None:
                sh.jump = np.asarray(st.get("jump_px", (0, 0)), float)
            self.shift[e] = sh
        self.slip = {e: eyes_model.SlipTracker(cal, e, window=eyes_model.JUMP_WINDOW) for e in (0, 1)}
        self.history = {0: deque(), 1: deque()}
        self.gaze = {0: None, 1: None}
        self.last_t = {0: None, 1: None}
        self.off = {0: None, 1: None}
        self.taught = self.skipped = 0

    def value(self, e):
        sh = self.shift[e]
        if self.jump == "both" and not all(np.any(self.shift[o].jump) for o in (0, 1)):
            return sh.base
        return sh.value

    def click(self, t, truth):
        if self.guard and self.box and not (self.box[0] - EDGE_MARGIN <= truth[0] <= self.box[1] + EDGE_MARGIN
                                            and self.box[2] - EDGE_MARGIN <= truth[1] <= self.box[3] + EDGE_MARGIN):
            self.skipped += 1
            return
        both = all(np.any(self.shift[o].jump) for o in (0, 1))
        for e in (0, 1):
            rows = [h for h in self.history[e] if t - CLICK_BEFORE <= h[0] <= t]
            if len(rows) < CLICK_MIN or not self.cal.has("where", e):
                continue
            pupil = np.median(np.array([h[1:3] for h in rows]), axis=0)
            d = self.cal.click_shift(e, pupil, truth)
            sh = self.shift[e]
            # A jump in force (both-jump: in both eyes) means the headset moved: the older clicks are stale
            if (both if self.jump == "both" else np.any(sh.jump)) or sh.reseated:
                sh.meas, sh.reseated = [], False
            sh.meas = (sh.meas + [np.asarray(d, float)])[-SHIFT_KEEP:]
            sh.base = np.median(sh.meas, axis=0)
            sh.jump, sh.held = np.zeros(2), None
            if self.slip[e].value is not None:
                sh.ref = np.asarray(self.slip[e].value, float)
        self.taught += 1

    def __call__(self):
        """The output, (times, yaw, pitch) arrays, one per frame that changed it."""
        out_t, out_g = [], []
        ci = 0
        last_out = None
        for t, e, x, y, mid, _ in self.frames:
            while ci < len(self.clicks) and self.clicks[ci][0] <= t:
                self.click(*self.clicks[ci][1:])
                ci += 1
            if self.last_t[e] is not None and t - self.last_t[e] > GAP:
                self.shift[e].reseat()
            self.last_t[e] = t
            h = self.history[e]
            h.append((t, x, y))
            while h[0][0] < t - HISTORY:
                h.popleft()
            if not self.cal.has("pupil", e):
                continue
            if mid is not None:
                self.slip[e].add(t, (x, y), mid)
            if self.jump is not None:
                self.shift[e].glint(self.slip[e].get(t), t)
            g = self.cal.gaze(e, x, y, self.value(e))
            self.gaze[e] = (t, float(g[0]), float(g[1]))
            latest = max(v[0] for v in self.gaze.values() if v)
            use = {k: v[1:] for k, v in self.gaze.items() if v and latest - v[0] < FRESH}
            out = self.cal.combine(use)
            if self.bridge:
                if len(use) == 2:
                    a = 1.0 if last_out is None else 1 - math.exp(-(t - last_out) / BRIDGE_TAU)
                    for k, v in use.items():
                        d = np.asarray(out) - np.asarray(v)
                        self.off[k] = d if self.off[k] is None else self.off[k] + a * (d - self.off[k])
                elif len(use) == 1:
                    (k, v), = use.items()
                    if self.off[k] is not None:
                        out = np.asarray(v) + self.off[k]
            last_out = t
            out_t.append(latest)
            out_g.append((float(out[0]), float(out[1])))
        return out_t, out_g


def calibration_box(run, cal):
    """The calibration's dots' extent (yaw min, max, pitch min, max), from calibration-dots.jsonl."""
    made = cal.info.get("made")
    pts = [r["truth"] for r in jsonl(run.dir / "start" / "eyes-calibration-dots.jsonl") if r.get("made") == made]
    if len(pts) < 4:
        return (-20.0, 20.0, -20.0, 20.0)
    return (min(p[0] for p in pts), max(p[0] for p in pts), min(p[1] for p in pts), max(p[1] for p in pts))


def replays(run, truths, only):
    """{name: function(t0, t1) -> (yaw, pitch) or None} for the replays, or {} with a note."""
    want = [n for n in REPLAYS if not only or n in only]
    if not want:
        return {}
    status = (run.info["start"] or {}).get("eyes")
    if status is None:
        run.notes.append("our tracker wasn't running at the start: no replays")
        return {}
    if status.get("feature", "pupil") != "pupil":
        run.notes.append(f"our tracker ran {status.get('feature')}: replays are of the classic tracker only")
        return {}
    calp = run.dir / "start" / "eyes-calibration.json"
    if not calp.exists():
        run.notes.append("no calibration in start/: no replays")
        return {}
    cal = eyes_model.Calibration.load(calp)
    shifts = {}
    try:
        st = json.loads((run.dir / "start" / "eyes-state.json").read_text())
        if st.get("calibration") == cal.info.get("made"):
            shifts = {int(k): v for k, v in st.get("shift", {}).items()}
    except (OSError, ValueError):
        pass
    frames = run.frames()
    if not frames:
        run.notes.append("no finds in own.jsonl: no replays")
        return {}
    nudges = [(ls["time"] - run.wall, ls["look_t"], tuple(ls["true"])) for ls in run.lessons
              if ls.get("kind") == "own" and ls.get("look_t") is not None]
    every = [(r["click"]["t"], r["click"]["t"] - run.look[1], truths[id(r)]) for r in run.trials
             if truths.get(id(r)) is not None]
    box = calibration_box(run, cal)
    out = {}
    for name in want:
        opt = dict(REPLAYS[name])
        teach = every if opt.pop("teach", None) == "every" else nudges
        t, g = Replay(run, frames, cal, shifts, status, teach, box=box, **opt)()

        def look(t0, t1, t=t, g=g):
            return median2(g[bisect.bisect_left(t, t0):bisect.bisect_right(t, t1)])
        out[name] = look
    return out


def score(run, only=None):
    """A record per click: where it was, and each technique's error (degrees, or None)."""
    truths = {id(r): run.truth(r) for r in run.trials}
    rep = replays(run, truths, only)
    last_lesson = run.info.get("last_lesson")
    recs, steam_offsets = [], []
    for r in run.trials:
        truth = truths[id(r)]
        t0, t1 = run.look_window(r)
        own, steam = run.source("own", t0, t1), run.source("mmap1", t0, t1)
        left, right = run.source("left", t0, t1), run.source("right", t0, t1)
        g = {"own-live": own, "steamvr": steam,
             "steamvr-eyes": ((left[0] + right[0]) / 2, (left[1] + right[1]) / 2) if left and right else None}
        ls = r.get("lesson")
        if ls:
            g["pointer"] = tuple(ls["raw"])
        else:
            c = r["click"]
            g["pointer"] = run.direction(run.window(t0, t1), c["x"] * run.sx, c["y"] * run.sy)
        g["fuse"] = (tuple(FUSE * a + (1 - FUSE) * b for a, b in zip(own, steam)) if own and steam else None)
        if steam and steam_offsets:
            keep = steam_offsets[-SHIFT_KEEP:]
            g["steamvr+clicks"] = (steam[0] + statistics.median(o[0] for o in keep),
                                   steam[1] + statistics.median(o[1] for o in keep))
        else:
            g["steamvr+clicks"] = steam
        if steam and truth:
            steam_offsets.append((truth[0] - steam[0], truth[1] - steam[1]))
        for name, look in rep.items():
            g[name] = look(t0, t1)
        wall = r["click"]["t"] + run.wall
        before = [ls_["time"] for ls_ in run.lessons if ls_["time"] < wall - LESSON_MATCH]
        last = before[-1] if before else last_lesson
        u, v = r["u"], r["v"]
        edges = sum(1 for w in (u, v) if w in (0.0, 1.0))
        rec = {"run": run.dir.name, "scenario": run.scenario, "i": r["i"], "u": u, "v": v,
               "where": ("corner" if edges == 2 else "edge" if edges else "inside"),
               "truth": truth, "ecc": math.hypot(*truth) if truth else None,
               "since": None if last is None else wall - last, "nudged": bool(ls),
               "err": {k: angle(val, truth) for k, val in g.items() if not only or k in only}}
        if ls:
            c = r["click"]
            cd = run.direction(run.window(t0, t1), c["x"] * run.sx, c["y"] * run.sy)
            rec["check"] = angle(cd, tuple(ls["true"]))
        if g.get("own-replay") and own:
            rec["replay_vs_live"] = angle(g["own-replay"], own)
        recs.append(rec)
    return recs


def bin_of(value, bins):
    if value is None:
        return "none" if bins is SINCE_BINS else "?"
    return next((name for lo, hi, name in bins if lo <= value < hi), "?")


def table(title, recs, key, names):
    groups = {}
    for r in recs:
        groups.setdefault(key(r), []).append(r)
    cols = list(groups)
    if not cols:
        return
    w = max(len(n) for n in names) + 2
    print(f"\n{title}: median degrees off (clicks)")
    print(" " * w + "".join(f"{c:>13}" for c in cols))
    for n in names:
        cells = []
        for c in cols:
            errs = sorted(e for e in (r["err"].get(n) for r in groups[c]) if e is not None)
            cells.append(f"{statistics.median(errs):6.2f} ({len(errs):3d})" if errs else f"{'-':>12}")
        print(f"{n:<{w}}" + "".join(f"{x:>13}" for x in cells))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("runs", nargs="+", help="run folders, or folders of them")
    ap.add_argument("--per-click", action="store_true", help="a line per click")
    ap.add_argument("--json", help="write the per-click records here")
    ap.add_argument("--only", help="comma-separated techniques")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None
    dirs = []
    for p in map(Path, a.runs):
        dirs += [p] if (p / "run.json").exists() else sorted(q.parent for q in p.glob("*/run.json"))
    recs = []
    for d in dirs:
        run = Run(d)
        got = score(run, only)
        recs += got
        print(f"{d.name}: {len(got)} clicks, {sum(r['nudged'] for r in got)} nudged"
              + "".join(f"; {n}" for n in run.notes))
    if not recs:
        sys.exit("no clicks")
    names = [n for n in LIVE + tuple(REPLAYS) if any(r["err"].get(n) is not None for r in recs)]
    print()
    w = max(len(n) for n in names) + 2
    print(f"{'':<{w}}{'median':>8}{'p90':>8}{'mean':>8}{'n':>6}")
    for n in names:
        errs = sorted(e for e in (r["err"].get(n) for r in recs) if e is not None)
        p90 = errs[min(len(errs) - 1, int(0.9 * len(errs)))]
        print(f"{n:<{w}}{statistics.median(errs):8.2f}{p90:8.2f}{statistics.fmean(errs):8.2f}{len(errs):6d}")
    apart = sorted(r["replay_vs_live"] for r in recs if r.get("replay_vs_live") is not None)
    if apart:
        p90 = apart[min(len(apart) - 1, int(0.9 * len(apart)))]
        print(f"\nreplay vs live: our tracker's replay is {statistics.median(apart):.2f}° from what it published, "
              f"at the median ({p90:.2f}° p90, {len(apart)} clicks); the replays are only as good as this")
    checks = [r["check"] for r in recs if r.get("check") is not None]
    if checks:
        print(f"screen to direction vs the pointer's own: {statistics.median(checks):.2f}° apart at the median "
              f"({len(checks)} nudges)")
    table("By scenario", recs, lambda r: r["scenario"], names)
    table("By where the target was", recs, lambda r: r["where"], names)
    table("By how far from straight ahead", recs, lambda r: bin_of(r["ecc"], ECC_BINS), names)
    table("By time since the last nudge", recs, lambda r: bin_of(r["since"], SINCE_BINS), names)
    if a.per_click:
        print()
        for r in recs:
            print(f"{r['run']} #{r['i']:<3} {r['where']:<6} ecc {r['ecc'] or 0:5.1f}°"
                  + (" nudged" if r["nudged"] else "       ")
                  + "".join(f"  {n} {r['err'][n]:.2f}" for n in names if r["err"].get(n) is not None))
    if a.json:
        Path(a.json).write_text(json.dumps(recs, indent=1))


if __name__ == "__main__":
    main()
