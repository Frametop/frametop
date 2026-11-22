"""clickrefit: a per-eye linear correction learned from clicks, on top of the calibration.

The slip filter (slipfilter.py) keeps the shift: where the pupil sits for a given gaze. What a
shift can't fix is the fit's shape after the headset moves: the eye seen at a slightly new
angle and distance turns the same pupil move into a slightly bigger or turned gaze move, so a
shift that's right in the middle leaves the edges off. A Refit is that part: a 2x2 matrix A
about the eye's calibration centre, gaze' = centre + A (gaze - centre).

`fit` makes one from recent clicks (each a calibration gaze with the shift of the time, where
you looked, and a weight), held near the identity, and keeps it only when it predicts the
newest clicks, held back, clearly better than none. The shift then stays the filter's: a click
teaches it against `undo(truth)`, the gaze the calibration should have given. Replayed over
2026-10-18..11-22's clicks (lab/ft-eyes-clickrefit), it was on for 75 of 465 and took those
from 2.78 to 2.25 degrees (median); fitted every time, ungated, it made them worse (3.7).

A `Learner` keeps one eye's clicks for its refit and, while its calibration has no glint fit
(the glint pair seen at too few dots), makes one from clicks that saw the pair.
"""
import numpy as np

import eyes_model

KEEP = 30          # the newest clicks a refit is made from
HALF = 15          # clicks: a click this many back counts half (newer ones say where it sits now)
GLANCE = 0.5       # a glance lesson's weight (a free mouse's click says less of where you looked)
MIN = 8            # clicks needed
SPREAD = 4.0       # degrees: the clicks' spread (SD) needed in yaw and in pitch
HOLD = 5           # the newest clicks held back to judge a refit
RIDGE = 2.0        # pull toward the identity, in clicks' worth at 10 degrees out
MARGIN = 0.25      # degrees: a refit must beat none on the held-back clicks by this (median)...
RATIO = 0.85       # ...and by this factor
TRIM = 6.0         # degrees: clicks this far off the rest (their median offset) aren't fitted:
                   # a click away from where you looked (12 degrees at the p90) bends a fit most
SCALE = (0.75, 1.35)   # A's singular values must lie within this
TURN = 12.0        # degrees: and it may turn the gaze by no more than this


class Refit:
    def __init__(self, centre, A=None, info=None):
        self.centre = np.asarray(centre, float)
        self.A = np.eye(2) if A is None else np.asarray(A, float)
        self.info = info or {}

    def apply(self, gaze):
        return self.centre + self.A @ (np.asarray(gaze, float) - self.centre)

    def undo(self, truth):
        """The calibration's gaze this refit turns into `truth`."""
        return self.centre + np.linalg.solve(self.A, np.asarray(truth, float) - self.centre)

    def to_json(self):
        return {"centre": self.centre.tolist(), "A": self.A.tolist(), "info": self.info}

    @classmethod
    def from_json(cls, d):
        return cls(d["centre"], d["A"], d.get("info"))


def weights(glances):
    """Recency and glance weights for clicks oldest first (`glances` their glance flags)."""
    n = len(glances)
    return np.array([0.5 ** ((n - 1 - i) / HALF) * (GLANCE if g else 1.0) for i, g in enumerate(glances)])


def solve(G, T, w, centre):
    """A and an offset b (degrees) minimising sum w |A (g - c) + b - (t - c)|^2 + ridge |A - I|^2."""
    X, Y = G - centre, T - centre
    D = np.c_[X, np.ones(len(X))]
    R = RIDGE * 100.0 * np.diag([1.0, 1.0, 0.0])
    M = D.T @ (D * w[:, None]) + R
    I = np.r_[np.eye(2), np.zeros((1, 2))]
    W = np.linalg.solve(M, D.T @ (Y * w[:, None]) + R @ I)
    return W[:2].T, W[2]


def offset(G, T, w):
    """The identity's best offset: the weighted mean miss."""
    return (w[:, None] * (T - G)).sum(0) / w.sum()


def bounded(A):
    s = np.linalg.svd(A, compute_uv=False)
    turn = np.degrees(abs(np.arctan2(A[1, 0] - A[0, 1], A[0, 0] + A[1, 1])))
    return SCALE[0] <= s.min() and s.max() <= SCALE[1] and turn <= TURN


def trim(clicks):
    """The clicks within TRIM of the others' median offset."""
    if len(clicks) < 3:
        return clicks
    G = np.array([c[0] for c in clicks], float)
    T = np.array([c[1] for c in clicks], float)
    miss = np.hypot(*(G + np.median(T - G, 0) - T).T)
    return [c for c, m in zip(clicks, miss) if m < TRIM]


