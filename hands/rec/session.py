#!/usr/bin/env python3
"""The hand recorder's session runner (DESIGN.md: "Processes during a session", "Files", "The
script", "Feedback while recording", "Controls").

It reads script.json (or a bounty's round script), starts what the session needs (ft-camd and
a tracking ft-hands, only if they aren't running), and runs the sections in order. Each section
is one take: a recording ft-hands child process (--record-only, 10 sets a second or the
script's record_hz) plus prompts.jsonl, take.json and the panel's poses.jsonl. The headset panel (ft-handpanel) shows the prompts; the live hands file
gives feedback ("I can't see your left hand").

Plain Python, standard library only: ft_handrec.py imports it, and it runs from the command line
for testing (in the dev container):

  python3 hands/rec/session.py --dry-run --speed 20 --next-after 0.2   # no processes: prints the panel commands
  python3 hands/rec/session.py --ring /tmp/ring --base /tmp/hr        # ft-ringplay's frames, no headset needed
  python3 hands/rec/session.py --board                                # the bounty board, and which are done
  python3 hands/rec/session.py --bounty index-taps --dry-run --speed 20 --next-after 0.2   # a bounty's round
  python3 hands/rec/session.py --debug        # a debug session: Frametop's hand tracking used as usual, recorded

Before anything starts it runs the camera check (hands/camcheck.py) and won't start while the
upper cameras are off (--ignore-cameras overrides it). If the hand-size section's first step sees
no hands at all, it stops that step and asks: try again, or stop (DESIGN.md, "Camera check").

Step mode (the default) shows each step and waits for Next (Space in the window, n or Enter
here); a 3-2-1 countdown, recorded, then the hold. Only the countdowns and holds are recorded:
each is a part of the take's recording (sets-N.bin). --auto is the old timed flow.

All _ns times are CLOCK_MONOTONIC nanoseconds.
"""
import argparse
import glob
import hashlib
import json
import math
import mmap
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HANDS = os.path.join(REPO, "hands")
sys.path.insert(0, HANDS)
import camcheck  # noqa: E402  (hands/camcheck.py: are all four mono cameras running?)
sys.path.insert(0, HERE)
import sides  # noqa: E402  (hands/rec/sides.py: which side camera is which)
import takes  # noqa: E402  (hands/rec/takes.py: the version, and the standalone recorder's build info)
sys.path.insert(0, REPO)
import livelog  # noqa: E402  (hands/rec/livelog.py: a debug session's copy of the live tracker's output)
import voicelog  # noqa: E402  (hands/rec/voicelog.py: a debug session's microphone, if ticked)
# What the recorders share (recorder/), under the names this module always had: the window,
# the standalone recorder's spike and the tests use them from here.
from recorder.common import (mono_ns, clock_sample, in_container, host_command, host_path,  # noqa: E402,F401
                             write_json, clean_text, os_version as _os_version, steamvr_version as _steamvr_version)
from recorder import bounties  # noqa: E402
from recorder.units import unit_active, start_unit, stop_unit, pause_units, resume_units, restart_units  # noqa: E402,F401
from recorder.button import (INPUT_DEVICES, INPUT_EVENT, EV_KEY, EV_REL, REL_X, REL_Y, KEY_SELECT,  # noqa: E402,F401
                             BUTTON_DEVICE, BUTTON_DOUBLE_S, BUTTON_HOLD_S, BUTTON_BOUNCE_S, parse_input_devices,
                             find_button, real_mouse, read_input_devices, mouse_connected, button_events,
                             ButtonGestures, ButtonReader)
FT_HANDS = os.path.join(HANDS, "build", "ft-hands")
FT_CAMD = os.path.join(HANDS, "build", "ft-camd")
PANEL_BIN = os.path.join(HERE, "build", "ft-handpanel")
# Runs a program in the container this tree was built for: "dev" in a clone, the release's own in a
# release (it starts the container first)
IN_BOX = os.path.join(REPO, "scripts", "in-box")
SCRIPT_PATH = os.path.join(HERE, "script.json")
DEBUG_SCRIPT = os.path.join(HERE, "debug.json")         # a debug session: Frametop's own hand tracking, used as usual
BOARD_PATH = os.path.join(HERE, "bounties.json")      # the bounty board shipped with the recorder
BOUNTIES_DIR = os.path.join(HERE, "bounties")         # a bounty's round script: bounties/<script>.json
POSES_DIR = os.path.join(HERE, "poses")   # the pose pictures: poses.json and its PNGs
DEMOS_DIR = os.path.join(HERE, "demos")   # the demo clips: <id>.gif (DESIGN.md "Demo clips")
RECORDED_DEMOS_DIR = os.path.join(HERE, "demos-recorded")   # the same parts from real camera frames
# The demo clips a contributor follows: recorded (a real person's hands, from a session's side
# cameras: demo/camgifs.py) or skeleton (the drawn hand, its palm light and its back dark).
GUIDANCE = (("recorded", "Recorded hands"), ("skeleton", "Skeleton hands"))
BASE_DIR = os.path.expanduser("~/.local/share/frametop/hands/contrib")
PANEL_SOCKET = "ft_handpanel"
CAMD_UNIT = "frametop-handrec-camd.service"
# Frametop's live hand tracking (hands/run.sh, ft-handsctl): an ft-camd the recorder uses as it is
LIVE_CAMD_UNIT, LIVE_HANDS_UNIT = "frametop-camd.service", "frametop-hands.service"
# Frametop's drawn hands and hand pointer: off for a session, so no hand overlay is in view and no gesture
# clicks or opens Steam's menu while the person performs the steps (Session._quiet_hands)
QUIET_UNITS = ("frametop-handview.service", "frametop-pointer.service")
PAUSED_UNITS_FILE = "handrec-paused-units"   # in run_dir(): the ones to start again, if a session didn't
SIDE_CAMERAS = ("slam_left", "slam_right")   # a recording needs these (Session._first_set)
CAMERAS_BACK_S = 120     # how long a lost recording waits for the cameras (Session._cameras_back)
STALL_S = 3.0            # a recording with no new set for this long is lost (Session._watch_recording)
MAX_CAMERA_LOSSES = 3    # then the session stops
CAMERAS_TITLE = "Waiting for the cameras"
CAMERAS_TEXT = ("The headset's cameras restarted (SteamVR moves them when the colour camera module is unplugged "
                "or plugged in), or they stopped. If you took the headset off, put it back on. This step goes "
                "again once they're back.")
HANDS_UNIT = "frametop-handrec-hands.service"
CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
LIVE_KEYS = re.compile(r"^(HANDS|POINTER)_[A-Z0-9_]+$")   # the settings a debug session records
MARK_FLASH_S = 1.0       # a debug session's mark: the panel shows "Marked" this long
DEBUG_NEEDS_HANDS = ("A testing session records Frametop's own hand tracking, and it isn't running. Turn on "
                     "hand tracking in Frametop (or start the desktop), then try again.")
# The standalone Hand Recorder (takes.standalone()): its binaries run on the host, so ft-hands
# starts directly rather than in the dev container, and repairs mean its install command.
STANDALONE = takes.standalone()
# A Frametop release (pack/install-release.sh) rather than a clone
RELEASE = os.path.exists(os.path.join(REPO, ".frametop-release"))

RECORD_HZ = 10           # sets a second; a script's "record_hz" sets its own (the bounties: 30)
SIDES_READ_S = 0.5       # how often the tracking ft-hands' side camera decision is read
TICK_S = 0.05            # the session loop's step (real time)
FRESH_S = 0.3            # the hands file counts as live if published this recently
LOST_S = 1.5             # an asked-for hand lost this long gets a note
CONTROLLER_LOST_S = 1.0  # a controller off 200 (Running_OK) this long gets a note
TOUCH_M = 0.03           # touch the dot: the index tip within this of the dot
MIN_FREE = 1.5e9         # stop the session before the disk fills
COUNTDOWN_S = 3          # step mode: the 3-2-1 before each step, recorded
FIRST_SET_S = 3.0        # step mode: how long the hold may wait for its recording's first set
RESUME_HINT = "Paused. Resume: P in the Hand recorder window"
RESUME_HINT_BUTTON = "Paused. Resume: press the headset button, or P in the Hand recorder window"
READY_TEXT = "Ready? Press Space or click Next"
# With the headset's button: it leads when no mouse is connected (the window's Next can't be clicked).
READY_BUTTON = "Ready? Press the button on the right side of the headset"
READY_BUTTON_MOUSE = "Ready? Press Space, click Next, or press the headset button"
KEYS_STEP = "Hand recorder window:  Space next  \u00b7  P pause  \u00b7  R redo  \u00b7  S skip section  \u00b7  Esc stop"
KEYS_AUTO = "Hand recorder window:  P pause  \u00b7  R redo  \u00b7  S skip section  \u00b7  Esc stop"
# The headset button: a press, two (redo) and a hold (stop): ButtonGestures. The window's keys follow
# by letter: the window lists what they do.
KEYS_STEP_BUTTON = ("Headset button: press for next or pause  \u00b7  press twice to redo  \u00b7  hold to stop  "
                    "\u00b7  Window keys: Space  P  R  S  Esc")
KEYS_AUTO_BUTTON = ("Headset button: press to pause  \u00b7  press twice to redo  \u00b7  hold to stop  "
                    "\u00b7  Window keys: P  R  S  Esc")
# The early no-hands stop (DESIGN.md, "Camera check"): the first step of this section has both
# hands up. If the live tracker publishes through its hold and never sees a hand, the session
# stops that step and asks: try again, or stop. Only when the tracker published in at least
# HANDS_CHECK_PUBLISHED of at least HANDS_CHECK_MIN_READS reads (about 20 a second): without
# a tracker it can't tell.
HANDS_CHECK_SECTION = "hand-size"
HANDS_CHECK_MIN_READS = 10
HANDS_CHECK_PUBLISHED = 0.5
NO_HANDS_TITLE = "I can't see your hands"
NO_HANDS_TEXT = "The hand tracker didn't see either of your hands during that whole step."
NO_HANDS_RETRY = ("Try again: hold both hands up in front of you, about 40 cm away. To stop instead: "
                  "Esc or Stop in the Hand recorder window.")
NO_HANDS_RETRY_BUTTON = ("Try again: hold both hands up in front of you, about 40 cm away, and press the headset "
                         "button. To stop instead: hold the button, or Esc or Stop in the Hand recorder window.")


def run_dir():
    return "/run/user/%d/frametop-hands" % os.getuid()


def fix_hint(frametop):
    """How to repair a missing or broken part: Frametop's command, or the standalone install's.
    A Frametop release never builds (its programs come in its image), so a build there means
    installing the release again."""
    if STANDALONE:
        return STANDALONE.get("reinstall") or "run the Hand Recorder's install command again"
    if RELEASE and frametop.endswith("build.sh"):
        return "install this Frametop release again"
    return frametop


# ------------------------------------------------------------------------------------------
# The camera ring (camd/fhring.h; tools/ring.py's layout, struct only)

RING_HDR = struct.Struct("<8sIIIIQqQ16x")              # 64 bytes
RING_CAM = struct.Struct("<32s32siIIIIIQQQQQIf24x")      # 160 bytes
RING_SLOT = struct.Struct("<QQQQQIf16x")                 # 64 bytes, the image follows
FH_CAM_DARK, FH_CAM_COLOR = 1, 2


class Ring:
    def __init__(self, path):
        fd = os.open(path, os.O_RDONLY)
        try:
            self.map = mmap.mmap(fd, 0, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)
        magic, version, _, ncams, _, _, self.writer_pid, _ = RING_HDR.unpack_from(self.map, 0)
        if magic != b"FHRING01" or version != 1:
            self.map.close()
            raise ValueError("%s isn't a camera ring" % path)
        self.cams = []
        for i in range(min(ncams, 8)):
            f = RING_CAM.unpack_from(self.map, RING_HDR.size + i * RING_CAM.size)
            self.cams.append({
                "index": i, "sensor": f[0].split(b"\0", 1)[0].decode(errors="replace"),
                "name": f[1].split(b"\0", 1)[0].decode(errors="replace"), "node": f[2],
                "width": f[4], "height": f[5], "stride": f[6], "nslots": f[7],
                "slot_offset": f[8], "slot_bytes": f[9], "flags": f[13]})

    def close(self):
        self.map.close()

    def alive(self, max_age=1.0):
        hb = struct.unpack_from("<Q", self.map, 40)[0]
        return hb != 0 and (mono_ns() - hb) / 1e9 < max_age

    def _cam_field(self, cam, fmt, off):
        return struct.unpack_from(fmt, self.map, RING_HDR.size + cam["index"] * RING_CAM.size + off)[0]

    def latest(self, cam):
        return self._cam_field(cam, "<Q", 104)

    def dark_mean(self, cam):
        return self._cam_field(cam, "<f", 132)

    def mono(self):
        """The mono tracking cameras (no dark twins, no colour cameras, also by name for
        ft-ringplay's rings, which carry no flags)."""
        return [c for c in self.cams if not c["flags"] & (FH_CAM_DARK | FH_CAM_COLOR)
                and not c["name"].endswith("_dk") and not c["name"].startswith("color")]

    def frame_mean(self, cam):
        """The newest frame's mean luma: ft-camd's from the slot header, else (ft-ringplay
        leaves it 0) measured on a sparse grid. None if there's no complete frame."""
        n = self.latest(cam)
        if not n:
            return None
        off = cam["slot_offset"] + (n % cam["nslots"]) * cam["slot_bytes"]
        seq, _, _, _, _, _, mean = RING_SLOT.unpack_from(self.map, off)
        if seq != 2 * n + 2:
            return None
        if mean <= 0:
            img, total, count = off + RING_SLOT.size, 0, 0
            for y in range(0, cam["height"], 16):
                row = self.map[img + y * cam["stride"]: img + y * cam["stride"] + cam["width"]: 16]
                total += sum(row)
                count += len(row)
            mean = total / count if count else 0.0
        if struct.unpack_from("<Q", self.map, off)[0] != seq:
            return None
        return float(mean)


def ring_lighting(ring_path=None, samples=5, interval=0.2):
    """Each mono camera's brightness now: {"<cam>": {"mean": float, "dark_mean": float}}, the
    newest frames' mean luma and ft-camd's near-black frame mean (the room's IR light), averaged
    over about a second. None if no camera ring is running."""
    path = ring_path or os.path.join(run_dir(), "cam-ring")
    try:
        ring = Ring(path)
    except (OSError, ValueError):
        return None
    try:
        if not ring.alive():
            return None
        sums = {}
        for k in range(samples):
            for cam in ring.mono():
                m = ring.frame_mean(cam)
                s = sums.setdefault(cam["name"], [0.0, 0, 0.0, 0])
                if m is not None:
                    s[0] += m
                    s[1] += 1
                s[2] += ring.dark_mean(cam)
                s[3] += 1
            if k + 1 < samples:
                time.sleep(interval)
        out = {name: {"mean": round(s[0] / s[1], 2) if s[1] else 0.0,
                      "dark_mean": round(s[2] / s[3], 2) if s[3] else 0.0} for name, s in sums.items()}
        return out or None
    finally:
        ring.close()


def _light_levels(ring):
    vals = [v for v in (ring or {}).values() if isinstance(v, dict)]
    if not vals:
        return None
    return (sum(float(v.get("mean", 0)) for v in vals) / len(vals),
            sum(float(v.get("dark_mean", 0)) for v in vals) / len(vals))


def _close(a, b, share=0.15, floor=1.0):
    """Within 15% of the larger; the floor keeps near-black levels (a dim room's dark mean is a
    few levels) from looking different over sensor noise."""
    return abs(a - b) <= max(share * max(abs(a), abs(b)), floor)


def similar_lighting(base_dir, lighting):
    """An earlier session whose cameras saw about the same light (the mean of every mono
    camera's mean and dark_mean, each within 15%): (session_id, chosen), the latest such, or
    None. lighting is ring_lighting()'s dict, or session.json's {"chosen", "ring"}."""
    if not lighting:
        return None
    ring = lighting.get("ring") if isinstance(lighting.get("ring"), dict) else lighting
    now = _light_levels(ring)
    if not now:
        return None
    for path in sorted(glob.glob(os.path.join(base_dir, "sessions", "*", "session.json")), reverse=True):
        try:
            with open(path) as f:
                light = json.load(f).get("lighting") or {}
        except (OSError, ValueError):
            continue
        then = _light_levels(light.get("ring"))
        if then and _close(now[0], then[0]) and _close(now[1], then[1]):
            return os.path.basename(os.path.dirname(path)), light.get("chosen", "")
    return None


DAYLIGHT_IR = 6.0   # ambient IR (the mono cameras' mean dark_mean) from which it's daylight; see classify_lighting


def ambient_ir(ring):
    """The mono cameras' mean dark_mean (the room's infrared light, as ft-hands logs it), or
    None if none has one. ring is ring_lighting()'s dict."""
    vals = [float(v.get("dark_mean", 0)) for v in (ring or {}).values() if isinstance(v, dict)]
    vals = [v for v in vals if v > 0]
    return round(sum(vals) / len(vals), 2) if vals else None


def classify_lighting(ring):
    """"daylight" or "indoor" from the room's infrared light, "" if it can't tell. Sunlight
    carries a lot of infrared; lamps and LEDs hardly any, so a dim room and a bright one read
    about the same (2026-10: 1.8 by one lamp, 2.2 in a lamp-lit room) and aren't told apart.
    Nor does a sunlit room reliably: the first daylight round (dataset PR #5, big sunlit windows)
    read 2.38, since the windows are a small part of each picture and the mean barely moves. So
    "indoor" means "no strong daylight on the cameras", and the window asks people to pick
    daylight themselves. Review corrects a missed one from the pictures (sunlit windows, the time
    of day): hands standing out little from the room goes with daylight but also with pale rooms
    at night, so it isn't a test either."""
    ir = ambient_ir(ring)
    if ir is None:
        return ""
    return "daylight" if ir >= DAYLIGHT_IR else "indoor"


def lighting_record(choice, ring):
    """session.json's "lighting": chosen is the person's pick, or with "auto" (or none) what
    the cameras measured; measured and ambient_ir are always the cameras' reading."""
    measured = classify_lighting(ring)
    picked = choice if choice in ("dim", "room", "daylight") else ""
    return {"chosen": picked or measured, "source": "picked" if picked else "measured",
            "measured": measured, "ambient_ir": ambient_ir(ring), "ring": ring or {}}


