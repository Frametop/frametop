"""slipfilter: where each eye sits in its camera's image now, against the calibration, learnt the
whole time the headset is worn and not only at clicks (ft-eyes; gaze/README.md, "Slip").

An eye's slip is in the calibration's feature units: pixels of pupil centre, or the eye net's
degrees. The pupil now, less where the calibration's `where` fit says it sits for the same
gaze, is the slip; ft-eyes takes it off each pupil before the `pupil` fit. Through the fit's
`jac` (d feature / d gaze at straight ahead) it is a gaze error in degrees, and in degrees one
small Kalman filter (Headset) holds both eyes':

  h        the headset's slip: what a move of the headset does to both eyes' gaze
  own_e    eye e's own part: its fit's error where the clicks were, a pupil find off in its
           own way. Each eye's slip is h + own_e
  glint_e  how far eye e's glint estimate (eyes_model.SlipTracker) reads off its slip
  steam_e  how far SteamVR's look, through our `where` fit, reads off it (SteamVR's own error)

It takes three kinds of reading, from either eye:

  click    z = h + own_e            a nudge's truth (ft-gazed's "click"): the one that pins it
  glint    z = h + own_e + glint_e  the glint pair's slip estimate, every GLINT_EVERY s
  steam    z = h + own_e + steam_e  SteamVR's look while it held still (ft-gazed's "look")

Between readings h wanders at SLIP_RATE (ADJUST_RATE while someone moves the headset on
purpose: `adjusting`, while the fit pill is open), each eye's own part at OWN_RATE and each bias
at BIAS_RATE. So a nudge counts most just after it and fades as the headset may have moved
since, while the steady readings follow a slow slip between nudges. Since h moves fastest, a
reading off in both eyes moves the headset's slip for both, while what the clicks keep finding
in one eye alone stays that eye's own: one eye's error doesn't pose as the headset moving, and
the fit pill reads the eyes' mean slip. A click pins the slip, and through the state's covariance it also
says how far each sensor read off, so the sensors stay honest without a second calibration.

The headset off and on again (`reseat`: frames stopped, or ft-eyes restarted) makes h nearly
unknown (and each own part a little less sure), so the next readings of any kind set it.

A reading far from what the filter expects (its Mahalanobis distance past HUBER) counts for less
(Huber weighting): a misread glint pair or a SteamVR blink pulls little. Clicks are judged
across both eyes instead (teach_click): both eyes off by MOVE_DEG the same way is the headset
moved, and that click is taken nearly whole; one eye far off while the other agrees counts for
less, as a bad find in that eye. Glint readings are judged across the eyes too
(eyeslip.take_glint): both eyes' glints far off the filter the same way for two readings in a
row, looking near straight ahead, is the headset knocked, taken nearly whole too.

Noise settings are first guesses (2026-11-22) from the click replay and the calibration dots:
tune them from slip.jsonl (ft-eyes' log). They're in degrees of gaze.

A click's pupils are from just before its time T, which can be up to 10 s ago (ft-gazed's
lessons), but it updates the filter as it stands now: the slip changes slowly, as the old
median-of-five did too.

Each eye reads the filter through its SlipFilter: the one eye's view, in its feature units.
"""
import math

import numpy as np

CLICK_SD = 0.6        # degrees: a click's reading (the pointer's truth, the pupil's median)
GLINT_SD = 3.0        # the glint estimate (1-4 px over its window)
STEAM_SD = 3.0        # SteamVR's look after its affine (1.8 median left, 2026-10-17 bench, and
                      # much of it depends on where you look, so it doesn't average away fast)
SLIP_RATE = 0.15      # degrees per root second: the headset's slip's wander while worn (3.5 in
                      # 10 min). 250 clicks replayed (clicks only, 2026-11-22): 0.3 predicted the
                      # next best (2.7 px median against the median-of-five's 3.1; 0.15: 2.9), but
                      # they were mostly the headset moved on purpose; faster lets SteamVR's
                      # error shake the slip more (it follows its looks over sqrt(R dt / q))
