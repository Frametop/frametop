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
SteamOS 0.5's driver (driver_cv.so) reads steamvr.gazeDepEyeToHeadEnable2 instead and ignores the
old name (seen 2026-11-17: 15 at every start with the old one on; with the new one on, the 15 was
no longer asked for), so this sets the name the installed driver reads. SteamVR's settings have
no switch for either.

  gaze/eye-rate.py on      # turn it on (gaze/run.sh install); it counts from SteamVR's next start
  gaze/eye-rate.py off     # turn it off again, if this turned it on (gaze/run.sh uninstall)
  gaze/eye-rate.py status  # the setting, and the rate the eye cameras run at now

With SteamVR running it goes through vrcmd, since SteamVR keeps its settings in memory and
writes over the file; without it, it edits SteamVR's settings file.
"""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "conf"))
import ft_vrsettings  # noqa: E402  (SteamVR's settings: vrcmd's output and the file)

HOME = Path.home()
SECTION = "steamvr"
OLD_KEY, NEW_KEY = "gazeDepEyeToHeadEnable", "gazeDepEyeToHeadEnable2"  # SteamOS 0.4's, 0.5's
DRIVER = Path("/opt/steamvr/drivers/cv/bin/linuxarm64/driver_cv.so")  # the Frame's tracking driver
STEAMVR_BIN = "/opt/steamvr/bin/linuxarm64"
# The first that exists is SteamVR's (as gaze/gazecal.py reads them).
STEAMVR_SETTINGS = ft_vrsettings.FILES
# There when this turned a setting on (it was off), so `off` leaves a setting someone chose alone:
# one line per setting, "steamvr.<key> was off; Frametop turned it on".
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
    return ft_vrsettings.settings_file(STEAMVR_SETTINGS)


def key():
    """The setting's name in the installed driver: SteamOS 0.5's when the driver has that name in
    it, else 0.4's. Unreadable (no SteamVR): 0.5's, the current one."""
    try:
        return NEW_KEY if (NEW_KEY + "\0").encode() in DRIVER.read_bytes() else OLD_KEY
    except OSError:
        return NEW_KEY


def get(name=None):
    """The setting: True, False, or None when SteamVR isn't running and its file doesn't say
    (then the driver's default, off)."""
    name = name or key()
    if steamvr_up():
        # vrcmd prints "steamvr.gazeDepEyeToHeadEnable2=true"
        return ft_vrsettings.vrcmd_bool(vrcmd("--settings-bool", f"{SECTION}.{name}"), SECTION, name)
    s = ft_vrsettings.read_section(settings_file(), SECTION)
    value = s.get(name) if isinstance(s, dict) else None
    return value if isinstance(value, bool) else None


def put(name, on):
    if steamvr_up():
        vrcmd("--set-settings-bool", f"{SECTION}.{name}", ft_vrsettings.bool_arg(on))
        return get(name) is on
    return ft_vrsettings.write_values(settings_file(), SECTION, {name: on})


def marked():
    """The settings this turned on, from MARK (an older mark names 0.4's)."""
    try:
        lines = MARK.read_text().splitlines()
    except OSError:
        return []
    names = [line.split()[0].removeprefix(SECTION + ".") for line in lines if line.strip()]
    return [n for n in names if n in (OLD_KEY, NEW_KEY)]


def last_start():
    """When the eye tracker last started (eyetracking.txt's clock, local time) as a Unix time, or
    None without the log or a start in it."""
    try:
        lines = EYE_LOG.read_text(errors="replace").splitlines()
    except OSError:
        return None
    for line in reversed(lines):
        if " startup with PID" in line:
            try:  # "Mon Nov 16 2026 12:24:34.650276 [Info] - eyetracking 2.19.3 startup with PID=..."
                return time.mktime(time.strptime(line[:24], "%a %b %d %Y %H:%M:%S"))
            except ValueError:
                return None
    return None


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
    name = key()
    if what == "on":
        if get(name):
            print(f"SteamVR's eye cameras: {SECTION}.{name} is on already, so they run at 90 fps")
            return 0
        if not put(name, True):
            print(f"couldn't turn on {SECTION}.{name}: the eye cameras may stay at 15 fps, and our eye "
                  "tracker can't calibrate at that. Try again with: gaze/eye-rate.py on", file=sys.stderr)
            return 1
        MARK.parent.mkdir(parents=True, exist_ok=True)
        with MARK.open("a") as f:
            f.write(f"{SECTION}.{name} was off; Frametop turned it on\n")
        print(f"SteamVR's eye cameras: turned on {SECTION}.{name}, so they run at 90 fps, not 15, "
              "once SteamVR restarts")
        return 0
    if what == "off":
        names = marked()
        if not names:
            print(f"SteamVR's eye cameras: left as they are (Frametop didn't turn on {SECTION}.{name})")
            return 0
        for n in dict.fromkeys(names):
            if not put(n, False):
                print(f"couldn't turn off {SECTION}.{n}. Try again with: gaze/eye-rate.py off", file=sys.stderr)
                return 1
        MARK.unlink()
        print(f"SteamVR's eye cameras: turned {', '.join(f'{SECTION}.{n}' for n in dict.fromkeys(names))} "
              "off again (back to 15 fps once SteamVR restarts)")
        return 0
    if what == "status":
        value = get(name)
        rate = camera_rate()
        print(f"{SECTION}.{name}: {'on' if value else 'off'}" + (" (set by Frametop)" if name in marked() else ""))
        print("eye cameras: " + (f"{rate:g} fps since the eye tracker started" if rate else "unknown (no eyetracking.txt)"))
        return 0
    print("usage: gaze/eye-rate.py on|off|status", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
