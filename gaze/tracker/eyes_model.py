"""The gaze calibration: from pupil and glint positions to head-relative gaze angles.

Shared by ft-eyes (live) and the lab tools (fitting and scoring on recordings). Gaze angles are
ft-gaze's: degrees relative to the head, yaw positive to the left, pitch positive up.

Per eye (0 = right, camera 0; 1 = left, camera 1), three quadratic fits:
  pupil   pupil centre -> gaze. The one used for output, after the slip correction.
  glint   pupil minus the glint pair's midpoint -> gaze. Slip moves both alike, so this
          holds up when the headset shifts, but it's noisier, and the pair is often gone.
  where   gaze -> pupil centre: where the pupil sits for a gaze, with no slip.
Slip: wherever the pair is seen, `glint` gives the gaze, `where` says where the pupil should
be, and the difference is how far the eye has moved in the image. Slip changes slowly, so
the median over the last SLIP_WINDOW seconds shifts every frame, glints or not.

That glint estimate is only good to 1-4 px (1-3 degrees), though. Clicks are better: each one
says where the pupil should have been for a known gaze (`where`), so pupil minus that is the
shift. `Shift` keeps the median of the last few, and uses the glints only to notice a sudden
jump (the headset nudged or put back on), until clicks catch up. On practice2 with
practice1's calibration: 1.2 degrees median, against 2.7 with the glints alone (findings.md).
"""
import json
import time
from collections import deque

import numpy as np

SLIP_WINDOW = 30.0
SLIP_MIN = 10        # pair sightings needed before trusting a slip estimate
MIN_CLICKS = 12
SPREAD_MIN = 0.3     # degrees: floor for an eye's fit spread, so one eye can't take all the weight
SHIFT_KEEP = 5       # clicks in the shift estimate
SHIFT_JUMP = 3.0     # a glint slip change this big (px) since the last click is a nudge
JUMP_HOLD = 1.0      # s: ...if it holds this long (a bad glint pair gives a jump that snaps back)
JUMP_MAX = 40.0      # px: bigger is a bad glint pair, not the headset (a re-seat moved 20-30)
JUMP_WINDOW = 10.0   # seconds of glint sightings for noticing a jump


class Quad:
    """Ridged quadratic least squares from 2-D inputs, normalised on the training set."""

    def __init__(self, X=None, Y=None, ridge=1e-3):
        if X is None:
            return
        X, Y = np.asarray(X, float), np.asarray(Y, float)
        self.m, self.s = X.mean(0), X.std(0) + 1e-9
        A = self.terms(X)
        R = ridge * np.eye(A.shape[1])
        R[0, 0] = 0
        self.w = np.linalg.solve(A.T @ A + R, A.T @ Y)

    def terms(self, X):
        P = (np.atleast_2d(np.asarray(X, float)) - self.m) / self.s
        return np.c_[np.ones(len(P)), P, P ** 2, P[:, 0] * P[:, 1]]

    def __call__(self, X):
        return self.terms(X) @ self.w

    def one(self, x, y):
        """Faster for a single point (the live path)."""
        px, py = (x - self.m[0]) / self.s[0], (y - self.m[1]) / self.s[1]
        t = np.array([1.0, px, py, px * px, py * py, px * py])
        return t @ self.w

    def to_json(self):
        return {"m": self.m.tolist(), "s": self.s.tolist(), "w": self.w.tolist()}

    @classmethod
    def from_json(cls, d):
        q = cls()
        q.m, q.s, q.w = (np.array(d[k]) for k in ("m", "s", "w"))
        return q


def pair_mid(pair):
    return ((pair[0][0] + pair[1][0]) / 2, (pair[0][1] + pair[1][1]) / 2)


def eye_of(click, eye):
    """A click's features for one eye, or None: `eye` is a list by eye (the lab tools) or a dict
    of the eyes seen (ft-eyes' calibration dots, which may hold one)."""
    e = click["eye"]
    return e.get(eye) if isinstance(e, dict) else e[eye]