ADJUST_RATE = 3.0     # ...while the headset is being moved on purpose: SteamVR's looks alone
                      # follow a move nine tenths in two seconds
OWN_RATE = 0.05       # an eye's own part wanders far slower: it's the fit's and the find's
BIAS_RATE = 0.02      # a sensor's bias wanders slower still
START_SD = 1.0        # after a calibration: h is about 0 (the dots were taken at that seating,
                      # though it may have moved by its last), so the first click counts most
OWN_START_SD = 0.7    # each eye's own part about 0 too: its fit's error at the dots
GLINT_START_SD = 3.0  # each bias unknown until readings and clicks tell them apart
STEAM_START_SD = 4.0  # (SteamVR raw is off by up to a quarter of the look: biased, not noise)
RESEAT_SD = 12.0      # the headset off and on: re-seats moved the pupil 20-30 px
RESEAT_OWN_SD = 2.0   # a new seating changes each eye's own part a little
RESEAT_GLINT_SD = 1.5  # the glint pair and SteamVR read a new seating a little differently too
RESEAT_STEAM_SD = 1.0
HUBER = 3.0           # Mahalanobis distance past which a steady reading counts for less
MOVE_DEG = 3.5        # a click off this much in both eyes, the same way: the headset moved
MOVE_COS = 0.5        # ("the same way": the cosine between the two eyes' innovations)
LONE_DEG = 5.0        # one eye off this much while the other is within AGREE_DEG: a bad find
GLANCE_SCALE = 2.0    # a glance (a free mouse's click: you most likely looked at what you
                      # clicked) reads with this times CLICK_SD's noise, Huber-weighted, and is
                      # never the headset moved: a click while reading elsewhere pulls little
AGREE_DEG = 2.0

N = 14
H = slice(0, 2)
_I2 = np.eye(2)
_SD = {"click": CLICK_SD, "glint": GLINT_SD, "steam": STEAM_SD}


def _own(e):
    return slice(2 + 2 * e, 4 + 2 * e)


def _bias(e, kind):
    return slice((6 if kind == "glint" else 10) + 2 * e, (8 if kind == "glint" else 12) + 2 * e)


def _rows(e, kind):
    """The 2 x N reading matrix of eye e's `kind`."""
    m = np.zeros((2, N))
    m[:, H] = _I2
    m[:, _own(e)] = _I2
    if kind != "click":
        m[:, _bias(e, kind)] = _I2
    return m


