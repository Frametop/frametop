#!/usr/bin/env python3
"""The eye recorder's session runner (DESIGN.md: "Bounty board", "Files", "Panel", "Fit coach").

A session is one round for one bounty. It starts its own copies of what it needs, next to
Frametop's (whose gaze service it pauses for the round): the gaze panel in rec mode, our eye
tracker ft-eyes (the fit coach's finds, and own.jsonl), ft-gaze (SteamVR's gaze, steamvr.jsonl).
Then, in step mode:

  1. Fitment, not a take: the fit coach (gaze/fitcoach.py) on a centre dot until the headset
     sits well and the person presses Next; Next before that warns, a second press goes on and
     the round records it was skipped. Its readings go into fit.json. The coach is off for now
     (FIT_COACH): then this only asks for a comfortable fit, Next goes on, and its readings are
     kept without being shown. Either way it shows the IPD setting, read from SteamVR when the
     round starts: it can't be changed with the headset on, so there's no IPD step, and a wrong
     setting means stopping and setting it first.
  2. Nine dots, a take: a 3x3 grid to 20 degrees on the mid background, the round's reference.
  3. The bounty's steps, from its round script (rounds/<script>.json, below).

Each step that's "ready" shows first and waits for Next; then a 3-2-1 countdown, recorded, then
the step. Steps of the same group share a take (takes/NN-group): the eye frames (eyerec.py),
targets.jsonl, poses.jsonl and ipd.jsonl (the panel's log), own.jsonl (ft-eyes' finds),
steamvr.jsonl, prompts.jsonl and take.json, as DESIGN.md's "Files" lists. Only countdowns and
steps are recorded; the frame writer pauses while a step waits. During the round the fit coach
looks again at the end of each centre dot, and a headset that moved since the fitment gets a
note (fit.json "slips").

Controls, as the hand recorder's: Next, pause, redo (the step running, or at a ready screen the
one before; the range is marked with a "redo" event), skip (the rest of the take), stop. The
headset button: press = next or pause, two presses = redo, hold = stop.

Round scripts, rounds/<id>.json: {"schema": 1, "id", "title", "intro", "shift_check": true,
"steps": [step]}. A step: {"id", "kind", "text" ("|" starts a line), "group" (default its id),
"ready" (default true), "bg" (0..1, default the mid background), "distance" (metres)} and by kind:
  dots     "dots": [[yaw, pitch]...] or "grid": [columns, rows, yaw extent, pitch extent];
           "hold" s (the ring fills), "settle" s (before it), "shuffle"
  path     "path": "circle|line|eight <numbers>" (the panel's path command), "seconds"
  jumps    "count", "range": [yaw, pitch] (a box), "each": [shortest, longest] s
  prompt   "dot": [yaw, pitch] or null (none), and "seconds" or "cues": [[word, s]...] (shown
           big in turn; [word, s, sound] also plays a beep as it starts, for steps done with the
           eyes shut: sound blink, close or open, sounds.py; rounds/README.md has the rules,
           and load_round enforces them), "cue_distance": {word: metres}
  light    "dot", "levels": [[background, s]...]
  world    "point": [x, y, z] (metres from the head when it starts, -z ahead), "seconds"
  reseat   not recorded: the text, Next, then a fit reading
Degrees are head-relative, yaw +left, pitch +up; every dot must fit the panel (load_round checks).

Plain Python, standard library only (zstd as eyerec.py finds it). From the command line, for
testing (in the dev container, which has Python 3.14's zstd):

  python3 gaze/rec/session.py --board                         the bounties, and which are done
  python3 gaze/rec/session.py edges --plan                    a round's steps and their length
  python3 gaze/rec/session.py edges --dry-run --speed 20 --next-after 0.1
  python3 gaze/rec/session.py edges --cams /dev/shm/frametop-eyes-cams-replay --panel-no-vr \\
      --no-steamvr-gaze --no-headset-button --next-after 1      a replay (lab/ft-eyes-replay)

All _ns times are CLOCK_MONOTONIC nanoseconds (prompts.jsonl, take.json, as hands); the eye
frames and the panel's logs are on CLOCK_MONOTONIC_RAW, and take.json's clock samples join them.
"""
import argparse
import hashlib
import json
import math
import os
import random
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.dirname(HERE)
REPO = os.path.dirname(GAZE)
sys.path.insert(0, REPO)
sys.path.insert(0, GAZE)
sys.path.insert(0, HERE)
import eyerec  # noqa: E402  (gaze/rec/eyerec.py: the frame writer)
import fitcoach  # noqa: E402  (gaze/fitcoach.py)
import sounds  # noqa: E402  (gaze/rec/sounds.py: the beeps)
from recorder import bounties  # noqa: E402
from recorder.button import BUTTON_DEVICE, ButtonReader, find_button, mouse_connected, read_input_devices  # noqa: E402
from recorder.common import (clean_text, clock_sample, in_container, mono_ns, os_version,  # noqa: E402
                             steamvr_version, write_json)
from recorder.units import pause_units, resume_units  # noqa: E402

ROUNDS_DIR = os.path.join(HERE, "rounds")
BOARD_PATH = os.path.join(HERE, "bounties.json")
BASE_DIR = os.path.expanduser("~/.local/share/frametop/eyes/contrib")
PANEL_BIN = os.path.join(GAZE, "build", "ft-gazepanel")
FT_GAZE = os.path.join(GAZE, "build", "ft-gaze")
FT_EYES = os.path.join(GAZE, "tracker", "ft-eyes")
EYES_PYTHON = os.environ.get("FT_EYES_PYTHON") or os.path.join(GAZE, "tracker", "build", "venv", "bin", "python")
IN_BOX = os.path.join(REPO, "scripts", "in-box")
# The recorder's own copies, next to the gaze service's (which it pauses anyway)
PANEL_SOCKET, PANEL_OVERLAY = "ft_eyerecpanel", "frametop.eyerecpanel"
EYES_SOCKET, EYES_GAZE = "ft_eyes_rec", "/dev/shm/frametop-eyes-gaze-rec"
# Frametop's gaze pointer and its tracker: off for the round, so the mouse doesn't follow the dots
# and two copies of ft-eyes don't share the CPU
QUIET_UNITS = ("frametop-gaze.service",)
PAUSED_UNITS_FILE = "eyerec-paused-units"   # in run_dir(): the ones to start again, if a session didn't

PANEL_DEG, PANEL_ASPECT = 64.0, 0.75   # ft-gazepanel's rec panel (kFullDeg, 4:3)
MID_BG = 0.33                          # the nine dots' background, and a step's default
PANEL_DISTANCE = 1.5                   # ft-gazepanel's default
NINE_DOTS_DEG = 20.0
COUNTDOWN_S = 3
TICK_S = 0.05
DONE_S = 0.2              # a dot shows "done" this long before the next
CAPTURE_STEP_S = 0.1      # how often a dot's ring is updated
FIT_ASK_S = 0.5           # the fit coach's reading, this often during the fitment
FIT_GOOD_S = 2.0          # OK this long: the fit is good
READING_WAIT_S = 6.0      # a reseat's fit reading waits this long for both eyes
CENTRE_DEG = 1.0          # a dot this near the middle gets the shift check after its hold
VERSION = "ft-eyerec 0.1"
READY_TEXT = "Ready? Press Space or click Next"
READY_BUTTON = "Ready? Press the button on the right side of the headset"
READY_BUTTON_MOUSE = "Ready? Press Space, click Next, or press the headset button"
RESUME_HINT = "Paused. Resume: P in the Eye recorder window"
RESUME_HINT_BUTTON = "Paused. Resume: press the headset button, or P in the Eye recorder window"
KEYS = "Eye recorder window:  Space next  ·  P pause  ·  R redo  ·  S skip  ·  Esc stop"
KEYS_BUTTON = ("Headset button: press for next or pause  ·  press twice to redo  ·  hold to stop  "
               "·  Window: Space P R S Esc")
FIT_TITLE = "Headset fit"
FIT_TEXT = "Look at the dot. Adjust the headset until this says good."
FIT_ADJUST = "Adjust the headset until it says good"
FIT_SKIP_NOTE = "The fit isn't good yet: recordings may be less useful. Press again to start anyway."
# The fit coach is off until it's trained on more people's fits (2026-10-16: its hints jumped
# around and "good" was out of reach). Off, the fitment only asks for a comfortable fit and a look
# at the dot, Next goes straight on, and the coach's readings still go into fit.json, silently:
# they're what it will be trained on. No baseline then, so no slip notes either.
# FT_EYEREC_FIT_COACH=1 (or session.py --fit-coach) turns it back on.
FIT_COACH = os.environ.get("FT_EYEREC_FIT_COACH", "") == "1"
FIT_OFF_TITLE = "Put the headset on"
FIT_OFF_TEXT = "Put the headset on so it's comfortable and the picture is sharp. Then look at the dot."
# The IPD setting is the headset's (SteamOS, read through SteamVR), shown at the fitment. It can't be
# changed with the headset on, so a wrong one means stopping the round.
IPD_TEXT = "Your IPD setting: {mm:.1f} mm. If that's not right for you, stop, take the headset off and set it."
IPD_UNKNOWN = "Couldn't read the IPD setting from SteamVR."
NINE_DOTS_TEXT = "Look at each dot until its ring fills. Keep your head still."
DONE_SCREEN = {"title": "All done", "text": "Thank you! This recording session will close. Continue in the application.|Review the round in the Eye "
                                            "recorder window.", "seconds": 5}
