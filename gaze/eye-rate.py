#!/usr/bin/env python3
"""Keep SteamVR's eye cameras at 90 fps, for gaze mode.

The Frame's SteamVR driver runs the eye cameras at 15 to 90 fps (steamvr.eyeTrackingRateMin
and eyeTrackingRateMax, in drivers/frame_hmd/resources/frame_hmd_additional.vrsettings),
and asks for the rate once, as SteamVR starts (vrserver.txt: "cv: Request eye tracking
framerate"). With its defaults it asks for 15 (eyetracking.txt: "Set framerate 15"). SteamVR's
own tracker copes, but our tracker gets 8 or 9 frames in a calibration dot's 0.6 s, where it
needs 15, so it can never calibrate, and the gaze pointer would move 15 times a second.
With steamvr.gazeDepEyeToHeadEnable on, SteamVR asks for 90 (seen 2026-10-10: a headset with
it on asks for 90 at every start; a user's with it off, at 15, calibrated once it was on).
SteamVR's settings have no switch for it.

  gaze/eye-rate.py on      # turn it on (gaze/run.sh install); it counts from SteamVR's next start
  gaze/eye-rate.py off     # turn it off again, if this turned it on (gaze/run.sh uninstall)
  gaze/eye-rate.py status  # the setting, and the rate the eye cameras run at now

With SteamVR running it goes through vrcmd, since SteamVR keeps its settings in memory and
writes over the file; without it, it edits SteamVR's settings file.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HOME = Path.home()
SECTION, KEY = "steamvr", "gazeDepEyeToHeadEnable"
STEAMVR_BIN = "/opt/steamvr/bin/linuxarm64"
# The first that exists is SteamVR's (as gaze/gazecal.py reads them).
STEAMVR_SETTINGS = (HOME / ".config/openvr/config/steamvr.vrsettings",
                    HOME / ".steam/steam/config/steamvr.vrsettings")
# There when this turned the setting on (it was off), so `off` leaves a setting someone chose alone.
MARK = HOME / ".local/state/frametop/gaze/steamvr-eye-rate"
EYE_LOG = HOME / ".local/share/Steam/logs/eyetracking.txt"
# A terminal in the desktop has the desktop's own config folder, where SteamVR's path registry
# isn't (see scripts/update-check.py): vrcmd needs the real one.
ENV = dict(os.environ, XDG_CONFIG_HOME=str(HOME / ".config"), LD_LIBRARY_PATH=STEAMVR_BIN)


def steamvr_up():
    return subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0


def vrcmd(*args):
    p = subprocess.run([f"{STEAMVR_BIN}/vrcmd", *args], capture_output=True, text=True, timeout=15, env=ENV)
    return p.stdout


def settings_file():
    return next((p for p in STEAMVR_SETTINGS if p.exists()), STEAMVR_SETTINGS[0])


def get():
    """The setting: True, False, or None when SteamVR isn't running and its file doesn't say
    (then the driver's default, off)."""
    if steamvr_up():
        # vrcmd prints "steamvr.gazeDepEyeToHeadEnable=true"
        m = re.search(rf"^{SECTION}\.{KEY}=(\w+)", vrcmd("--settings-bool", f"{SECTION}.{KEY}"), re.M)
        return None if not m else m.group(1) in ("true", "1")
    try:
        value = json.loads(settings_file().read_text()).get(SECTION, {}).get(KEY)
    except (OSError, ValueError, AttributeError):
        return None
    return value if isinstance(value, bool) else None


def put(on):
    if steamvr_up():
        # 1 or 0: vrcmd reads "true" as 0.
        vrcmd("--set-settings-bool", f"{SECTION}.{KEY}", "1" if on else "0")
        return get() is on
    path = settings_file()
    try:
        d = json.loads(path.read_text())
    except FileNotFoundError:
        d = {}
    except (OSError, ValueError) as e:
        print(f"can't read {path}: {e}", file=sys.stderr)
        return False
    d.setdefault(SECTION, {})[KEY] = on
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".frametop-tmp")
    tmp.write_text(json.dumps(d, indent=3) + "\n")
    tmp.replace(path)
    return True


def camera_rate():
    """The rate the eye cameras run at since the eye tracker last started: the last "Set
    framerate" after its last start, or 90 when it set none (it starts at 90). None without
    the log or a start in it."""
    try:
        lines = EYE_LOG.read_text(errors="replace").splitlines()
    except OSError:
        return None
    starts = [i for i, line in enumerate(lines) if " startup with PID" in line]
    if not starts:
        return None
    rates = re.findall(r"CStereoAdspCams: Set framerate ([\d.]+)", "\n".join(lines[starts[-1]:]))
    return float(rates[-1]) if rates else 90.0


def main():
    what = sys.argv[1] if len(sys.argv) == 2 else ""
    if what == "on":
        if get():
            print(f"SteamVR's eye cameras: {SECTION}.{KEY} is on already, so they run at 90 fps")
            return 0
        if not put(True):
            print(f"couldn't turn on {SECTION}.{KEY}: the eye cameras may stay at 15 fps, and our eye "
                  "tracker can't calibrate at that. Try again with: gaze/eye-rate.py on", file=sys.stderr)
            return 1
        MARK.parent.mkdir(parents=True, exist_ok=True)
        MARK.write_text(f"{SECTION}.{KEY} was off; Frametop turned it on\n")
        print(f"SteamVR's eye cameras: turned on {SECTION}.{KEY}, so they run at 90 fps, not 15, "
              "once SteamVR restarts")
        return 0
    if what == "off":
        if not MARK.exists():
            print(f"SteamVR's eye cameras: left as they are (Frametop didn't turn on {SECTION}.{KEY})")
            return 0
        if not put(False):
            print(f"couldn't turn off {SECTION}.{KEY}. Try again with: gaze/eye-rate.py off", file=sys.stderr)
            return 1
        MARK.unlink()
        print(f"SteamVR's eye cameras: turned {SECTION}.{KEY} off again (back to 15 fps once SteamVR restarts)")
        return 0
    if what == "status":
        value = get()
        rate = camera_rate()
        print(f"{SECTION}.{KEY}: {'on' if value else 'off'}" + (" (set by Frametop)" if MARK.exists() else ""))
        print("eye cameras: " + (f"{rate:g} fps since the eye tracker started" if rate else "unknown (no eyetracking.txt)"))
        return 0
    print("usage: gaze/eye-rate.py on|off|status", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
