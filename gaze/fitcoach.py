#!/usr/bin/env python3
"""fitcoach: how the headset sits on this face, from where our tracker sees each eye, and what
to do about it (gaze/rec/DESIGN.md, "Fit coach"). The eye recorder runs it before each round;
the gaze service's fit check, put-on check and slip notice use it too (gaze/gazecheck.py).

It takes per-frame pupil finds (gaze/tracker/eyes_pupil.find_pupil: centre, axes, glints) for
each camera (0 the right eye, 1 the left), keeps the last second, and compares the medians
with a reference: where a well-fitted eye sits in that camera's image while looking straight
ahead, and how the pupil moves per degree of gaze (the reference's Jacobian, fitted on
calibration dots). Offsets go through that Jacobian into degrees as if the eye had turned, so
the cameras' orientations don't matter (camera 1's image is turned 180 degrees from camera 0's:
looking left moves its pupil the other way) and both eyes' readings can be added up:

  both eyes appear to look up       the eyes sit high in the headset: it's too low    -> push it up
  both appear to look left          the headset sits off to the right                 -> slide it left
  the eyes appear to look apart     the eyes are wider apart than the lenses           -> IPD wider
  one appears to look higher        the headset is tilted                              -> level it

The IPD reading only becomes a hint once we know the eye cameras move with the lenses
(HEADSET-TESTS, 2026-10-14): if they're fixed to the housing, the offset also depends on the IPD
setting, and that needs a reference per setting. Until then it's reported, not advised.

A baseline is the same reading saved from one person's good fit. Compared with a baseline the
glint pair's spacing says whether the eye is nearer the camera than then (the eye relief: on
the maintainer's headset it moved 18 % between two seatings, and 4 % within one), and the put-on
check says how far the headset moved since. The reference starts as the maintainer's
(gaze/fit-reference.json); its hints are marked provisional until it's built from more faces.

Measured on the maintainer's headset (2026-10-14, three calibrations of 27 dots): straight-ahead
pupils moved 15-20 px between seatings, at about 1.3 px per degree; so OK within OK_DEG and
adjust past ADJUST_DEG, both provisional.

Usage (on the Frame, in the dev container, with frametop-eyegrab running):
  python3 gaze/fitcoach.py live [--baseline]   print the reading twice a second; --baseline saves
                                               the last one as your baseline on Ctrl-C
  python3 gaze/fitcoach.py reference [DOTS...] fit a reference from calibration-dots.jsonl files
"""
import json
import math
import os
import statistics
import sys
import threading
import time
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
REFERENCE = os.path.join(HERE, "fit-reference.json")
BASELINE = os.path.expanduser("~/.local/state/frametop/gaze/fit-baseline.json")
DOTS = os.path.expanduser("~/.local/state/frametop/gaze/eyes/calibration-dots.jsonl")
EYES = ("right", "left")       # camera 0, camera 1
WINDOW = 1.0                   # s of finds per reading
MIN_FINDS = 10                 # finds in the window before an eye is judged
MIN_FOUND = 0.5                # share of frames with a pupil: under this, the eye is lost
OK_DEG = 5.0                   # every term within this: the fit is good (provisional)
ADJUST_DEG = 8.0               # a term past this: say what to do (between the two: keep the last state)
TILT_DEG = 4.0                 # half the eyes' vertical difference
RELIEF_TOL = 0.08              # glint spacing this much off the baseline: nearer or further
MANY_GLINTS = 5                # a find with this many glints has stray reflections. Without glasses
                               # (three maintainer captures, 2026-10-14) 3+ glints came in up to 37 % of
                               # finds and 5+ in up to 12 %: a guess until the Glasses bounty's data
EXTRA_GLINTS = 0.5             # share of finds with MANY_GLINTS: reflections (glasses, a smudge)
MOVED_DEG = 5.0                # put-on check: this far from the baseline, the headset moved; the slip
                               # notice: ft-eyes' glint jump this far


# ------------------------------------------------------------------------------------------
# Readings

def glint_spacing(p):
    """The LED glint pair's spacing in pixels, or None (eyes_pupil.glint_pair's rule)."""
    pair = _glint_pair(p)
    if not pair:
        return None
    (x0, y0), (x1, y1) = pair
    return math.hypot(x1 - x0, y1 - y0)