def ring_alive(path=None):
    try:
        r = Ring(path or os.path.join(run_dir(), "cam-ring"))
    except (OSError, ValueError):
        return False
    try:
        return r.alive()
    finally:
        r.close()


def start_camd(path=None, log=lambda line: None, stop=lambda: False, timeout=15.0):
    """Make sure ft-camd fills the camera ring: start it (CAMD_UNIT) if nothing does. True if
    it started it, False if a ring was live already; raises RuntimeError if it can't."""
    path = path or os.path.join(run_dir(), "cam-ring")
    if ring_alive(path):
        return False
    if not os.access(FT_CAMD, os.X_OK):
        raise RuntimeError("ft-camd isn't built: " + fix_hint("hands/build.sh"))
    caps = subprocess.run(["getcap", FT_CAMD], capture_output=True, text=True) if shutil.which("getcap") else None
    if caps is not None and "cap_sys_ptrace" not in caps.stdout:
        raise RuntimeError("ft-camd needs its capabilities: " + fix_hint("hands/run.sh caps (asks for sudo)"))
    started = start_unit(CAMD_UNIT, "the camera broker", [FT_CAMD, "--status", "60"], log)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if ring_alive(path):
            return started
        if stop():
            return started
        time.sleep(0.1)
    raise RuntimeError("ft-camd didn't start (is SteamVR running?): journalctl --user -u " + CAMD_UNIT)


def openvr_env():
    """The environment for an OpenVR client (ft-handpanel): Frametop's desktop points
    XDG_CONFIG_HOME at ~/.config/frametop, where OpenVR finds no openvrpaths.vrpath."""
    env = dict(os.environ)
    config = env.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    home = os.path.expanduser("~/.config")
    if (not os.path.exists(os.path.join(config, "openvr", "openvrpaths.vrpath"))
            and os.path.exists(os.path.join(home, "openvr", "openvrpaths.vrpath"))):
        env["XDG_CONFIG_HOME"] = home
    return env


def start_panel(panel_bin=None, stdout=None, timeout=15.0):
    """Start ft-handpanel (--watch-stdin: it ends when the returned process's stdin closes) and
    wait until it answers. Returns the process; raises RuntimeError if it can't."""
    panel_bin = panel_bin or PANEL_BIN
    if not os.access(panel_bin, os.X_OK):
        raise RuntimeError("ft-handpanel isn't built: " + fix_hint("hands/rec/build.sh"))
    out = stdout if stdout is not None else subprocess.DEVNULL
    proc = subprocess.Popen([panel_bin, "--watch-stdin"], stdin=subprocess.PIPE, stdout=out, stderr=out,
                            env=openvr_env())
    panel = Panel()
    try:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if panel.cmd("ping", reply=True, timeout=0.2):
                return proc
            if proc.poll() is not None:
                raise RuntimeError("ft-handpanel exited (is SteamVR running?)")
            time.sleep(0.1)
    finally:
        panel.close()
    proc.kill()
    raise RuntimeError("ft-handpanel doesn't answer")


def steamvr_running():
    return bool(find_processes("vrserver"))


READY_PARTS = (("steamvr", "SteamVR"), ("cameras", "The cameras"), ("hands", "Hand tracking"),
               ("panel", "The headset panel"))
READY_WAIT_S = 20.0      # how long get_ready waits for each part it starts


def get_ready(frametop_hands=False, need_live=False, panel_bin=None, on_part=lambda key, state, text: None,
              log=lambda line: None):
    """Start what a round needs before its checklist, in order: SteamVR (only checked), the
    cameras (ft-camd), hand tracking (ft-hands) and the headset panel (ft-handpanel). With
    Frametop's hand tracking installed (frametop_hands) its own units start; otherwise the
    recorder's. need_live: a testing session, which records Frametop's own tracking.
    on_part(key, state, text) reports each part ("busy", "ok" or "failed"). Returns
    {"ok", "error", "units" (the units it started, to stop later), "panel" (the ft-handpanel
    process it started, or None)}."""
    out = {"ok": False, "error": "", "units": [], "panel": None}

    def fail(key, text):
        on_part(key, "failed", text)
        out["error"] = text
        return out

    def wait(test):
        end = time.monotonic() + READY_WAIT_S
        while time.monotonic() < end:
            if test():
                return True
            time.sleep(0.2)
        return test()

    def start_live(key):
        if unit_active(LIVE_HANDS_UNIT):
            return True
        r = subprocess.run(host_command("systemctl", "--user", "start", LIVE_HANDS_UNIT),
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            fail(key, "Frametop's hand tracking didn't start: %s" % (r.stderr or r.stdout).strip())
            return False
        out["units"].append(LIVE_HANDS_UNIT)
        log("started %s" % LIVE_HANDS_UNIT)
        return True

    on_part("steamvr", "busy", "")
    if not steamvr_running():
        return fail("steamvr", "SteamVR isn't running. Put the headset on (or start SteamVR), then try again.")
    on_part("steamvr", "ok", "Running")

    if need_live and not frametop_hands:
        return fail("cameras", DEBUG_NEEDS_HANDS)
    on_part("cameras", "busy", "")
    if not ring_alive():
        if frametop_hands:
            if not start_live("cameras"):
                return out
            if not wait(ring_alive):
                return fail("cameras", "The cameras didn't start (journalctl --user -u %s)" % LIVE_CAMD_UNIT)
        else:
            try:
                if start_camd(log=log, timeout=READY_WAIT_S):
                    out["units"].append(CAMD_UNIT)
            except RuntimeError as e:
                return fail("cameras", str(e))
    on_part("cameras", "ok", "Running")

    on_part("hands", "busy", "")
    if not tracker_running() or (need_live and not unit_active(LIVE_HANDS_UNIT)):
        if frametop_hands:
            if not start_live("hands"):
                return out
        else:
            argv = [FT_HANDS, "--no-gestures", "--status", "0"]
            if not STANDALONE:   # built in Frametop's container: it runs there
                argv = [IN_BOX] + argv
            try:
                if start_unit(HANDS_UNIT, "hand tracking for feedback", argv, log):
                    out["units"].append(HANDS_UNIT)
            except RuntimeError as e:
                return fail("hands", str(e))
        if not wait(tracker_running):
            return fail("hands", "Hand tracking didn't start")
    on_part("hands", "ok", "Running")

    on_part("panel", "busy", "")
    panel = Panel()
    try:
        running = bool(panel.cmd("ping", reply=True))
    finally:
        panel.close()
    if not running:
        try:
            out["panel"] = start_panel(panel_bin)
        except RuntimeError as e:
            return fail("panel", str(e))
    on_part("panel", "ok", "Ready (it shows when the round starts)")
    out["ok"] = True
    return out


# ------------------------------------------------------------------------------------------
# The factory calibration, without what identifies the unit

ID_TOKENS = {"serial", "sn", "uuid", "guid", "mac", "id", "ids", "identifier"}


def _key_identifies(key):
    k = str(key)
    if re.search(r"serial|uuid", k, re.I):
        return True
    tokens = re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", k)
    return any(t.lower() in ID_TOKENS for t in tokens)


def _value_identifies(value):
    """A string holding something like a serial number: a run of 8 or more letters and digits
    with at least 4 digits and a letter (sensor names like og01a1b are shorter)."""
    if not isinstance(value, str):
        return False
    for run in re.findall(r"[A-Za-z0-9]+", value):
        if len(run) >= 8 and sum(c.isdigit() for c in run) >= 4 and any(c.isalpha() for c in run):
            return True
    return False


def strip_calibration(xrservice_json_dict):
    """/persist/xrservice.json without what identifies the unit: keys naming a serial, sn,
    uuid, mac or id, and string values that look like serial numbers. Returns (cleaned dict,
    removed key paths such as "cameras[0].serial")."""
    removed = []

    def walk(o, path):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                p = "%s.%s" % (path, k) if path else str(k)
                if _key_identifies(k) or _value_identifies(v):
                    removed.append(p)
                    continue
                out[k] = walk(v, p)
            return out
        if isinstance(o, list):
            out = []
            for i, v in enumerate(o):
                p = "%s[%d]" % (path, i)
                if _value_identifies(v):
                    removed.append(p)
                    out.append(None)   # keep the other items' positions
                    continue
                out.append(walk(v, p))
            return out
        return o

    return walk(xrservice_json_dict, ""), removed


# ------------------------------------------------------------------------------------------
# The live hands file (include/fh_hands.h)

HANDS_HDR = struct.Struct("<8sIIQQQII16x")    # 64 bytes
HAND = struct.Struct("<IIff63fI")             # 272 bytes
FH_HAND_RIGHT = 1
PALM = (0, 5, 9, 13, 17)                      # wrist and knuckles
INDEX_TIP = 8


class HandsFile:
    """Reads ft-hands' hands file under its sequence lock. read() gives None while no tracker
    publishes (no file, or nothing for FRESH_S), else {"left": hand|None, "right": hand|None}
    with hand = {"palm": [x,y,z], "palm_m": float, "tip": [x,y,z]} in the head frame."""

    def __init__(self, path):
        self.path = path

    def read(self):
        try:
            fd = os.open(self.path, os.O_RDONLY)
        except OSError:
            return None
        try:
            size = HANDS_HDR.size + 2 * HAND.size
            for _ in range(4):
                data = os.pread(fd, size, 0)
                if len(data) < size:
                    return None
                magic, version, _, seq, _, publish_ns, nhands, _ = HANDS_HDR.unpack_from(data, 0)
                if magic != b"FHHANDS1" or version < 1:   # 2 appends the skeleton; the start is unchanged
                    return None
                if seq % 2 or os.pread(fd, 8, 16) != data[16:24]:
                    continue
                if (mono_ns() - publish_ns) / 1e9 > FRESH_S:
                    return None
                out = {"left": None, "right": None}
                for k in range(min(nhands, 2)):
                    f = HAND.unpack_from(data, HANDS_HDR.size + k * HAND.size)
                    side = "right" if f[1] & FH_HAND_RIGHT else "left"
                    if out[side]:
                        continue
                    pts = [f[4 + 3 * i: 7 + 3 * i] for i in range(21)]
                    palm = [sum(pts[i][j] for i in PALM) / len(PALM) for j in range(3)]
                    out[side] = {"palm": palm, "palm_m": math.sqrt(sum(v * v for v in palm)),
                                 "tip": list(pts[INDEX_TIP])}
                return out
            return None
        finally:
            os.close(fd)


# ------------------------------------------------------------------------------------------
# The panel (ft-handpanel, @ft_handpanel)

def _lit_cmd(a):
    """The panel command lighting an act's (or a steady light's) part."""
    return " ".join(["win", a["win"], "lit", a["lit"], a["colour"]] + ([a["label"]] if a["label"] else []))


def _pose_reply(reply):
    """The 12 numbers of an "ok <3x4 pose>" reply (row-major, the room's standing universe), or None."""
    tok = (reply or "").split()
    try:
        return [round(float(v), 5) for v in tok[1:13]] if tok[:1] == ["ok"] and len(tok) == 13 else None
    except ValueError:
        return None


class Panel:
    """Commands to ft-handpanel over its datagram socket. Commands that need an answer wait
    for it; the rest don't, and their replies are read (and errors logged) later. With
    dry=True nothing is sent: commands are printed and replies made up."""

    def __init__(self, name=PANEL_SOCKET, dry=False, log=None, out=None):
        self.name, self.dry, self.log, self.out = name, dry, log or (lambda s: None), out or print
        self.sock = None
        self.sent = {}
        self.pending = 0   # commands sent and not answered yet
        if not dry:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
            self.sock.bind("\0ft_handrec.%d.%d" % (os.getpid(), id(self) & 0xffff))
            self.sock.setblocking(False)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _drain(self):
        while self.sock:
            try:
                r = self.sock.recv(4096).decode(errors="replace")
            except (BlockingIOError, OSError):
                return
            self.pending = max(0, self.pending - 1)
            if r.startswith("error"):
                self.log("panel: %s" % r)

    def cmd(self, text, reply=False, timeout=0.5):
        """Send a command; with reply=True, its answer ("ok ..." or "error ..."), or None.
        The panel answers every command in order, so the answer is the one after those of
        the commands still unanswered."""
        if self.dry:
            self.out("panel: %s" % text)
            if not reply:
                return None
            if text.startswith("devices"):
                return "ok hmd 200 left 200 right 200"
            return "ok shown" if text == "ping" else "ok"
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
        while time.monotonic() < end:
            try:
                self.sock.settimeout(max(0.01, end - time.monotonic()))
                r = self.sock.recv(4096).decode(errors="replace")
            except OSError:
                break
            finally:
                self.sock.setblocking(False)
            self.pending = max(0, self.pending - 1)
            if self.pending == 0:
                return r
            if r.startswith("error"):
                self.log("panel: %s" % r)
        self.pending = 0   # a lost answer: start counting afresh
        return None

    def set(self, key, text):
        """Send a command only if it changes what that part of the panel shows."""
        if self.sent.get(key) == text:
            return
        self.sent[key] = text
        self.cmd(text)


# ------------------------------------------------------------------------------------------
# Processes

def find_processes(name):
    """[(pid, argv)] of running processes called name (the container shares the host's PIDs)."""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid) as f:
                if f.read().strip() != name:
                    continue
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                argv = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
        except OSError:
            continue
        out.append((int(pid), argv))
    return out


def tracker_running():
    """A tracking ft-hands (not a --record-only recorder)."""
    return any("--record-only" not in argv for _, argv in find_processes("ft-hands"))


class Recorder:
    """One part of a take's recording: ft-hands --record-only as a child process. Part 1
    writes TAKE/sets.bin; part N (after a pause) records into TAKE/.part-N and is moved to
    TAKE/sets-N.bin when it ends (ft-hands never overwrites, and always writes DIR/sets.bin)."""

    def __init__(self, take_dir, part, seconds, ring, log_file, swap=None, hz=RECORD_HZ):
        """swap: the side cameras' decision (sides.py), passed on as --sides 1 or 0; None (not
        known yet) records them as ft-camd names them (--sides auto, whatever the config says)."""
        self.take_dir, self.part = take_dir, part
        self.dir = take_dir if part == 1 else os.path.join(take_dir, ".part-%d" % part)
        self.file = "sets.bin" if part == 1 else "sets-%d.bin" % part
        self.names_swapped = bool(swap)
        argv = [FT_HANDS, "--record-only", "--record", self.dir, "--record-for", "%.0f" % max(seconds, 5),
                "--record-hz", "%g" % hz, "--status", "0",
                "--sides", "auto" if swap is None else "1" if swap else "0"]
        if ring:
            argv += ["--ring", ring]
        if not in_container() and not STANDALONE:
            argv = [IN_BOX] + argv
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log_file, stderr=log_file)
        self.started_ns = mono_ns()

    def exited(self):
        return self.proc.poll() is not None

    def has_data(self):
        """It has written a set (ft-hands starts recording within a few tens of milliseconds)."""
        try:
            return os.path.getsize(os.path.join(self.dir, "sets.bin")) > 0
        except OSError:
            return False

    def sets_written(self):
        """How many sets the part has written so far (0 before the first)."""
        try:
            path = os.path.join(self.dir, "sets.bin")
            with open(path, "rb") as f:
                head = f.read(16)
            magic, _, size = struct.unpack("<8sII", head)
            return os.path.getsize(path) // size if magic.rstrip(b"\0") == b"FHSET01" and size else 0
        except (OSError, struct.error):
            return 0

    def cameras(self):
        """The cameras in the first set written (their calibration names), [] before there is one."""
        try:
            with open(os.path.join(self.dir, "sets.bin"), "rb") as f:
                data = f.read(16 + 8 * 40)
            magic, n, _ = struct.unpack_from("<8sII", data, 0)
            if magic.rstrip(b"\0") != b"FHSET01" or len(data) < 16 + n * 40:
                return []
            return [struct.unpack_from("<16s", data, 16 + i * 40)[0].split(b"\0", 1)[0].decode(errors="replace")
                    for i in range(n)]
        except (OSError, struct.error):
            return []

    def stop(self):
        """End it (SIGTERM: ft-hands writes out its queue) and put the part in place. Afterwards
        names_swapped is what ft-hands says it applied (its DIR/sides.json, which goes into
        take.json's "parts" and is removed here)."""
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        side_file = os.path.join(self.dir, "sides.json")
        try:
            with open(side_file) as f:
                runs = json.load(f).get("names_swapped") or []
            if runs:
                self.names_swapped = bool(runs[0][1])   # --record-only never changes it
            os.remove(side_file)
        except (OSError, ValueError, TypeError, IndexError):
            pass
        if self.part > 1:
            src = os.path.join(self.dir, "sets.bin")
            if os.path.exists(src):
                os.replace(src, os.path.join(self.take_dir, "sets-%d.bin" % self.part))
            try:
                os.rmdir(self.dir)
            except OSError:
                pass
        return self.proc.returncode


# The pose pictures (poses/poses.json: {"<pose>": {"file": "<name>.png", "two_hands", "caption"}})

def load_poses(poses_dir):
    """{pose id: {"path", "two_hands", "caption"}} from poses_dir/poses.json, or {} without one."""
    try:
        with open(os.path.join(poses_dir, "poses.json")) as f:
            raw = json.load(f)
    except (OSError, ValueError):
        return {}
    out = {}
    for pose, e in (raw.items() if isinstance(raw, dict) else ()):
        if isinstance(e, dict) and isinstance(e.get("file"), str) and e["file"]:
            out[pose] = {"path": os.path.join(poses_dir, os.path.basename(e["file"])),
                         "two_hands": bool(e.get("two_hands")), "caption": clean_text(e.get("caption", ""))}
    return out


def pose_view(poses, p):
    """A prompt's picture: (path, mode, caption), mode "" (as drawn: a right hand, or both hands),
    "mirror" (a left hand) or "both" (a mirrored copy on the left, the picture on the right).
    The prompt's "picture" (a pose id) picks a picture other than its pose's, and
    "picture_mirror" mirrors a two-hand picture. ("", "", "") when there's none."""
    e = poses.get(p.get("picture") or p.get("pose") or "")
    if not e or not os.path.isfile(e["path"]):
        return "", "", ""
    hands = p.get("hands")
    if e["two_hands"]:
        mode = "mirror" if p.get("picture_mirror") else ""
    else:
        mode = "mirror" if hands == "left" else "both" if hands == "both" else ""
    return e["path"], mode, e["caption"]