class Calibration:
    """The three fits per eye. Build from clicks (ft-eyes-score's features) or load from JSON.

    `spread` is each eye's RMS miss (degrees) on the clicks its pupil fit was made from.
    `combine` weights the eyes by its inverse square: on practice2 (one headset position,
    leave-one-out) that gave 0.75 median against 0.84 for the plain average, since one eye
    is usually much better than the other (left 0.73, right 1.32 there).

    Each eye is fitted on the clicks it was seen in, so a click may hold one eye only, and an
    eye seen in under MIN_CLICKS is left out (the calibration then maps the other alone). A
    second person's eyes (2026-10-17 rounds) each lost the pupil looking 20 degrees outward:
    with only the clicks that saw both, each eye's fit had none out there and guessed (frame
    errors 8.1 degrees median, 84 p90, held-out takes); each on its own clicks, 2.3 and 5.0."""

    def __init__(self, fits=None, info=None, spread=None):
        self.fits = fits or {}   # (name, eye) -> Quad
        self.info = info or {}
        self.spread = spread or {}  # eye -> degrees

    @classmethod
    def fit(cls, clicks, info=None):
        truth = lambda cs: np.array([k["truth"] for k in cs])  # noqa: E731
        fits, spread = {}, {}
        for c in (0, 1):
            cs = [k for k in clicks if eye_of(k, c) is not None]
            if len(cs) < MIN_CLICKS:
                continue
            pupils = [eye_of(k, c)["pupil"] for k in cs]
            fits["pupil", c] = Quad(pupils, truth(cs))
            miss = fits["pupil", c](pupils) - truth(cs)
            spread[c] = float(np.sqrt(np.mean(np.sum(miss ** 2, axis=1))))
            fits["where", c] = Quad(truth(cs), pupils)
            gs = [k for k in cs if eye_of(k, c)["mid"] is not None]
            if len(gs) >= MIN_CLICKS:
                fits["glint", c] = Quad([eye_of(k, c)["pupil"] - eye_of(k, c)["mid"] for k in gs], truth(gs))
        return cls(fits, info, spread)

    def has(self, name, eye):
        return (name, eye) in self.fits

    def eyes(self):
        """The eyes it maps (0 right, 1 left)."""
        return [c for c in (0, 1) if self.has("pupil", c)]

    def slip(self, eye, rows):
        """Median slip in pixels from pair sightings `rows` (t, px, py, mx, my), or None."""
        if not self.has("glint", eye) or len(rows) < SLIP_MIN:
            return None
        rows = np.asarray(rows, float)
        gaze = self.fits["glint", eye](rows[:, 1:3] - rows[:, 3:5])
        return np.median(rows[:, 1:3] - self.fits["where", eye](gaze), axis=0)

    def click_shift(self, eye, pupil, truth):
        """The shift a click measures: the pupil, less where it sits for that gaze."""
        return np.asarray(pupil, float) - self.fits["where", eye].one(*truth)

    def gaze(self, eye, x, y, slip=None):
        """Gaze (yaw, pitch) for a pupil centre, less a slip if there is one."""
        if slip is not None:
            x, y = x - slip[0], y - slip[1]
        return self.fits["pupil", eye].one(x, y)

    def weight(self, eye):
        s = self.spread.get(eye)
        return 1.0 if s is None else 1.0 / max(s, SPREAD_MIN) ** 2

    def combine(self, gazes):
        """The weighted mean of {eye: (yaw, pitch)}, or None if empty."""
        if not gazes:
            return None
        w = {c: self.weight(c) for c in gazes}
        return sum(w[c] * np.asarray(g, float) for c, g in gazes.items()) / sum(w.values())

    def save(self, path):
        d = {"version": 1, "info": self.info, "spread": {str(e): s for e, s in self.spread.items()},
             "fits": [{"name": n, "eye": e, **q.to_json()} for (n, e), q in self.fits.items()]}
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(d, indent=1))
        tmp.replace(path)

    @classmethod
    def load(cls, path):
        d = json.loads(path.read_text())
        return cls({(f["name"], f["eye"]): Quad.from_json(f) for f in d["fits"]}, d.get("info"),
                   {int(e): s for e, s in d.get("spread", {}).items()})


class Shift:
    """Where one eye sits in the image now, relative to the calibration (pixels).

    `base` is the median shift the last SHIFT_KEEP clicks measured. `ref` is the glint slip
    estimate at the last click; if the glint estimate has since moved more than SHIFT_JUMP
    (and less than JUMP_MAX) and stayed there for JUMP_HOLD seconds, the headset moved, and
    the change is added until the next click. That click then starts the history over,
    since the older ones describe the old position. Live, bad glint pairs made the estimate
    leap by up to 68 px for under a second (practice2), hence the hold. `reseat` does the
    same for the next click without the glints: the frames stopped (the headset was off),
    so the headset may be anywhere now."""

    def __init__(self, base=(0.0, 0.0), ref=None):
        self.meas = []
        self.base = np.asarray(base, float)
        self.ref = None if ref is None else np.asarray(ref, float)
        self.jump = np.zeros(2)
        self.held = None  # (change, since when) while a jump waits out JUMP_HOLD
        self.reseated = False

    def glint(self, g, t):
        """The latest glint slip estimate (or None), at time t (s)."""
        if g is None:
            return
        if self.ref is None:
            self.ref = np.asarray(g, float)
        d = np.asarray(g, float) - self.ref
        size = np.hypot(*d)
        if size > JUMP_MAX:
            return
        if size <= SHIFT_JUMP:
            self.jump, self.held = np.zeros(2), None
            return
        if self.held is None or np.hypot(*(d - self.held[0])) > SHIFT_JUMP:
            self.held = (d, t)
        elif t - self.held[1] >= JUMP_HOLD:
            self.jump = d

    def reseat(self):
        self.reseated = True

    def click(self, d, g=None):
        """A click measured the shift d; g is the glint estimate then."""
        if np.any(self.jump) or self.reseated:
            self.meas = []
            self.reseated = False
        self.meas = (self.meas + [np.asarray(d, float)])[-SHIFT_KEEP:]
        self.base = np.median(self.meas, axis=0)
        self.jump, self.held = np.zeros(2), None
        if g is not None:
            self.ref = np.asarray(g, float)

    @property
    def value(self):
        return self.base + self.jump

    def to_json(self):
        return {"base": self.base.tolist(), "ref": None if self.ref is None else self.ref.tolist(),
                "meas": [m.tolist() for m in self.meas]}

    @classmethod
    def from_json(cls, d):
        s = cls(d.get("base", (0, 0)), d.get("ref"))
        s.meas = [np.asarray(m, float) for m in d.get("meas", [])]
        return s


class SlipTracker:
    """Live slip estimate for one eye: pair sightings over the last SLIP_WINDOW seconds,
    re-estimated at most every `every` seconds."""

    def __init__(self, cal, eye, every=0.5, window=SLIP_WINDOW):
        self.cal, self.eye, self.every, self.window = cal, eye, every, window
        self.rows = deque()
        self.value, self.at = None, 0.0

    def add(self, t, pupil, mid):
        self.rows.append((t, pupil[0], pupil[1], mid[0], mid[1]))
        while self.rows and self.rows[0][0] < t - self.window:
            self.rows.popleft()

    def get(self, now=None):
        now = time.monotonic() if now is None else now
        if now - self.at >= self.every:
            self.at = now
            s = self.cal.slip(self.eye, list(self.rows))
            if s is not None:
                self.value = s
        return self.value
