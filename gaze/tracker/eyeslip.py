"""eyeslip: ft-eyes' readings for its slip filters (slipfilter.py), and what it says of them: a
click's and a steady SteamVR look's pupils against the calibration, the glint estimate (judged
across both eyes for a knock), the `adjusting` lease, each eye's status, and slip.jsonl's rows
(sliplog.py).

`eyes` are ft-eyes' Eye objects: each has .eye (0 right, 1 left), .filter (its SlipFilter, None
while unmapped), .window(t0, t1) (its median pupil over that span's frames), its newest
readings .find, .gaze and .glint, and for take_glint .cal, .slip (eyes_model.SlipTracker) and
.glint_innov (its last glint reading's innovation). `cal` is the tracker's calibration. Times are frame times.
"""
import math
from collections import namedtuple

import numpy as np

import slipfilter

CLICK_MIN = 5          # frames an eye needs before a click to be taught by it
GLINT_PAIR = 1.5       # s: two glint readings this close are of the same moment (each eye takes
                       # one every GLINT_EVERY, 1 s), in one eye or across the two
KNOCK_D = 2.0          # a glint reading this far off what the filter expects (Mahalanobis: at
                       # GLINT_SD's noise, 1 in 7 readings; about 6 degrees)...
KNOCK_STREAK = 2       # ...this many in a row the same way, in both eyes (3 when only this eye
                       # has a glint fit),
                       # each past MOVE_DEG: a misread pair snaps back. Simulated glints with
                       # GLINT_SD's noise (half of it shared by the eyes) knocked never in an
                       # hour; 8-11 degree knocks were followed in 3-4 s (2026-11-22)
KNOCK_MAX_DEG = 12.0   # bigger is a bad glint pair, not the headset (re-seats moved 5-8 degrees)
KNOCK_CENTRE = 20.0    # degrees: only looking within this of straight ahead, where the glint fit
                       # is best (looking far aside, its error is shared by both eyes)

# An eye's last glint reading: when, how far off the filter (degrees, feature units), and how
# many readings in a row have been far off the same way (0: this one wasn't).
Innov = namedtuple("Innov", "t deg y streak")
LOOK_WINDOW = 0.3      # a steady SteamVR look's frames: the 300 ms before it (steamlook.py's window)
LOOK_MIN = 10          # frames an eye needs in that window


def _r(v, digits=3):
    return [round(float(x), digits) for x in v]


def teach_click(eyes, cal, t, truth, before):
    """A click at t where you looked at `truth` (yaw, pitch): each eye seen over the `before` s
    up to it, and within its calibration dots, teaches its filter (judged across the eyes:
    slipfilter.teach_click). Returns the reply's words (each eye's measured slip, or nan) and
    the clicks log's record per eye."""
    out, rec, lessons = [], {}, {}
    for e in eyes:
        g = e.window(t - before, t)
        if g is None or g["n"] < CLICK_MIN or not e.filter:
            out += ["nan", "nan"]
            continue
        if not cal.inside(e.eye, truth):
            out += ["nan", "nan"]
            rec[e.eye] = {"pupil": g["pupil"].tolist(), "skipped": "outside its calibration dots"}
            continue
        d = cal.click_shift(e.eye, g["pupil"], truth)
        lessons[e.eye] = d
        e.filter.predict(t)
        rec[e.eye] = {"pupil": g["pupil"].tolist(), "measured": d.tolist(), "before": e.filter.s.tolist(),
                      "sd_before": round(e.filter.slip_sd(), 3), "glint": e.glint[1] if e.glint else None,
                      "a": round(e.find[3], 2) if e.find and e.find[3] is not None else None}
        out += [f"{d[0]:.2f}", f"{d[1]:.2f}"]
    taught = slipfilter.teach_click({e.eye: e.filter for e in eyes if e.filter}, lessons, t)
    for c, r in taught.items():
        rec[c].update(after=eyes[c].filter.s.tolist(), **r)
    return out, rec