def demo_view(demos_dir, p):
    """A prompt's demo clip: (path, mode), mode "" (as made: a right hand, or both hands) or
    "mirror" (the prompt's "demo_mirror": a right hand's clip for the left hand). ("", "")
    when it has none, or its file isn't there."""
    demo = p.get("demo") or ""
    path = os.path.join(demos_dir, os.path.basename(demo) + ".gif") if demo else ""
    if not path or not os.path.isfile(path):
        return "", ""
    return path, "mirror" if p.get("demo_mirror") else ""


def has_clips(demos_dir):
    try:
        return any(n.endswith(".gif") for n in os.listdir(demos_dir))
    except OSError:
        return False


def default_guidance(recorded_dir=None):
    """Recorded clips when there are any, else the skeleton ones."""
    return "recorded" if has_clips(recorded_dir or RECORDED_DEMOS_DIR) else "skeleton"


def guided_demo_view(guidance, skeleton_dir, recorded_dir, p):
    """demo_view in the guidance's folder, else the other one (a part with no clip of that kind):
    (path, mode, key), key true for a skeleton clip (the panel and window show its colours' key)."""
    dirs = [(recorded_dir, False), (skeleton_dir, True)]
    if guidance != "recorded":
        dirs.reverse()
    for d, key in dirs:
        path, mode = demo_view(d, p)
        if path:
            return path, mode, key
    return "", "", False


# ------------------------------------------------------------------------------------------
# The script

HAND_VALUES = ("left", "right", "both", "none", "any", "")
PROMPT_KEYS = ("text", "seconds", "hands", "pose", "distance", "position", "object", "controller", "demo")


def _when_ok(cond, ctx):
    """A prompt's or block's "when": "controllers", "objects:keyboard", either negated with "!",
    or a list of them, all to hold."""
    if not cond:
        return True
    if isinstance(cond, list):   # all of them
        return all(_when_ok(c, ctx) for c in cond)
    neg = cond.startswith("!")
    c = cond.lstrip("!")
    value = c[len("objects:"):] in ctx["objects"] if c.startswith("objects:") else bool(ctx.get(c))
    return value != neg


def _prompt(section, raw, ctx, subst=None):
    p = dict(section.get("defaults") or {})
    p.update(raw)
    p.pop("when", None)
    for k, v in (subst or {}).items():
        for key in ("text", "object"):
            if isinstance(p.get(key), str):
                p[key] = p[key].replace("{%s}" % k, v)
    if subst and "object_key" in subst:
        p["object"] = subst["object_key"]
    for key in PROMPT_KEYS:
        p.setdefault(key, False if key == "controller" else 0 if key == "seconds" else "")
    p["controllers"] = list(p.get("controllers") or [])
    p["switch"] = _switch_list(section, p.get("switch"))
    if p["hands"] not in HAND_VALUES:
        raise ValueError("%s: hands %r" % (section["id"], p["hands"]))
    return p


def _switch_list(section, raw):
    """A step's pose changes (DESIGN.md, "Two poses in a step"): "backs toward you" (at half
    time), or [{"at": share of the step's time, "to": the pose after, "text": the instruction
    after (optional; a scene's acts keep their own)}] in order."""
    if not raw:
        return []
    if isinstance(raw, str):
        raw = [{"at": 0.5, "to": raw}]
    out, last = [], 0.0
    for w in raw if isinstance(raw, list) else [raw]:
        at = w.get("at") if isinstance(w, dict) else None
        to = w.get("to") if isinstance(w, dict) else None
        if not isinstance(at, (int, float)) or not last < at < 1 or not isinstance(to, str) or not to.strip():
            raise ValueError("%s: switch %r: each needs \"at\" (after the one before, under 1) and \"to\"" % (section["id"], w))
        if isinstance(w, dict) and not isinstance(w.get("text", ""), str):
            raise ValueError("%s: switch %r: \"text\" is the instruction after" % (section["id"], w))
        out.append({"at": float(at), "to": to.strip(), "text": w.get("text", "") if isinstance(w, dict) else ""})
        last = at
    return out


def _set_ids(section, prompts):
    seen = {}
    for p in prompts:
        if not p.get("id"):
            parts = [section["id"]] + [re.sub(r"[^a-z0-9]+", "-", str(p[k]).lower()).strip("-")
                                       for k in ("demo", "pose", "hands", "distance", "position", "object") if p[k]]
            p["id"] = "/".join(parts) if len(parts) > 1 else "%s/%d" % (section["id"], len(seen) + 1)
        n = seen.get(p["id"], 0) + 1
        seen[p["id"]] = n
        if n > 1:
            p["id"] = "%s#%d" % (p["id"], n)


def load_script(path):
    with open(path) as f:
        script = json.load(f)
    if script.get("version") != 1 or not isinstance(script.get("sections"), list):
        raise ValueError("%s: not a version 1 script" % path)
    return script


def bounty_script(script, bounties_dir=None):
    """A bounty's round script's path."""
    return os.path.join(bounties_dir or BOUNTIES_DIR, script + ".json")


def have_bounty_script(script, bounties_dir=None):
    return bool(bounties.ID.match(script)) and os.path.exists(bounty_script(script, bounties_dir))


def load_board(base_dir=None):
    """The board: the shipped one, or the copy fetched from the dataset when it's newer (hub.py board)."""
    cache = os.path.join(base_dir or BASE_DIR, "board-cache.json")
    return bounties.newest(bounties.load(BOARD_PATH), bounties.load(cache))


def session_seed(session_id):
    """The shuffle's seed for a session: from its id, so the same id gives the same plan."""
    return int(hashlib.sha256(session_id.encode()).hexdigest()[:8], 16)


def _sweep_prompts(s, raw, ctx, rng):
    """A sweep section's steps (DESIGN.md, "Sweeps"): each a prompt with "cues", the pose of each
    cue_s slot in order. A step names its cues, or takes a group: "next" (the groups in turn, in
    the shuffled order) or "any" (a group drawn at random, different for each "any" while there
    are groups left). With rng (the session's shuffle) the groups' order, the "any" draws and the
    cues' order within a step are shuffled, unless the section says "shuffle": false or the
    step "fixed": true; without it, everything goes in the script's order."""
    groups = [list(g) for g in raw.get("groups") or []]
    shuffle = rng is not None and raw.get("shuffle", True)
    order = list(range(len(groups)))
    pool = list(range(len(groups)))
    if shuffle:
        rng.shuffle(order)
        rng.shuffle(pool)
    taken, drawn, counts = 0, 0, {}
    out = []
    for step in raw.get("sweeps") or []:
        if not _when_ok(step.get("when"), ctx):
            continue
        g = step.get("group")
        if step.get("cues"):
            cues = list(step["cues"])
        elif g == "next" and groups:
            cues, taken = list(groups[order[taken % len(order)]]), taken + 1
        elif g == "any" and groups:
            cues, drawn = list(groups[pool[drawn % len(pool)]]), drawn + 1
        elif isinstance(g, int) and 0 <= g < len(groups):
            cues = list(groups[g])
        else:
            raise ValueError("%s: a sweep needs cues or a group" % s["id"])
        if shuffle and not step.get("fixed"):
            rng.shuffle(cues)
        cue_s = float(step.get("cue_s", raw.get("cue_s", 4)))
        slots = max(len(cues), int(math.ceil(float(step.get("seconds", raw.get("step_s", cue_s * len(cues)))) / cue_s
                                             - 1e-9))) if step.get("cycle", raw.get("cycle", True)) else len(cues)
        p = _prompt(s, {k: v for k, v in step.items() if k not in ("group", "cues", "fixed", "cycle", "quick")}, ctx)
        p["cues"] = [cues[k % len(cues)] for k in range(slots)]
        p["cue_s"] = cue_s
        p["seconds"] = cue_s * slots
        p["pose"] = p["cues"][0]
        if not step.get("id"):
            counts[p["hands"]] = counts.get(p["hands"], 0) + 1
            p["id"] = "%s/%s-%d" % (s["id"], p["hands"] or "any", counts[p["hands"]])
        else:
            p["id"] = "%s/%s" % (s["id"], step["id"])
        if step.get("quick") is False:
            p["quick"] = False
        out.append(p)
    return out


# A scene's gestures (DESIGN.md, "Scenes"): the lit part's colour and label for each, and what
# the band says while it's lit. A no-go is drawn hollow with a cross.
# A scene step whose acts switch between the hands (L or R on the button) gives each act at
# least this long lit, so there's time to see which hand and use it (user, 2026-10-14).
CHOICE_MIN_S = 2.0

GESTURES = {
    "tap": ("cyan", "", "Tap: thumb to index fingertip"),
    "double": ("cyan", "2", "Double tap"),
    "hold": ("cyan", "hold", "Press and hold: thumb to index, about 1 s"),
    "middle": ("purple", "", "Middle tap: thumb to middle fingertip"),
    "middle-hold": ("purple", "hold", "Middle press and hold, about 1 s"),
    "nogo": ("orange", "", "Don't touch: stop about 1 cm short"),
    "pinch": ("cyan", "", "Pinch it: thumb and index"),
    "card": ("cyan", "", "Grab it like a playing card: thumb, index and middle"),
    "fist": ("green", "", "Grab it with your fist"),
    "touch": ("orange", "", "Touch it with an open hand: no grab"),
    "rest": ("cyan", "", ""),
}
SCENE_LOOKS = re.compile(r"^(plain|grid:[1-6]x[1-6]|list:([1-9]|10))$")
SCENE_PART = r"(bar|left|right|top|bottom|tl|tr|bl|br|button:\d+|item:\d+)"
SCENE_PARTS = re.compile(r"^(none|%s(\+%s)*)$" % (SCENE_PART, SCENE_PART))   # "tl+br": a two-hand grab


def _cells(look):
    """How many buttons or rows a window's look has (0: plain)."""
    m = re.match(r"^grid:(\d)x(\d)$", look)
    if m:
        return int(m.group(1)) * int(m.group(2))
    m = re.match(r"^list:(\d+)$", look)
    return int(m.group(1)) if m else 0


def _act(p, a, windows):
    """One act of a scene step, filled in: {"s", "win", "lit", "gesture", "colour", "label", "hand",
    "say", "ghost"}. The colour and label come from the gesture unless the act gives them."""
    g = a.get("gesture", "tap")
    if g not in GESTURES:
        raise ValueError("%s: gesture %r" % (p.get("id") or p.get("text", "")[:30], g))
    colour, label, say = GESTURES[g]
    win = a.get("win") or windows[0]["id"]
    if win not in [w["id"] for w in windows]:
        raise ValueError("%s: no window %r" % (p.get("text", "")[:30], win))
    lit = a.get("lit", "none" if g == "rest" else "bar")
    if not SCENE_PARTS.match(lit):
        raise ValueError("lit %r" % lit)
    ghost = a.get("ghost")
    if ghost is not None:
        ghost = [float(v) for v in ghost] + [0.0] * (6 - len(ghost))
        if len(ghost) not in (6, 7):
            raise ValueError("ghost %r: dx dy dz yaw pitch roll [scale]" % a.get("ghost"))
    return {"s": float(a.get("s", 2.0)), "win": win, "lit": lit, "gesture": g, "colour": a.get("colour", colour),
            "label": a.get("label", label), "hand": a.get("hand", ""), "say": a.get("say") or say or p.get("say", ""),
            "ghost": ghost}


def _scene_buttons(p, a, windows, rng):
    """Paced acts: the windows' buttons (or rows) lit one at a time, every every_s, lit for lit_s
    with an unlit rest after; the gesture drawn from mix (weights), each in its colour; with
    hands, each act names the hand to use (L or R on the button)."""
    every = float(a.get("every_s", 1.5))
    lit_s = min(every, float(a.get("lit_s", every * 0.7)))
    hands = list(a.get("hands") or [])
    if len(set(hands)) > 1 and lit_s < CHOICE_MIN_S:   # a hand to pick: lit long enough, the rest kept
        every, lit_s = every - lit_s + CHOICE_MIN_S, CHOICE_MIN_S
    n = int(a.get("count") or max(1, round(float(a.get("seconds", 12)) / every)))
    wins = [w for w in windows if w["id"] in (a.get("windows") or [w["id"] for w in windows]) and _cells(w["look"])]
    if not wins:
        raise ValueError("%s: buttons need a window with a grid or a list" % p.get("text", "")[:30])
    mix = a.get("mix") or {a.get("gesture", "tap"): 1}
    names, weights = list(mix), [float(mix[k]) for k in mix]
    says = a.get("says") or {}
    out, last = [], None
    for _ in range(n):
        w = rng.choice(wins)
        cells = _cells(w["look"])
        part = "button" if w["look"].startswith("grid") else "item"
        k = rng.randrange(cells)
        if cells > 1:
            while (w["id"], k) == last:
                k = rng.randrange(cells)
        last = (w["id"], k)
        g = rng.choices(names, weights)[0]
        act = {"s": lit_s, "win": w["id"], "lit": "%s:%d" % (part, k), "gesture": g}
        if g in says:
            act["say"] = says[g]
        if hands:
            h = rng.choice(hands)
            mark = GESTURES[g][1]
            act.update(hand=h, label=("L" if h == "left" else "R") + ("_" + mark if mark else ""))
        out.append(_act(p, act, windows))
        if every - lit_s > 0.05:
            out.append(_act(p, {"s": every - lit_s, "win": w["id"], "gesture": "rest", "say": out[-1]["say"]}, windows))
    return out


def _scene_moves(p, a, windows, rng):
    """Grabs: each move lights a part to grab and, with a ghost, shows where to take the window;
    one every every_s, shuffled unless "shuffle": false, each followed by a let-go rest of gap_s.
    A move is a ghost ([dx, dy, dz, yaw, pitch, roll, scale], in the window's frame) or
    {"ghost", "grab", "win", "gesture", "hand", "label", "say", "s"}, each defaulting to the
    generator's own."""
    moves = [dict(m) if isinstance(m, dict) else {"ghost": m} for m in a.get("moves") or []]
    if not moves:
        raise ValueError("%s: moves needs a list" % p.get("text", "")[:30])
    if a.get("shuffle", True):
        rng.shuffle(moves)
    n = int(a.get("count") or len(moves))
    out = []
    for j in range(n):
        m = moves[j % len(moves)]
        act = {"s": float(m.get("s", a.get("every_s", 4))), "win": m.get("win", a.get("win")),
               "lit": m.get("grab", a.get("grab", "bar")), "gesture": m.get("gesture", a.get("gesture", "pinch")),
               "ghost": m.get("ghost"), "say": m.get("say", a.get("say")), "hand": m.get("hand", a.get("hand", ""))}
        if "label" in m:
            act["label"] = m["label"]
        out.append(_act(p, act, windows))
        gap = float(a.get("gap_s", 1.0))
        if gap > 0:
            out.append(_act(p, {"s": gap, "win": out[-1]["win"], "gesture": "rest", "say": a.get("rest_say", "Let go")},
                            windows))
    return out


def _scene_step(s, p, rng):
    """A scene step's windows and acts (DESIGN.md, "Scenes"), into p["scene"]; its seconds are the
    acts'. "acts" is a list of acts and generators ({"gen": "buttons"|"moves", ...})."""
    windows = []
    for w in p.get("windows") or []:
        look = w.get("look", "plain")
        if not SCENE_LOOKS.match(look) or len(w.get("at") or []) != 3 or len(w.get("size") or []) != 2:
            raise ValueError("%s: window %r" % (s["id"], w))
        windows.append({"id": str(w.get("id", "a")), "at": [float(v) for v in w["at"]],
                        "size": [float(v) for v in w["size"]], "turn": [float(v) for v in (w.get("turn") or [0, 0])],
                        "look": look})
    if not windows or len(windows) > 4 or len({w["id"] for w in windows}) != len(windows):
        raise ValueError("%s: a scene step needs 1 to 4 windows, each its own id" % s["id"])
    acts = []
    spec = p.get("acts") or []
    for a in [spec] if isinstance(spec, dict) else spec:
        gen = a.get("gen")
        if gen == "buttons":
            acts += _scene_buttons(p, a, windows, rng)
        elif gen == "moves":
            acts += _scene_moves(p, a, windows, rng)
        elif gen:
            raise ValueError("%s: gen %r" % (s["id"], gen))
        else:
            acts.append(_act(p, a, windows))
    if not acts:
        raise ValueError("%s: a scene step needs acts" % s["id"])
    asked = [a for a in acts if a["gesture"] != "rest"]
    if len({a["hand"] for a in asked if a["hand"] in ("left", "right")}) > 1:
        short = [a for a in asked if a["s"] < CHOICE_MIN_S - 1e-9]
        if short:
            raise ValueError("%s: its acts switch hands, so each needs at least %g s (%g here)"
                             % (p.get("id") or s["id"], CHOICE_MIN_S, short[0]["s"]))
    # Lit for the whole step (one hand holds a pinch while the other taps): on windows no act uses.
    steady = [_act(p, dict(x, s=0), windows) for x in p.get("steady") or []]
    if {x["win"] for x in steady} & {a["win"] for a in acts}:
        raise ValueError("%s: a steady light's window is one the acts use" % s["id"])
    p["scene"] = {"windows": windows, "acts": acts, "steady": steady}
    p["seconds"] = round(sum(a["s"] for a in acts), 3)
    for key in ("acts", "windows", "steady"):
        p.pop(key, None)
    p.setdefault("say", acts[0]["say"])
    if not p.get("pose"):
        p["pose"] = next((a["gesture"] for a in acts if a["gesture"] != "rest"), "")