class Headset:
    """Both eyes' slips and their sensors' biases, in degrees (see the top). Times are frame
    times (CLOCK_MONOTONIC_RAW live, the recording's in a replay); one older than the last is
    taken as the last."""

    def __init__(self):
        self.x = np.zeros(N)
        self.P = np.diag([START_SD] * 2 + [OWN_START_SD] * 4 + [GLINT_START_SD] * 4 + [STEAM_START_SD] * 4) ** 2
        self.t = None
        self.adjust_until = -math.inf
        self.reseat_open = False   # reseated and no reading since: both eyes' reseats count once

    def slip(self, e):
        return self.x[H] + self.x[_own(e)]

    def slip_cov(self, e):
        m = _rows(e, "click")
        return m @ self.P @ m.T

    def predict(self, t):
        """Moves the filter on to time t: the slips and biases may have wandered since."""
        if self.t is None or t <= self.t:
            self.t = t if self.t is None else self.t
            return
        dt = t - self.t
        fast = min(max(self.adjust_until - self.t, 0.0), dt)   # the part of dt under the lease
        self.P[H, H] += _I2 * (ADJUST_RATE ** 2 * fast + SLIP_RATE ** 2 * (dt - fast))
        for e in (0, 1):
            self.P[_own(e), _own(e)] += _I2 * (OWN_RATE ** 2 * dt)
            for kind in ("glint", "steam"):
                b = _bias(e, kind)
                self.P[b, b] += _I2 * (BIAS_RATE ** 2 * dt)
        self.t = t

    def adjusting(self, t, seconds):
        """The headset is being moved on purpose for the next `seconds` from t: h may change fast."""
        self.predict(t)
        self.adjust_until = max(self.adjust_until, t + seconds)

    def reseat(self):
        """The headset was off (or ft-eyes restarted): it may sit anywhere now."""
        if self.reseat_open:
            return
        self.P[H, H] += _I2 * RESEAT_SD ** 2
        for e in (0, 1):
            self.P[_own(e), _own(e)] += _I2 * RESEAT_OWN_SD ** 2
            for kind, sd in (("glint", RESEAT_GLINT_SD), ("steam", RESEAT_STEAM_SD)):
                b = _bias(e, kind)
                self.P[b, b] += _I2 * sd ** 2
        self.reseat_open = True

    def innovation(self, e, kind, z, t):
        """How far eye e's reading z (degrees) of `kind` at t is from what the filter expects."""
        self.predict(t)
        return np.asarray(z, float) - _rows(e, kind) @ self.x

    def distance(self, e, kind, y):
        """How unlikely an innovation y (degrees) of eye e's `kind` is: its Mahalanobis distance."""
        m = _rows(e, kind)
        S_ = m @ self.P @ m.T + _I2 * _SD[kind] ** 2
        y = np.asarray(y, float)
        return float(math.sqrt(max(y @ np.linalg.solve(S_, y), 0.0)))

    def inflate(self, y):
        """The headset moved by about y (degrees): h is that much less sure, so the readings
        that say so are taken nearly whole."""
        y = np.asarray(y, float)
        self.P[H, H] += _I2 * float(y @ y)

    def update(self, e, kind, z, t, sd_scale=1.0, robust=True):
        """Takes eye e's reading z (degrees) of `kind` at t. Returns (innovation, Mahalanobis
        distance, Huber weight)."""
        self.predict(t)
        m = _rows(e, kind)
        R = _I2 * (_SD[kind] * sd_scale) ** 2
        y = np.asarray(z, float) - m @ self.x
        S_ = m @ self.P @ m.T + R
        d = float(math.sqrt(max(y @ np.linalg.solve(S_, y), 0.0)))
        w = 1.0
        if robust and d > HUBER:
            w = HUBER / d
            R = R / w
            S_ = m @ self.P @ m.T + R
        K = self.P @ m.T @ np.linalg.inv(S_)
        self.x = self.x + K @ y
        A = np.eye(N) - K @ m
        self.P = A @ self.P @ A.T + K @ R @ K.T   # Joseph form: stays symmetric and positive
        self.P = (self.P + self.P.T) / 2
        self.reseat_open = False
        return y, d, w


