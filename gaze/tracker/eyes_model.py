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

A glint pair misread (more often looking far out) also makes the glint estimate leap, though
the headset didn't move. With no headset movement a jump was in force on 42-75% of the
recorder rounds' dot frames and cost up to 6 degrees median (7.07 against 1.91 on the edge dots
past 35); a jump counted only while both eyes have one kept the re-seat gains and took most of
that away (frame-eyes findings.md, "The glint jump replayed"). So JUMP_RULES "both", ft-eyes'
default, applied an eye's jump only while the other eye had one too (or wasn't mapped). Live
ft-eyes left Shift and JUMP_RULES for its slip filter on 2026-11-22 (slipfilter.py, which judges
glint moves across the eyes the same way); they stay for bench/compare.py's and lab/'s replays.

A click far outside the calibration's dots measures its shift through the `where` fit's
extrapolation: a nudge at 42 degrees yaw (the calibration covered +-20) taught a shift that left
the next nudge in the middle 35 degrees off. `Calibration.inside` says whether a gaze is within
an eye's dots (by EDGE_MARGIN), and ft-eyes teaches no shift from a click outside them.
"""
import json
import os
import re
import sys
import time
from collections import deque
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "conf"))
import ft_store  # noqa: E402  (JSON files written safely; standard library only)

SLIP_WINDOW = 30.0
SLIP_MIN = 10        # pair sightings needed before trusting a slip estimate
MIN_CLICKS = 12
SPREAD_MIN = 0.3     # degrees: floor for an eye's fit spread, so one eye can't take all the weight
SHIFT_KEEP = 5       # clicks in the shift estimate
SHIFT_JUMP = 3.0     # a glint slip change this big (px) since the last click is a nudge
JUMP_HOLD = 1.0      # s: ...if it holds this long (a bad glint pair gives a jump that snaps back)
JUMP_MAX = 40.0      # px: bigger is a bad glint pair, not the headset (a re-seat moved 20-30)
JUMP_WINDOW = 10.0   # seconds of glint sightings for noticing a jump
JUMP_RULES = ("both", "each")   # a glint jump counts: only while the other eye has one too | always
EDGE_MARGIN = 2.0    # degrees past an eye's outermost calibration dots that a click still teaches
                     # (gaze/bench/compare.py's edge-guard uses the same)


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

    def __init__(self, fits=None, info=None, spread=None, box=None):
        self.fits = fits or {}   # (name, eye) -> Quad
        self.info = info or {}
        self.spread = spread or {}  # eye -> degrees
        self.box = box or {}        # eye -> [yaw min, max, pitch min, max] of its dots (degrees)

    @classmethod
    def fit(cls, clicks, info=None):
        truth = lambda cs: np.array([k["truth"] for k in cs])  # noqa: E731
        fits, spread, box = {}, {}, {}
        for c in (0, 1):
            cs = [k for k in clicks if eye_of(k, c) is not None]
            if len(cs) < MIN_CLICKS:
                continue
            pupils = [eye_of(k, c)["pupil"] for k in cs]
            fits["pupil", c] = Quad(pupils, truth(cs))
            lo, hi = truth(cs).min(0), truth(cs).max(0)
            box[c] = [float(lo[0]), float(hi[0]), float(lo[1]), float(hi[1])]
            miss = fits["pupil", c](pupils) - truth(cs)
            spread[c] = float(np.sqrt(np.mean(np.sum(miss ** 2, axis=1))))
            fits["where", c] = Quad(truth(cs), pupils)
            gs = [k for k in cs if eye_of(k, c)["mid"] is not None]
            if len(gs) >= MIN_CLICKS:
                fits["glint", c] = Quad([eye_of(k, c)["pupil"] - eye_of(k, c)["mid"] for k in gs], truth(gs))
        return cls(fits, info, spread, box)

    def has(self, name, eye):
        return (name, eye) in self.fits

    def eyes(self):
        """The eyes it maps (0 right, 1 left)."""
        return [c for c in (0, 1) if self.has("pupil", c)]

    def glint_eyes(self):
        """The eyes with a glint fit: only these can have a glint jump. An eye seen with its pair
        at under MIN_CLICKS dots has none (2026-11-22's calibration: the left eye at 11 of 26)."""
        return [c for c in (0, 1) if self.has("glint", c)]

    def jacobian(self, eye, gaze=(0.0, 0.0), step=1.0):
        """d feature / d gaze of that eye's `where` fit at gaze (yaw, pitch): 2x2, feature units
        per degree, columns yaw and pitch."""
        w = self.fits["where", eye]
        y, p = gaze
        return np.column_stack([(w.one(y + step, p) - w.one(y - step, p)) / (2 * step),
                                (w.one(y, p + step) - w.one(y, p - step)) / (2 * step)])

    def slip(self, eye, rows):
        """Median slip in pixels from pair sightings `rows` (t, px, py, mx, my), or None."""
        if not self.has("glint", eye) or len(rows) < SLIP_MIN:
            return None
        rows = np.asarray(rows, float)
        gaze = self.fits["glint", eye](rows[:, 1:3] - rows[:, 3:5])
        return np.median(rows[:, 1:3] - self.fits["where", eye](gaze), axis=0)

    def inside(self, eye, gaze, margin=EDGE_MARGIN):
        """Whether gaze (yaw, pitch) lies within that eye's calibration dots, `margin` degrees to
        spare: True when its dots' extent isn't known (a calibration saved without it)."""
        b = self.box.get(eye)
        if b is None:
            return True
        return b[0] - margin <= gaze[0] <= b[1] + margin and b[2] - margin <= gaze[1] <= b[3] + margin

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
             "box": {str(e): b for e, b in self.box.items()},
             "fits": [{"name": n, "eye": e, **q.to_json()} for (n, e), q in self.fits.items()]}
        ft_store.write_json(path, d, indent=1, newline=False)   # temp file of its own, fsync, rename

    @classmethod
    def load(cls, path):
        d = json.loads(path.read_text())
        return cls({(f["name"], f["eye"]): Quad.from_json(f) for f in d["fits"]}, d.get("info"),
                   {int(e): s for e, s in d.get("spread", {}).items()},
                   {int(e): b for e, b in d.get("box", {}).items()})

    def fill_from_dots(self, path):
        """Fill in the eyes' fit spreads and dots' extent of a calibration saved without them,
        from its dots in calibration-dots.jsonl at `path` (each refitted). Left as it is if its
        dots aren't there: the eyes are then weighted alike, and every click teaches."""
        if self.spread and self.box:
            return
        try:
            lines = Path(path).read_text().splitlines()
        except OSError:
            return
        dots = []
        for line in lines:
            try:   # a line cut short (the file is appended to) is skipped, not fatal at start
                d = json.loads(line)
                if d.get("made") == self.info.get("made"):
                    dots.append(dict(truth=d["truth"], eye={
                        int(c): dict(pupil=np.array(v["pupil"]), mid=None if v["mid"] is None else np.array(v["mid"]))
                        for c, v in d["eye"].items()}))
            except (ValueError, KeyError, TypeError, AttributeError):
                continue
        if dots:
            refit = Calibration.fit(dots)
            self.spread = self.spread or refit.spread
            self.box = self.box or refit.box