STOPPED_SCREEN = {"title": "Stopped", "text": "What was recorded so far is saved. Review it in the Eye recorder window.",
                  "seconds": 4}
ERROR_SCREEN = {"title": "Something went wrong", "text": "See the Eye recorder window.", "seconds": 4}

STEP_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
KINDS = ("dots", "path", "jumps", "prompt", "light", "world", "reseat")
PATH_ARGS = {"circle": 4, "line": 5, "eight": 5}


def run_dir():
    return "/run/user/%d/frametop-eyes" % os.getuid()


def fix_hint(build):
    return "run %s in the dev container" % build


def boxed(argv):
    """A program built in the dev container: run it there (scripts/in-box from the host)."""
    return list(argv) if in_container() or not os.path.exists(IN_BOX) else [IN_BOX] + list(argv)


# ------------------------------------------------------------------------------------------
# Round scripts

def off_panel(yaw, pitch, margin=1.0):
    """The dot wouldn't show whole on the rec panel (ft-gazepanel's ToPixel, gazecheck.off_panel),
    with margin degrees to spare for its ring."""
    half = math.tan(math.radians(PANEL_DEG / 2))
    m = math.tan(math.radians(margin)) / (2 * half)
    if abs(yaw) >= 80:
        return True
    x = 0.5 - math.tan(math.radians(yaw)) / (2 * half)
    y = 0.5 - math.tan(math.radians(pitch)) / math.cos(math.radians(yaw)) / (2 * half) / PANEL_ASPECT
    return not (m <= x <= 1 - m and m <= y <= 1 - m)


def grid(columns, rows, yaw, pitch):
    """A grid's dots, row by row from the top left, to yaw and pitch degrees out."""
    out = []
    for r in range(rows):
        for c in range(columns):
            out.append([round(yaw * (1 - 2 * c / (columns - 1)), 3) if columns > 1 else 0.0,
                        round(pitch * (1 - 2 * r / (rows - 1)), 3) if rows > 1 else 0.0])
    return out


def path_points(spec):
    """Points along a path command's shape, for the panel check."""
    w = spec.split()
    v = [float(x) for x in w[1:]]
    if w[0] == "circle":
        return [(v[0] + v[2] * math.cos(a), v[1] + v[2] * math.sin(a)) for a in (i * math.pi / 18 for i in range(36))]
    if w[0] == "line":
        return [(v[0], v[1]), (v[2], v[3])]
    return [(v[0] + v[2] * math.sin(a), v[1] + v[3] * math.sin(2 * a)) for a in (i * math.pi / 36 for i in range(72))]


def _num(v, lo, hi, what):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not lo <= v <= hi:
        raise ValueError("%s: want a number from %g to %g, not %r" % (what, lo, hi, v))
    return float(v)


def _dot(v, what):
    if not isinstance(v, list) or len(v) != 2:
        raise ValueError("%s: want [yaw, pitch]" % what)
    d = [_num(v[0], -80, 80, what), _num(v[1], -80, 80, what)]
    if off_panel(*d):
        raise ValueError("%s: %g, %g is off the panel" % (what, d[0], d[1]))
    return d


def cue_needs(word):
    """The sound a cue must have because the person can't see the panel then (rounds/README.md):
    "blink" for a blink, "close" for shutting both eyes, else None. Half-closed eyes and one eye
    (a wink: the other one reads the panel) don't need one."""
    w = word.lower()
    if "half" in w or "left" in w or "right" in w:
        return None
    if "blink" in w:
        return "blink"
    if "close" in w or "shut" in w:
        return "close"
    return None


def _check_sounds(cues, sid):
    """Eyes-shut cues beep, and the cue after a "close" plays "open": with the eyes shut, the
    words can't tell anyone when to open them."""
    for k, (word, _secs, *sound) in enumerate(cues):
        need = cue_needs(word)
        if need and sound != [need]:
            raise ValueError('%s: cue "%s" needs the sound "%s" (the eyes are shut then; rounds/README.md)'
                             % (sid, word, need))
        if sound == ["close"] and (k + 1 == len(cues) or cues[k + 1][2:] != ["open"]):
            raise ValueError('%s: after "%s" comes a cue with the sound "open" (rounds/README.md)' % (sid, word))


def _step(raw, i):
    """One step, checked and filled in."""
    if not isinstance(raw, dict):
        raise ValueError("step %d isn't an object" % (i + 1))
    sid, kind = raw.get("id"), raw.get("kind")
    if not isinstance(sid, str) or not STEP_ID.match(sid):
        raise ValueError("step %d: bad id %r" % (i + 1, sid))
    if kind not in KINDS:
        raise ValueError("%s: unknown kind %r" % (sid, kind))
    group = raw.get("group", sid)
    if not isinstance(group, str) or not STEP_ID.match(group):
        raise ValueError("%s: bad group %r" % (sid, group))
    st = {"id": sid, "kind": kind, "group": group, "text": str(raw.get("text", "")), "ready": raw.get("ready", True) is not False,
          "bg": _num(raw["bg"], 0, 1, sid + " bg") if raw.get("bg") is not None else MID_BG,
          "distance": _num(raw["distance"], 0.25, 10, sid + " distance") if raw.get("distance") is not None else PANEL_DISTANCE}
    if kind == "dots":
        if "grid" in raw:
            g = raw["grid"]
            if not isinstance(g, list) or len(g) != 4:
                raise ValueError("%s: grid wants [columns, rows, yaw, pitch]" % sid)
            dots = grid(int(_num(g[0], 1, 9, sid)), int(_num(g[1], 1, 9, sid)), _num(g[2], 0, 60, sid), _num(g[3], 0, 60, sid))
        else:
            dots = raw.get("dots")
            if not isinstance(dots, list) or not dots:
                raise ValueError("%s: no dots" % sid)
        st["dots"] = [_dot(d, "%s dot %d" % (sid, k + 1)) for k, d in enumerate(dots)]
        st["hold"] = _num(raw.get("hold", 1.2), 0.2, 10, sid + " hold")
        st["settle"] = _num(raw.get("settle", 0.6), 0, 5, sid + " settle")
        st["shuffle"] = bool(raw.get("shuffle", False))
    elif kind == "path":
        spec = raw.get("path")
        w = spec.split() if isinstance(spec, str) else []
        if not w or w[0] not in PATH_ARGS or len(w) != PATH_ARGS[w[0]] + 1:
            raise ValueError("%s: bad path %r" % (sid, spec))
        try:
            [float(x) for x in w[1:]]
        except ValueError:
            raise ValueError("%s: bad path %r" % (sid, spec))
        if float(w[-1]) <= 0:
            raise ValueError("%s: a path's seconds must be positive" % sid)
        for k, (y, p) in enumerate(path_points(spec)):
            if off_panel(y, p):
                raise ValueError("%s: the path goes off the panel at %.1f, %.1f" % (sid, y, p))
        st["path"] = " ".join(w)
        st["seconds"] = _num(raw.get("seconds"), 0.5, 120, sid + " seconds")
    elif kind == "jumps":
        st["count"] = int(_num(raw.get("count"), 1, 500, sid + " count"))
        r = raw.get("range")
        if not isinstance(r, list) or len(r) != 2:
            raise ValueError("%s: range wants [yaw, pitch]" % sid)
        st["range"] = [_num(r[0], 0, 60, sid), _num(r[1], 0, 60, sid)]
        _dot(st["range"], sid + " range")
        e = raw.get("each")
        if not isinstance(e, list) or len(e) != 2 or e[0] > e[1]:
            raise ValueError("%s: each wants [shortest, longest]" % sid)
        st["each"] = [_num(e[0], 0.1, 10, sid), _num(e[1], 0.1, 10, sid)]
    elif kind == "prompt":
        st["dot"] = None if "dot" in raw and raw["dot"] is None else _dot(raw.get("dot", [0, 0]), sid + " dot")
        cues = raw.get("cues")
        if cues is not None:
            want = "%s: cues want [[word, seconds]...] or [word, seconds, sound] (sound: %s)" % (
                sid, ", ".join(sounds.SOUNDS))
            if not isinstance(cues, list) or not cues:
                raise ValueError(want)
            st["cues"] = [[clean_text(c[0])[:40], _num(c[1], 0.2, 60, sid + " cue")] + c[2:] for c in cues
                          if isinstance(c, list) and len(c) in (2, 3) and isinstance(c[0], str)
                          and (len(c) == 2 or c[2] in sounds.SOUNDS)]
            if len(st["cues"]) != len(cues):
                raise ValueError(want)
            _check_sounds(st["cues"], sid)
            cd = raw.get("cue_distance") or {}
            st["cue_distance"] = {str(k): _num(v, 0.25, 10, sid + " cue distance") for k, v in cd.items()}
        else:
            st["seconds"] = _num(raw.get("seconds"), 0.5, 120, sid + " seconds")
    elif kind == "light":
        st["dot"] = _dot(raw.get("dot", [0, 0]), sid + " dot")
        lv = raw.get("levels")
        if not isinstance(lv, list) or not lv or not all(isinstance(x, list) and len(x) == 2 for x in lv):
            raise ValueError("%s: levels want [[background, seconds]...]" % sid)
        st["levels"] = [[_num(b, 0, 1, sid), _num(s, 0.2, 60, sid)] for b, s in lv]
    elif kind == "world":
        pt = raw.get("point")
        if not isinstance(pt, list) or len(pt) != 3:
            raise ValueError("%s: point wants [x, y, z]" % sid)
        st["point"] = [_num(v, -10, 10, sid + " point") for v in pt]
        if st["point"][2] >= -0.25:
            raise ValueError("%s: the point must be ahead (z under -0.25)" % sid)
        st["seconds"] = _num(raw.get("seconds"), 0.5, 120, sid + " seconds")
    return st