def _glint_pair(p):
    """eyes_pupil.glint_pair without numpy: the vertical-ish pair nearest the pupil centre."""
    g, best = p.get("glints") or [], None
    for i in range(len(g)):
        for j in range(i + 1, len(g)):
            dx, dy = g[j][0] - g[i][0], g[j][1] - g[i][1]
            d = math.hypot(dx, dy)
            if not 5 <= d <= 45 or abs(dy) < 2 * abs(dx):
                continue
            cost = math.hypot((g[i][0] + g[j][0]) / 2 - p["x"], (g[i][1] + g[j][1]) / 2 - p["y"])
            if best is None or cost < best[0]:
                best = (cost, (g[i], g[j]) if dy > 0 else (g[j], g[i]))
    return best[1] if best else None


class Observations:
    """The last WINDOW seconds of pupil finds per camera. add() from any thread."""

    def __init__(self, window=WINDOW):
        self.window = window
        self._rows = {0: deque(), 1: deque()}   # (t, x, y, a, spacing or None, extra glints) or (t, None)
        self._lock = threading.Lock()

    def add(self, cam, t, p):
        if p is None:
            row = (t, None)
        else:
            row = (t, float(p["x"]), float(p["y"]), float(p["a"]), glint_spacing(p), len(p.get("glints") or []) >= MANY_GLINTS)
        with self._lock:
            rows = self._rows[cam]
            rows.append(row)
            while rows and rows[0][0] < t - self.window:
                rows.popleft()

    def summary(self, now=None):
        """{camera: {"n", "found", "x", "y", "a", "spacing", "extra"}}: medians of the finds,
        found the share of frames with a pupil, extra the share with MANY_GLINTS or more. A camera with
        no frames in the window: {"n": 0}."""
        out = {}
        with self._lock:
            for cam, rows in self._rows.items():
                rs = [r for r in rows if now is None or r[0] >= now - self.window]
                finds = [r for r in rs if r[1] is not None]
                if not rs:
                    out[cam] = {"n": 0}
                    continue
                sp = [r[4] for r in finds if r[4] is not None]
                out[cam] = {"n": len(rs), "finds": len(finds), "found": len(finds) / len(rs),
                            "x": statistics.median(r[1] for r in finds) if finds else None,
                            "y": statistics.median(r[2] for r in finds) if finds else None,
                            "a": statistics.median(r[3] for r in finds) if finds else None,
                            "spacing": statistics.median(sp) if len(sp) >= 3 else None,
                            "extra": sum(r[5] for r in finds) / len(finds) if finds else 0.0}
        return out


# ------------------------------------------------------------------------------------------
# The reference and the baseline

def _solve3(a, b):
    """x for a 3x3 system a x = b (Gaussian elimination with pivoting), or None if singular."""
    m = [row[:] + [v] for row, v in zip(a, b)]
    for i in range(3):
        k = max(range(i, 3), key=lambda r: abs(m[r][i]))
        if abs(m[k][i]) < 1e-12:
            return None
        m[i], m[k] = m[k], m[i]
        for r in range(3):
            if r != i:
                f = m[r][i] / m[i][i]
                m[r] = [x - f * y for x, y in zip(m[r], m[i])]
    return [m[i][3] / m[i][i] for i in range(3)]


def fit_linear(truths, values):
    """value = c0 + c1 yaw + c2 pitch by least squares: [c0, c1, c2], or None."""
    rows = [(1.0, float(t[0]), float(t[1])) for t in truths]
    ata = [[sum(r[i] * r[j] for r in rows) for j in range(3)] for i in range(3)]
    atb = [sum(r[i] * v for r, v in zip(rows, values)) for i in range(3)]
    return _solve3(ata, atb)