BACKUP = re.compile(r"calibration-\d{8}-\d{6}\.json\Z")   # not calibration-dots.jsonl
KEEP_BACKUPS = 10


def prune_backups(folder, keep=KEEP_BACKUPS):
    """Remove all but the newest `keep` earlier calibrations (calibration-YYYYmmdd-HHMMSS.json,
    which ft-eyes keeps on each new one) from folder. Nothing else there is touched. The names
    removed."""
    names = sorted(n for n in os.listdir(folder) if BACKUP.match(n))
    gone = []
    for n in names[:max(0, len(names) - keep)]:
        try:
            os.unlink(os.path.join(folder, n))
            gone.append(n)
        except OSError:
            pass
    return gone


class Shift:
    """Where one eye sits in the image now, relative to the calibration (pixels).

    `base` is the median shift the last SHIFT_KEEP clicks measured. `ref` is the glint slip
    estimate at the last click; if the glint estimate has since moved more than SHIFT_JUMP
    (and less than JUMP_MAX) and stayed there for JUMP_HOLD seconds, the headset moved, and
    the change is added until the next click. That click then starts the history over,
    since the older ones describe the old position. Live, bad glint pairs made the estimate
    leap by up to 68 px for under a second (practice2), hence the hold. `reseat` does the
    same for the next click without the glints: the frames stopped (the headset was off),
    so the headset may be anywhere now.

    Whether a jump counts can depend on the other eye (JUMP_RULES): `current` and `in_force`
    take the other eye's Shift (None when that eye isn't mapped) and the rule. When a click
    teaches the other eye but not this one while this one's jump is in force (`teach`), this
    one is `vouched`: the other's lesson cleared its jump, but the headset did move, so this
    jump still counts until this eye's own lesson. A new jump (its glints moved again) needs
    the other eye's again."""

    def __init__(self, base=(0.0, 0.0), ref=None):
        self.meas = []
        self.base = np.asarray(base, float)
        self.ref = None if ref is None else np.asarray(ref, float)
        self.jump = np.zeros(2)
        self.held = None  # (change, since when) while a jump waits out JUMP_HOLD
        self.reseated = False
        self.vouched = False  # its jump counts whatever the other eye's (see teach)

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
            self.jump, self.held, self.vouched = np.zeros(2), None, False
            return
        if self.held is None or np.hypot(*(d - self.held[0])) > SHIFT_JUMP:
            self.held, self.vouched = (d, t), False   # moved again: the vouch was for the last move
        elif t - self.held[1] >= JUMP_HOLD:
            self.jump = d

    def reseat(self):
        self.reseated = True

    def click(self, d, g=None, jumped=None):
        """A click measured the shift d; g is the glint estimate then. `jumped`: whether a jump
        was in force (the headset moved, so the older clicks describe the old position); None
        is whether this eye has one."""
        if (np.any(self.jump) if jumped is None else jumped) or self.reseated:
            self.meas = []
            self.reseated = False
        self.meas = (self.meas + [np.asarray(d, float)])[-SHIFT_KEEP:]
        self.base = np.median(self.meas, axis=0)
        self.jump, self.held, self.vouched = np.zeros(2), None, False
        if g is not None:
            self.ref = np.asarray(g, float)

    def in_force(self, other=None, rule="each"):
        """Whether this eye's glint jump counts now: it has one and, with rule "both", so does
        `other` (the other eye's Shift), unless there's no other (that eye isn't mapped) or a
        click vouched for it."""
        if not np.any(self.jump):
            return False
        return rule != "both" or other is None or self.vouched or bool(np.any(other.jump))

    def current(self, other=None, rule="each"):
        """The shift to use: the clicks' base, plus the jump while it counts (in_force)."""
        return self.base + self.jump if self.in_force(other, rule) else self.base

    def to_json(self):
        return {"base": self.base.tolist(), "ref": None if self.ref is None else self.ref.tolist(),
                "meas": [m.tolist() for m in self.meas],
                "vouched": self.jump.tolist() if self.vouched else None}   # the move it vouches for

    @classmethod
    def from_json(cls, d):
        s = cls(d.get("base", (0, 0)), d.get("ref"))
        s.meas = [np.asarray(m, float) for m in d.get("meas", [])]
        if isinstance(d.get("vouched"), list):
            s.hold_vouched(d["vouched"])
        return s

    def hold_vouched(self, move):
        """Take up a vouched jump again (after a restart, or a replay's start): `move` as the change
        held long since, so glint estimates still at it keep it, in force at once, and any other
        move ends the vouch as it would have live."""
        self.held, self.vouched = (np.asarray(move, float), float("-inf")), True


