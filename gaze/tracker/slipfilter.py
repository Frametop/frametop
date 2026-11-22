"""slipfilter: where one eye sits in its camera's image now, against the calibration, learnt the
whole time the headset is worn and not only at clicks (ft-eyes; gaze/README.md, "Slip").

The slip is in the calibration's feature units: pixels of pupil centre, or the eye net's
degrees. The pupil now, less where the calibration's `where` fit says it sits for the same
gaze, is the slip; ft-eyes takes it off each pupil before the `pupil` fit. One small Kalman
filter per eye holds it, with two sensor biases:

  s        the slip
  b_glint  how far the glint estimate (eyes_model.SlipTracker) reads off s
  b_steam  how far SteamVR's look, through our `where` fit, reads off s (SteamVR's own error)

It takes three kinds of reading:

  click    z = s            a nudge's truth (ft-gazed's "click"): the one that pins the slip
  glint    z = s + b_glint  the glint pair's slip estimate, every GLINT_EVERY s
  steam    z = s + b_steam  SteamVR's look while it held still (ft-gazed's "look")

Between readings s wanders at SLIP_RATE (ADJUST_RATE while someone moves the headset on
purpose: `adjusting`, while the fit pill is open) and each bias at BIAS_RATE. So a nudge counts
most just after it and fades as the headset may have moved since, while the steady readings
follow a slow slip between nudges. A click pins s, and through the state's covariance it also
says how far each sensor read off, so the sensors stay honest without a second calibration.

The headset off and on again (`reseat`: frames stopped, or ft-eyes restarted) makes s nearly
unknown, so the next readings of any kind set it.

A reading far from what the filter expects (its Mahalanobis distance past HUBER) counts for less
(Huber weighting): a misread glint pair or a SteamVR blink pulls little. Clicks are judged
across both eyes instead (teach_click): both eyes off by MOVE_DEG the same way is the headset
moved, and that click is taken nearly whole; one eye far off while the other agrees counts for
less, as a bad find in that eye. Glint readings are judged across the eyes too
(eyeslip.take_glint): both eyes' glints far off the filter the same way for two readings in a
row, looking near straight ahead, is the headset knocked, taken nearly whole too, as the old
glint jump did.

Noise settings are first guesses (2026-11-22) from the click replay and the calibration dots:
tune them from slip.jsonl (ft-eyes' log). They're in degrees of gaze; `jac` (the `where` fit's
d feature / d gaze at straight ahead) turns them into feature units per eye.

A click's pupils are from just before its time T, which can be up to 10 s ago (ft-gazed's
lessons), but it updates the filter as it stands now: the slip changes slowly, as the old
median-of-five did too.
"""
import math

import numpy as np

CLICK_SD = 0.6        # degrees: a click's reading (the pointer's truth, the pupil's median)
GLINT_SD = 3.0        # the glint estimate (1-4 px over its window)
STEAM_SD = 3.0        # SteamVR's look after its affine (1.8 median left, 2026-10-17 bench, and
                      # much of it depends on where you look, so it doesn't average away fast)
SLIP_RATE = 0.15      # degrees per root second: the slip's wander while worn (3.5 in 10 min).
                      # 250 clicks replayed (clicks only, 2026-11-22): 0.3 predicted the next
                      # best (2.7 px median against the median-of-five's 3.1; 0.15: 2.9), but
                      # they were mostly the headset moved on purpose; faster lets SteamVR's
                      # error shake the slip more (it follows its looks over sqrt(R dt / q))
ADJUST_RATE = 3.0     # ...while the headset is being moved on purpose: SteamVR's looks alone
                      # follow a move nine tenths in two seconds
BIAS_RATE = 0.02      # a sensor's bias wanders far slower
START_SD = 1.0        # after a calibration: s is about 0 (the dots were taken at that seating,
                      # though it may have moved by its last), so the first click counts for 0.6
GLINT_START_SD = 3.0  # each bias unknown until readings and clicks tell them apart
STEAM_START_SD = 4.0  # (SteamVR raw is off by up to a quarter of the look: biased, not noise)
RESEAT_SD = 12.0      # the headset off and on: re-seats moved the pupil 20-30 px
RESEAT_GLINT_SD = 1.5  # the glint pair and SteamVR read a new seating a little differently too
RESEAT_STEAM_SD = 1.0
HUBER = 3.0           # Mahalanobis distance past which a steady reading counts for less
MOVE_DEG = 3.5        # a click off this much in both eyes, the same way: the headset moved
MOVE_COS = 0.5        # ("the same way": the cosine between the two eyes' innovations)
LONE_DEG = 5.0        # one eye off this much while the other is within AGREE_DEG: a bad find
AGREE_DEG = 2.0