class Reference:
    """Per camera: the straight-ahead pupil centre (px) and the Jacobian [[dx/dyaw, dx/dpitch],
    [dy/dyaw, dy/dpitch]] (px per degree); optionally the glint spacing, the IPD it was made
    at (m), whether the eye cameras move with the lenses, where it came from and from how many faces."""

    def __init__(self, eyes, ipd=None, source="", faces=1, cameras_follow_lenses=None, made=""):
        self.eyes, self.ipd, self.source, self.faces = eyes, ipd, source, faces
        self.cameras_follow_lenses, self.made = cameras_follow_lenses, made

    @property
    def provisional(self):
        return self.faces < 3

    @classmethod
    def from_dots(cls, rows, source="", ipd=None, max_err=4.0):
        """From calibration-dots.jsonl rows ({"made", "truth": [yaw, pitch], "eye": {"0": {"pupil": [x, y]}}}).
        Each calibration ("made") is fitted on its own; those that fit worse than max_err px
        (median) are left out, and the rest averaged."""
        fits = {0: [], 1: []}
        for made in sorted({r.get("made", "") for r in rows}):
            for cam in (0, 1):
                rs = [r for r in rows if r.get("made", "") == made and (r.get("eye") or {}).get(str(cam))]
                if len(rs) < 6:
                    continue
                truths = [r["truth"] for r in rs]
                px = [r["eye"][str(cam)]["pupil"] for r in rs]
                cx, cy = fit_linear(truths, [p[0] for p in px]), fit_linear(truths, [p[1] for p in px])
                if cx is None or cy is None:
                    continue
                err = statistics.median(math.hypot(cx[0] + cx[1] * t[0] + cx[2] * t[1] - p[0],
                                                   cy[0] + cy[1] * t[0] + cy[2] * t[1] - p[1]) for t, p in zip(truths, px))
                if err <= max_err:
                    fits[cam].append((cx, cy, err))
        eyes = {}
        for cam, fs in fits.items():
            if not fs:
                continue
            avg = lambda k, i: sum(f[k][i] for f in fs) / len(fs)   # noqa: E731
            eyes[cam] = {"centre": [avg(0, 0), avg(1, 0)], "jacobian": [[avg(0, 1), avg(0, 2)], [avg(1, 1), avg(1, 2)]],
                         "fits": len(fs), "fit_err": statistics.median(f[2] for f in fs)}
        return cls(eyes, ipd=ipd, source=source, made=time.strftime("%Y-%m-%d"))

    def degrees(self, cam, dx, dy):
        """A pupil offset in px as the gaze change it would be: (yaw, pitch) degrees."""
        (a, b), (c, d) = self.eyes[cam]["jacobian"]
        det = a * d - b * c
        if abs(det) < 1e-9:
            return 0.0, 0.0
        return (d * dx - b * dy) / det, (-c * dx + a * dy) / det

    def to_json(self):
        eyes = {str(k): {"centre": [round(v, 2) for v in e["centre"]],
                         "jacobian": [[round(v, 4) for v in row] for row in e["jacobian"]],
                         **{k2: (round(v2, 3) if isinstance(v2, float) else v2) for k2, v2 in e.items()
                            if k2 not in ("centre", "jacobian")}}
                for k, e in sorted(self.eyes.items())}
        return {"schema": 1, "made": self.made, "source": self.source, "faces": self.faces, "ipd": self.ipd,
                "cameras_follow_lenses": self.cameras_follow_lenses,
                "eyes": eyes}

    @classmethod
    def from_json(cls, d):
        return cls({int(k): v for k, v in d.get("eyes", {}).items()}, ipd=d.get("ipd"), source=d.get("source", ""),
                   faces=d.get("faces", 1), cameras_follow_lenses=d.get("cameras_follow_lenses"), made=d.get("made", ""))

    @classmethod
    def load(cls, path=REFERENCE):
        try:
            with open(path) as f:
                return cls.from_json(json.load(f))
        except (OSError, ValueError):
            return None

    def save(self, path=REFERENCE):
        _write_json(path, self.to_json())


def make_baseline(summary, ipd=None):
    """A baseline from a good reading: per camera the centre and glint spacing, and the IPD."""
    eyes = {str(c): {"centre": [s["x"], s["y"]], "spacing": s.get("spacing"), "a": s.get("a")}
            for c, s in summary.items() if s.get("x") is not None}
    return {"schema": 1, "made": time.strftime("%Y-%m-%d %H:%M:%S"), "ipd": ipd, "eyes": eyes}


def load_baseline(path=BASELINE):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def save_baseline(baseline, path=None):
    """Keep a good fit as the person's (the gaze service's fit check, the eye recorder's fitment)."""
    _write_json(path or BASELINE, baseline)