def partner(shifts, eye, mapped):
    """The other eye's Shift from {eye: Shift}, or None when it can't have a jump (`mapped`: the
    eyes with a glint fit, Calibration.glint_eyes), so its jump can't be asked. The eyes with a
    pupil fit were passed until 2026-11-22: an eye mapped without a glint fit never jumps, so
    with rule "both" the other eye's jumps never counted."""
    return shifts[1 - eye] if (1 - eye) in mapped else None


def teach(shifts, lessons, rule, mapped):
    """One click's lessons for both eyes: `shifts` {0: Shift, 1: Shift}, `lessons` {eye:
    (measured shift, glint estimate or None)} for the eyes it teaches (seen, and within their
    dots), `mapped` the eyes with a glint fit (Calibration.glint_eyes). Whether each eye's jump was in force is read
    before any lesson, since a lesson clears that eye's jump, which the other's rule asks about.
    An eye whose jump was in force, left untaught while the other was taught, is vouched."""
    jumped = {e: sh.in_force(partner(shifts, e, mapped), rule) for e, sh in shifts.items()}
    for e, (d, g) in lessons.items():
        shifts[e].click(d, g, jumped[e])
    for e, sh in shifts.items():
        if jumped[e] and e not in lessons and (1 - e) in lessons:
            sh.vouched = True


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
        self.update(now)
        return self.value

    def update(self, now=None):
        """A new estimate if one is due (every `every` s) and there are enough sightings, else None."""
        now = time.monotonic() if now is None else now
        if now - self.at < self.every:
            return None
        self.at = now
        while self.rows and self.rows[0][0] < now - self.window:
            self.rows.popleft()   # only sightings within the window: no pair seen lately, no estimate
        s = self.cal.slip(self.eye, list(self.rows))
        if s is not None:
            self.value = s
        return s