S, GLINT, STEAM = slice(0, 2), slice(2, 4), slice(4, 6)
_I2 = np.eye(2)
_Z2 = np.zeros((2, 2))
_H = {"click": np.hstack([_I2, _Z2, _Z2]), "glint": np.hstack([_I2, _I2, _Z2]),
      "steam": np.hstack([_I2, _Z2, _I2])}
_SD = {"click": CLICK_SD, "glint": GLINT_SD, "steam": STEAM_SD}


class SlipFilter:
    """One eye's slip and its sensors' biases. `jac`: the calibration's d feature / d gaze for
    this eye (2x2, feature units per degree; eyes_model.Calibration.jacobian). Times are frame
    times (CLOCK_MONOTONIC_RAW live, the recording's in a replay); one older than the last is
    taken as the last."""

    def __init__(self, jac, s=(0.0, 0.0)):
        self.jac = np.asarray(jac, float)
        det = abs(float(np.linalg.det(self.jac)))
        if not np.all(np.isfinite(self.jac)) or det < 1e-6:
            self.jac, det = np.eye(2), 1.0   # a degenerate fit: read feature units as degrees
        self.unit = math.sqrt(det)           # feature units per degree, for the noise settings
        self.x = np.zeros(6)
        self.x[S] = s
        self.P = np.diag([START_SD] * 2 + [GLINT_START_SD] * 2 + [STEAM_START_SD] * 2) ** 2 * self.unit ** 2
        self.t = None
        self.adjust_until = -math.inf
        self.clicks = 0          # clicks taught since the calibration (since the last reseat's first)
        self.reseated = False    # the headset may sit anywhere: set by reseat(), cleared by a click

    # --- What it says ---

    @property
    def s(self):
        return self.x[S].copy()

    @property
    def taught(self):
        """A click has said where it sits since the calibration or the last reseat."""
        return bool(self.clicks) and not self.reseated

    def deg(self, v):
        """Feature units as the gaze change they'd be (yaw, pitch degrees)."""
        return np.linalg.solve(self.jac, np.asarray(v, float))

    def slip_deg(self):
        """The slip as the gaze error it would cause looking straight ahead: (yaw, pitch)
        degrees, the sign fitcoach.assess's offsets have (+ pitch: the eyes sit high)."""
        return self.deg(self.x[S])

    def slip_sd(self):
        """How sure that is: the largest standard deviation of slip_deg (degrees)."""
        Ji = np.linalg.inv(self.jac)
        C = Ji @ self.P[S, S] @ Ji.T
        return float(math.sqrt(max(np.linalg.eigvalsh((C + C.T) / 2).max(), 0.0)))

    def bias_deg(self, which):
        return self.deg(self.x[GLINT if which == "glint" else STEAM])

    # --- Time ---

    def predict(self, t):
        """Moves the filter on to time t: the slip and biases may have wandered since."""
        if self.t is None or t <= self.t:
            self.t = t if self.t is None else self.t
            return
        dt = t - self.t
        fast = min(max(self.adjust_until - self.t, 0.0), dt)   # the part of dt under the lease
        u2 = self.unit ** 2
        self.P[S, S] += _I2 * ((ADJUST_RATE ** 2 * fast + SLIP_RATE ** 2 * (dt - fast)) * u2)
        for b in (GLINT, STEAM):
            self.P[b, b] += _I2 * (BIAS_RATE ** 2 * dt * u2)
        self.t = t

    def adjusting(self, t, seconds):
        """The headset is being moved on purpose for the next `seconds` from t: s may change fast."""
        self.predict(t)
        self.adjust_until = max(self.adjust_until, t + seconds)

    def reseat(self):
        """The headset was off (or ft-eyes restarted): it may sit anywhere now."""
        u2 = self.unit ** 2
        self.P[S, S] += _I2 * (RESEAT_SD ** 2 * u2)
        self.P[GLINT, GLINT] += _I2 * (RESEAT_GLINT_SD ** 2 * u2)
        self.P[STEAM, STEAM] += _I2 * (RESEAT_STEAM_SD ** 2 * u2)
        self.reseated = True

    # --- Readings ---

    def expect(self, kind):
        return _H[kind] @ self.x

    def innovation(self, kind, z, t):
        """How far a reading z of `kind` at t is from what the filter expects (feature units)."""
        self.predict(t)
        return np.asarray(z, float) - _H[kind] @ self.x

    def distance(self, kind, y):
        """How unlikely an innovation y of `kind` is: its Mahalanobis distance (sds)."""
        H = _H[kind]
        S_ = H @ self.P @ H.T + _I2 * (_SD[kind] * self.unit) ** 2
        y = np.asarray(y, float)
        return float(math.sqrt(max(y @ np.linalg.solve(S_, y), 0.0)))

    def inflate(self, y):
        """The headset moved by about y (feature units): s is that much less sure, so the
        readings that say so are taken nearly whole."""
        y = np.asarray(y, float)
        self.P[S, S] += _I2 * float(y @ y)

    def update(self, kind, z, t, sd_scale=1.0, robust=True):
        """Takes a reading z (feature units) of `kind` at time t. sd_scale widens its noise.
        With `robust`, a reading past HUBER counts for less. What happened, for the log:
        {"kind", "innov_deg", "d", "w"}. A reading or time that isn't finite: ValueError."""
        z = np.asarray(z, float)
        if not (np.all(np.isfinite(z)) and math.isfinite(t) and math.isfinite(sd_scale)):
            raise ValueError("a reading that isn't finite")
        self.predict(t)
        H = _H[kind]
        R = _I2 * (_SD[kind] * sd_scale * self.unit) ** 2
        y = np.asarray(z, float) - H @ self.x
        S_ = H @ self.P @ H.T + R
        d = float(math.sqrt(max(y @ np.linalg.solve(S_, y), 0.0)))
        w = 1.0
        if robust and d > HUBER:
            w = HUBER / d
            R = R / w
            S_ = H @ self.P @ H.T + R
        K = self.P @ H.T @ np.linalg.inv(S_)
        self.x = self.x + K @ y
        A = np.eye(6) - K @ H
        self.P = A @ self.P @ A.T + K @ R @ K.T   # Joseph form: stays symmetric and positive
        self.P = (self.P + self.P.T) / 2
        return {"kind": kind, "innov_deg": [round(float(v), 3) for v in self.deg(y)], "d": round(d, 2),
                "w": round(w, 3)}

    def click(self, z, t, sd_scale=1.0, moved=False):
        """A click's reading. `moved`: the headset moved (teach_click), so the slip is taken
        nearly whole: its uncertainty grows by the innovation first."""
        if moved:
            self.inflate(self.innovation("click", z, t))
        r = self.update("click", z, t, sd_scale, robust=False)
        self.clicks = 1 if self.reseated else self.clicks + 1
        self.reseated = False
        return r

    # --- Kept across restarts (state.json) ---

    def to_json(self):
        return {"x": [round(float(v), 4) for v in self.x], "P": [[float(v) for v in row] for row in self.P],
                "clicks": self.clicks}

    @classmethod
    def from_json(cls, d, jac):
        """A saved filter; or the old eyes_model.Shift's JSON ({"base": ...}: its slip, no more);
        or a fresh one if d doesn't read."""
        f = cls(jac)
        try:
            if "x" in d:
                x, P = np.asarray(d["x"], float), np.asarray(d["P"], float)
                if x.shape == (6,) and P.shape == (6, 6) and np.all(np.isfinite(x)) and np.all(np.isfinite(P)):
                    f.x, f.P = x, (P + P.T) / 2
                f.clicks = int(d.get("clicks", 0))
            elif "base" in d:
                f.x[S] = np.asarray(d["base"], float)[:2]
                f.clicks = len(d.get("meas") or [])
        except (TypeError, ValueError, KeyError):
            return cls(jac)
        return f