def _write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


# ------------------------------------------------------------------------------------------
# The assessment

HINTS = {
    "up": "Push the headset up a little (the top strap).",
    "down": "Let the headset down a little.",
    "left": "Slide the headset a little to your left.",
    "right": "Slide the headset a little to your right.",
    "ipd-wider": "Turn the IPD wheel a little wider.",
    "ipd-narrower": "Turn the IPD wheel a little narrower.",
    "tilt-left-up": "Level the headset: lift its left side a little.",
    "tilt-right-up": "Level the headset: lift its right side a little.",
    "nearer": "The headset sits closer to your eyes than your saved fit: loosen the back strap a little.",
    "further": "The headset sits further from your eyes than your saved fit: tighten the back strap a little.",
    "reflections": "Stray reflections in the {eye} eye's camera: tilt your glasses a little, or wipe the lens.",
    "lost": "The {eye} eye is hard to see: lashes touching the lens, or the headset too low on that side?",
    "ipd-changed": "The IPD setting changed since your saved fit ({was:.1f} mm, now {now:.1f} mm).",
}


def assess(summary, ref, ipd=None, baseline=None, previous=None):
    """The reading: {"state": "ok" | "adjust" | "lost" | "waiting", "eyes": {camera: {...}},
    "terms": {"up", "left", "ipd", "tilt"} in degrees, "hints": [{"key", "text", "amount"}],
    "provisional", "against": "baseline" | "reference"}.

    Against a baseline (the person's own good fit) when there is one, else the reference.
    previous (an earlier assess() result) keeps the state between OK_DEG and ADJUST_DEG."""
    eyes, hints, offsets = {}, [], {}
    centres = {}
    if baseline:
        centres = {int(k): v["centre"] for k, v in baseline.get("eyes", {}).items()}
    for cam, s in summary.items():
        e = {"name": EYES[cam], "found": round(s.get("found", 0.0), 3), "seen": bool(s.get("n"))}
        if not s.get("n") or s.get("finds", 0) < MIN_FINDS:
            e["state"] = "waiting" if not s.get("n") else "lost"
        elif s["found"] < MIN_FOUND:
            e["state"] = "lost"
        else:
            e["state"] = "found"
            centre = centres.get(cam) or (ref.eyes.get(cam) or {}).get("centre") if ref else centres.get(cam)
            if centre and ref and cam in ref.eyes:
                dx, dy = s["x"] - centre[0], s["y"] - centre[1]
                offsets[cam] = ref.degrees(cam, dx, dy)
                e["offset_px"] = [round(dx, 1), round(dy, 1)]
                e["offset_deg"] = [round(v, 2) for v in offsets[cam]]
            if s.get("extra", 0.0) > EXTRA_GLINTS:
                hints.append({"key": "reflections", "text": HINTS["reflections"].format(eye=EYES[cam]), "amount": s["extra"]})
            if baseline:
                b = baseline.get("eyes", {}).get(str(cam)) or {}
                if b.get("spacing") and s.get("spacing"):
                    rel = s["spacing"] / b["spacing"] - 1.0
                    e["relief"] = round(rel, 3)
        eyes[cam] = e
    lost = [c for c, e in eyes.items() if e["state"] == "lost"]
    for c in lost:
        hints.append({"key": "lost", "text": HINTS["lost"].format(eye=EYES[c]), "amount": 1.0 - eyes[c]["found"]})
    terms = {}
    if offsets:
        yaws = {c: v[0] for c, v in offsets.items()}
        pitches = {c: v[1] for c, v in offsets.items()}
        terms["up"] = sum(pitches.values()) / len(pitches)          # + : the eyes sit high: the headset is low
        terms["left"] = sum(yaws.values()) / len(yaws)              # + : the eyes sit left: the headset is right
        if len(offsets) == 2:
            terms["ipd"] = (yaws[1] - yaws[0]) / 2                  # + : the eyes are wider apart than the lenses
            terms["tilt"] = (pitches[1] - pitches[0]) / 2           # + : the left eye sits higher: its side is low
    t = terms
    if abs(t.get("up", 0)) > ADJUST_DEG:
        hints.append(_hint("up" if t["up"] > 0 else "down", t["up"]))
    if abs(t.get("left", 0)) > ADJUST_DEG:
        hints.append(_hint("left" if t["left"] > 0 else "right", t["left"]))
    ipd_advice = ref is not None and ref.cameras_follow_lenses is True
    if ipd_advice and abs(t.get("ipd", 0)) > ADJUST_DEG:
        hints.append(_hint("ipd-wider" if t["ipd"] > 0 else "ipd-narrower", t["ipd"]))
    if abs(t.get("tilt", 0)) > TILT_DEG:
        hints.append(_hint("tilt-left-up" if t["tilt"] > 0 else "tilt-right-up", t["tilt"]))
    reliefs = [e["relief"] for e in eyes.values() if "relief" in e]
    if reliefs:
        rel = sum(reliefs) / len(reliefs)
        terms["relief"] = rel
        if abs(rel) > RELIEF_TOL:
            hints.append({"key": "nearer" if rel > 0 else "further", "text": HINTS["nearer" if rel > 0 else "further"],
                          "amount": rel})
    if baseline and ipd and baseline.get("ipd") and abs(ipd - baseline["ipd"]) >= 0.001:
        hints.insert(0, {"key": "ipd-changed", "amount": ipd - baseline["ipd"],
                         "text": HINTS["ipd-changed"].format(was=baseline["ipd"] * 1000, now=ipd * 1000)})
    # The state, with hysteresis on the position terms.
    big = max([abs(t.get(k, 0.0)) for k in ("up", "left")] + ([abs(t.get("ipd", 0.0))] if ipd_advice else []), default=0.0)
    if any(e["state"] == "waiting" for e in eyes.values()) or not eyes:
        state = "waiting"
    elif lost:
        state = "lost"
    elif not offsets:
        state = "waiting"
    elif big > ADJUST_DEG or abs(t.get("tilt", 0)) > TILT_DEG or abs(t.get("relief", 0)) > RELIEF_TOL:
        state = "adjust"
    elif big <= OK_DEG:
        state = "ok"
    else:
        state = (previous or {}).get("state") if (previous or {}).get("state") in ("ok", "adjust") else "adjust"
    if state == "adjust" and not hints:
        # Between OK_DEG and ADJUST_DEG, and not OK before: name the biggest term.
        names = {"up": ("up", "down"), "left": ("left", "right")}
        if ipd_advice:
            names["ipd"] = ("ipd-wider", "ipd-narrower")
        term = max((k for k in names if k in t), key=lambda k: abs(t[k]), default=None)
        if term:
            hints.append(_hint(names[term][0] if t[term] > 0 else names[term][1], t[term]))
    hints.sort(key=lambda h: (h["key"] not in ("ipd-changed", "lost"), -abs(h["amount"])))
    return {"state": state, "eyes": eyes, "terms": {k: round(v, 3) for k, v in terms.items()}, "hints": hints,
            "provisional": bool(ref is None or ref.provisional), "against": "baseline" if centres else "reference",
            "ipd": ipd, "ipd_advice": ipd_advice}