def load_round(path):
    """A round script, checked: raises ValueError (or OSError) with what's wrong."""
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, dict) or raw.get("schema") != 1 or not isinstance(raw.get("steps"), list) or not raw["steps"]:
        raise ValueError("%s: not a round script (schema 1, steps)" % path)
    steps = [_step(s, i) for i, s in enumerate(raw["steps"])]
    ids = [s["id"] for s in steps]
    if len(set(ids)) != len(ids):
        raise ValueError("%s: step ids repeat" % path)
    if steps[0]["kind"] != "reseat" and not steps[0]["ready"]:
        steps[0]["ready"] = True
    groups, last = set(), None
    for s in steps:   # a group's steps follow each other: one take
        if s["kind"] == "reseat":
            last = None
            continue
        if s["group"] != last and s["group"] in groups:
            raise ValueError("%s: group %s comes back after another" % (path, s["group"]))
        groups.add(s["group"])
        last = s["group"]
    if "dots" in groups:
        raise ValueError("%s: the group dots is the opener's" % path)
    rid = raw.get("id")
    return {"id": rid if isinstance(rid, str) else os.path.basename(path)[:-5], "title": str(raw.get("title", rid)),
            "intro": str(raw.get("intro", "")), "shift_check": raw.get("shift_check", True) is not False and
            not any(s["kind"] == "reseat" for s in steps), "steps": steps}


def round_path(script, rounds_dir=None):
    return os.path.join(rounds_dir or ROUNDS_DIR, script + ".json")


def have_round(script, rounds_dir=None):
    return bool(STEP_ID.match(script)) and os.path.exists(round_path(script, rounds_dir))


def load_board(base_dir=None):
    """The board: the shipped one, or the copy fetched from the dataset when it's newer."""
    cache = os.path.join(base_dir or BASE_DIR, "board-cache.json")
    return bounties.newest(bounties.load(BOARD_PATH), bounties.load(cache))


def session_seed(session_id):
    return int(hashlib.sha256(session_id.encode()).hexdigest()[:8], 16)


def nine_dots(rng):
    """The opener's nine dots: the middle, the other eight shuffled, the middle again."""
    ring = [d for d in grid(3, 3, NINE_DOTS_DEG, NINE_DOTS_DEG) if d != [0.0, 0.0]]
    rng.shuffle(ring)
    return {"id": "dots", "kind": "dots", "group": "dots", "text": NINE_DOTS_TEXT, "ready": True, "bg": MID_BG,
            "distance": PANEL_DISTANCE, "dots": [[0.0, 0.0]] + ring + [[0.0, 0.0]], "hold": 1.5, "settle": 0.6,
            "shuffle": False}


def build_plan(rnd, seed):
    """The round's steps in order, the opener's first, shuffles and jumps made from seed."""
    rng = random.Random(seed)
    steps = [nine_dots(rng)]
    for s in rnd["steps"]:
        s = json.loads(json.dumps(s))
        if s["kind"] == "dots" and s["shuffle"]:
            rng.shuffle(s["dots"])
        if s["kind"] == "jumps":
            ry, rp = s["range"]
            s["dots"], s["times"], last = [], [], None
            while len(s["dots"]) < s["count"]:
                d = [round(rng.uniform(-ry, ry), 2), round(rng.uniform(-rp, rp), 2)]
                if last and math.hypot(d[0] - last[0], d[1] - last[1]) < 5:   # a jump, not a twitch
                    continue
                s["dots"].append(d)
                s["times"].append(round(rng.uniform(*s["each"]), 3))
                last = d
        steps.append(s)
    return steps


def step_seconds(s):
    """How long a step runs, without its countdown."""
    k = s["kind"]
    if k == "dots":
        return len(s["dots"]) * (s["settle"] + s["hold"] + DONE_S)
    if k == "jumps":
        return sum(s["times"]) if "times" in s else s["count"] * sum(s["each"]) / 2
    if k == "prompt":
        return sum(c[1] for c in s["cues"]) if "cues" in s else s["seconds"]
    if k == "light":
        return sum(lv[1] for lv in s["levels"])
    if k in ("path", "world"):
        return s["seconds"]
    return 0.0


def plan_seconds(plan):
    return sum(step_seconds(s) + (COUNTDOWN_S if s["ready"] and s["kind"] != "reseat" else 0) for s in plan)


# ------------------------------------------------------------------------------------------
# The processes' sockets

class Socket:
    """Commands to a datagram control socket (the panel's, ft-eyes'). Both answer every command,
    in order, so a command that wants its answer takes the one after those of the commands still
    unanswered; the others' answers are read later and errors logged. With dry=True nothing is
    sent: commands are printed (out) and fake(text) makes the reply."""

    def __init__(self, name, dry=False, fake=None, log=None, out=None):
        self.name, self.dry, self.fake = name, dry, fake or (lambda text: "ok")
        self.log, self.out = log or (lambda s: None), out
        self.sent = {}
        self.pending = 0   # commands sent and not answered yet
        self.sock = None
        if not dry:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
            self.sock.bind("")
            self.sock.setblocking(False)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _got(self, r):
        self.pending = max(0, self.pending - 1)
        if r.startswith("error"):
            self.log("%s: %s" % (self.name, r))

    def _drain(self):
        while self.sock:
            try:
                r = self.sock.recv(65536).decode(errors="replace")
            except OSError:
                return
            self._got(r)

    def cmd(self, text, reply=False, timeout=0.5):
        """Send a command; with reply=True wait for its answer (or None)."""
        if self.dry:
            if self.out:
                self.out("%s: %s" % (self.name, text))
            return self.fake(text) if reply else None
        if not self.sock:
            return None
        self._drain()
        try:
            self.sock.sendto(text.encode(), "\0" + self.name)
        except OSError:
            return None
        self.pending += 1
        if not reply:
            return None
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
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
        if self.sent.get(key) == text:
            return
        self.sent[key] = text
        self.cmd(text)


def fake_panel(text):
    if text == "ipd":
        return "ok 0.06300"
    if text.startswith("log stop"):
        return "ok 0 0 0"
    if text.startswith("world "):
        return "ok " + " ".join(text.split()[1:4])
    return "ok"


def fake_eyes(ref):
    """A dry run's ft-eyes: both eyes where the reference puts a well-fitted eye."""
    def answer(text):
        if text != "fit":
            return "ok 0" if text.startswith("finds stop") else "ok"
        if ref is None:
            return "{}"
        return json.dumps({str(cam): {"n": 90, "finds": 90, "found": 1.0, "x": e["centre"][0], "y": e["centre"][1],
                                      "a": 28.0, "spacing": 15.0, "extra": 0.0} for cam, e in ref.eyes.items()})
    return answer


def fit_summary(reply):
    try:
        return {int(cam): v for cam, v in json.loads(reply).items()}
    except (TypeError, ValueError, AttributeError):
        return None


def coach_words(r):
    """gazecheck.Checks.coach_words: the reading in a line."""
    if r is None or r["state"] == "waiting":
        return "waiting for both eyes."
    hints = " ".join(h["text"] for h in r["hints"][:2])
    words = ("good." + (" " + hints if hints else "")) if r["state"] == "ok" else (hints or "a little off.")
    if r["provisional"] and r["against"] == "reference":
        words += " (A first guess: measured on one face so far.)"
    return words


def fit_card(r):
    """A reading for the window's fit card: the state, each eye's (by name), the hints' texts."""
    if r is None:
        return None
    eyes = {}
    for cam, e in r["eyes"].items():
        eyes[e["name"]] = {"state": e["state"], "found": e["found"], "offset_deg": e.get("offset_deg")}
    return {"state": r["state"], "eyes": eyes, "hints": [h["text"] for h in r["hints"]],
            "provisional": r["provisional"], "against": r["against"]}