def moved(a, b):
    """Two eyes' innovations (degrees) say the headset moved: both past MOVE_DEG, the same way."""
    na, nb = float(np.hypot(*a)), float(np.hypot(*b))
    return min(na, nb) > MOVE_DEG and float(np.asarray(a) @ np.asarray(b)) / max(na * nb, 1e-9) > MOVE_COS


def teach_click(filters, lessons, t):
    """One click for both eyes: `filters` {eye: SlipFilter}, `lessons` {eye: z} for the eyes it
    teaches (seen, and within their calibration dots). Judged across the eyes first (see the
    top). Returns {eye: what happened}, each with "gate": "moved", "lone" or "plain"."""
    innov = {e: filters[e].deg(filters[e].innovation("click", z, t)) for e, z in lessons.items()}
    size = {e: float(np.hypot(*v)) for e, v in innov.items()}
    both = len(innov) == 2 and moved(innov[0], innov[1])
    out = {}
    for e, z in lessons.items():
        scale, gate = 1.0, "moved" if both else "plain"
        o = 1 - e
        if not both and o in size and size[e] > LONE_DEG and size[o] <= AGREE_DEG:
            scale, gate = size[e] / AGREE_DEG, "lone"
        r = filters[e].click(z, t, scale, both)
        r["gate"] = gate
        out[e] = r
    return out