def _hint(key, amount):
    return {"key": key, "text": HINTS[key], "amount": amount}


def moved(reading):
    """Put-on check: the headset sits more than MOVED_DEG from the baseline (or changed IPD)."""
    t = reading.get("terms", {})
    return reading.get("against") == "baseline" and (
        max(abs(t.get("up", 0.0)), abs(t.get("left", 0.0))) > MOVED_DEG or
        any(h["key"] == "ipd-changed" for h in reading.get("hints", [])))


def position_hint(terms, limit=MOVED_DEG):
    """The biggest position term (up, left) past limit as a hint ("up", "down", "left" or
    "right"), or None."""
    names = {"up": ("up", "down"), "left": ("left", "right")}
    term = max((k for k in names if abs(terms.get(k, 0.0)) > limit), key=lambda k: abs(terms[k]), default=None)
    if term is None:
        return None
    return _hint(names[term][0] if terms[term] > 0 else names[term][1], terms[term])


def slip_terms(jumps, ref):
    """ft-eyes' glint jumps ({camera: (dx, dy)}, pixels: how far each pupil moved in its image
    since the last click, the headset having slipped) as the coach's position terms, {"up",
    "left"} in degrees, through the reference's Jacobian as assess() does; {} without any."""
    offsets = [ref.degrees(cam, dx, dy) for cam, (dx, dy) in jumps.items() if ref and cam in ref.eyes]
    if not offsets:
        return {}
    return {"up": sum(o[1] for o in offsets) / len(offsets), "left": sum(o[0] for o in offsets) / len(offsets)}