class SlipFilter:
    """Eye `eye`'s view of the Headset filter (a fresh one of its own if none), in its feature
    units. `jac`: the calibration's d feature / d gaze for this eye (2x2, feature units per
    degree; eyes_model.Calibration.jacobian). `s`: a starting slip (feature units)."""

    def __init__(self, jac, s=None, headset=None, eye=0):
        self.jac = np.asarray(jac, float)
        if not np.all(np.isfinite(self.jac)) or abs(float(np.linalg.det(self.jac))) < 1e-6:
            self.jac = np.eye(2)   # a degenerate fit: read feature units as degrees
        self.headset = headset if headset is not None else Headset()
        self.eye = eye
        if s is not None:
            self.headset.x[H] = self.deg(s)
        self.clicks = 0          # clicks taught since the calibration (since the last reseat's first)
        self.reseated = False    # the headset may sit anywhere: set by reseat(), cleared by a click

    # --- What it says ---

    @property
    def s(self):
        return self.jac @ self.headset.slip(self.eye)

    @property
    def x(self):
        """The eye's slip, glint bias and steam bias (feature units: slip.jsonl's "x")."""
        hs = self.headset
        return np.concatenate([self.s, self.jac @ hs.x[_bias(self.eye, "glint")],
                               self.jac @ hs.x[_bias(self.eye, "steam")]])

    @property
    def P(self):
        """...and their covariance (feature units)."""
        T = np.vstack([_rows(self.eye, "click"), np.zeros((4, N))])
        T[2:4, _bias(self.eye, "glint")] = _I2
        T[4:6, _bias(self.eye, "steam")] = _I2
        J = np.kron(np.eye(3), self.jac)
        return J @ T @ self.headset.P @ T.T @ J.T

    @property
    def t(self):
        return self.headset.t

    @property
    def adjust_until(self):
        return self.headset.adjust_until

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
        return self.headset.slip(self.eye).copy()

    def slip_sd(self):
        """How sure that is: the largest standard deviation of slip_deg (degrees)."""
        return _largest_sd(self.headset.slip_cov(self.eye))

    def bias_deg(self, which):
        return self.headset.x[_bias(self.eye, which)].copy()

    def own_deg(self):
        """This eye's own part of its slip (degrees): what isn't the headset's."""
        return self.headset.x[_own(self.eye)].copy()

    # --- Time ---

    def predict(self, t):
        self.headset.predict(t)

    def adjusting(self, t, seconds):
        self.headset.adjusting(t, seconds)

    def reseat(self):
        """The headset was off (or ft-eyes restarted): it may sit anywhere now."""
        self.headset.reseat()
        self.reseated = True

    # --- Readings (feature units) ---

    def expect(self, kind):
        return self.jac @ (_rows(self.eye, kind) @ self.headset.x)

    def innovation(self, kind, z, t):
        """How far a reading z of `kind` at t is from what the filter expects (feature units)."""
        return self.jac @ self.headset.innovation(self.eye, kind, self.deg(z), t)

    def distance(self, kind, y):
        """How unlikely an innovation y of `kind` is: its Mahalanobis distance (sds)."""
        return self.headset.distance(self.eye, kind, self.deg(y))

    def inflate(self, y):
        """The headset moved by about y (feature units): see Headset.inflate."""
        self.headset.inflate(self.deg(y))

    def update(self, kind, z, t, sd_scale=1.0, robust=True):
        """Takes a reading z (feature units) of `kind` at time t. sd_scale widens its noise.
        With `robust`, a reading past HUBER counts for less. What happened, for the log:
        {"kind", "innov_deg", "d", "w"}. A reading or time that isn't finite: ValueError."""
        z = np.asarray(z, float)
        if not (np.all(np.isfinite(z)) and math.isfinite(t) and math.isfinite(sd_scale)):
            raise ValueError("a reading that isn't finite")
        y, d, w = self.headset.update(self.eye, kind, self.deg(z), t, sd_scale, robust)
        return {"kind": kind, "innov_deg": [round(float(v), 3) for v in y], "d": round(d, 2), "w": round(w, 3)}

    def click(self, z, t, sd_scale=1.0, moved=False, glance=False):
        """A click's reading. `moved`: the headset moved (teach_click), so the slip is taken
        nearly whole: its uncertainty grows by the innovation first. A `glance` (GLANCE_SCALE)
        is Huber-weighted and isn't counted as a click: after a reseat it leaves the one-dot
        check to come."""
        if moved:
            self.inflate(self.innovation("click", z, t))
        r = self.update("click", z, t, sd_scale, robust=glance)
        if not glance:
            self.clicks = 1 if self.reseated else self.clicks + 1
            self.reseated = False
        return r


def _largest_sd(C):
    return float(math.sqrt(max(np.linalg.eigvalsh((C + C.T) / 2).max(), 0.0)))


def headset_status(headset):
    """The eyes' mean slip (degrees, as slip_deg) and how sure it is: what the fit pill reads.

    Not h alone: how the slip splits between h and the eyes' own parts isn't observable (every
    reading sees only their sum), so h's sd grows with each reseat and never comes back. It sat
    near 5 degrees all day (2026-11-22) and the pill, which needs 2.5, never opened. The mean is
    what the readings pin, and it is still the headset's slip once the eyes' own parts average out.
    """
    m = 0.5 * (_rows(0, "click") + _rows(1, "click"))
    return {"slip_deg": [round(float(v), 3) for v in m @ headset.x],
            "slip_sd": round(_largest_sd(m @ headset.P @ m.T), 3)}


# --- Kept across restarts (state.json's "slip") ---