def compact(r):
    """A reading for fit.json: no per-hint text."""
    return {"state": r["state"], "terms": r["terms"], "hints": [h["key"] for h in r["hints"]], "eyes": r["eyes"],
            "against": r["against"], "provisional": r["provisional"]}


# ------------------------------------------------------------------------------------------
# The session

class _Skip(Exception):
    pass


class _Stop(Exception):
    pass


class _Fail(Exception):
    pass


class _Redo(Exception):
    pass


class Session:
    """One round. start() runs it on its own thread; next_step(), pause(), resume(), redo(),
    skip() and stop() steer it from any thread; on_status(dict) is called on every change.

    bounty: the board's entry (bounties.validate's), or None for a round script by name (script).
    dry_run: no processes, the panel's and ft-eyes' commands printed (print) and their replies
    made up. cams: the eye frames' share (default ft-eyegrab's; a replay's for testing).
    next_after (a test hook): press Next after that many seconds of waiting. speed: run the
    round's times faster (tests). profile_path: profile.json, where the bounty is marked done."""

    def __init__(self, base_dir, script, *, bounty=None, profile=None, checklist=None, health=None, dry_run=False,
                 speed=1.0, on_status=None, next_after=None, cams=None, panel_bin=None, panel_no_vr=False,
                 steamvr_gaze=True, button=True, button_device=None, seed=None, rounds_dir=None,
                 profile_path=None, eyes_python=None, fit_coach=None):
        self.base_dir = os.path.abspath(os.path.expanduser(base_dir or BASE_DIR))
        self.script = script
        self.round = load_round(round_path(script, rounds_dir))
        self.bounty = bounty or {"id": script, "title": self.round["title"], "script": script}
        self.profile = dict(profile or {})
        self.profile_path = profile_path
        self.checklist, self.health = dict(checklist or {}), dict(health or {})
        self.dry_run, self.speed = dry_run, max(float(speed or 1.0), 0.01)
        self.on_status, self.next_after = on_status, next_after
        self.cams, self.panel_bin, self.panel_no_vr = cams, panel_bin or PANEL_BIN, panel_no_vr
        self.steamvr_gaze = steamvr_gaze and not dry_run
        self.button, self.button_device = button, button_device
        self.eyes_python = eyes_python or EYES_PYTHON
        self.fit_coach = FIT_COACH if fit_coach is None else bool(fit_coach)
        self.seed = seed
        self.print = print
        self.quiet_units = () if dry_run or cams else QUIET_UNITS
        self.input_devices = "/proc/bus/input/devices"
        self.ref = fitcoach.Reference.load()
        self.plan = []
        self.session_dir = ""
        self._thread = None
        self._lock = threading.Lock()
        self._want = {"pause": False, "skip": False, "stop": False, "next": False, "redo": False}
        self._wake = threading.Event()
        self._status = {"state": "starting", "bounty": self.bounty["id"], "title": "", "step": "", "step_index": 0,
                        "step_count": 0, "prompt": "", "note": "", "take": None, "error": "", "waiting": False,
                        "countdown": 0, "big": "", "can_redo": False, "fit": None, "fit_words": "", "fit_card": None,
                        "ipd": None, "ready_text": READY_TEXT, "button": False, "seconds_left": 0.0,
                        "fit_coach": self.fit_coach}
        self._last_emit = 0.0
        self._log_file = None
        self._panel = self._eyes = self._sounds = None
        self._procs = {}            # name: Popen
        self._gaze_thread = None
        self._gaze_lock = threading.Lock()
        self._gaze_out = None       # the take's steamvr.jsonl
        self._session_json = None
        self._fit = {"schema": 1, "reference": None, "fitment": None, "readings": [], "slips": [], "baseline": None}
        self._take = None
        self._writer = None
        self._recording = False
        self._paused = self._paused_recording = False
        self._waiting = False
        self._redo_ok = False
        self._button_reader = None
        self._ipd = None
        self._step_t0 = None

    # --- controls (any thread)
    def start(self):
        if self._thread:
            raise RuntimeError("the session has started already")
        self._thread = threading.Thread(target=self._run, name="eyerec-session", daemon=True)
        self._thread.start()

    def _set(self, key, value):
        with self._lock:
            self._want[key] = value
        self._wake.set()

    def button_gesture(self, gesture):
        """The headset button: press = next, pause or resume; double = redo; hold = stop."""
        if self._status["state"] in ("done", "stopped", "error"):
            return
        if gesture == "press":
            with self._lock:
                paused = self._want["pause"]
            if paused:
                self.resume()
            elif self._waiting:
                self.next_step()
            elif self._status["state"] in ("countdown", "running"):
                self.pause()
            else:
                return
            self._log("headset button (%s)" % ("resume" if paused else "next" if self._waiting else "pause"))
        elif gesture == "double" and self._status.get("can_redo"):
            self._log("headset button (redo)")
            self.redo()
        elif gesture == "hold":
            self._log("headset button (stop)")
            self.stop(wait=0)

    def next_step(self):
        self._set("next", True)

    def redo(self):
        self._set("redo", True)

    def pause(self):
        self._set("pause", True)

    def resume(self):
        self._set("pause", False)

    def skip(self):
        self._set("skip", True)

    def stop(self, wait=20.0):
        self._set("stop", True)
        if self._thread and threading.current_thread() is not self._thread and wait:
            self._thread.join(wait)

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout)

    @property
    def state(self):
        return self._status["state"]

    # --- status and logs
    def _emit(self, force=True, **changes):
        self._status.update(changes)
        now = time.monotonic()
        if not force and now - self._last_emit < 0.25:
            return
        self._last_emit = now
        if self.on_status:
            try:
                self.on_status(dict(self._status))
            except Exception:
                self._log("on_status: " + traceback.format_exc())

    def _log(self, text):
        if self._log_file:
            self._log_file.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))

    def _event(self, event, **fields):
        """A line in the take's prompts.jsonl; returns its time."""
        t = mono_ns()
        if self._take:
            line = {"t": t, "event": event}
            line.update(fields)
            self._take["prompts"].write(json.dumps(line) + "\n")
        return t

    # --- the run
    def _run(self):
        try:
            resume_units(os.path.join(run_dir(), PAUSED_UNITS_FILE))   # left paused by a session that crashed
            self._make_dir()
            self._setup()
            self._emit(state="fitment")
            self._fitment()
            self._steps()
            self._finish("done")
        except _Stop:
            self._finish("stopped")
        except _Fail as e:
            self._finish("error", str(e))
        except Exception as e:
            self._log(traceback.format_exc())
            self._finish("error", "%s: %s" % (type(e).__name__, e))

    def _make_dir(self):
        rounds = os.path.join(self.base_dir, "rounds")
        os.makedirs(rounds, exist_ok=True)
        stem = "%s-%s" % (time.strftime("%Y%m%d-%H%M%S"), self.bounty["id"])
        path, n = os.path.join(rounds, stem), 1
        while True:
            try:
                os.mkdir(path)
                break
            except FileExistsError:
                n += 1
                path = os.path.join(rounds, "%s-%d" % (stem, n))
        os.mkdir(os.path.join(path, "takes"))
        self.session_dir = path
        if self.seed is None:
            self.seed = session_seed(os.path.basename(path))
        self._log_file = open(os.path.join(path, "session.log"), "a", buffering=1)
        self._log("round %s%s: bounty %s, script %s" % (os.path.basename(path), " (dry run)" if self.dry_run else "",
                                                         self.bounty["id"], self.script))

    def _setup(self):
        self._panel = Socket(PANEL_SOCKET, self.dry_run, fake_panel, self._log, self.print)
        self._eyes = Socket(EYES_SOCKET, self.dry_run, fake_eyes(self.ref), self._log)
        self._sounds = sounds.Player(on=not self.dry_run, log=self._log)
        if not self.dry_run:
            for path, build in ((self.panel_bin, "gaze/build.sh"), (FT_EYES, "gaze/tracker/build.sh")):
                if not os.path.exists(path):
                    raise _Fail("%s isn't there: %s" % (os.path.basename(path), fix_hint(build)))
            if not os.path.exists(self.eyes_python):
                raise _Fail("ft-eyes' Python isn't built (%s): %s" % (self.eyes_python, fix_hint("gaze/tracker/build.sh")))
            if self.quiet_units:
                pause_units(self.quiet_units, os.path.join(run_dir(), PAUSED_UNITS_FILE), self._log)
            self._start_panel()
            self._start_eyes()
            if self.steamvr_gaze:
                self._start_gaze()
            self._start_button()
        r = self._panel.cmd("ipd", reply=True)
        self._ipd = float(r.split()[1]) if r and r.startswith("ok ") else None
        self._status["ipd"] = self._ipd
        self._log("IPD setting: %s" % ("%.1f mm" % (self._ipd * 1000) if self._ipd is not None else "unknown"))
        self.plan = build_plan(self.round, self.seed)
        self._fit["reference"] = {"source": self.ref.source, "faces": self.ref.faces, "made": self.ref.made,
                                  "provisional": self.ref.provisional} if self.ref else None
        self._session_json = {
            "schema": 1, "round": os.path.basename(self.session_dir), "bounty": self.bounty["id"], "script": self.script,
            "title": self.bounty.get("title") or self.round["title"], "board_updated": self.bounty.get("updated"),
            "checklist": self.checklist, "recorder": VERSION, "os": os_version(), "steamvr": steamvr_version(),
            "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "ended": None, "status": "running", "seed": self.seed,
            "dry_run": self.dry_run, "replay": self.cams or None, "ipd_start": self._ipd,
            "fit": None, "eye_fps": None, "takes": [],
            "plan": [{"id": s["id"], "kind": s["kind"], "group": s["group"]} for s in self.plan]}
        if self.health:
            write_json(os.path.join(self.session_dir, "health.json"), self.health)
        self._save_session()
        self._save_fit()
        self._panel.cmd("show rec")
        self._panel.cmd("bg %.3f" % MID_BG)
        self._panel.set("step", "step " + clean_text(self._session_json["title"]))

    def _save_session(self):
        write_json(os.path.join(self.session_dir, "session.json"), self._session_json)

    def _save_fit(self):
        write_json(os.path.join(self.session_dir, "fit.json"), self._fit)

    def _spawn(self, name, argv, env=None):
        e = dict(os.environ, XDG_RUNTIME_DIR="/run/user/%d" % os.getuid())
        e.update(env or {})
        p = subprocess.Popen(boxed(argv), env=e, stdin=subprocess.PIPE, stdout=subprocess.PIPE if name == "ft-gaze" else self._log_file,
                             stderr=self._log_file, start_new_session=True)
        self._procs[name] = p
        self._log("started %s (pid %d)" % (name, p.pid))
        return p

    def _wait_answer(self, sock, name, cmd="ping", timeout=20.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if sock.cmd(cmd, reply=True, timeout=0.3):
                return
            if self._procs[name].poll() is not None:
                raise _Fail("%s exited: see session.log" % name)
            if self._want["stop"]:
                raise _Stop()
        raise _Fail("%s doesn't answer" % name)

    def _start_panel(self):
        argv = [self.panel_bin, "--watch-stdin", "--socket", PANEL_SOCKET, "--overlay", PANEL_OVERLAY]
        self._spawn("ft-gazepanel", argv + (["--no-vr"] if self.panel_no_vr else []))
        self._wait_answer(self._panel, "ft-gazepanel")

    def _start_eyes(self):
        env = {"FT_EYES_SOCKET": EYES_SOCKET, "FT_EYES_GAZE": EYES_GAZE}
        if self.cams:
            env["FT_EYES_CAMS"] = self.cams
        self._spawn("ft-eyes", [self.eyes_python, FT_EYES, "-v", "--watch-stdin"], env)
        self._wait_answer(self._eyes, "ft-eyes", "status")

    def _start_gaze(self):
        p = self._spawn("ft-gaze", [FT_GAZE, "--watch-stdin", "--sources", "all"])

        def read():
            for line in p.stdout:
                with self._gaze_lock:
                    if self._gaze_out and self._recording:
                        self._gaze_out.write(line.decode("utf-8", "replace"))
        self._gaze_thread = threading.Thread(target=read, name="eyerec-gaze", daemon=True)
        self._gaze_thread.start()

    def _start_button(self):
        if not self.button:
            return
        path = self.button_device or find_button(read_input_devices(self.input_devices))
        if not path:
            self._log("headset button: no %s device" % BUTTON_DEVICE)
            return
        self._button_reader = ButtonReader(path, self.button_gesture, self._log).start()

    def _stop_procs(self):
        for name in ("ft-gaze", "ft-eyes", "ft-gazepanel"):
            p = self._procs.pop(name, None)
            if not p:
                continue
            try:
                p.stdin.close()
                p.wait(5)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                    p.wait(3)
                except (OSError, subprocess.TimeoutExpired):
                    p.kill()
            self._log("%s stopped (exit %s)" % (name, p.returncode))
        if self._gaze_thread:
            self._gaze_thread.join(2)

    def _finish(self, state, error=""):
        if self._take:
            try:
                self._end_take("complete" if state == "done" else "stopped")
            except Exception:
                self._log(traceback.format_exc())
        if self._session_json is not None:
            self._session_json.update(status=state, ended=time.strftime("%Y-%m-%dT%H:%M:%S%z"))
            if error:
                self._session_json["error"] = error
            self._save_session()
        if self.session_dir:
            self._save_fit()
        self._log("%s%s" % (state, ": " + error if error else ""))
        if state == "done":
            self._mark_done()
        if self._button_reader:
            self._button_reader.stop()
            self._button_reader = None
        if self._panel:
            screen = {"done": DONE_SCREEN, "stopped": STOPPED_SCREEN, "error": ERROR_SCREEN}[state]
            for key, c in (("big", "big "), ("action", "action "), ("rec", "rec off"), ("keys", "keys "),
                           ("note", "note "), ("countdown", "countdown off")):
                self._panel.set(key, c)
            self._panel.cmd("paused off")
            self._panel.cmd("dot 0 0 off")
            self._show(screen["title"], screen["text"])
            end = time.monotonic() + screen["seconds"] / self.speed
            while time.monotonic() < end and not (self._want["stop"] and state == "done"):
                time.sleep(TICK_S)
            self._panel.cmd("hide")
        self._stop_procs()
        for s in (self._panel, self._eyes):
            if s:
                s.close()
        if self.quiet_units:
            resume_units(os.path.join(run_dir(), PAUSED_UNITS_FILE), self._log)
        if self._log_file:
            self._log_file.close()
            self._log_file = None
        self._emit(state=state, error=error, take=None, waiting=False, countdown=0, big="", can_redo=False, note="",
                   prompt=error or {"done": DONE_SCREEN, "stopped": STOPPED_SCREEN}.get(state, {}).get("text", "").replace("|", "\n"))

    def _mark_done(self):
        """The bounty counted in profile.json (read again first: the window may have changed it)."""
        if self.profile_path:
            try:
                with open(self.profile_path) as f:
                    self.profile = json.load(f)
            except (OSError, ValueError):
                pass
        bounties.mark_done(self.profile, self.bounty["id"], os.path.basename(self.session_dir))
        if self._ipd is not None:
            self.profile["eyes_ipd"] = self._ipd
        if self.profile_path:
            write_json(self.profile_path, self.profile)

    # --- waiting, and the controls
    def _controls(self):
        """A pause (blocking until resumed), skip, stop, redo (where _redo_ok). True after a pause."""
        with self._lock:
            want = dict(self._want)
            self._want["skip"] = self._want["redo"] = False
        if want["stop"]:
            raise _Stop()
        if want["skip"]:
            raise _Skip()
        if want["redo"] and self._redo_ok:
            raise _Redo()
        if not want["pause"]:
            return False
        self._pause()
        outcome = None
        while outcome is None:
            self._wake.wait(0.2)
            self._wake.clear()
            with self._lock:
                want = dict(self._want)
                self._want["skip"] = self._want["redo"] = False
                redo = want["redo"] and self._redo_ok
                if want["skip"] or redo:
                    self._want["pause"] = False
            outcome = _Stop if want["stop"] else _Skip if want["skip"] else _Redo if redo else \
                True if not want["pause"] else None
        self._unpause(record=outcome is True)
        if outcome is not True:
            raise outcome()
        return True

    def _pause(self):
        self._paused = True
        self._state_before = self._status["state"]
        self._paused_recording = self._recording
        if self._recording:
            self._record(False)
            self._event("pause")
        hint = RESUME_HINT_BUTTON if self._button_ok() else RESUME_HINT
        self._panel.cmd("paused on")
        self._panel.set("note", "note " + hint)
        self._emit(state="paused", note=hint)
        self._log("paused")

    def _unpause(self, record=True):
        self._paused = False
        self._panel.cmd("paused off")
        self._panel.set("note", "note ")
        with self._lock:
            self._want["next"] = False
        if self._take and self._paused_recording and record:
            self._event("resume")
            self._record(True)
        self._emit(state=self._state_before, note="")
        self._log("resumed")

    def _wait(self, seconds, tick=None, bar=True):
        """Let seconds (round time) pass, with the controls. tick(dt) runs every step; it ends the
        wait early by returning True."""
        left, last, last_bar = float(seconds), time.monotonic(), -1.0
        while True:
            if self._controls():
                last = time.monotonic()
            now = time.monotonic()
            dt = (now - last) * self.speed
            last = now
            left -= dt
            self._check_writer()
            if tick and tick(dt):
                return True
            if bar and seconds > 0 and (now - last_bar >= 0.25 or left <= 0):
                last_bar = now
                self._panel.set("countdown", "countdown %.2f" % max(0.0, min(1.0, left / seconds)))
            self._emit(force=False, seconds_left=round(max(left, 0.0), 1))
            if left <= 0:
                return False
            self._wake.wait(TICK_S)
            self._wake.clear()

    def _await_next(self, tick=None):
        """Wait for Next (or the next_after hook); tick(dt) runs meanwhile. Nothing records."""
        with self._lock:
            self._want["next"] = False
        self._waiting = True
        self._hints()
        self._emit(waiting=True, seconds_left=0.0)
        t0 = last = time.monotonic()
        try:
            while True:
                self._controls()
                now = time.monotonic()
                if tick:
                    tick((now - last) * self.speed)
                last = now
                with self._lock:
                    go, self._want["next"] = self._want["next"], False
                if go or (self.next_after is not None and now - t0 >= self.next_after):
                    return
                self._wake.wait(TICK_S)
                self._wake.clear()
        finally:
            self._waiting = False
            self._emit(waiting=False)

    def _button_ok(self):
        return bool(self._button_reader and self._button_reader.ok)

    def _hints(self, action=True):
        button = self._button_ok()
        text = (READY_BUTTON_MOUSE if mouse_connected(self.input_devices) else READY_BUTTON) if button else READY_TEXT
        if action:
            self._panel.set("action", "action " + text)
        self._panel.set("keys", "keys " + (KEYS_BUTTON if button else KEYS))
        self._status.update(ready_text=text, button=button)

    def _show(self, title, text):
        self._panel.set("title", "title " + clean_text(title))
        self._panel.set("text", "text " + clean_text(text))

    def _ipd_text(self):
        return IPD_UNKNOWN if self._ipd is None else IPD_TEXT.format(mm=self._ipd * 1000)

    # --- the fit coach
    def _reading(self, previous=None, baseline=None):
        """The fit coach's reading of ft-eyes' last second, and the finds it's from (or None, None)."""
        summary = fit_summary(self._eyes.cmd("fit", reply=True, timeout=0.5))
        if summary is None or self.ref is None:
            return None, summary
        return fitcoach.assess(summary, self.ref, ipd=self._ipd, baseline=baseline, previous=previous), summary

    def _fitment(self):
        """The fit coach on a centre dot until the fit is good and Next is pressed; Next before
        that warns, and a second press goes on (fit.json records it was skipped). With the coach
        off (FIT_COACH), _fitment_quiet instead."""
        if not self.fit_coach:
            return self._fitment_quiet()
        title = "%s: %s" % (FIT_TITLE, self._session_json["title"])
        intro = self.round["intro"]
        self._show(title, FIT_TEXT + "||" + self._ipd_text() + ("||" + intro if intro else ""))
        self._panel.cmd("dot 0 0 look")
        f = {"reading": None, "summary": None, "since": 0.0, "good": False, "ask": 0.0, "warned": False}
        started = mono_ns()

        def tick(dt):
            f["ask"] -= dt
            if f["ask"] > 0:
                return
            f["ask"] = FIT_ASK_S
            r, summary = self._reading(f["reading"])
            if r is None:
                words = "starting our eye tracker..." if summary is None else "no reference to compare with."
                f["since"] = 0.0
            else:
                f["reading"], f["summary"] = r, summary
                self._fit["readings"].append({"t": mono_ns(), "phase": "fitment", **compact(r)})
                f["since"] = f["since"] + FIT_ASK_S if r["state"] == "ok" else 0.0
                words = coach_words(r)
            good = f["since"] >= FIT_GOOD_S
            if good != f["good"]:
                f["good"] = good
                if good:
                    f["warned"] = False
                    self._panel.set("note", "note ")
            self._panel.cmd("dot 0 0 %s" % ("done" if good else "look"))
            self._panel.set("action", "action " + (self._status["ready_text"] if good or f["warned"] else FIT_ADJUST))
            self._show(title, "Headset position: " + words + "||" + self._ipd_text() + ("||" + intro if intro else ""))
            self._emit(force=False, fit=r["state"] if r else None, fit_words=words, fit_card=fit_card(r))

        self._redo_ok = False
        while True:
            self._await_next(tick)
            if f["good"] or f["warned"] or self.ref is None:
                break
            f["warned"] = True
            self._panel.set("note", "note " + FIT_SKIP_NOTE)
            self._emit(note=FIT_SKIP_NOTE)
        result = "good" if f["good"] else "skipped"
        self._fit["fitment"] = {"from": started, "to": mono_ns(), "result": result, "ipd": self._ipd,
                                "last": compact(f["reading"]) if f["reading"] else None}
        if f["summary"] and f["good"]:
            self._fit["baseline"] = fitcoach.make_baseline(f["summary"], self._ipd)
            if not self.dry_run and not self.cams:   # (not a replay's eyes): the gaze service's put-on check uses it
                try:
                    fitcoach.save_baseline(self._fit["baseline"])
                except OSError as e:
                    self._log("can't save the fit as yours: %s" % e)
        self._session_json["fit"] = result
        self._save_session()
        self._save_fit()
        self._log("fitment: %s%s" % (result, (", " + coach_words(f["reading"])) if f["reading"] else ""))
        self._panel.set("note", "note ")
        self._panel.cmd("dot 0 0 off")
        self._emit(note="", fit=None, fit_words="", fit_card=None)

    def _fitment_quiet(self):
        """The fitment with the coach off: a comfortable fit and the centre dot, Next goes on.
        The coach's readings are kept in fit.json, but nothing of them is shown."""
        intro = self.round["intro"]
        self._show("%s: %s" % (FIT_OFF_TITLE, self._session_json["title"]),
                   FIT_OFF_TEXT + "||" + self._ipd_text() + ("||" + intro if intro else ""))
        self._panel.cmd("dot 0 0 look")
        self._panel.set("action", "action " + self._status["ready_text"])
        f = {"reading": None, "ask": 0.0}
        started = mono_ns()

        def tick(dt):
            f["ask"] -= dt
            if f["ask"] > 0:
                return
            f["ask"] = FIT_ASK_S
            r, _ = self._reading(f["reading"])
            if r is not None:
                f["reading"] = r
                self._fit["readings"].append({"t": mono_ns(), "phase": "fitment", **compact(r)})

        self._redo_ok = False
        self._await_next(tick)
        self._fit["fitment"] = {"from": started, "to": mono_ns(), "result": "off", "ipd": self._ipd,
                                "last": compact(f["reading"]) if f["reading"] else None}
        self._session_json["fit"] = "off"
        self._save_session()
        self._save_fit()
        self._log("fitment: the coach is off%s" % ((", it read " + coach_words(f["reading"])) if f["reading"] else ""))
        self._panel.cmd("dot 0 0 off")

    def _shift_check(self, step):
        """After a centre dot's hold: has the headset moved since the fitment?"""
        if not self.round["shift_check"] or not self._fit["baseline"]:
            return
        r, _ = self._reading(baseline=self._fit["baseline"])
        if r is None or r["state"] == "waiting" or not fitcoach.moved(r):
            return
        hint = next((h["text"] for h in r["hints"] if h["key"] in ("up", "down", "left", "right")), "")
        note = "The headset moved since the fit check. " + (hint or "Put it back as it was.")
        self._fit["slips"].append({"t": mono_ns(), "step": step["id"], **compact(r)})
        self._event("slip", id=step["id"], terms=r["terms"])
        self._panel.set("note", "note " + note)
        self._emit(note=note)
        self._log("    slip: %s" % json.dumps(r["terms"]))

    def _reseat_reading(self, step):
        """After putting the headset back on: a reading at the centre dot, for fit.json."""
        self._panel.cmd("dot 0 0 look")
        self._show("Look at the dot", "Look at the dot for a moment.")
        got = {"r": None}

        def tick(dt, waited=[0.0]):
            waited[0] += dt
            if waited[0] < fitcoach.WINDOW + 0.2:
                return False
            r, _ = self._reading(baseline=self._fit["baseline"])
            if r and r["state"] != "waiting":
                got["r"] = r
                return True
            return False
        self._wait(READING_WAIT_S + fitcoach.WINDOW, tick, bar=False)
        self._panel.cmd("dot 0 0 off")
        r = got["r"]
        if r:
            self._fit["readings"].append({"t": mono_ns(), "phase": "reseat", "step": step["id"], **compact(r)})
            words = coach_words(r)
            if self.fit_coach:
                self._panel.set("note", "note Headset position: " + words if r["state"] != "ok" else "note ")
            self._log("    reseat: %s" % words)
        self._save_fit()

    # --- takes and recording
    def _start_take(self, group):
        n = len(self._session_json["takes"]) + 1
        take_id = "%02d-%s" % (n, group)
        d = os.path.join(self.session_dir, "takes", take_id)
        os.makedirs(d)
        t = {"id": take_id, "dir": d, "group": group, "prompts": open(os.path.join(d, "prompts.jsonl"), "a", buffering=1),
             "json": {"group": group, "started_ns": mono_ns(), "ended_ns": None, "status": "stopped", "steps": [],
                      "deleted": [], "clock": [clock_sample()], "notes": ""}}
        self._take = t
        write_json(os.path.join(d, "take.json"), t["json"])
        self._session_json["takes"].append(take_id)
        self._save_session()
        self._event("take", group=group, take=take_id)
        if not self.dry_run:
            self._writer = eyerec.Writer(d, share_path=self.cams, log=self._log).start()
        r = self._panel.cmd("log start " + d, reply=True)
        if r is not None and not r.startswith("ok"):
            raise _Fail("the panel can't log the take: %s" % r)
        self._eyes.cmd("finds start " + os.path.join(d, "own.jsonl"), reply=True)
        with self._gaze_lock:
            self._gaze_out = open(os.path.join(d, "steamvr.jsonl"), "a") if self.steamvr_gaze else None
        self._log("take %s" % take_id)
        self._emit(take=take_id)

    def _record(self, on):
        if on == self._recording:
            return
        self._recording = on
        if self._writer:
            self._writer.record(on)
        if self._take:
            self._take["json"]["clock"].append(clock_sample())
        self._event("rec", on=on)
        self._panel.set("rec", "rec on" if on else "rec off")

    def _check_writer(self):
        if self._writer and self._writer.error:
            raise _Fail("recording the eye cameras failed: %s" % self._writer.error)

    def _end_take(self, status):
        t = self._take
        try:
            self._record(False)
        finally:
            self._event("end", status=status)
            self._take = None
            with self._gaze_lock:
                if self._gaze_out:
                    self._gaze_out.close()
                self._gaze_out = None
            logged = self._panel.cmd("log stop", reply=True, timeout=1.0)
            finds = self._eyes.cmd("finds stop", reply=True, timeout=1.0)
            j = t["json"]
            if logged and logged.startswith("ok "):
                j["logged"] = dict(zip(("targets", "poses", "ipd"), (int(v) for v in logged.split()[1:4])))
            if finds and finds.startswith("ok "):
                j["own_rows"] = int(finds.split()[1])
            if self._writer:
                w, self._writer = self._writer, None
                stats = w.stop()
                j["frames"] = stats
                if any(stats["fps"]) and not self._session_json.get("eye_fps"):
                    self._session_json["eye_fps"] = stats["fps"]
                self._log("take %s: frames %s, dropped %s, %.0f MB" % (t["id"], stats["frames"], stats["dropped"],
                                                                       stats["bytes"] / 1e6))
            t["prompts"].close()
            j.update(ended_ns=mono_ns(), status=status)
            j["clock"].append(clock_sample())
            write_json(os.path.join(t["dir"], "take.json"), j)
            self._save_session()
            self._log("take %s %s" % (t["id"], status))

    # --- the steps
    def _steps(self):
        """The plan's steps. Each "ready" one (and one to do again) shows first and waits for
        Next, then a recorded countdown; the recording pauses after a step unless the next one
        follows straight on. A new group starts a new take. R during a step: it starts again;
        at a ready screen: the step before (in the same take) goes again."""
        plan = self.plan
        done = []       # steps finished in this take: (index, id, from_ns, to_ns)
        again = set()
        k = 0
        while k < len(plan):
            st = plan[k]
            where = "%s · step %d of %d" % (self._session_json["title"], k + 1, len(plan))
            self._status.update(step_index=k + 1, step_count=len(plan), step=st["id"])
            self._panel.set("step", "step " + where)
            if st["kind"] == "reseat":
                if self._take:
                    self._end_take("complete")
                done = []
                self._reseat(st)
                k += 1
                continue
            if self._take and self._take["group"] != st["group"]:
                self._end_take("complete")
                done = []
            started = self._step_t0 = None
            try:
                if st["ready"] or k in again:
                    self._redo_ok = bool(done)
                    self._ready(st, can_redo=bool(done))
                    self._await_next()
                    self._redo_ok = True
                    started = self._countdown(st)
                else:
                    self._redo_ok = True
                    self._show(self._title(st), st["text"])
                    self._emit(can_redo=True, prompt=st["text"].replace("|", "\n"))
                    self._step_t0 = started = self._event("step", id=st["id"], kind=st["kind"])
                self._take["json"]["steps"].append(st["id"]) if st["id"] not in self._take["json"]["steps"] else None
                self._run_step(st)
                done.append((k, st["id"], started, mono_ns()))
                follows = k + 1 < len(plan) and not plan[k + 1]["ready"] and plan[k + 1]["group"] == st["group"] \
                    and k + 1 not in again
                if not follows:
                    self._step_end()
                again.discard(k)
                k += 1
            except _Redo:
                self._redo_ok = False
                if started is not None:
                    self._event("redo", id=st["id"], **{"from": started, "to": mono_ns()})
                    self._log("    redo %s" % st["id"])
                    self._step_end()
                    again.add(k)
                elif done:
                    k, sid, a, b = done.pop()
                    self._event("redo", id=sid, **{"from": a, "to": b})
                    self._log("    redo %s" % sid)
                    again.add(k)
            except _Skip:
                group = st["group"]
                self._log("skipped the rest of %s" % group)
                if self._take:
                    self._step_end()
                    self._end_take("skipped")
                done = []
                while k < len(plan) and plan[k]["group"] == group and plan[k]["kind"] != "reseat":
                    k += 1
            finally:
                self._redo_ok = False
        if self._take:
            self._end_take("complete")

    def _ready(self, st, can_redo):
        self._show(self._title(st), st["text"])
        for key, c in (("big", "big "), ("countdown", "countdown off")):
            self._panel.set(key, c)
        self._panel.cmd("dot 0 0 off")
        self._panel.cmd("distance %.3f" % st["distance"])
        self._panel.cmd("bg %.3f" % st["bg"])
        self._emit(state="ready", title=self._title(st), prompt=st["text"].replace("|", "\n"), can_redo=can_redo,
                   countdown=0, big="", seconds_left=round(step_seconds(st), 1))

    def _title(self, st):
        return {"dots": "Nine dots"}.get(st["group"], self.round["title"])

    def _first_dot(self, st):
        """Where the eyes should be when the step starts: shown during its countdown."""
        if st["kind"] in ("dots", "jumps"):
            return st["dots"][0]
        if st["kind"] in ("prompt", "light"):
            return st["dot"]
        if st["kind"] == "path":
            return path_points(st["path"])[0] if st["path"].startswith("line") else None
        return None

    def _countdown(self, st):
        """A take if there's none, the recording on, 3-2-1 (a "ready" event, so labels cover only
        the step). Returns the ready event's time."""
        if not self._take:
            self._start_take(st["group"])
        t = self._step_t0 = self._event("ready", id=st["id"], seconds=COUNTDOWN_S)
        self._record(True)
        first = self._first_dot(st)
        if first:
            self._panel.cmd("dot %.3f %.3f look" % tuple(first))
        for n in range(COUNTDOWN_S, 0, -1):
            self._panel.set("big", "big %d" % n)
            self._emit(state="countdown", countdown=n, big=str(n), can_redo=True)
            self._wait(1.0, bar=False)
        self._panel.set("big", "big ")
        self._panel.set("action", "action ")
        self._event("step", id=st["id"], kind=st["kind"])
        self._emit(state="running", countdown=0, big="")
        return t

    def _step_end(self):
        """The step is over: a "wait" event ends its labels and the recording pauses."""
        self._event("wait")
        self._record(False)
        self._panel.cmd("dot 0 0 off")
        for key, c in (("big", "big "), ("countdown", "countdown off")):
            self._panel.set(key, c)
        self._panel.set("note", "note ")
        self._emit(note="", big="")

    def _run_step(self, st):
        self._log("  %s" % st["id"])
        getattr(self, "_run_" + st["kind"])(st)

    def _dot(self, yaw, pitch, state, progress=None):
        self._panel.cmd("dot %.3f %.3f %s%s" % (yaw, pitch, state, "" if progress is None else " %.2f" % progress))

    def _run_dots(self, st):
        total = len(st["dots"])
        for n, (yaw, pitch) in enumerate(st["dots"]):
            self._event("dot", id=st["id"], n=n, yaw=yaw, pitch=pitch)
            self._dot(yaw, pitch, "look")
            self._emit(force=False, prompt="%s (%d of %d)" % (st["text"].split("|")[0], n + 1, total))
            self._wait(st["settle"], bar=False)
            held = {"s": 0.0, "shown": -1.0}

            def tick(dt):
                held["s"] += dt
                if held["s"] - held["shown"] >= CAPTURE_STEP_S:
                    held["shown"] = held["s"]
                    self._dot(yaw, pitch, "capture", min(1.0, held["s"] / st["hold"]))
                return False
            self._wait(st["hold"], tick, bar=False)
            if abs(yaw) <= CENTRE_DEG and abs(pitch) <= CENTRE_DEG:
                self._shift_check(st)
            self._dot(yaw, pitch, "done")
            self._wait(DONE_S, bar=False)
            self._panel.set("countdown", "countdown %.2f" % (1 - (n + 1) / total))

    def _run_jumps(self, st):
        for n, ((yaw, pitch), secs) in enumerate(zip(st["dots"], st["times"])):
            self._event("dot", id=st["id"], n=n, yaw=yaw, pitch=pitch)
            self._dot(yaw, pitch, "look")
            self._wait(secs, bar=False)
            self._panel.set("countdown", "countdown %.2f" % (1 - (n + 1) / len(st["dots"])))

    def _run_path(self, st):
        r = self._panel.cmd("path " + st["path"], reply=True)
        if r is not None and not r.startswith("ok"):
            raise _Fail("the panel can't show path %s: %s" % (st["path"], r))
        self._event("path", id=st["id"], path=st["path"])
        self._wait(st["seconds"])
        self._panel.cmd("dot 0 0 off")

    def _run_prompt(self, st):
        if st["dot"]:
            self._dot(st["dot"][0], st["dot"][1], "look")
        else:
            self._panel.cmd("dot 0 0 off")
        if "cues" not in st:
            self._wait(st["seconds"])
            return
        total, gone = sum(c[1] for c in st["cues"]), 0.0
        for word, secs, *sound in st["cues"]:
            self._panel.set("big", "big " + word)
            dist = st["cue_distance"].get(word)
            if dist:
                self._panel.cmd("distance %.3f" % dist)
            self._event("cue", id=st["id"], word=word, **({"distance": dist} if dist else {}),
                        **({"sound": sound[0]} if sound else {}))
            if sound:
                self._sounds.play(sound[0])
                if self.dry_run:
                    self.print("sound: " + sound[0])
            self._emit(big=word)
            self._wait(secs, bar=False)
            gone += secs
            self._panel.set("countdown", "countdown %.2f" % max(0.0, 1 - gone / total))
        self._panel.set("big", "big ")

    def _run_light(self, st):
        self._dot(st["dot"][0], st["dot"][1], "look")
        for bg, secs in st["levels"]:
            self._panel.cmd("bg %.3f" % bg)
            self._event("bg", id=st["id"], bg=bg)
            self._wait(secs)
        self._panel.cmd("bg %.3f" % st["bg"])

    def _run_world(self, st):
        r = self._panel.cmd("world %.3f %.3f %.3f look" % tuple(st["point"]), reply=True)
        self._event("world", id=st["id"], point=st["point"], room=[float(v) for v in r.split()[1:4]] if r and
                    r.startswith("ok ") else None)
        self._wait(st["seconds"])
        self._panel.cmd("dot 0 0 off")

    def _reseat(self, st):
        """Not recorded: the headset off and on, Next, then a fit reading."""
        self._redo_ok = False
        self._show("Take it off and on", st["text"])
        self._panel.cmd("dot 0 0 off")
        self._emit(state="ready", title="Take it off and on", prompt=st["text"].replace("|", "\n"), can_redo=False)
        try:
            self._await_next()
            self._reseat_reading(st)
        except _Skip:
            pass


# ------------------------------------------------------------------------------------------
# The command line

def print_board(base):
    board = load_board(base)
    profile = {}
    try:
        with open(os.path.join(base, "profile.json")) as f:
            profile = json.load(f)
    except (OSError, ValueError):
        pass
    if not board:
        print("no bounty board (%s)" % BOARD_PATH)
        return
    print("bounty board of %s" % board["updated"])
    for b in bounties.view(board, profile, profile.get("checklist"), have_round):
        print("  %-9s %-7s %4s min  done %d  %-20s %s" % (b["id"], b["priority"], "%g" % b["minutes"] if b["minutes"] else "?",
                                                          b["done"], b["title"], b["available"]))


def main():
    ap = argparse.ArgumentParser(description="Run an eye recording round (the eye recorder's session runner).")
    ap.add_argument("bounty", nargs="?", help="the bounty (its id on the board) or, with --script, any name")
    ap.add_argument("--board", action="store_true", help="print the bounty board and exit")
    ap.add_argument("--script", help="run this round script (rounds/NAME.json) instead of the bounty's")
    ap.add_argument("--plan", action="store_true", help="print the round's steps and their length, and exit")
    ap.add_argument("--dry-run", action="store_true", help="run no processes; print the panel commands")
    ap.add_argument("--speed", type=float, default=1.0, help="run the round's times this many times faster")
    ap.add_argument("--next-after", type=float, metavar="S", help="test: press Next after S seconds of waiting")
    ap.add_argument("--base", help="where rounds go (default %s; a temporary folder with --dry-run)" % BASE_DIR)
    ap.add_argument("--cams", help="read eye frames from this share (lab/ft-eyes-replay's) instead of ft-eyegrab's")
    ap.add_argument("--panel", help="the panel program (default gaze/build/ft-gazepanel)")
    ap.add_argument("--panel-no-vr", action="store_true", help="test: the panel without SteamVR (--no-vr)")
    ap.add_argument("--no-steamvr-gaze", action="store_true", help="don't run ft-gaze for steamvr.jsonl")
    ap.add_argument("--eyes-python", help="the Python that runs ft-eyes (default gaze/tracker/build/venv; or FT_EYES_PYTHON)")
    ap.add_argument("--no-headset-button", action="store_true", help="don't read the headset's button")
    ap.add_argument("--button-device", metavar="PATH", help="test: read the button from this device or FIFO")
    ap.add_argument("--seed", type=int, help="the shuffles and jumps (default: from the round's id)")
    ap.add_argument("--fit-coach", action="store_true", help="the fit coach on (off by default for now: FIT_COACH)")
    ap.add_argument("--glasses", choices=("none", "glasses", "inserts", "contacts"), help="the checklist's answer")
    a = ap.parse_args()

    base = a.base or (tempfile.mkdtemp(prefix="eyerec-dry-") if a.dry_run else BASE_DIR)
    if a.board:
        print_board(base)
        return 0
    if not a.bounty:
        ap.error("which bounty? (--board lists them)")
    profile_path = os.path.join(base, "profile.json")
    try:
        with open(profile_path) as f:
            profile = json.load(f)
    except (OSError, ValueError):
        profile = {}
    checklist = dict(profile.get("checklist") or {})
    if a.glasses:
        checklist["glasses"] = a.glasses
    board = load_board(base)
    entry = None
    if not a.script:
        entry = next((b for b in bounties.view(board, profile, checklist, have_round) if b["id"] == a.bounty), None)
        if not entry:
            print("no bounty %s on the board (--board lists them)" % a.bounty)
            return 2
        if entry["available"]:
            print("%s: %s" % (entry["title"], entry["available"]))
            return 2
        entry = dict(entry, updated=board["updated"])
    script = a.script or entry["script"]
    try:
        rnd = load_round(round_path(script))
    except (OSError, ValueError) as e:
        print("round %s: %s" % (script, e))
        return 2
    if a.plan:
        plan = build_plan(rnd, a.seed or 0)
        print("%s: %d steps, about %.1f min recorded (plus the fitment)"
              % (rnd["title"], len(plan), plan_seconds(plan) / 60))
        for s in plan:
            extra = len(s["dots"]) if "dots" in s else ""
            print("  %-14s %-7s %-10s %3s %5.1f s%s" % (s["id"], s["kind"], s["group"], extra, step_seconds(s),
                                                        "" if s["ready"] else "  (follows on)"))
        return 0

    t0 = time.monotonic()
    last = {}

    def on_status(st):
        key = (st["state"], st["step"], st["prompt"], st["note"], st["take"], st["waiting"], st["countdown"],
               st["fit"], st["error"])
        if key == last.get("key"):
            return
        last["key"] = key
        state = "%s %d" % (st["state"], st["countdown"]) if st["state"] == "countdown" else st["state"]
        line = "[%6.1f] %-11s %d/%d %-14s %s" % (time.monotonic() - t0, state, st["step_index"], st["step_count"],
                                                 st["step"] or "-", (st["fit_words"] or st["prompt"]).replace("\n", " | "))
        if st["waiting"]:
            line += "  (waiting for Next)"
        if st["note"]:
            line += "  (%s)" % st["note"]
        if st["error"]:
            line += "  ERROR: %s" % st["error"]
        print(line, flush=True)

    s = Session(base, script, bounty=entry, profile=profile, checklist=checklist, dry_run=a.dry_run, speed=a.speed,
                on_status=on_status, next_after=a.next_after, cams=a.cams, panel_bin=a.panel, panel_no_vr=a.panel_no_vr,
                steamvr_gaze=not a.no_steamvr_gaze, button=not a.no_headset_button and (not a.dry_run or a.button_device),
                button_device=a.button_device, seed=a.seed, profile_path=profile_path, eyes_python=a.eyes_python,
                fit_coach=True if a.fit_coach else None)

    def on_signal(*_):
        print("stopping", flush=True)
        threading.Thread(target=s.stop, daemon=True).start()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    def keys():
        if sys.stdin.isatty():
            print("keys (then Enter): n or Enter next, p pause/resume, r redo, s skip, q stop", flush=True)
        for line in sys.stdin:
            c = line.strip()[:1].lower()
            if c in ("n", ""):
                s.next_step()
            elif c == "p":
                s.resume() if s.state == "paused" else s.pause()
            elif c == "r":
                s.redo()
            elif c == "s":
                s.skip()
            elif c == "q":
                s.stop(wait=0)
    if sys.stdin is not None:
        threading.Thread(target=keys, daemon=True).start()
    s.start()
    while s._thread.is_alive():
        s.join(0.5)
    print("round: %s" % s.session_dir)
    return 0 if s.state in ("done", "stopped") else 1


if __name__ == "__main__":
    sys.exit(main())