# ------------------------------------------------------------------------------------------
# Live finds from the shared frames (numpy and OpenCV: the dev container, or a venv with them)

class LiveFinder:
    """Runs the pupil finder on each camera's newest frame, at most rate times a second, and adds
    the finds to obs. About 0.4 ms a frame while it follows the pupil, 2 ms when it searches."""

    def __init__(self, obs, rate=30.0, share_path=None, want_path=None, log=None):
        sys.path.insert(0, os.path.join(HERE, "tracker"))
        import eyes_pupil   # noqa: E402  (numpy, OpenCV)
        import eyes_share   # noqa: E402
        import numpy as np  # noqa: E402
        self._pupil, self._share_mod, self._np = eyes_pupil, eyes_share, np
        self.obs, self.period = obs, 1.0 / rate
        self.share_path, self.want_path = share_path, want_path
        self.log = log or (lambda line: None)
        self.error = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="fitcoach-finder", daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(2)

    def _run(self):
        share = self._share_mod.open_share(10, self.share_path, self.want_path)
        if share is None:
            self.error = "no eye camera frames: is frametop-eyegrab.service running?"
            self.log("fit coach: " + self.error)
            return
        last, near, touched = {0: -1, 1: -1}, {0: None, 1: None}, 0.0
        np = self._np
        while not self._stop.is_set():
            t0 = time.monotonic()
            for cam in (0, 1):
                n = share.count(cam)
                if n == last[cam] or n == 0:
                    continue
                last[cam] = n
                f = share.frame(cam, n - 1)
                if f is None:
                    continue
                img = np.frombuffer(f[2], np.uint8).reshape(self._share_mod.H, self._share_mod.W)
                p = self._pupil.find_pupil(img, near[cam])
                near[cam] = p
                self.obs.add(cam, f[0], p)
            if t0 - touched >= 1.0:
                touched = t0
                self._share_mod.touch_want(self.want_path)
                if share.replaced():
                    share.close()
                    share = self._share_mod.open_share(10, self.share_path, self.want_path)
                    if share is None:
                        self.error = "the eye camera frames stopped"
                        return
            self._stop.wait(max(0.0, self.period - (time.monotonic() - t0)))
        share.close()


# ------------------------------------------------------------------------------------------

def _print_reading(r):
    eyes = "; ".join("%s %s found %.0f%%%s" % (e["name"], e["state"], 100 * e["found"],
                                               " off %+.1f,%+.1f deg" % tuple(e["offset_deg"]) if "offset_deg" in e else "")
                     for _c, e in sorted(r["eyes"].items()))
    terms = " ".join("%s %+.1f" % kv for kv in sorted(r["terms"].items()))
    print("%-7s %s | %s%s" % (r["state"], eyes, terms, " (provisional)" if r["provisional"] else ""))
    for h in r["hints"]:
        print("        -> " + h["text"])


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if argv[0] == "reference":
        paths = argv[1:] or [DOTS]
        rows = []
        for p in paths:
            with open(p) as f:
                rows += [json.loads(line) for line in f if line.strip()]
        ref = Reference.from_dots(rows, source="calibration dots: " + ", ".join(os.path.basename(p) for p in paths))
        print(json.dumps(ref.to_json(), indent=1))
        return
    if argv[0] == "live":
        ref = Reference.load()
        baseline = load_baseline()
        obs = Observations()
        finder = LiveFinder(obs, log=print).start()
        last = None
        try:
            while finder.error is None:
                time.sleep(0.5)
                last = assess(obs.summary(), ref, baseline=baseline, previous=last)
                _print_reading(last)
        except KeyboardInterrupt:
            pass
        finder.stop()
        if "--baseline" in argv and last and last["state"] in ("ok", "adjust"):
            _write_json(BASELINE, make_baseline(obs.summary()))
            print("saved the baseline: " + BASELINE)
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