def save(filters):
    """state.json's "slip" for `filters` {eye: SlipFilter}, all views of one Headset."""
    if not filters:
        return {}
    hs = next(iter(filters.values())).headset
    return {"x": [round(float(v), 4) for v in hs.x], "P": [[float(v) for v in row] for row in hs.P],
            "clicks": {str(e): f.clicks for e, f in filters.items()}}


def load(d, jacs):
    """{eye: SlipFilter} for the eyes in `jacs` {eye: jac}, sharing one Headset, from
    state.json's "slip" `d`: this module's save(); or the per-eye filters' saved before
    2026-11-23 ({eye: {"x", "P", "clicks"}}, feature units); or the old eyes_model.Shift's
    ({eye: {"base", "meas"}}). The old ones' slips are split into the headset's (their mean)
    and each eye's own part, so each eye's slip stays as it was. Anything that doesn't read: fresh."""
    hs = Headset()
    filters = {e: SlipFilter(j, headset=hs, eye=e) for e, j in jacs.items()}
    d = d if isinstance(d, dict) else {}
    try:
        if len(d.get("x") or []) == N:
            x, P = np.asarray(d["x"], float), np.asarray(d["P"], float)
            if P.shape == (N, N) and np.all(np.isfinite(x)) and np.all(np.isfinite(P)):
                hs.x, hs.P = x, (P + P.T) / 2
            for e, f in filters.items():
                f.clicks = int((d.get("clicks") or {}).get(str(e), 0))
            return filters
        old = {int(k): v for k, v in d.items() if isinstance(v, dict) and int(k) in filters}
        slips = {}
        for e, v in old.items():
            f = filters[e]
            if len(v.get("x") or []) == 6:
                x = np.asarray(v["x"], float)
                if np.all(np.isfinite(x)):
                    slips[e] = f.deg(x[:2])
                    hs.x[_bias(e, "glint")], hs.x[_bias(e, "steam")] = f.deg(x[2:4]), f.deg(x[4:6])
                f.clicks = int(v.get("clicks", 0))
            elif "base" in v:
                slips[e] = f.deg(np.asarray(v["base"], float)[:2])
                f.clicks = len(v.get("meas") or [])
        if slips:
            for e in filters:
                slips.setdefault(e, np.zeros(2))   # an eye it hadn't seen was fresh: no slip
            hs.x[H] = np.mean(list(slips.values()), axis=0)
            for e, s in slips.items():
                hs.x[_own(e)] = s - hs.x[H]
    except (TypeError, ValueError, KeyError, AttributeError):
        return load({}, jacs)
    return filters


def moved(a, b):
    """Two eyes' innovations (degrees) say the headset moved: both past MOVE_DEG, the same way."""
    na, nb = float(np.hypot(*a)), float(np.hypot(*b))
    return min(na, nb) > MOVE_DEG and float(np.asarray(a) @ np.asarray(b)) / max(na * nb, 1e-9) > MOVE_COS


def teach_click(filters, lessons, t, glance=False):
    """One click for both eyes: `filters` {eye: SlipFilter}, `lessons` {eye: z} for the eyes it
    teaches (seen, and within their calibration dots). Judged across the eyes first (see the
    top); a `glance` is never "moved" and reads at GLANCE_SCALE. Returns {eye: what happened},
    each with "gate": "moved", "lone", "plain" or "glance"."""
    innov = {e: filters[e].deg(filters[e].innovation("click", z, t)) for e, z in lessons.items()}
    size = {e: float(np.hypot(*v)) for e, v in innov.items()}
    both = not glance and len(innov) == 2 and moved(innov[0], innov[1])
    out, inflated = {}, set()
    for e, z in lessons.items():
        scale, gate = (GLANCE_SCALE, "glance") if glance else (1.0, "moved" if both else "plain")
        o = 1 - e
        if not both and o in size and size[e] > LONE_DEG and size[o] <= AGREE_DEG:
            scale, gate = scale * size[e] / AGREE_DEG, "lone"
        first = id(filters[e].headset) not in inflated   # eyes sharing a Headset: its h once
        inflated.add(id(filters[e].headset))
        r = filters[e].click(z, t, scale, both and first, glance)
        r["gate"] = gate
        out[e] = r
    return out