def take_glint(e, other, t, gaze):
    """Eye e's glint estimate for its filter when one is due, while its gaze is within its dots
    (further out the glint fit guesses, and a misread pair is likelier). Far off what the filter
    expects (KNOCK_D) for KNOCK_STREAK readings in a row, the same way, in both eyes (slipfilter
    .moved), looking near straight ahead: the headset was knocked, and both filters take it
    nearly whole. The log's reading, or None."""
    z = e.slip.update(t)
    if (z is None or not np.all(np.isfinite(z)) or not e.cal.has("glint", e.eye)
            or not e.cal.inside(e.eye, gaze)):
        return None
    y = e.filter.innovation("glint", z, t)
    deg = e.filter.deg(y)
    size = float(np.hypot(*deg))
    streak = 0
    if (e.filter.distance("glint", y) > KNOCK_D and size <= KNOCK_MAX_DEG
            and math.hypot(gaze[0], gaze[1]) <= KNOCK_CENTRE):
        prev = e.glint_innov
        same = prev is not None and prev.streak and t - prev.t <= GLINT_PAIR and slipfilter.moved(prev.deg, deg)
        streak = prev.streak + 1 if same else 1
    e.glint_innov = Innov(t, deg, y, streak)
    knocked, o = False, None
    paired = other is not None and other.filter and other.cal.has("glint", other.eye)
    if streak >= KNOCK_STREAK and paired:
        o = other.glint_innov
        knocked = (o is not None and o.streak >= KNOCK_STREAK and t - o.t <= GLINT_PAIR
                   and slipfilter.moved(deg, o.deg))
    elif not paired:
        knocked = streak >= KNOCK_STREAK + 1   # this eye's glints alone
    if knocked:
        e.filter.inflate(y)
        if o is not None:
            other.filter.inflate(o.y)   # its next reading, within a second, takes it
            other.glint_innov = None
        e.glint_innov = None
    r = e.filter.update("glint", z, t)
    e.glint = (t, _r(z, 2))
    return {"z": e.glint[1], "moved": knocked, "streak": streak, **r}


def teach_look(eyes, cal, t, truth, spread):
    """SteamVR's look at `truth` held over the LOOK_WINDOW up to t (ft-gazed's "look"): each eye
    seen steady over it (within `spread`, as a calibration dot) and within its dots takes it as
    a steam reading. Returns the reply's words and the log's {eye: reading}."""
    out, took = [], {}
    for e in eyes:
        g = e.window(t - LOOK_WINDOW, t)
        if (not e.filter or g is None or g["n"] < LOOK_MIN or g["spread"] > spread
                or not cal.inside(e.eye, truth)):
            out += ["nan", "nan"]
            continue
        d = cal.click_shift(e.eye, g["pupil"], truth)
        took[str(e.eye)] = {"z": _r(d, 2), **e.filter.update("steam", d, t)}
        out += [f"{d[0]:.2f}", f"{d[1]:.2f}"]
    return out, took


def adjusting(eyes, t, seconds):
    """The headset is being moved on purpose for `seconds` from t (the fit pill shows)."""
    for e in eyes:
        if e.filter:
            e.filter.adjusting(t, seconds)


def eye_status(f, t):
    """An eye's part of ft-eyes' status: its filter `f` (or None) moved on to t."""
    if not f:
        return {"shift": [0.0, 0.0], "clicks": 0, "reseat": False, "slip_deg": None, "slip_sd": None}
    if t is not None:
        f.predict(t)
    return {"shift": _r(f.s), "clicks": f.clicks, "reseat": f.reseated, "slip_deg": _r(f.slip_deg()),
            "slip_sd": round(f.slip_sd(), 3), "glint_bias_deg": _r(f.bias_deg("glint")),
            "steam_bias_deg": _r(f.bias_deg("steam"))}


def log_row(eyes, t):
    """slip.jsonl's row at t (sliplog.py), or None before any frame."""
    if t is None:
        return None
    out = {}
    for e in eyes:
        row = {}
        if e.find:
            ft, x, y, a, mid = e.find
            row.update(found_t=round(ft, 4), pupil=[round(x, 2), round(y, 2)], a=None if a is None else round(a, 2),
                       mid=None if mid is None else _r(mid, 2))
        if e.gaze:
            row["gaze"] = [round(e.gaze[1], 3), round(e.gaze[2], 3)]
        if e.glint:
            row["glint"] = e.glint[1]
        if e.filter:
            f = e.filter
            f.predict(t)
            row.update(x=_r(f.x), P=_r(np.diag(f.P)), slip_deg=_r(f.slip_deg()), slip_sd=round(f.slip_sd(), 3))
        out[str(e.eye)] = row
    return {"t": round(t, 4), "eyes": out}