def build_plan(script, checklist, seed=None, quick=False):
    """The sections this session runs, prompts expanded for the checklist, and the ones
    skipped: (plan, skipped) with skipped = [{"section", "reason"}]. seed: the shuffle of the
    sweeps (session_seed; None: the script's order). quick: the quick round, only sections
    marked "quick" and, in those, no step marked "quick": false."""
    objects = [o for o in (checklist.get("objects") or []) if o]
    own = [o for o in (checklist.get("own_objects") or []) if str(o).strip()]
    ctx = {"objects": set(objects) | set(own), "controllers": checklist.get("controllers") == "straps"}
    names = script.get("objects") or {}
    plan, skipped = [], []
    for raw in script["sections"]:
        sid = raw["id"]
        if quick and not raw.get("quick"):
            skipped.append({"section": sid, "reason": "not in a quick round"})
            continue
        missing = [r for r in raw.get("requires") or [] if not ctx.get(r)]
        if missing:
            skipped.append({"section": sid, "reason": "needs " + ", ".join(missing)})
            continue
        s = dict(raw)
        s["kind"] = raw.get("kind", "prompts")
        s["intro_s"] = float(raw.get("intro_s", script.get("intro_s", 4)))
        prompts = []
        if s["kind"] == "sweep":
            rng = random.Random("%d/%s" % (seed, sid)) if seed is not None else None
            prompts = _sweep_prompts(s, raw, ctx, rng)
            if quick:
                prompts = [p for p in prompts if p.get("quick") is not False]
        elif raw.get("for_each") == "object":
            skip = set(raw.get("skip_objects") or [])
            for o in [o for o in objects if o not in skip] + own:
                label = clean_text(names.get(o, o)).replace("|", "/")
                for p in raw.get("prompts") or []:
                    if _when_ok(p.get("when"), ctx):
                        prompts.append(_prompt(s, p, ctx, {"object": label, "object_key": clean_text(o)}))
        else:
            prompts = [_prompt(s, p, ctx) for p in raw.get("prompts") or [] if _when_ok(p.get("when"), ctx)
                       and not (quick and p.get("quick") is False)]
            for p in prompts:   # a prompt may have cues too (hand size): its own, in its order
                if p.get("cues"):
                    cue_s = float(p.get("cue_s", raw.get("cue_s", 4)))
                    p["cues"], p["cue_s"] = list(p["cues"]), cue_s
                    p["seconds"] = cue_s * len(p["cues"])
                    p["pose"] = p["cues"][0]
        if s["kind"] == "scene":   # a step's own id goes under its section's, as a sweep step's
            for p in prompts:
                if p.get("id") and "/" not in p["id"]:
                    p["id"] = "%s/%s" % (sid, p["id"])
        _set_ids(s, prompts)
        if s["kind"] == "scene":
            # The acts' draws, per step: the session's (seed), or the step's own for a plan
            # without one, so --plan shows the same each time.
            for p in prompts:
                _scene_step(s, p, random.Random("%s/%s/%s" % (seed, sid, p["id"])))
        s["prompts"] = prompts
        if s["kind"] == "bar":
            heights = []
            for h in raw.get("heights") or []:
                h = {"id": h} if isinstance(h, str) else dict(h)
                h.setdefault("text", (raw.get("height_text") or {}).get(h["id"], raw.get("text", "")))
                h.setdefault("reps", raw.get("reps", 5))
                heights.append(h)
            s["heights"] = heights
        before = raw.get("before")
        s["before"] = before if before and _when_ok(before.get("when"), ctx) else None
        if s["kind"] in ("prompts", "sweep", "scene") and not prompts:
            skipped.append({"section": sid, "reason": "nothing to do"})
            continue
        if s["kind"] == "targets" and not raw.get("targets"):
            skipped.append({"section": sid, "reason": "no targets"})
            continue
        plan.append(s)
    return plan, skipped


def plan_record(plan):
    """What the shuffle chose, for session.json: each section's sweep steps and their cues."""
    out = {}
    for s in plan:
        steps = [{"id": p["id"], "hands": p["hands"], "cues": p["cues"]} for p in s["prompts"] if p.get("cues")]
        steps += [{"id": p["id"], "hands": p["hands"],
                   "acts": [[a["gesture"], a["win"], a["lit"], a["hand"]] for a in p["scene"]["acts"]]}
                  for p in s["prompts"] if p.get("scene")]
        if steps:
            out[s["id"]] = steps
    return out


def section_steps(s):
    """The steps that wait for Next in step mode: each prompt and bar height, and the first target."""
    return (len(s["prompts"]) + (len(s.get("heights") or []) if s["kind"] == "bar" else 0)
            + (1 if s["kind"] == "targets" and s.get("targets") else 0))


def section_seconds(s, worst=False, auto=True):
    """A section's length in script seconds (targets: about 4 s each, or the timeout if worst).
    In step mode, what's recorded: a countdown before each step and the holds; the intro and
    the waits for Next aren't."""
    t = (s["intro_s"] if auto else COUNTDOWN_S * section_steps(s)) + sum(p["seconds"] for p in s["prompts"])
    if s["kind"] == "targets":
        t += len(s["targets"]) * (s.get("timeout_s", 8) if worst else min(4.0, s.get("timeout_s", 8)))
    if s["kind"] == "bar":
        lead = s.get("lead_s", 3) if auto else 0
        t += sum(lead + h["reps"] * s.get("period_s", 6) for h in s["heights"])
    return t


def plan_seconds(script, plan, worst=False, auto=True):
    """The session's length: in auto mode all of it; in step mode only what's recorded, as the
    time spent reading each step before pressing Next is up to the person (plan_steps)."""
    if not auto:
        return sum(section_seconds(s, worst, auto=False) for s in plan)
    t = (script.get("welcome") or {}).get("seconds", 0) + (script.get("done") or {}).get("seconds", 0)
    for i, s in enumerate(plan):
        t += section_seconds(s, worst)
        t += s["before"]["seconds"] if s.get("before") else (script.get("between_s", 3) if i else 0)
    return t


def plan_steps(plan):
    return sum(section_steps(s) for s in plan)


def plan_summary(script, plan, auto=False):
    """The length in words, for the window and --plan (the plan of a quick round or a full one)."""
    minutes = max(1, round(plan_seconds(script, plan, auto=auto) / 60))
    if auto:
        return "about %d min, each step advancing by itself" % minutes
    n = plan_steps(plan)
    return ("about %d min of recording in %d steps, plus the time you take to read each step before "
            "pressing Next (at 5 s a step, about %d min more)" % (minutes, n, max(1, round(n * 5 / 60))))


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


class _CamerasLost(_Fail):
    """The recording stopped by itself or has no side cameras: usually ft-camd restarting after
    XRService moved the cameras (camd.c, SILENT_S). Step mode waits for them and does the step
    again (Session._cameras_back); anywhere else it fails the session like any _Fail."""


_camd_restarted = set()   # ft-camd pids camera_check(repair=True) has restarted: once each


def _unit_pid(unit):
    r = subprocess.run(host_command("systemctl", "--user", "show", "-p", "MainPID", "--value", unit),
                       capture_output=True, text=True, timeout=30)
    try:
        return int(r.stdout.strip() or 0)
    except ValueError:
        return 0


def live_settings(conf_path=None):
    """The HANDS_* and POINTER_* lines of ~/.config/frametop.conf, comments dropped; a value
    naming a path keeps only its last part (a path says whose home it is)."""
    out = {}
    try:
        with open(conf_path or CONF_PATH) as f:
            lines = f.read().splitlines()
    except OSError:
        return out
    for line in lines:
        key, eq, value = line.partition("=")
        key = key.strip()
        if not eq or not LIVE_KEYS.match(key):
            continue
        value = value.split("#", 1)[0].strip().strip("\"'")
        if "/" in value:
            value = os.path.basename(value.rstrip("/"))
        out[key] = value
    return out