def fit(clicks, centre):
    """A Refit from `clicks`, oldest first, each (gaze, truth, glance): gaze the calibration's
    with the shift before that click, truth where you looked. Returns (Refit or None, why)."""
    clicks = trim(clicks[-KEEP:])
    if len(clicks) < MIN + HOLD:
        return None, f"{len(clicks)} clicks (needs {MIN + HOLD})"
    G = np.array([c[0] for c in clicks], float)
    T = np.array([c[1] for c in clicks], float)
    w = weights([c[2] for c in clicks])
    sd = T.std(0)
    if sd.min() < SPREAD:
        return None, f"clicks spread {sd[0]:.1f}, {sd[1]:.1f} degrees (needs {SPREAD})"
    train, test = slice(None, -HOLD), slice(-HOLD, None)
    A, b = solve(G[train], T[train], w[train], centre)
    b0 = offset(G[train], T[train], w[train])
    with_ = np.median(np.hypot(*(centre + (G[test] - centre) @ A.T + b - T[test]).T))
    without = np.median(np.hypot(*(G[test] + b0 - T[test]).T))
    if with_ > without - MARGIN or with_ > RATIO * without:
        return None, f"held-back clicks {with_:.2f} vs {without:.2f} degrees without"
    A, _ = solve(G, T, w, centre)
    if not bounded(A):
        return None, "out of bounds"
    return Refit(centre, A, {"clicks": len(clicks), "held_back": [round(float(with_), 2),
                                                                  round(float(without), 2)]}), "ok"


GLINT_KEEP = 60    # clicks kept for a glint fit made from clicks


class Learner:
    """What one eye's clicks teach beyond the shift: its refit, and its glint fit while `cal`
    has none (put in cal.fits, where the slip tracker and knock detection find it)."""

    def __init__(self, cal, eye, saved=None):
        self.cal, self.eye = cal, eye
        b = cal.box.get(eye)
        self.centre = np.array([(b[0] + b[1]) / 2, (b[2] + b[3]) / 2]) if b else np.zeros(2)
        self.own_glint = not cal.has("glint", eye)
        saved = saved or {}
        self.clicks = [(np.array(g), np.array(t), bool(gl)) for g, t, gl in saved.get("clicks", [])]
        self.glints = [(np.array(x), np.array(t)) for x, t in saved.get("glints", [])]
        self.refit = Refit.from_json(saved["refit"]) if saved.get("refit") else None
        if self.own_glint:
            self._glint_fit()

    def apply(self, gaze):
        return self.refit.apply(gaze) if self.refit else gaze

    def undo(self, truth):
        return self.refit.undo(truth) if self.refit else np.asarray(truth, float)

    def add(self, raw, truth, glance, pupil, mid):
        """A click: `raw` the calibration's gaze with the shift before it, `truth` where you
        looked, `pupil` and the glint pair's `mid` (or None) over its window. Returns what
        changed, for the log: [] or dicts."""
        events = []
        self.clicks = (self.clicks + [(np.asarray(raw, float), np.asarray(truth, float), bool(glance))])[-KEEP:]
        was = self.refit
        self.refit, why = fit(self.clicks, self.centre)
        if (was is None) != (self.refit is None):
            events.append({"refit": "on" if self.refit else "off", "why": why,
                           **({"A": np.round(self.refit.A, 3).tolist()} if self.refit else {})})
        if mid is not None and not glance:
            self.glints = (self.glints + [(np.asarray(pupil, float) - np.asarray(mid, float),
                                           np.asarray(truth, float))])[-GLINT_KEEP:]
            if self.own_glint:
                had = self.cal.has("glint", self.eye)
                if self._glint_fit() and not had:
                    events.append({"glint_fit": len(self.glints)})
        return events

    def _glint_fit(self):
        """A glint fit from the clicks that saw the pair, once enough are spread out: True if made."""
        keep = self.glints
        if len(keep) < eyes_model.MIN_CLICKS or np.array([t for _, t in keep]).std(0).min() < SPREAD:
            self.cal.fits.pop(("glint", self.eye), None)
            return False
        X, T = np.array([x for x, _ in keep]), np.array([t for _, t in keep])
        q = eyes_model.Quad(X, T)
        good = np.hypot(*(q(X) - T).T) < TRIM
        if good.sum() >= eyes_model.MIN_CLICKS:
            q = eyes_model.Quad(X[good], T[good])
        self.cal.fits["glint", self.eye] = q
        return True

    def to_json(self):
        r = lambda v: [round(float(x), 3) for x in v]  # noqa: E731
        return {"clicks": [[r(g), r(t), gl] for g, t, gl in self.clicks],
                "glints": [[r(x), r(t)] for x, t in self.glints],
                "refit": self.refit.to_json() if self.refit else None}

    def status(self):
        return {"refit": np.round(self.refit.A, 3).tolist() if self.refit else None,
                "clicks": len(self.clicks),
                "glint_fit": ("calibration" if not self.own_glint else
                              "clicks" if self.cal.has("glint", self.eye) else None),
                "glint_clicks": len(self.glints)}