def live_tracker(unit=LIVE_HANDS_UNIT):
    """What Frametop's live ft-hands is: {"frametop": its checkout's git describe, "models":
    {file: sha256 (16 hex)}} of the models it loads (HANDS_MODELS, else hands/models/student
    or the stock hands/models/ncnn next to it). Empty fields when it can't be told."""
    out = {"frametop": "", "models": {}}
    try:
        r = subprocess.run(host_command("systemctl", "--user", "show", "-p", "ExecStart", "--value", unit),
                           capture_output=True, text=True, timeout=30)
        exe = next((w for w in r.stdout.split() if w.endswith("/ft-hands")), "")
    except (OSError, subprocess.SubprocessError):
        exe = ""
    repo = os.path.dirname(os.path.dirname(os.path.dirname(exe))) if exe else ""
    if repo:
        try:
            out["frametop"] = subprocess.run(["git", "-C", repo, "describe", "--always", "--dirty", "--tags"],
                                             capture_output=True, text=True, timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    conf = {}
    try:
        with open(CONF_PATH) as f:
            for line in f:
                key, eq, value = line.partition("=")
                if eq and key.strip() == "HANDS_MODELS":
                    conf["HANDS_MODELS"] = value.split("#", 1)[0].strip().strip("\"'")
    except OSError:
        pass
    dirs = [os.path.expanduser(conf["HANDS_MODELS"])] if conf.get("HANDS_MODELS") else []
    if repo:
        dirs += [os.path.join(repo, "hands", "models", d) for d in ("student", "ncnn")]
    for d in dirs:
        files = sorted(glob.glob(os.path.join(d, "*.param")) + glob.glob(os.path.join(d, "*.bin")))
        if files:
            for path in files:
                h = hashlib.sha256()
                with open(path, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                out["models"][os.path.basename(path)] = h.hexdigest()[:16]
            out["models_dir"] = os.path.basename(d.rstrip("/"))
            break
    return out


def camera_check(repair=False):
    """camcheck.check() (the XRService log, its open cameras where readable, ft-camd's ring);
    never raises: a failure is "unknown". repair (the window, never during a session): when
    XRService runs every tracking camera but the recorder's own ft-camd doesn't publish them all
    (it started while some were missing), restart it, once per ft-camd, and check again."""
    try:
        r = camcheck.check()
        pid = (r.get("ring") or {}).get("writer_pid")
        if repair and camcheck.is_ring_short(r) and pid not in _camd_restarted:
            if _unit_pid(CAMD_UNIT) == pid:
                _camd_restarted.add(pid)
                stop_unit(CAMD_UNIT)
                start_camd()
            elif _unit_pid(LIVE_CAMD_UNIT) == pid:
                # Frametop's live hand tracking: its ft-hands names the cameras when it starts,
                # so it goes again after ft-camd
                _camd_restarted.add(pid)
                restart_units([LIVE_CAMD_UNIT, LIVE_HANDS_UNIT])
                end = time.monotonic() + 15
                while not ring_alive() and time.monotonic() < end:
                    time.sleep(0.2)
            else:
                return r
            r = camcheck.check()
            r["evidence"].append("restarted ft-camd (pid %d) because it published only some of the cameras" % pid)
        return r
    except Exception as e:
        return {"status": "unknown", "summary": "unknown: the camera check failed (%s)" % e,
                "reason": str(e), "evidence": []}


def camera_text(result):
    """What to tell the person about a camera check that isn't ok ("" if it's ok or unknown)."""
    if not result or result.get("status") != "degraded":
        return ""
    if camcheck.is_vcint_failure(result):
        return camcheck.USER_TEXT
    if camcheck.is_ring_short(result) and "moved them" in result.get("reason", ""):
        return ("SteamVR moved the headset's tracking cameras after the camera broker started (the colour "
                "camera module was unplugged or plugged in), so the recorder can't read the side cameras. Close "
                "the Hand Recorder and open it again: it restarts the camera broker.")
    if camcheck.is_ring_short(result):
        return ("The headset's tracking cameras are all running, but the recorder can't read some of them (%s). "
                "Close the Hand Recorder, %s, and open it again. If that doesn't help, ask in the Frametop "
                "Discord." % (result.get("reason", ""), fix_hint("run %s again" % os.path.join(REPO, "hands", "rec", "install.sh"))))
    return ("Not all of the headset's tracking cameras are running (%s). Restart SteamVR, or restart the "
            "headset if that doesn't fix it." % result.get("reason", ""))


class Session:
    """One recording session. start() runs it in its own thread; next_step(), pause(), resume(),
    redo(), skip() and stop() steer it from any thread. on_status(dict) is called from the
    session thread whenever something changes (the keys of _status).

    auto=False is step mode: each step waits for next_step(), then a recorded countdown, then
    the hold; nothing is recorded while it waits. auto=True: the timed flow, each prompt
    advancing by itself, one recording per take. next_after (a test hook): press Next by itself
    after that many seconds of waiting. poses_dir: the pose pictures (poses/). button: read the
    headset's button (not in a dry run unless button_device, a test hook, names the device or
    a FIFO of input_event structs). check_cameras: run the camera check first and don't start
    if it fails (not in a dry run, nor with ring). hands_reader (a test hook): an object whose
    read() stands in for the hands file, read in a dry run too. quick: the quick round (a few
    sections, about 3 min). seed: the sweeps' shuffle (default: from the session's id)."""

    def __init__(self, base_dir, profile, checklist, lighting_choice, script_path, *, ring=None,
                 start_processes=True, dry_run=False, speed=1.0, on_status=None, hands_dir=None, panel_bin=None,
                 auto=False, next_after=None, poses_dir=None, button=True, button_device=None,
                 check_cameras=True, hands_reader=None, quick=False, seed=None, demos_dir=None,
                 recorded_demos_dir=None, guidance=None, bounty=None):
        self.base_dir = os.path.abspath(os.path.expanduser(base_dir or BASE_DIR))
        self.profile = dict(profile or {})
        self.checklist = dict(checklist or {})
        self.lighting_choice = lighting_choice or ""
        self.script_path = script_path or SCRIPT_PATH
        self.ring = ring
        self.start_processes = start_processes
        self.dry_run = dry_run
        self.speed = max(float(speed or 1.0), 0.01)
        self.on_status = on_status
        self.hands_dir = hands_dir or run_dir()
        self.panel_bin = panel_bin or PANEL_BIN
        self.auto = bool(auto)
        self.next_after = next_after
        self.button, self.button_device = bool(button), button_device
        self.check_cameras = bool(check_cameras)
        self.quick = bool(quick)
        self.seed = seed   # set from the session's id when it starts, unless given
        # the camera check (a test hook: tests swap it); a dry run looks at no real cameras
        self.camera_check_fn = (lambda: None) if dry_run else camera_check
        self.repair_cameras_fn = (lambda: None) if dry_run else (lambda: camera_check(repair=True))
        self._camera_losses = 0
        # Frametop's drawn hands and hand pointer, paused for the session (a test hook: tests empty it)
        self.quiet_units = () if dry_run or ring else QUIET_UNITS
        self._rec_watch = None   # (recorder, sets written, when that changed): _watch_recording
        self.input_devices = INPUT_DEVICES   # where mice are looked for (a test hook)
        self._button = None                  # the ButtonReader
        self.print = print   # where dry-run panel commands go (the CLI's stdout)
        self.script = load_script(self.script_path)
        # A debug session (debug.json): one long free step with Frametop's own hand tracking on,
        # its output copied beside the recording (livelog.py); the button marks what went wrong.
        self.debug = bool(self.script.get("debug"))
        if self.debug:
            self.quiet_units = ()
        self.consent_kind = takes.round_consent_kind(bool(bounty), self.debug)   # DESIGN.md "Testing"
        self.live_dir = self.hands_dir   # where Frametop's ft-hands publishes (a test hook)
        self._live_log = None      # the LiveLog of the recording part running
        # the person's voice beside it, if they ticked "Record my voice" (voicelog.py): what they
        # say about what went wrong becomes flags later, transcribed on the maintainer's machines
        # (not in a dry run: a rehearsal records nothing)
        self.voice = self.debug and bool(self.checklist.get("voice")) and not dry_run
        self._voice_log = None
        self._marks = 0            # marks asked for (any thread); _marked: those logged
        self._marked = 0
        self.record_hz = max(1.0, min(30.0, float(self.script.get("record_hz", RECORD_HZ))))
        # the board's entry this round is for (bounties.validate's, plus the board's "updated"), or None
        self.bounty = dict(bounty) if bounty else None
        self._scene_on = False     # a scene's windows are up (_place_scene)
        # the plan in the script's order; the session's own shuffle comes with its id (_make_dir)
        self.plan, self.skipped = build_plan(self.script, self.checklist, seed=seed, quick=self.quick)
        self._poses = load_poses(poses_dir or POSES_DIR)
        self._demos_dir = demos_dir or DEMOS_DIR
        self._recorded_demos_dir = recorded_demos_dir or RECORDED_DEMOS_DIR
        self.guidance = guidance if guidance in dict(GUIDANCE) else default_guidance(self._recorded_demos_dir)
        self.session_dir = ""
        self._thread = None
        self._lock = threading.Lock()
        self._want = {"pause": False, "skip": False, "stop": False, "next": False, "redo": False, "end": False}
        self._wake = threading.Event()
        # state: starting, intro, ready (a step waits for Next), countdown, running (a hold),
        # between, paused, done, stopped, error. waiting: Next is wanted. big: the countdown's
        # number, then the hold's word ("Hold", "Go"), as the panel shows them. The picture and the
        # diagram: image (a path or ""), image_mode ("", "mirror", "both"), position, distance.
        self._status = {"state": "starting", "mode": "auto" if self.auto else "step", "section": "", "title": "",
                        "section_index": 0, "section_count": len(self.plan), "step_index": 0, "step_count": 0,
                        "prompt": "", "seconds_left": 0.0, "note": "", "hands": {"left": None, "right": None},
                        "take": None, "error": "", "waiting": False, "countdown": 0, "big": "", "can_redo": False,
                        "image": "", "image_mode": "", "caption": "", "position": "", "distance": "",
                        "demo": "", "demo_mode": "", "demo_key": False, "demo_run": 0, "ready_text": READY_TEXT, "button": False, "mouse": True,
                        "camera": None, "nohands": False, "strip": [], "cue": -1, "quick": self.quick,
                        "debug": self.debug, "marks": 0}
        self._last_emit = 0.0
        self._log_file = None
        self._panel = None
        self._panel_proc = None
        self._units = []           # transient units this session started
        self._session_json = None
        self._take = None          # the take in progress: dict
        self._recorder = None
        self._prompt = None        # the prompt shown: dict (hands, controllers)
        self._live = None          # the hands file's last read
        self._hands_file = hands_reader or HandsFile(os.path.join(self.hands_dir, "hands"))
        self._read_hands = not dry_run or hands_reader is not None
        self._hold_watch = None    # the no-hands check's counts during a hold
        self._stop_note = ""       # why the session stopped, when it says more than "stopped"
        self._paused = False
        self._recording = False    # a recording part is running
        self._paused_recording = False
        self._waiting = False      # waiting for Next: no notes about lost hands
        self._redo_ok = False      # R does something now
        self._step_t0 = None       # the step's first event (ready or prompt), for R
        self._fb = {}              # feedback timers
        self._ring_path = None
        self._sides_read = 0.0     # when the side camera decision was last read

    # --- controls (any thread)
    def start(self):
        if self._thread:
            raise RuntimeError("the session has started already")
        self._thread = threading.Thread(target=self._run, name="handrec-session", daemon=True)
        self._thread.start()

    def _set(self, key, value):
        with self._lock:
            self._want[key] = value
        self._wake.set()

    def button_gesture(self, gesture):
        """The headset's button (ButtonGestures). A press: Next while a step waits, pause during
        a countdown or hold (and auto mode's timed screens), resume while paused. A double press:
        redo, as R. A hold: stop, as Esc. In a debug session's free step a press marks the
        moment and a hold ends the step (the session is done, not stopped)."""
        if self.debug and self._status["state"] == "running" and gesture in ("press", "hold"):
            if gesture == "press":
                self.mark()
            else:
                self.finish()
            self._log("headset button (%s)" % ("mark" if gesture == "press" else "finish"))
            return
        if gesture == "press":
            self.button_press()
        elif gesture == "double":
            if self._status["state"] in ("done", "stopped", "error") or not self._status.get("can_redo"):
                return
            self.redo()
            self._log("headset button (redo)")
        elif gesture == "hold":
            if self._status["state"] in ("done", "stopped", "error"):
                return
            self._log("headset button (stop)")
            self.stop(wait=0)

    def button_press(self):
        """The headset button's press: Next while a step waits, pause during a countdown or hold
        (and auto mode's timed screens), resume while paused."""
        with self._lock:
            paused = self._want["pause"]
        state = self._status["state"]
        if paused:
            self.resume()
        elif self._waiting:
            self.next_step()
        elif state in ("countdown", "running", "intro", "between"):
            self.pause()
        else:
            return
        self._log("headset button (%s)" % ("resume" if paused else "next" if self._waiting else "pause"))

    def next_step(self):
        """Step mode: start the step that's waiting (its countdown)."""
        self._set("next", True)

    def mark(self):
        """A debug session: something went wrong just now (a "mark" event in the take)."""
        with self._lock:
            self._marks += 1
        self._wake.set()

    def finish(self):
        """A debug session: end the free step there; the session is done."""
        self._set("end", True)

    def redo(self):
        """Record a step again: the one running, or at a step's ready screen the one before."""
        self._set("redo", True)

    def pause(self):
        self._set("pause", True)

    def resume(self):
        self._set("pause", False)

    def skip(self):
        self._set("skip", True)

    def stop(self, wait=20.0):
        """Stop. The take in progress is kept as far as it got. Blocks until the session has
        written its files (up to wait seconds), unless called from the session thread."""
        self._set("stop", True)
        if self._thread and threading.current_thread() is not self._thread and wait:
            self._thread.join(wait)

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout)

    @property
    def state(self):
        return self._status["state"]

    # --- status and logging
    def _emit(self, force=True, **changes):
        self._status.update(changes)
        now = time.monotonic()
        if not force and now - self._last_emit < 0.25:
            return
        self._last_emit = now
        if self.on_status:
            try:
                self.on_status(dict(self._status, hands=dict(self._status["hands"])))
            except Exception:
                self._log("on_status: " + traceback.format_exc())

    def _log(self, text):
        if self._log_file:
            self._log_file.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
            self._log_file.flush()

    def _event(self, event, **fields):
        """A line in the take's prompts.jsonl; returns its time."""
        t = mono_ns()
        if not self._take:
            return t
        line = {"t": t, "event": event}
        line.update(fields)
        self._take["prompts"].write(json.dumps(line) + "\n")
        self._take["prompts"].flush()
        return t

    # --- the run
    def _run(self):
        try:
            self._resume_quiet()   # left paused by a session that crashed
            if self._preflight():
                return
            self._make_dir()
            self._emit(state="starting")
            self._setup()
            self._screen("starting", self.script.get("welcome") or {})
            for i, s in enumerate(self.plan):
                self._section(i, s)
            self._finish("done")
        except _Stop:
            self._finish("stopped")
        except _Fail as e:
            self._finish("error", str(e))
        except Exception as e:
            self._log(traceback.format_exc())
            self._finish("error", "%s: %s" % (type(e).__name__, e))

    def _preflight(self):
        """The camera check, before anything is made or started (camcheck.py): with the upper
        cameras off a session records nothing useful. True if it stopped the session (state
        "error", no session folder)."""
        if self.debug and not (self.dry_run or self.ring) and not unit_active(LIVE_HANDS_UNIT):
            self._emit(state="error", error=DEBUG_NEEDS_HANDS, prompt=DEBUG_NEEDS_HANDS)
            return True
        if self.dry_run or self.ring or not self.check_cameras:
            return False
        r = self.camera_check_fn()
        if camcheck.is_ring_short(r):   # an ft-camd left on moved cameras: nothing records yet, so restart it
            r = self.repair_cameras_fn() or r
        self._status["camera"] = r
        text = camera_text(r)
        if not text:
            return False
        self._emit(state="error", error=text, prompt=text, camera=r)
        return True

    def _make_dir(self):
        sessions = os.path.join(self.base_dir, "sessions")
        os.makedirs(sessions, exist_ok=True)
        sid = time.strftime("%Y%m%d-%H%M%S")
        path, n = os.path.join(sessions, sid), 1
        while True:
            try:
                os.mkdir(path)
                break
            except FileExistsError:
                n += 1
                path = os.path.join(sessions, "%s-%d" % (sid, n))
        os.mkdir(os.path.join(path, "takes"))
        self.session_dir = path
        if self.seed is None:
            self.seed = session_seed(os.path.basename(path))
        # the same sections and steps, the sweeps shuffled for this session
        self.plan, self.skipped = build_plan(self.script, self.checklist, seed=self.seed, quick=self.quick)
        self._log_file = open(os.path.join(path, "session.log"), "a", buffering=1)
        self._log("session %s%s, script %s" % (os.path.basename(path), " (dry run)" if self.dry_run else "",
                                                self.script_path))
        for s in self.skipped:
            self._log("skipping %s: %s" % (s["section"], s["reason"]))
        cam = self._status.get("camera")
        if cam:
            self._log("camera check: %s" % cam.get("summary", cam.get("status")))
            for line in cam.get("evidence", []):
                self._log("  " + line)
        elif not self.check_cameras and not self.dry_run:
            self._log("camera check skipped (--ignore-cameras)")

    def _setup(self):
        ring_path = self.ring or os.path.join(self.hands_dir, "cam-ring")
        self._ring_path = ring_path
        if not self.dry_run:
            if not os.access(FT_HANDS, os.X_OK):
                raise _Fail("ft-hands isn't built: " + fix_hint("hands/build.sh"))
            self._ensure_ring(ring_path)
            if not tracker_running():
                if self.start_processes:
                    self._start_tracker()
                else:
                    self._log("no tracking ft-hands found: feedback only if one publishes")
            self._quiet_hands()
        lighting = ring_lighting(ring_path)
        cams = []
        try:
            ring = Ring(ring_path)
            cams = [{"name": c["name"], "width": c["width"], "height": c["height"]}
                    for c in ring.cams if not c["flags"] & FH_CAM_DARK and not c["name"].endswith("_dk")]
            ring.close()
        except (OSError, ValueError):
            pass
        removed = self._write_calibration() + self._write_device()
        self._session_json = {
            "schema": 1, "tool": takes.tool_version(), "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "contributor": self.profile.get("contributor", ""),
            "lighting": lighting_record(self.lighting_choice, lighting),
            "checklist": self.checklist,
            "device": {"steamos": _os_version(), "steamvr": _steamvr_version(), "cameras": cams},
            "calibration_removed": removed,
            "script": {"version": self.script.get("version"), "name": self.script.get("name", ""),
                       "sections": [s["id"] for s in self.plan],
                       "skipped": self.skipped},
            "mode": "auto" if self.auto else "step", "quick": self.quick, "guidance": self.guidance,
            "shuffle": {"seed": self.seed, "sweeps": plan_record(self.plan)},
            "record_hz": self.record_hz,
            "takes": [], "status": "recording", "sides": {"swapped": None},
            "consent": {"kind": self.consent_kind,
                        "version": (self.profile.get(takes.CONSENT_KEYS[self.consent_kind]) or {}).get("version", "")}}
        if self.bounty:
            self._session_json["bounty"] = {"id": self.bounty["id"], "title": self.bounty.get("title", ""),
                                            "board_updated": self.bounty.get("updated")}
        self._read_sides(force=True)
        cam = self._status.get("camera")
        if cam:
            self._session_json["camera"] = {"status": cam.get("status"), "reason": cam.get("reason", "")}
            # which device each calibrated camera was (XRService's numbering) and whether XRService
            # ran the side cameras through the ISP (no colour module): to check the names later
            self._session_json["device"]["camera_map"] = cam.get("map") or {}
            self._session_json["device"]["isp"] = (cam.get("episode") or {}).get("isp")
        if self.debug:
            # testing only, never training (the debug dataset's terms); what the live tracker was
            self._session_json["debug"] = {"use": "testing", "max_s": max(section_seconds(s) for s in self.plan),
                                           "settings": live_settings(),
                                           **(live_tracker() if not self.dry_run else {"frametop": "", "models": {}})}
        if self.dry_run:
            self._session_json["dry_run"] = True
        if self.speed != 1:
            self._session_json["speed"] = self.speed
        self._save_session()
        self._panel = Panel(dry=self.dry_run, log=self._log, out=self.print)
        if not self.dry_run:
            self._start_panel()
        self._start_button()
        self._panel.cmd("show")
        self._panel.cmd("paused off")
        for key, c in (("note", "note "), ("countdown", "countdown off"), ("hands", "hands off off"),
                       ("bar", "bar off"), ("target", "target off"), ("image", "image off"), ("where", "where off"),
                       ("demo", "demo off"), ("big", "big "), ("action", "action "), ("rec", "rec off"), ("strip", "strip off"), ("split", "split off")):
            self._panel.set(key, c)
        self._hints(action=False)

    def _start_button(self):
        """The headset button's reader, unless turned off (a dry run has none unless a test
        device is given). Without the device the session goes on: the window's keys work."""
        if not self.button or (self.dry_run and not self.button_device):
            return
        path = self.button_device or find_button(read_input_devices())
        if not path:
            self._log("headset button: no %s device with KEY_SELECT in %s" % (BUTTON_DEVICE, INPUT_DEVICES))
            return
        self._button = ButtonReader(path, self.button_gesture, log=self._log, double_s=BUTTON_DOUBLE_S,
                                    hold_s=BUTTON_HOLD_S, bounce_s=BUTTON_BOUNCE_S).start()
        end = time.monotonic() + 0.5   # opened in a moment, or it isn't reachable
        while not self._button.ok and time.monotonic() < end:
            time.sleep(0.01)

    def _write_calibration(self):
        src = host_path("/persist/xrservice.json")
        if not src:
            self._log("no /persist/xrservice.json: no calibration.json")
            return []
        with open(src) as f:
            clean, removed = strip_calibration(json.load(f))
        write_json(os.path.join(self.session_dir, "calibration.json"), clean)
        return removed

    def _write_device(self):
        """device.json: the rig's pose in the CAD frame from /persist/device_config.json, only
        cv.cad_from_cal (Cam0 in CAD) and head (the head in CAD), in the shape the labeller reads
        (frame-hands train/label, as its cut.py writes it). The rest of that file names the unit
        (serial number, EDID). Returns what was removed, as "device.json:<path>"."""
        src = host_path("/persist/device_config.json")
        if not src:
            self._log("no /persist/device_config.json: no device.json")
            return []
        try:
            with open(src) as f:
                dev = json.load(f)
            picked = {"cv": {"cad_from_cal": dev["cv"]["cad_from_cal"]}, "head": dev["head"]}
        except (OSError, ValueError, KeyError, TypeError) as e:
            self._log("device_config.json unreadable (%s): no device.json" % e)
            return []
        clean, removed = strip_calibration(picked)
        write_json(os.path.join(self.session_dir, "device.json"), clean)
        return ["device.json:" + r for r in removed]

    def _save_session(self):
        if self._session_json is not None:
            write_json(os.path.join(self.session_dir, "session.json"), self._session_json)

    def _read_sides(self, force=False):
        """The tracking ft-hands' side camera decision (sides.py, read_live) into session.json's
        "sides": {"swapped", "decided_by", "state", "evidence", "decided_at"}. Later recording
        parts are named right (Recorder's swap); parts before it are renamed when read. Without
        a tracking ft-hands it stays undecided ("swapped": null): export then leaves the names,
        and the maintainer's check (hub_review check) tells."""
        now = time.monotonic()
        if self._session_json is None or (not force and now - self._sides_read < SIDES_READ_S):
            return
        self._sides_read = now
        live = sides.read_live(os.path.join(self.hands_dir, "sides.json"), self._ring_path)
        if not live or live.get("swapped") is None:
            return
        cur = self._session_json.get("sides") or {}
        swapped = bool(live["swapped"])
        if cur.get("swapped") is not None and bool(cur["swapped"]) == swapped:
            if live.get("state") != cur.get("state"):   # e.g. decided -> confirmed
                cur["state"] = live.get("state")
                self._save_session()
            return
        new = {"swapped": swapped, "decided_by": live.get("decided_by"), "state": live.get("state"),
               "evidence": live.get("evidence"), "decided_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "decided_ns": mono_ns()}
        if cur.get("swapped") is not None:
            new["reversed_from"] = cur
        self._session_json["sides"] = new
        self._save_session()
        self._log("side cameras: %s (%s, %s)" % ("SWAPPED" if swapped else "as named", new["decided_by"],
                                                  json.dumps(new["evidence"])))
        if new["state"] == "forced, disagrees":
            self._log("side cameras: HANDS_SWAP_SIDES in ~/.config/frametop.conf forces names the hands say are "
                      "backwards; the recording goes by the hands. Set HANDS_SWAP_SIDES=auto.")

    def _sides_swapped(self):
        """session.json's decision: True, False, or None (not known yet)."""
        return (self._session_json or {}).get("sides", {}).get("swapped")

    def _ensure_ring(self, path):
        if ring_alive(path):
            return
        if self.ring:
            raise _Fail("No frames in %s (start ft-ringplay first)" % path)
        if not self.start_processes:
            raise _Fail("ft-camd isn't running (and --no-start)")
        try:
            if start_camd(path, self._log, lambda: self._want["stop"]):
                self._units.append(CAMD_UNIT)
        except RuntimeError as e:
            raise _Fail(str(e))
        if self._want["stop"]:
            raise _Stop()

    def _start_tracker(self):
        argv = [FT_HANDS, "--no-gestures", "--status", "0"]
        if not STANDALONE:   # built in Frametop's container: it runs there
            argv = [IN_BOX] + argv
        if self.ring:
            argv += ["--ring", self.ring]
        self._start_unit(HANDS_UNIT, "hand tracking for feedback", argv)

    def _start_unit(self, unit, what, argv):
        try:
            if start_unit(unit, what, argv, self._log):
                self._units.append(unit)
        except RuntimeError as e:
            raise _Fail(str(e))

    def _quiet_hands(self):
        """Frametop's drawn hands and hand pointer off until the session ends: the hand overlay
        covers what the person watches, and gestures would click or open Steam's menu."""
        if self.quiet_units:
            pause_units(self.quiet_units, os.path.join(self.hands_dir, PAUSED_UNITS_FILE), self._log)

    def _resume_quiet(self):
        if self.quiet_units:
            resume_units(os.path.join(self.hands_dir, PAUSED_UNITS_FILE), self._log)

    def _stop_units(self):
        for unit in reversed(self._units):
            stop_unit(unit)
            self._log("stopped %s" % unit)
        self._units = []

    def _start_panel(self):
        if self._panel.cmd("ping", reply=True):
            self._log("using the ft-handpanel that's running")
            return
        if not os.access(self.panel_bin, os.X_OK):
            raise _Fail("ft-handpanel isn't built: " + fix_hint("hands/rec/build.sh"))
        self._panel_proc = subprocess.Popen([self.panel_bin, "--watch-stdin"], stdin=subprocess.PIPE,
                                            stdout=self._log_file, stderr=self._log_file, env=openvr_env())
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if self._panel.cmd("ping", reply=True, timeout=0.2):
                return
            if self._panel_proc.poll() is not None:
                raise _Fail("ft-handpanel exited (is SteamVR running?): see session.log")
            time.sleep(0.1)
        raise _Fail("ft-handpanel doesn't answer")

    def _stop_panel(self):
        if self._panel:
            self._panel.cmd("hide")
        if self._panel_proc:
            try:
                self._panel_proc.stdin.close()
                self._panel_proc.wait(3)
            except (OSError, subprocess.TimeoutExpired):
                self._panel_proc.terminate()
                try:
                    self._panel_proc.wait(3)
                except subprocess.TimeoutExpired:
                    self._panel_proc.kill()
            self._panel_proc = None
        if self._panel:
            self._panel.close()

    def _finish(self, state, error=""):
        if self._take:
            self._end_take({"done": "complete", "stopped": "stopped"}.get(state, "stopped"))
        if self._session_json is not None:
            self._session_json["status"] = state
            self._session_json["ended"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            if error:
                self._session_json["error"] = error
            if self._stop_note and state == "stopped":
                self._session_json["stop_reason"] = self._stop_note
            self._save_session()
            if state == "done" and self.bounty and not self.dry_run:
                self._mark_done()
        self._log("%s%s" % (state, ": " + error if error else ""))
        if self._stop_note and state == "stopped":
            self._log(self._stop_note)
        self._stop_units()
        self._resume_quiet()
        if self._button:
            self._button.stop()
            self._button = None
        if self._panel:
            screen = {"done": self.script.get("done") or {}, "stopped": self.script.get("stopped") or {},
                      "error": {"title": "Something went wrong", "text": "See the Hand recorder window.",
                                "seconds": 4}}[state]
            try:
                self._panel.cmd("paused off")
                for key, c in (("big", "big "), ("action", "action "), ("rec", "rec off"), ("keys", "keys "),
                               ("bar", "bar off"), ("target", "target off"), ("strip", "strip off")):
                    self._panel.set(key, c)
                self._clear_scene()
                self._screen(state, screen, controls=False)
            except (_Stop, _Skip, _Fail):
                pass
        self._stop_panel()
        if self._log_file:
            self._log_file.close()
            self._log_file = None
        screen = {"done": self.script.get("done"), "stopped": self.script.get("stopped")}.get(state) or {}
        self._emit(state=state, error=error, seconds_left=0.0, note="", take=None, section="",
                   prompt=error or (self._stop_note if state == "stopped" else "") or screen.get("text", "").replace("|", "\n"),
                   hands={"left": None, "right": None}, nohands=False,
                   waiting=False, countdown=0, big="", can_redo=False, image="", image_mode="", caption="",
                   position="", distance="", strip=[], cue=-1, demo="", demo_mode="", demo_key=False)

    def _mark_done(self):
        """The bounty counted in profile.json, read again first (the window may have changed it)."""
        path = os.path.join(self.base_dir, "profile.json")
        try:
            with open(path) as f:
                profile = json.load(f)
        except (OSError, ValueError):
            return   # no consent on file: nothing to count it in
        bounties.mark_done(profile, self.bounty["id"], os.path.basename(self.session_dir))
        write_json(path, profile)
        self.profile = profile

    # --- the timing loop
    def _controls(self):
        """Handle a pause (blocking until resumed), skip, stop and redo (only where _redo_ok:
        else it's dropped). True if a pause happened."""
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
            if want["stop"]:
                outcome = _Stop
            elif want["skip"]:
                outcome = _Skip
            elif redo:
                outcome = _Redo
            elif not want["pause"]:
                outcome = True
        self._unpause(record=outcome is True)
        if outcome is not True:
            raise outcome()
        return True

    def _pause(self):
        self._paused = True
        self._state_before = self._status["state"]
        self._paused_recording = self._recording
        if self._recording:
            self._stop_recording()
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
        self._fb.clear()
        with self._lock:
            self._want["next"] = False   # a Next pressed while paused doesn't count
        if self._take and self._paused_recording and record:
            self._event("resume")
            self._start_recording()
        self._emit(state=self._state_before, note="")
        self._log("resumed")

    def _wait(self, seconds, tick=None, countdown=True):
        """Let `seconds` (script time) pass, handling the controls and feedback. tick(dt) runs
        every step with the script time passed; it ends the wait early by returning True.
        Returns True if tick ended it, else False."""
        left = float(seconds)
        last = time.monotonic()
        last_cd = -1.0
        while True:
            if self._controls():
                last = time.monotonic()
            now = time.monotonic()
            dt = (now - last) * self.speed
            last = now
            left -= dt
            self._feedback()
            if tick and tick(dt):
                return True
            if countdown and seconds > 0 and (now - last_cd >= 0.25 or left <= 0):
                last_cd = now
                self._panel.set("countdown", "countdown %.2f" % max(0.0, min(1.0, left / seconds)))
            self._emit(force=False, seconds_left=round(max(left, 0.0), 1))
            if left <= 0:
                return False
            self._wake.wait(TICK_S)
            self._wake.clear()

    # --- feedback
    def _feedback(self):
        now = time.monotonic()
        fb = self._fb
        if self._panel_proc and self._panel_proc.poll() is not None:
            raise _Fail("The headset panel closed (did SteamVR quit?)")
        if self._recorder and self._recorder.exited():
            raise _CamerasLost("The recording stopped by itself: see session.log")
        self._watch_recording(now)
        if not self.dry_run and now - fb.get("disk", 0) > 5:
            fb["disk"] = now
            if shutil.disk_usage(self.session_dir).free < MIN_FREE:
                raise _Fail("The disk is nearly full: the session stopped")
        if not self.dry_run:
            self._read_sides()
        live = self._hands_file.read() if self._read_hands else None
        self._live = live
        w = self._hold_watch
        if w is not None:
            w["reads"] += 1
            if live is not None:
                w["published"] += 1
                if live["left"] or live["right"]:
                    w["seen"] += 1
        p = self._prompt or {}
        asked = p.get("hands", "")
        # While a step waits for Next the chips show what's seen, with no notes yet.
        seen = {s: (bool(live[s]) if live else None) for s in ("left", "right")}
        # the chips: the asked-for hands, seen or lost, while the tracker publishes
        chips = []
        for side in ("left", "right"):
            show = live is not None and (asked in (side, "both", "any"))
            chips.append(("seen" if seen[side] else "lost") if show else "off")
        self._panel.set("hands", "hands %s %s" % tuple(chips))
        # a note when an asked-for hand stays lost, or a hand shows when none is wanted
        notes = []
        if live is not None and asked and not self._waiting:
            if asked == "none":
                missing = [] if not (seen["left"] or seen["right"]) else ["shown"]
            elif asked == "any":
                missing = [] if (seen["left"] or seen["right"]) else ["any"]
            else:
                missing = [s for s in ("left", "right") if asked in (s, "both") and not seen[s]]
            key = ",".join(missing)
            if key != fb.get("lost_key"):
                fb["lost_key"], fb["lost_since"] = key, now
            if missing and now - fb["lost_since"] > LOST_S:
                if missing == ["shown"]:
                    notes.append("I can see a hand: keep them out of view")
                elif missing == ["any"] or len(missing) == 2:
                    notes.append("I can't see your hands: bring them into view")
                else:
                    notes.append("I can't see your %s hand: bring it into view" % missing[0])
        else:
            fb.pop("lost_key", None)
        notes = ([] if self._waiting else self._controller_feedback(now, p)) + notes
        note = notes[0] if notes else ""
        if not self._paused:
            self._panel.set("note", "note " + note)
        hands = {"left": seen["left"], "right": seen["right"]}
        if note != self._status["note"] or hands != self._status["hands"]:
            self._emit(note=note, hands=hands)
        if self._take and self._recording and live is not None and now - fb.get("logged", 0) >= 0.5:
            fb["logged"] = now
            self._event("feedback", left=seen["left"], right=seen["right"],
                        palm_m=[round(live[s]["palm_m"], 4) if live[s] else None for s in ("left", "right")])

    def _controller_feedback(self, now, p):
        """Sections with controllers: `devices` once a second; a result other than 200 for
        more than a second gets a note, and each change goes into prompts.jsonl."""
        sides = p.get("controllers") or []
        fb = self._fb
        if not sides:
            fb.pop("ctl", None)
            return []
        ctl = fb.setdefault("ctl", {"polled": 0, "r": {}, "bad_since": {}, "lost": []})
        if now - ctl["polled"] >= 1.0:
            ctl["polled"] = now
            reply = self._panel.cmd("devices", reply=True, timeout=0.3) or ""
            tok = reply.split()
            vals = dict(zip(tok[1::2], tok[2::2]))
            if tok[:1] == ["ok"] and "hmd" in vals:
                ctl["r"] = {s: vals.get(s, "-") for s in ("left", "right")}
            for s in sides:
                r = ctl["r"].get(s)
                if r is None or r == "200":
                    ctl["bad_since"].pop(s, None)
                else:
                    ctl["bad_since"].setdefault(s, now)
        lost = sorted(s for s, t in ctl["bad_since"].items() if s in sides and now - t > CONTROLLER_LOST_S)
        if lost != ctl["lost"]:
            ctl["lost"] = lost
            self._event("feedback", controller={s: (None if ctl["r"].get(s) in (None, "-") else int(ctl["r"][s]))
                                                for s in ("left", "right")}, controller_lost=lost)
        notes = []
        for s in lost:
            if ctl["r"].get(s) == "-":
                notes.append("I can't find the %s controller: is it on?" % s)
            else:
                notes.append("The %s controller lost tracking: turn your palm slightly toward you" % s)
        return notes

    # --- sections and takes
    def _show(self, title=None, step=None, text=None):
        if title is not None:
            self._panel.set("title", "title " + clean_text(title))
        if step is not None:
            self._panel.set("step", "step " + clean_text(step))
        if text is not None:
            self._panel.set("text", "text " + clean_text(text))

    def _view(self, p=None):
        """The prompt's picture or demo clip and where-to diagram, on the panel and in the status
        (sent with the next _emit); none without a prompt. A demo clip takes the picture's place."""
        p = p or {}
        demo, demo_mode, demo_key = guided_demo_view(self.guidance, self._demos_dir, self._recorded_demos_dir, p)
        # a scene's act names its gesture in "pose": the windows show what to do, not a picture
        path, mode, caption = ("", "", "") if p.get("cues") or p.get("scene") or demo else pose_view(self._poses, p)
        self._panel.set("demo", "demo %s%s%s" % (demo, " " + demo_mode if demo_mode else "", "" if demo_key else " nokey")
                        if demo else "demo off")
        self._status.update(demo=demo, demo_mode=demo_mode, demo_key=demo_key)
        self._strip(p)
        pos, dist = p.get("position") or "", p.get("distance") or ""
        self._panel.set("image", "image %s%s" % (path, " " + mode if mode else "") if path else "image off")
        self._panel.set("where", "where %s %s" % (clean_text(pos) or "-", clean_text(dist) or "-")
                        if pos or dist else "where off")
        self._status.update(image=path, image_mode=mode, caption=caption, position=pos, distance=dist)

    def _cue_name(self, pose):
        return clean_text((self.script.get("cue_names") or {}).get(pose) or pose.replace("-", " ").capitalize())

    def _strip(self, p):
        """A sweep's strip of pictures, the cue's highlighted (panel "strip", status strip and
        cue), or none. One hand per picture, as both hands make the same shape: a left hand's
        flipped, the rest as drawn."""
        poses = list(dict.fromkeys(p.get("cues") or []))   # each once, in the step's order
        if not poses:
            self._panel.set("strip", "strip off")
            self._status.update(strip=[], cue=-1)
            return
        items = []
        for pose in poses:
            path, mode, _ = pose_view(self._poses, {"pose": pose, "hands": p.get("hands")})
            items.append({"image": path, "mode": "" if mode == "both" else mode, "label": self._cue_name(pose)})
        cue = poses.index(p["pose"]) if p.get("pose") in poses else -1
        self._panel.set("strip", "strip %d %s" % (cue, ";".join(
            "%s|%s|%s" % (e["image"] or "-", e["mode"] or "-", e["label"].replace("|", "/").replace(";", ","))
            for e in items)))
        self._status.update(strip=items, cue=cue)

    def _button_ok(self):
        return bool(self._button and self._button.ok)

    def _hints(self, action=True):
        """The Next hint (with action) and the key line for what's there now: the headset button
        leads when no mouse is connected. Looked at again for each step, so a mouse plugged in counts."""
        button = self._button_ok()
        mouse = mouse_connected(self.input_devices)
        text = (READY_BUTTON_MOUSE if mouse else READY_BUTTON) if button else READY_TEXT
        keys = (KEYS_AUTO_BUTTON if self.auto else KEYS_STEP_BUTTON) if button else (KEYS_AUTO if self.auto else KEYS_STEP)
        if action:
            self._panel.set("action", "action " + text)
        self._panel.set("keys", "keys " + keys)
        self._status.update(ready_text=text, button=button, mouse=mouse)

    def _await_next(self):
        """Step mode: wait for Next (or the next_after test hook), handling the controls and the
        hands chips. Nothing records meanwhile."""
        with self._lock:
            self._want["next"] = False   # one pressed during the hold doesn't skip this
        self._waiting = True
        self._hints()
        self._emit(waiting=True, seconds_left=0.0)
        t0 = time.monotonic()
        try:
            while True:
                self._controls()
                self._feedback()
                with self._lock:
                    go, self._want["next"] = self._want["next"], False
                if go or (self.next_after is not None and time.monotonic() - t0 >= self.next_after):
                    break
                self._wake.wait(TICK_S)
                self._wake.clear()
        finally:
            self._waiting = False
            self._panel.set("action", "action ")
        self._emit(waiting=False)

    def _screen(self, state, screen, controls=True):
        """A screen of its own (welcome, done): title, text, a few seconds, or in step mode
        until Next. controls=False: the session's end, which reports its state once all is done."""
        if not screen:
            return
        self._prompt = None
        self._show(screen.get("title", ""), "", screen.get("text", ""))
        self._view()
        self._panel.set("countdown", "countdown off")
        if controls:
            self._emit(state=state, title=screen.get("title", ""), prompt=screen.get("text", "").replace("|", "\n"),
                       section="", seconds_left=float(screen.get("seconds", 0)))
            try:
                if self.auto:
                    self._wait(screen.get("seconds", 0), countdown=False)
                else:
                    self._await_next()
            except _Skip:
                pass
        else:
            end = time.monotonic() + screen.get("seconds", 0) / self.speed
            while time.monotonic() < end and not self._want["stop"]:
                time.sleep(TICK_S)

    def _section(self, i, s):
        step = "Section %d of %d" % (i + 1, len(self.plan))
        status = {"section": s["id"], "title": s["title"], "section_index": i + 1, "section_count": len(self.plan),
                  "step_index": 0, "step_count": section_steps(s) if not self.auto else 0, "can_redo": False}
        try:
            before = s.get("before")
            self._prompt = None
            self._view()
            if self.auto:
                if before or i > 0:
                    text = before["text"] if before else "Next: %s" % s["title"]
                    secs = before.get("seconds", 10) if before else self.script.get("between_s", 3)
                    self._show("Get ready" if before else s["title"], step, text)
                    self._emit(state="between", prompt=text.replace("|", "\n"), take=None, **status)
                    self._wait(secs)
                self._start_take(i, s)
                self._status.update(state="intro", take=self._take["id"], **status)   # sent with the intro
                self._run_prompt(s, {"id": s["id"] + "/intro", "text": s.get("intro", ""), "seconds": s["intro_s"],
                                     "hands": "", "pose": "", "distance": "", "position": "", "object": "",
                                     "controller": False, "controllers": []}, step, intro=True)
            else:
                # One screen before the section: what to get ready, and the intro. The take
                # starts with the first step's countdown, so a section skipped here leaves none.
                text = "|".join(t for t in ((before or {}).get("text", ""), s.get("intro", "")) if t)
                self._show(s["title"], step, text)
                self._panel.set("countdown", "countdown off")
                self._emit(state="intro", prompt=text.replace("|", "\n"), take=None, seconds_left=0.0, **status)
                self._await_next()
            self._status["state"] = "running"   # sent with the first prompt
            self._steps(i, s, step)
            if self._take:
                self._end_take("complete")
        except _Skip:
            self._log("skipped %s" % s["id"])
            if self._take:
                self._end_take("skipped")
        except _Stop:
            raise
        finally:
            self._redo_ok = False
            self._status["big"] = ""
            for key, c in (("bar", "bar off"), ("target", "target off"), ("big", "big "), ("action", "action ")):
                self._panel.set(key, c)
            self._clear_scene()

    def _start_take(self, i, s, record=True):
        n = len(self._session_json["takes"]) + 1
        take_id = "%02d-%s" % (n, s["id"])
        d = os.path.join(self.session_dir, "takes", take_id)
        os.makedirs(d)
        self._take = {"id": take_id, "dir": d, "section": s, "part": 0,
                      "prompts": open(os.path.join(d, "prompts.jsonl"), "a", buffering=1),
                      "json": {"section": s["id"], "title": s["title"], "started_ns": mono_ns(), "ended_ns": None,
                               "status": "stopped", "deleted": [], "notes": ""}}
        write_json(os.path.join(d, "take.json"), self._take["json"])
        self._session_json["takes"].append(take_id)
        self._save_session()
        self._event("take", section=s["id"], take=take_id)
        if record:
            self._start_recording()
        self._log("take %s" % take_id)
        self._emit(force=False, take=take_id)

    def _start_recording(self):
        t = self._take
        t["part"] += 1
        t["json"].setdefault("clock", []).append(clock_sample())
        if not self.dry_run:
            # a safety net only: the session ends the recording itself
            remaining = section_seconds(t["section"], worst=True, auto=self.auto) / self.speed
            self._recorder = Recorder(t["dir"], t["part"], remaining * 1.5 + 60, self.ring, self._log_file,
                                      swap=self._sides_swapped(), hz=self.record_hz)
        if self.debug:
            name = "live.bin" if t["part"] == 1 else "live-%d.bin" % t["part"]
            self._live_log = livelog.LiveLog(self.live_dir, os.path.join(t["dir"], name)).start()
            self._live_log.name = name
        if self.voice:
            name = "voice.bin" if t["part"] == 1 else "voice-%d.bin" % t["part"]
            self._voice_log = voicelog.VoiceLog(os.path.join(t["dir"], name)).start()
            self._voice_log.name = name
            if self._voice_log.error:
                self._log("voice: %s" % self._voice_log.error)
        self._recording = True
        self._panel.cmd("poses start " + os.path.join(t["dir"], "poses.jsonl"))
        self._panel.set("rec", "rec on")

    def _stop_recording(self):
        if not self._recording:
            return
        self._recording = False
        self._panel.set("rec", "rec off")
        self._panel.cmd("poses stop", reply=not self.dry_run, timeout=1.0)
        if self._live_log:
            log, self._live_log = self._live_log, None
            counts = log.stop()
            self._log("live log %s: %d hands, %d gestures" % (log.name, counts["hands"], counts["gestures"]))
            if self._take:
                self._take["json"].setdefault("live", {})[log.name] = counts
                write_json(os.path.join(self._take["dir"], "take.json"), self._take["json"])
        if self._voice_log:
            log, self._voice_log = self._voice_log, None
            counts = log.stop()
            self._log("voice %s: %.1f s, peak %.3f%s" % (log.name, counts["seconds"], counts["peak"],
                                                       ", " + counts["error"] if counts["error"] else ""))
            if self._take:
                self._take["json"].setdefault("voice", {})[log.name] = counts
                write_json(os.path.join(self._take["dir"], "take.json"), self._take["json"])
        if self._recorder:
            rec, self._recorder = self._recorder, None
            code = rec.stop()
            self._log("recording part %d ended (%s)" % (rec.part, code))
            if self._take:   # how this part's side cameras are named (sides.py)
                self._take["json"].setdefault("parts", {})[rec.file] = {"names_swapped": rec.names_swapped}
                self._take["json"].setdefault("clock", []).append(clock_sample())
                write_json(os.path.join(self._take["dir"], "take.json"), self._take["json"])

    def _end_take(self, status):
        t = self._take
        try:
            self._stop_recording()
        finally:
            self._event("end", status=status)
            self._take = None
            t["prompts"].close()
            t["json"]["ended_ns"] = mono_ns()
            t["json"]["status"] = status
            write_json(os.path.join(t["dir"], "take.json"), t["json"])
            self._log("take %s %s" % (t["id"], status))

    def _prompt_event(self, p):
        extra = {"controllers": p["controllers"]} if p["controllers"] else {}
        if p.get("demo"):
            extra["demo"] = p["demo"]
        if p.get("cue"):   # a sweep's cue: the pose highlighted from now, within the step
            extra.update(cue=True, step=p["step"])
        if p.get("act"):   # a scene's act: what's lit (and the ghost's pose in the room) from now
            extra["act"] = p["act"]
        if p.get("switch") and not p.get("cue"):   # the step's pose changes, each logged as a "switch" when it comes
            extra["switch"] = p["switch"]
        return self._event("prompt", id=p["id"], text=p["text"], hands=p["hands"], pose=p["pose"],
                           distance=p["distance"], position=p["position"], object=p["object"],
                           controller=bool(p["controller"]), **extra)

    def _begin_prompt(self, s, p, step, state="running"):
        self._prompt = p
        self._fb.pop("lost_key", None)
        self._show(s["title"], step, p["text"])
        self._view(p)
        if state == "running" and self._status["demo"]:
            # The hold starts: the clip from its start, so it can be followed (the window's too:
            # demo_run counts the starts).
            self._panel.cmd("demo restart")
            self._status["demo_run"] += 1
        t = self._prompt_event(p)
        if self._step_t0 is None:
            self._step_t0 = t
        self._emit(state=state, prompt=p["text"].replace("|", "\n"), seconds_left=float(p["seconds"]), countdown=0)
        self._log("  %s" % p["id"])

    def _run_prompt(self, s, p, step, intro=False):
        if intro and not p["text"]:
            return
        self._begin_prompt(s, p, step, state="intro" if intro else "running")
        if self.debug and not intro:
            self._free(p)
            return
        self._wait(p["seconds"], None if intro else self._switches(p))

    def _free(self, p):
        """A debug session's free step: the panel out of the way while the person uses Frametop
        as usual, up to the step's seconds. The button's press is a "mark" (the panel flashes
        "Marked"), its hold (or finish()) ends the step early, with an "end_early" event."""
        with self._lock:
            self._want["end"] = False
            self._marks = self._marked
        self._panel.cmd("hide")
        flash = [0.0]

        def tick(dt):
            with self._lock:
                marks, end = self._marks, self._want["end"]
            while self._marked < marks:
                self._marked += 1
                self._event("mark", id=p["id"], n=self._marked)
                self._log("    mark %d" % self._marked)
                self._panel.set("big", "big Marked")
                self._panel.cmd("show")
                flash[0] = MARK_FLASH_S
                self._emit(marks=self._marked)
            if flash[0] > 0:
                flash[0] -= dt / self.speed
                if flash[0] <= 0:
                    self._panel.cmd("hide")
                    self._panel.set("big", "big ")
            if end:
                self._event("end_early", id=p["id"])
                self._log("    finished early")
            return end
        try:
            self._wait(p["seconds"], tick)
        finally:
            with self._lock:
                self._want["end"] = False
            self._panel.set("big", "big ")
            self._panel.cmd("show")

    def _switches(self, p, tick=None):
        """A step's pose changes: the panel's ticks on the time-left bar ("Switch to ..."), and
        a "switch" event as each one's time comes, so the labels say which pose when. Returns
        the tick for _wait (tick, if given, runs after)."""
        sw, seconds = p.get("switch") or [], float(p["seconds"])
        if not sw or seconds <= 0:
            self._panel.set("split", "split off")
            return tick
        self._panel.set("split", "split " + ";".join("%.3f %s" % (1 - w["at"], clean_text(w["to"]).replace(";", ","))
                                                     for w in sw))
        state = {"t": 0.0, "k": 0}

        def both(dt):
            state["t"] += dt
            while state["k"] < len(sw) and state["t"] >= sw[state["k"]]["at"] * seconds - 1e-9:
                w = sw[state["k"]]
                state["k"] += 1
                self._event("switch", id=p["id"], to=w["to"], k=state["k"])
                self._log("    switch to %s" % w["to"])
                if w["text"] and not p.get("scene"):
                    self._panel.set("text", "text " + clean_text(w["text"]))
                    self._status["prompt"] = w["text"].replace("|", "\n")
                    self._emit()
            return tick(dt) if tick else False
        return both

    # --- steps: a section's prompts, bar heights and targets
    def _step_list(self, s):
        """[{"kind": "prompt"|"sweep"|"bar"|"target", "p": prompt, "ready": waits for Next in step mode, ...}]."""
        out = []
        defaults = s.get("defaults") or {}
        if s["kind"] == "targets":
            for k, pt in enumerate(s["targets"]):
                p = {"id": "%s/%d" % (s["id"], k + 1), "text": s.get("text", ""), "seconds": float(s.get("timeout_s", 8)),
                     "hands": defaults.get("hands", "any"), "pose": defaults.get("pose", "point"), "distance": "",
                     "position": "", "object": "", "controller": False, "controllers": []}
                out.append({"kind": "target", "p": p, "pt": pt, "ready": k == 0})
        elif s["kind"] == "bar":
            lead = float(s.get("lead_s", 3)) if self.auto else 0.0   # step mode: the countdown shows the bar at near
            for h in s["heights"]:
                p = {"id": "%s/%s" % (s["id"], h["id"]), "text": h["text"], "hands": defaults.get("hands", "both"),
                     "pose": defaults.get("pose", "open"), "distance": "", "position": h["id"], "object": "",
                     "picture": h.get("picture", defaults.get("picture", "")),
                     "controller": bool(defaults.get("controller", False)),
                     "controllers": list(defaults.get("controllers") or []),
                     "seconds": lead + h["reps"] * float(s.get("period_s", 6))}
                out.append({"kind": "bar", "p": p, "h": h, "lead": lead, "ready": True})
        out += [{"kind": "scene" if p.get("scene") else "sweep" if p.get("cues") else "prompt", "p": p, "ready": True}
                for p in s["prompts"]]
        return out

    def _steps(self, i, s, label):
        """Run a section's steps. Step mode: each that's "ready" (and each to do again) shows
        first and waits for Next; then the recorded countdown; the recording stops after a
        step unless the next one follows straight on (the targets after the first). R: the
        step running starts again; at a ready screen, the step before goes again. Either way
        the range done before is marked with a "redo" event."""
        steps = self._step_list(s)
        done = []      # the steps finished in this take: (index, id, from_ns, to_ns), for R
        again = set()  # steps to do again: they wait for Next too
        retry = False  # after the no-hands stop: Next was pressed there, go straight to the countdown
        k = 0
        while k < len(steps):
            st, p = steps[k], steps[k]["p"]
            where = label if self.auto else "%s · step %d of %d" % (label, k + 1, len(steps))
            self._status.update(step_index=k + 1, step_count=len(steps))
            started, self._step_t0 = None, None
            try:
                if not self.auto and (st["ready"] or k in again):
                    self._redo_ok = bool(done)
                    self._ready(s, st, where, can_redo=bool(done))
                    if not retry:
                        self._await_next()
                    retry = False
                    self._redo_ok = True
                    started = self._countdown(i, s, st)
                else:
                    retry = False
                    self._redo_ok = True
                    self._emit(force=False, can_redo=True)
                check = self._read_hands and s["id"] == HANDS_CHECK_SECTION and k == 0 and st["kind"] in ("prompt", "sweep")
                self._hold_watch = {"reads": 0, "published": 0, "seen": 0} if check else None
                try:
                    self._run_step(s, st, where)
                finally:
                    watch, self._hold_watch = self._hold_watch, None
                if check and self._no_hands_seen(p, watch):
                    self._no_hands(s, st, where, started if started is not None else self._step_t0, watch)
                    again.add(k)
                    retry = True
                    continue
                done.append((k, p["id"], started if started is not None else self._step_t0, mono_ns()))
                if not self.auto and (k + 1 == len(steps) or steps[k + 1]["ready"] or k + 1 in again):
                    self._hold_end()
                again.discard(k)
                k += 1
            except _CamerasLost as e:
                if self.auto:
                    raise
                from_ns = started if started is not None else self._step_t0
                self._hold_end()
                if from_ns is not None:
                    self._event("redo", id=p["id"], **{"from": from_ns, "to": mono_ns()})
                self._cameras_back(p, str(e), where)
                again.add(k)
                retry = False
            except _Redo:
                self._redo_ok = False
                from_ns = started if started is not None else self._step_t0
                self._panel.set("target", "target off")
                self._panel.set("bar", "bar off")
                if from_ns is not None:   # R during the step: it starts again
                    self._event("redo", id=p["id"], **{"from": from_ns, "to": mono_ns()})
                    self._log("    redo %s" % p["id"])
                    if not self.auto:
                        self._hold_end()
                    elif self._take and not self._recording:   # R ended a pause: record again
                        self._event("resume")
                        self._start_recording()
                    again.add(k)
                elif done:                # R at its ready screen: the step before goes again
                    k, pid, a, b = done.pop()
                    self._event("redo", id=pid, **{"from": a, "to": b})
                    self._log("    redo %s" % pid)
                    again.add(k)

    def _watch_recording(self, now):
        """A recording that has started writing sets but writes no new one for STALL_S is lost
        (_CamerasLost): the cameras stopped or moved, and the rest of the hold would be empty."""
        rec = self._recorder
        if rec is None or self.dry_run or not hasattr(rec, "sets_written"):
            self._rec_watch = None
            return
        n = rec.sets_written()
        w = self._rec_watch
        if w is None or w[0] is not rec or n != w[1]:
            self._rec_watch = (rec, n, now)
        elif n > 0 and now - w[2] > STALL_S:
            self._rec_watch = None
            raise _CamerasLost("The recording got no new frames for %.0f s (after %d sets)" % (now - w[2], n))

    def _cameras_back(self, p, why, where):
        """A step's recording was lost (_CamerasLost): its try is marked redone. Wait for the
        camera check to pass, restarting a camera broker left on moved cameras (camera_check's
        repair), then the step shows again. _Fail if they don't come back, or too often."""
        self._camera_losses += 1
        self._log("    %s: %s (%d this session); waiting for the cameras" % (p["id"], why, self._camera_losses))
        if self._camera_losses > MAX_CAMERA_LOSSES:
            raise _Fail("The recording kept stopping (%d times): %s" % (self._camera_losses, why))
        self._prompt = None
        self._view()
        self._show(CAMERAS_TITLE, where, CAMERAS_TEXT)
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("note", "note ")):
            self._panel.set(key, c)
        self._emit(state="between", prompt=CAMERAS_TEXT, big="", countdown=0, seconds_left=0.0, can_redo=False)
        if self.ring:
            return
        end, r = time.monotonic() + CAMERAS_BACK_S / self.speed, None
        while time.monotonic() < end:
            if self._want["stop"]:
                raise _Stop()
            r = self.repair_cameras_fn()
            if r is None:   # a dry run: nothing to check
                return
            if r.get("status") == "ok":
                self._log("    cameras back: %s" % r.get("summary", "ok"))
                return
            self._wake.wait(2.0 / self.speed)
            self._wake.clear()
        summary = (r or {}).get("summary", "not run")
        for line in (r or {}).get("evidence", []):
            self._log("      " + line)
        raise _Fail("The recording stopped and the cameras didn't come back (camera check: %s). %s"
                    % (summary, camera_text(r) or why))

    def _no_hands_seen(self, p, w):
        """The no-hands check's verdict on a hold (logged either way): True if the tracker
        published through it and never saw a hand."""
        enough = w["reads"] >= HANDS_CHECK_MIN_READS and w["published"] >= HANDS_CHECK_PUBLISHED * w["reads"]
        if not enough:
            self._log("    hands check %s: the tracker published in %d of %d reads: can't tell"
                      % (p["id"], w["published"], w["reads"]))
            return False
        self._log("    hands check %s: a hand in %d of %d reads (%d published)"
                  % (p["id"], w["seen"], w["reads"], w["published"]))
        return w["seen"] == 0

    def _no_hands(self, s, st, where, from_ns, w):
        """The first step saw no hands at all: stop it (its range marked as redone, so it gets
        no labels), run the camera check, say so, and wait. Next or R tries the step again; Stop
        (Esc) ends the session with the camera check's result; S skips the section."""
        p = st["p"]
        if not self.auto:
            self._hold_end()
        elif self._recording:
            self._stop_recording()
            self._event("pause")
        self._event("nohands", id=p["id"], reads=w["reads"], published=w["published"])
        if from_ns is not None:
            self._event("redo", id=p["id"], **{"from": from_ns, "to": mono_ns()})
        cam = self.camera_check_fn() if self.check_cameras else None
        cam_text = camera_text(cam)
        summary = (cam or {}).get("summary", "not run")
        self._log("    no hands seen in %s: asking to try again or stop; camera check: %s" % (p["id"], summary))
        for line in (cam or {}).get("evidence", []):
            self._log("      " + line)
        text = "%s|%s|%s" % (NO_HANDS_TEXT, cam_text or "The camera check found nothing wrong (%s)." % summary,
                             NO_HANDS_RETRY_BUTTON if self._button_ok() else NO_HANDS_RETRY)
        self._stop_note = "Stopped: no hands were seen in the first step. Camera check: %s." % summary
        if cam_text:
            self._stop_note += " " + cam_text
        self._prompt = None
        self._view()
        self._show(NO_HANDS_TITLE, where, text)
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off"),
                       ("note", "note "), ("hands", "hands off off")):
            self._panel.set(key, c)
        self._emit(state="nohands", prompt=text.replace("|", "\n"), camera=cam, nohands=True, can_redo=True,
                   big="", countdown=0, seconds_left=0.0)
        self._redo_ok = True
        try:
            self._await_next()
        except _Redo:
            pass   # R here is the same as Next: try again
        except _Skip:
            self._stop_note = ""
            self._emit(nohands=False)
            raise
        self._stop_note = ""
        self._log("    trying %s again" % p["id"])
        self._emit(nohands=False)
        if self.auto and self._take and not self._recording:
            self._event("resume")
            self._start_recording()

    def _ready(self, s, st, where, can_redo):
        """Step mode: show the step (text, picture, diagram) with "Ready?"."""
        p = st["p"]
        self._prompt = p   # the chips show which hands are seen while the person gets ready
        self._fb.pop("lost_key", None)
        self._show(s["title"], where, p["text"])
        self._view(p)
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off"),
                       ("split", "split off")):
            self._panel.set(key, c)
        self._clear_scene()
        self._emit(state="ready", prompt=p["text"].replace("|", "\n"), seconds_left=float(p["seconds"]),
                   can_redo=can_redo, countdown=0, big="")

    def _countdown(self, i, s, st):
        """Step mode: start recording and count 3-2-1 (logged as a "ready" event, so labels
        cover only the hold), then make sure the recording has its first set. Returns the
        ready event's time."""
        p = st["p"]
        if not self._take:
            self._start_take(i, s, record=False)
        t = self._event("ready", id=p["id"], seconds=COUNTDOWN_S)
        self._step_t0 = t
        self._start_recording()
        tick = None
        if st["kind"] == "scene":   # the windows go up for the countdown, the panel to its band
            self._place_scene(p)
            self._panel.set("text", "text " + clean_text(p.get("say") or ""))
        if st["kind"] == "bar":   # the bar at near, where the sweep starts
            labels = "%s|%s" % (s.get("near_label", "Near"), s.get("far_label", "Far"))
            near, far = float(s.get("near_m", 0.2)), float(s.get("far_m", 0.6))

            def tick(dt):
                self._panel.set("bar", "bar 0.000 %.3f %s" % (self._palm_share(near, far), labels))
                return False
        for n in range(COUNTDOWN_S, 0, -1):
            self._panel.set("big", "big %d" % n)
            self._emit(state="countdown", countdown=n, big=str(n), can_redo=True)
            self._wait(1.0, tick, countdown=False)
        self._first_set()
        self._panel.set("big", "big " + s.get("go", "Go"))
        self._status["big"] = s.get("go", "Go")   # sent with the prompt
        return t

    def _first_set(self):
        """The hold starts once its recording has a set. ft-hands --record-only writes its first
        within about 30 ms of starting, so the countdown covers it; this is a safety net."""
        rec = self._recorder
        if not rec:
            return
        end = time.monotonic() + FIRST_SET_S
        while not rec.has_data():
            if rec.exited():
                raise _CamerasLost("The recording stopped by itself: see session.log")
            if self._want["stop"]:
                raise _Stop()
            if time.monotonic() > end:
                self._log("recording part %d: no set yet after the countdown" % rec.part)
                return
            time.sleep(0.02)
        # Without the side pair a recording is no use for training (2026-10-12: XRService moved
        # the side cameras while ft-camd kept publishing the old devices, and ft-hands left them
        # out of a whole session). Stop at once rather than record the rest of the session.
        cams = rec.cameras()
        missing = [c for c in SIDE_CAMERAS if c not in cams]
        if cams and missing:
            self._log("recording part %d has only %s" % (rec.part, " ".join(cams)))
            raise _CamerasLost("The recording is missing the side cameras (%s), so it can't be used. Close the "
                               "Hand Recorder and open it again: it checks the cameras and restarts the camera "
                               "broker if it needs to." % " ".join(missing))

    def _hold_end(self):
        """Step mode: the step is over. A "wait" event ends its labels and the recording stops
        until the next countdown."""
        self._event("wait")
        self._stop_recording()
        self._status["big"] = ""
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off"),
                       ("split", "split off")):
            self._panel.set(key, c)
        self._clear_scene()

    def _run_step(self, s, st, where):
        if st["kind"] != "bar":
            self._panel.set("bar", "bar off")
        if st["kind"] == "target":
            self._target(s, st, where)
        elif st["kind"] == "bar":
            self._bar(s, st, where)
        elif st["kind"] == "sweep":
            self._sweep(s, st, where)
        elif st["kind"] == "scene":
            self._scene(s, st, where)
        else:
            self._run_prompt(s, st["p"], where)

    def _sweep(self, s, st, where):
        """A sweep: the strip's pictures highlighted in turn, one every cue_s, while the hands
        keep moving. Each cue is a prompt event of its own, with that pose, "cue": true and the
        step's id, so the timeline tags each pose roughly; the time-left bar covers the step."""
        p = st["p"]
        cues, cue_s = p["cues"], p["cue_s"]
        seen = {}

        def cue_prompt(j):
            pose = cues[j]
            seen[pose] = seen.get(pose, 0) + 1
            return dict(p, id="%s/%s%s" % (p["id"], pose, "#%d" % seen[pose] if seen[pose] > 1 else ""),
                        pose=pose, cue=True, step=p["id"])

        self._begin_prompt(s, cue_prompt(0), where)
        state = {"t": 0.0, "j": 0}

        def tick(dt):
            state["t"] += dt
            j = min(len(cues) - 1, int(state["t"] / cue_s + 1e-9))
            if j != state["j"]:
                state["j"] = j
                cp = cue_prompt(j)
                self._prompt = cp
                self._strip(cp)
                self._prompt_event(cp)
                self._emit()
                self._log("    cue %s" % cp["id"])
            return False

        self._wait(p["seconds"], tick)

    def _place_scene(self, p):
        """A scene step's windows into the room, where the head faces now (levelled), and the
        panel to its band along the top. A "scene" event logs each window's pose in the room."""
        self._panel.cmd("win clear")
        self._panel.set("layout", "layout top")
        placed = []
        for w in p["scene"]["windows"]:
            reply = self._panel.cmd("win %s place %.3f %.3f %.3f %.3f %.3f %.1f %.1f %s" % (
                w["id"], *w["at"], *w["size"], *w["turn"], w["look"]), reply=True) or ""
            placed.append(dict(w, room=_pose_reply(reply)))
        self._scene_on = True
        self._event("scene", id=p["id"], windows=placed)

    def _clear_scene(self):
        if self._scene_on:
            self._scene_on = False
            self._panel.cmd("win clear")
            self._event("scene", windows=[])
        self._panel.set("layout", "layout full")

    def _scene(self, s, st, where):
        """A scene step: its acts in turn, each lighting a window's part (and showing a ghost
        where to take it) for its seconds. Each act is a prompt event of its own, with "cue": true,
        the step's id and "act" (window, part, gesture, hand, the ghost's pose in the room), so the
        timeline says what was asked where and when; the time-left bar covers the step."""
        p = st["p"]
        acts = p["scene"]["acts"]
        if not self._scene_on:   # auto mode: no countdown put them up
            self._place_scene(p)
        for a in p["scene"]["steady"]:
            self._panel.cmd(_lit_cmd(a))
            self._event("steady", win=a["win"], lit=a["lit"], gesture=a["gesture"], hand=a["hand"])
        state = {"t": 0.0, "j": -1, "prev": None}
        ends = []
        for a in acts:
            ends.append((ends[-1] if ends else 0.0) + a["s"])

        def act(j):
            a, prev = acts[j], state["prev"]
            if prev and (prev["win"] != a["win"] or a["lit"] == "none"):
                self._panel.cmd("win %s lit none" % prev["win"])
            if a["lit"] != "none":
                self._panel.cmd(_lit_cmd(a))
            ghost = None
            if a["ghost"]:
                ghost = _pose_reply(self._panel.cmd("win %s ghost %s" % (a["win"], " ".join("%g" % v for v in a["ghost"])),
                                                    reply=True) or "")
            elif prev and prev["ghost"]:
                self._panel.cmd("win %s ghost off" % prev["win"])
            state["prev"] = a
            info = {k: a[k] for k in ("win", "lit", "gesture", "hand", "label")}
            if a["ghost"]:
                info.update(ghost=a["ghost"], ghost_room=ghost)
            return dict(p, id="%s/%d-%s" % (p["id"], j + 1, a["gesture"]), text=a["say"] or p.get("say", ""),
                        pose=a["gesture"], cue=True, step=p["id"], act=info, seconds=a["s"])

        def tick(dt):
            state["t"] += dt
            j = min(len(acts) - 1, next((k for k, e in enumerate(ends) if state["t"] < e - 1e-9), len(acts) - 1))
            while state["j"] < j:   # each act in turn, even when a slow tick passed a short one
                state["j"] += 1
                ap = act(state["j"])
                self._prompt = ap
                self._prompt_event(ap)
                self._log("    act %s" % ap["id"])
                if state["j"] == j:
                    self._panel.set("text", "text " + clean_text(ap["text"]))
                    self._status["prompt"] = ap["text"]
                    self._emit()
            return False

        state["j"] = 0
        first = act(0)
        self._begin_prompt(s, dict(first, seconds=p["seconds"]), where)
        self._panel.set("text", "text " + clean_text(first["text"]))
        self._wait(p["seconds"], self._switches(p, tick))
        if self.auto:
            self._clear_scene()

    def _target(self, s, st, where):
        hold_s, timeout_s = float(s.get("hold_s", 1.0)), float(s.get("timeout_s", 8))
        p, pt = st["p"], st["pt"]
        self._begin_prompt(s, p, where)
        xyz = "%.3f %.3f %.3f" % tuple(pt)
        reply = self._panel.cmd("target %s show" % xyz, reply=True) or ""
        self._panel.sent["target"] = "target %s show" % xyz
        tok = reply.split()
        room = [float(v) for v in tok[1:4]] if tok[:1] == ["ok"] and len(tok) >= 4 else None
        target = {"id": p["id"], "head": list(pt), "room": room}
        self._event("target", state="show", **target)
        state = {"hold": 0.0, "sent": 0.0, "holding": False}

        def tick(dt):
            d = self._tip_distance(pt, room)
            if d is not None and d <= TOUCH_M:
                if not state["holding"]:
                    state["holding"] = True
                    self._event("target", state="hold", **target)
                state["hold"] += dt
            elif state["holding"]:
                state["holding"], state["hold"] = False, 0.0
                self._panel.set("target", "target %s show" % xyz)
            if state["holding"]:
                frac = min(1.0, state["hold"] / hold_s)
                if time.monotonic() - state["sent"] >= 0.1 or frac >= 1:
                    state["sent"] = time.monotonic()
                    self._panel.set("target", "target %s hold %.2f" % (xyz, frac))
            return state["hold"] >= hold_s

        done = self._wait(timeout_s, tick)
        result = "done" if done else "timeout"
        if done:
            self._panel.set("target", "target %s done" % xyz)
        self._event("target", state=result, **target)
        self._log("    %s %s" % (p["id"], result))
        if done:
            self._wait(0.6, countdown=False)
        self._panel.set("target", "target off")

    def _tip_distance(self, pt, room):
        """The nearest seen index tip's distance from the target: in the room (with the head's
        pose now) when the panel placed the target there, else in the head frame."""
        live = self._live
        if not live:
            return None
        tips = [live[s]["tip"] for s in ("left", "right") if live[s]]
        if not tips:
            return None
        ref = pt
        if room is not None:
            reply = (self._panel.cmd("head", reply=True, timeout=0.1) or "").split()
            if reply[:1] == ["ok"] and len(reply) == 13:
                m = [float(v) for v in reply[1:]]
                tips = [[m[4 * r] * t[0] + m[4 * r + 1] * t[1] + m[4 * r + 2] * t[2] + m[4 * r + 3] for r in range(3)]
                        for t in tips]
                ref = room
        return min(math.dist(t, ref) for t in tips)

    def _palm_share(self, near, far):
        live = self._live
        if not live:
            return -1.0
        ds = [live[s]["palm_m"] for s in ("left", "right") if live[s]]
        if not ds:
            return -1.0
        return max(0.0, min(1.0, (sum(ds) / len(ds) - near) / (far - near)))

    def _bar(self, s, st, where):
        """One height of the push out and back: the target sweeps near to far and back."""
        near, far = float(s.get("near_m", 0.2)), float(s.get("far_m", 0.6))
        period, lead = float(s.get("period_s", 6)), st["lead"]
        labels = "%s|%s" % (s.get("near_label", "Near"), s.get("far_label", "Far"))
        p = st["p"]
        self._begin_prompt(s, p, where)
        state = {"t": 0.0, "sent": 0.0}

        def tick(dt):
            state["t"] += dt
            sweep = state["t"] - lead
            target = 0.0 if sweep <= 0 else 1 - abs(1 - 2 * ((sweep % period) / period))
            if time.monotonic() - state["sent"] >= 0.1:
                state["sent"] = time.monotonic()
                cur = self._palm_share(near, far)
                self._panel.set("bar", "bar %.3f %.3f %s" % (target, cur, labels))
                if sweep > 0:
                    self._event("bar", target=round(target, 3), current=None if cur < 0 else round(cur, 3))
            return False

        self._wait(p["seconds"], tick)


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
    for b in bounties.view(board, profile, profile.get("checklist") or {}, have_bounty_script):
        print("  %-12s %-7s %4s min  done %d  %-20s %s" % (b["id"], b["priority"], "%g" % b["minutes"] if b["minutes"] else "?",
                                                           b["done"], b["title"], b["available"]))


def main():
    ap = argparse.ArgumentParser(description="Run a hand recording session (the hand recorder's session runner).")
    ap.add_argument("--bounty", metavar="ID", help="a round for this bounty (its id on the board): its own script")
    ap.add_argument("--board", action="store_true", help="print the bounty board and exit")
    ap.add_argument("--debug", action="store_true",
                    help="a debug session (debug.json): use Frametop's hand tracking as usual, its output recorded too")
    ap.add_argument("--dry-run", action="store_true", help="run no processes; print the panel commands")
    ap.add_argument("--speed", type=float, default=1.0, help="run the script this many times faster")
    ap.add_argument("--ring", help="read frames from this ring (ft-ringplay's) instead of ft-camd's")
    ap.add_argument("--no-start", action="store_true", help="start no ft-camd or tracking ft-hands")
    ap.add_argument("--base", help="where sessions go (default %s; a temporary folder with --dry-run)" % BASE_DIR)
    ap.add_argument("--objects", default="", help="ticked objects, comma-separated (unknown names are your own)")
    ap.add_argument("--controllers", action="store_true", help="controllers with the straps")
    ap.add_argument("--lighting", default="auto", choices=("auto", "dim", "room", "daylight"),
                    help="this round's light (auto: indoor or daylight, from the cameras)")
    ap.add_argument("--script", default=SCRIPT_PATH)
    ap.add_argument("--panel", help="the panel program (default hands/rec/build/ft-handpanel)")
    ap.add_argument("--hands-dir", help="where the hands file is (default /run/user/UID/frametop-hands)")
    ap.add_argument("--plan", action="store_true", help="print the sections and their length, and exit")
    ap.add_argument("--auto", action="store_true",
                    help="advance by itself: each prompt for its time, no waiting for Next (the old timed flow)")
    ap.add_argument("--next-after", type=float, metavar="S",
                    help="test: press Next by itself after S seconds of waiting (real time)")
    ap.add_argument("--poses", help="the pose pictures' folder, with poses.json (default hands/rec/poses)")
    ap.add_argument("--demos", help="the skeleton demo clips' folder, <id>.gif (default hands/rec/demos)")
    ap.add_argument("--recorded-demos", help="the recorded demo clips' folder (default hands/rec/demos-recorded)")
    ap.add_argument("--guidance", choices=[k for k, _ in GUIDANCE],
                    help="the demo clips to follow (default recorded when there are any, else skeleton)")
    ap.add_argument("--no-headset-button", action="store_true",
                    help="don't read the headset's button (gpio-keys KEY_SELECT: a press is Next, pause or resume; "
                         "two are redo; a hold is stop)")
    ap.add_argument("--button-device", metavar="PATH",
                    help="test: read the button from this event device or FIFO of input_event structs (also in a dry run)")
    ap.add_argument("--quick", action="store_true",
                    help="a quick round (about 3 min, for another lighting): hand size, the two-hand sweeps, touch, no hands")
    ap.add_argument("--seed", type=int, help="the sweeps' shuffle (default: from the session's id; --plan: the script's order)")
    ap.add_argument("--ignore-cameras", action="store_true",
                    help="start even if the camera check (hands/camcheck.py) finds the upper cameras off")
    a = ap.parse_args()

    known = ("pencil", "phone", "cup", "keyboard", "mouse", "gamepad", "small")
    names = [o.strip() for o in a.objects.split(",") if o.strip()]
    checklist = {"objects": [o for o in names if o in known], "own_objects": [o for o in names if o not in known],
                 "controllers": "straps" if a.controllers else "none", "sleeves": "", "rings": False,
                 "watch": False, "notes": ""}
    base = a.base
    if not base and a.dry_run:
        import tempfile
        base = tempfile.mkdtemp(prefix="handrec-dry-")
    base = base or BASE_DIR
    if a.board:
        print_board(base)
        return 0
    profile = {}
    try:
        with open(os.path.join(base, "profile.json")) as f:
            profile = json.load(f)
    except (OSError, ValueError):
        pass
    entry, script = None, a.script
    if a.debug:
        if a.bounty or a.quick or script != SCRIPT_PATH:
            ap.error("--debug runs debug.json: no --bounty, --quick or --script")
        script = DEBUG_SCRIPT
    if a.bounty:
        if a.quick or script != SCRIPT_PATH:
            ap.error("--bounty runs the bounty's own script: no --quick or --script")
        board = load_board(base)
        entry = next((b for b in bounties.view(board, profile, checklist, have_bounty_script) if b["id"] == a.bounty), None)
        if not entry:
            print("no bounty %s on the board (--board lists them)" % a.bounty)
            return 2
        if entry["available"]:
            print("%s: %s" % (entry["title"], entry["available"]))
            return 2
        entry = dict(entry, updated=board["updated"])
        script = bounty_script(entry["script"])
        print("bounty %s: %s" % (entry["id"], entry["title"]))

    t0 = time.monotonic()
    last = {}

    def on_status(st):
        key = (st["state"], st["section"], st["prompt"], st["note"], st["take"], st["waiting"], st["countdown"],
               st["hands"]["left"], st["hands"]["right"], st["error"])
        if key == last.get("key"):
            return
        last["key"] = key
        hands = "".join("%s%s" % (s[0].upper(), {True: "+", False: "-", None: "?"}[st["hands"][s]])
                        for s in ("left", "right"))
        state = "%s %d" % (st["state"], st["countdown"]) if st["state"] == "countdown" else st["state"]
        line = "[%6.1f] %-11s %d/%d %-16s %s %s" % (time.monotonic() - t0, state, st["section_index"],
                                                    st["section_count"], st["section"] or "-", hands,
                                                    st["prompt"].replace("\n", " | "))
        if st["image"]:
            line += "  [%s%s]" % (os.path.basename(st["image"]), " " + st["image_mode"] if st["image_mode"] else "")
        if st["demo"]:
            line += "  [%s%s]" % (os.path.basename(st["demo"]), " " + st["demo_mode"] if st["demo_mode"] else "")
        if st["waiting"]:
            line += "  (waiting for Next)"
        if st["note"]:
            line += "  (%s)" % st["note"]
        if st["error"]:
            line += "  ERROR: %s" % st["error"]
        print(line, flush=True)

    s = Session(base, profile, checklist, a.lighting, script, ring=a.ring, start_processes=not a.no_start,
                dry_run=a.dry_run, speed=a.speed, on_status=on_status, hands_dir=a.hands_dir, panel_bin=a.panel,
                auto=a.auto, next_after=a.next_after, poses_dir=a.poses, button=not a.no_headset_button,
                button_device=a.button_device, check_cameras=not a.ignore_cameras, quick=a.quick, seed=a.seed,
                demos_dir=a.demos, recorded_demos_dir=a.recorded_demos, guidance=a.guidance, bounty=entry)
    est = plan_seconds(s.script, s.plan, auto=a.auto)
    worst = plan_seconds(s.script, s.plan, worst=True, auto=a.auto)
    print("%d sections%s, %s mode: about %.1f min%s (at most %.1f)%s" % (
        len(s.plan), " (a quick round)" if a.quick else "", "auto" if a.auto else "step", est / 60, "" if a.auto else " recorded", worst / 60,
        ", %gx speed" % a.speed if a.speed != 1 else ""))
    if not a.auto:
        print("  %d steps wait for Next: add your reading time (at 5 s a step, %.1f min)"
              % (plan_steps(s.plan), plan_steps(s.plan) * 5 / 60))
    for sk in s.skipped:
        print("  skipping %s: %s" % (sk["section"], sk["reason"]))
    if a.plan:
        for sec in s.plan:
            print("  %-18s %-9s %3d prompts %3d steps %5.0f s" % (sec["id"], sec["kind"], len(sec["prompts"]),
                                                                section_steps(sec), section_seconds(sec, auto=a.auto)))
            for p in sec["prompts"]:
                if p.get("cues"):
                    print("      %-26s %-5s %3.0f s  %s" % (p["id"], p["hands"], p["seconds"], " ".join(p["cues"])))
        return 0
    if not a.dry_run and not a.ring:
        cam = camera_check()
        print("cameras: %s" % cam["summary"])
        for line in cam.get("evidence", []):
            print("  " + line)
        if camera_text(cam):
            print(camera_text(cam))
            if not a.ignore_cameras:
                print("Not starting (--ignore-cameras starts anyway).")
                return 3
    if not a.dry_run:
        light = ring_lighting(a.ring)
        match = similar_lighting(base, {"chosen": a.lighting, "ring": light}) if light else None
        print("lighting: %s" % (json.dumps(light) if light else "no camera ring"))
        if light:
            print("  measured: %s (ambient IR %s)" % (classify_lighting(light) or "can't tell", ambient_ir(light)))
        if match:
            print("  about the same light as session %s (%s)" % match)

    def on_signal(*_):
        print("stopping", flush=True)
        threading.Thread(target=s.stop, daemon=True).start()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    def keys():
        # From a terminal, or lines piped in (a test); /dev/null ends at once.
        if sys.stdin.isatty():
            print("keys (then Enter): n or just Enter next, p pause/resume, r redo, s skip section, q stop"
                  "; debug: m mark, f finish", flush=True)
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
            elif c == "m":
                s.mark()
            elif c == "f":
                s.finish()
    if sys.stdin is not None:
        threading.Thread(target=keys, daemon=True).start()
    s.start()
    while s._thread.is_alive():
        s.join(0.5)
    print("session: %s" % s.session_dir)
    return 0 if s.state in ("done", "stopped") else 1


if __name__ == "__main__":
    sys.exit(main())
