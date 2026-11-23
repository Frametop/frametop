#!/usr/bin/env python3
"""handsrate: hand tracking at 60 frames a second, as one switch (Frametop Settings' Hands page).

SteamVR runs the tracking cameras at 60 Hz with every other frame dark: the dark frames show the
controllers' lights, and ft-hands gets the 30 bright ones, tracking a fast hand at 30 Hz and a
slow one at 15. On, three settings change together:

  - XRService runs the cameras at 60 Hz with no dark frames: streamingMode 0 and slamFrameFPSMode 3
    in captureSessionSettings.trackingCameras of SteamVR's XRServiceSettings.json (the file's
    other keys are kept; hands/tools/slam-rate.py has the measured modes). With no dark frames
    the controllers stop tracking, and SteamVR says so in the headset while it's on.
  - ft-hands tracks every camera set with a hand in view: HANDS_MAX_HZ=60 and HANDS_FULL_RATE=1
    in ~/.config/frametop.conf.

Off takes the two camera keys out again (SteamVR's own mode) and writes ft-hands' defaults,
HANDS_MAX_HZ=30 and HANDS_FULL_RATE=0.

XRService and ft-hands read these as they start. --now restarts ft-hands if it runs and stops
XRService, so SteamVR starts a new one: head tracking drops for about 15 seconds (SteamVR waits
that long without tracking before it starts XRService again), and with the headset off SteamVR
starts it when the headset is next worn. The new XRService tracks in a new world, turned from the
old one, so ft-screens moves what you had open with it (screens/universefix.h). Without --now the cameras change
when SteamVR next starts. SteamVR gives up on an XRService that stops more than five times in a
minute, so Frametop Settings holds the switch for a while after each use.

The settings file is SteamVR's: this reads it without leaving anything next to it, writes it by a
temp file and a rename, and never writes over one that doesn't read. The mode numbers and
XRService's command line are the SteamOS image's (measured on 0.5.9): scripts/update-check.py
checks, with the switch on, that the cameras still give 60 frames a second and no dark ones
(camera_rates here reads them from ft-camd's status lines).

Known (2026-11-23): with no dark frames, changing Room View between colour and black and white
restarts the cameras out of step, and head tracking falls to 3DoF until SteamVR restarts
XRService, about 15 s later.

  python3 hands/handsrate.py status [--json]   # on or off, and what doesn't match
  python3 hands/handsrate.py on|off [--now]    # write the three settings, and apply them now
                                               # (exit 1: not written, or not applied; it says which)

On the host, system Python, standard library only (the settings app runs it through
distrobox-host-exec and imports it to read the state; uninstall.sh runs `off`).
"""
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "conf"))
sys.path.insert(0, HERE)
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ft_store  # noqa: E402  (write_json: a temp file, fsync and a rename)
import handsmode  # noqa: E402  (the units' names, and systemctl --user on the user manager's bus)

XR_PATH = os.path.expanduser("~/.config/openvr/config/cv/xrservice/XRServiceSettings.json")
CONF_PATH = handsmode.CONF_PATH
SECTION, CAMERAS = "captureSessionSettings", "trackingCameras"
HIGH = {"streamingMode": 0, "slamFrameFPSMode": 3}  # 60 Hz, no dark frames
CONF_ON = {"HANDS_MAX_HZ": "60", "HANDS_FULL_RATE": "1"}
CONF_OFF = {"HANDS_MAX_HZ": "30", "HANDS_FULL_RATE": "0"}  # ft-hands' defaults
HANDS_UNIT = handsmode.TRACKING[1]
# By its command line, as scripts/update-check.py finds it: XRService renames its main thread.
XRSERVICE = "^(/[^ ]*/)?XRService( |$)"
WARNING = ("While it's on the controllers don't track: the cameras stop taking the dark frames that "
           "show their lights, and SteamVR says so in the headset. Switching it restarts head tracking "
           "and hand tracking: the view stops following your head for about 15 seconds, and if the "
           "screens aren't in front of you afterwards, open the Steam dashboard. Changing Room View between colour and black and "
           "white drops head tracking for about 15 seconds while it's on.")


class RateError(RuntimeError):
    """The switch couldn't be read, written or applied; its text says which, for the user."""


def read_settings(path=XR_PATH):
    """XRService's settings: {} when there's no file or an empty one. RateError when it isn't JSON
    or isn't shaped as this edits it. Reads only: nothing is left next to SteamVR's file."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        data = json.loads(text) if text.strip() else {}
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        raise RateError(f"{path} doesn't read ({e})") from None
    section = data.get(SECTION, {}) if isinstance(data, dict) else None
    if not isinstance(section, dict) or not isinstance(section.get(CAMERAS, {}), dict):
        raise RateError(f"{path} isn't XRService's settings as Frametop knows them "
                        f"({SECTION}.{CAMERAS} isn't a JSON object)")
    return data


def cameras_of(data):
    """The camera mode in XRService's settings: the keys of HIGH that are set ({}: SteamVR's own)."""
    cameras = data.get(SECTION, {}).get(CAMERAS, {})
    return {k: cameras[k] for k in HIGH if k in cameras}


def read_cameras(path=XR_PATH):
    """cameras_of the file (hands/tools/slam-rate.py shows it)."""
    return cameras_of(read_settings(path))


def with_cameras(data, mode):
    """data with the camera mode MODE (HIGH, or {} for SteamVR's own): only HIGH's keys change,
    and a section this leaves empty is taken out, so off leaves the file as SteamVR wrote it."""
    section = data.setdefault(SECTION, {})
    cameras = section.setdefault(CAMERAS, {})
    for key in HIGH:
        cameras.pop(key, None)
    cameras.update(mode)
    if not cameras:
        del section[CAMERAS]
    if not section:
        del data[SECTION]
    return data


def write_cameras(mode, path=XR_PATH):
    """Set the camera mode in XRService's settings, the rest kept. A file that doesn't read is
    left as it is (RateError), and so is one that already has the mode."""
    data = read_settings(path)
    if cameras_of(data) == mode:
        return
    try:
        ft_store.write_json(path, with_cameras(data, mode), indent=4)
    except OSError as e:
        raise RateError(f"{path} couldn't be written ({e})") from None


def state(conf=None, xr_path=XR_PATH, conf_path=CONF_PATH):
    """{"on", "note"}: on while the cameras are in the no-dark mode (what stops the controllers);
    note says what doesn't match it, or why the camera settings can't be read. Reads only."""
    conf = ft_conf.read(conf_path) if conf is None else conf
    try:
        cameras = read_cameras(xr_path)
    except RateError as e:
        return {"on": False, "note": f"SteamVR's camera settings can't be used: {e}."}
    on = cameras == HIGH
    tracker = all(conf.get(k, CONF_OFF[k]) == v for k, v in CONF_ON.items())
    note = ""
    if cameras and not on:
        note = (f"The cameras are in another mode ({json.dumps(cameras)}), set outside this switch: "
                "turning it on replaces that mode.")
    elif on and not tracker:
        note = ("The cameras are in the 60 Hz mode but ft-hands isn't set to track every frame "
                "(HANDS_MAX_HZ, HANDS_FULL_RATE): turn it off, then on again.")
    elif tracker and not on:
        note = ("ft-hands is set to track every frame but the cameras aren't in the 60 Hz mode: "
                "turn it on to finish, or on and then off to put ft-hands' own rate back.")
    return {"on": on, "note": note}


def camera_rates(status):
    """What ft-camd's status lines (its journal's text, a line a minute) say of the tracking
    cameras: {"best": the most bright frames a second a camera gave in a minute, "minutes": how
    many full minutes the cameras ran, "dark": the share of their frames that were dark, None
    under 600 frames}. Each line is measured against the one before it (its dark count is a
    running total, and the first line's minute may have begun before the cameras did); a line
    whose clock went back is ft-camd started again, and is the next first line. A minute is full
    when the cameras ran (20 a second or more: with the headset off they give 0 to 5) in it and
    in the minutes before and after it: the headset goes on and comes off part way through one,
    which reads as a lower rate than the cameras' own."""
    best, ran, bright, dark, before = 0.0, [], 0.0, 0, None
    for line in status.splitlines():
        clock = re.match(r"\[(\d+) s\]", line)
        cams = re.findall(r"([0-9]+\.[0-9]) fps \(dark (\d+) ", line)
        if not clock or not cams:
            continue
        now = (int(clock.group(1)), [float(rate) for rate, _ in cams], sum(int(n) for _, n in cams))
        measured = bool(before) and now[0] > before[0] and now[2] >= before[2]
        if measured:
            best = max(best, *now[1])
            bright += sum(now[1]) * (now[0] - before[0])
            dark += now[2] - before[2]
        ran.append(measured and max(now[1]) >= 20)
        before = now
    minutes = sum(a and b and c for a, b, c in zip(ran, ran[1:], ran[2:]))
    return {"best": best, "minutes": minutes, "dark": dark / (bright + dark) if bright + dark >= 600 else None}


class Host:
    """The processes this restarts. Tests replace it, or its `user` (handsmode.Host)."""

    def __init__(self, user=None):
        self.user = user or handsmode.Host()

    def restart_hands(self):
        """ft-hands again, if it runs: it reads its settings as it starts. "" or what went wrong
        (exit 5 is systemctl's for a unit that isn't installed: nothing to restart)."""
        r = self.user.systemctl("try-restart", "--no-block", HANDS_UNIT)
        return "" if r.returncode in (0, 5) else (r.stderr.strip() or f"systemctl exit {r.returncode}")

    def xrservice(self):
        """XRService's process ids (an old one may still be going while a new one starts)."""
        return {int(pid) for pid in self.user.run(["pgrep", "-f", XRSERVICE]).stdout.split()}

    def stop(self, pids):
        """Ask them to stop, each on its own: one that has gone by itself meanwhile (an old one,
        as a new one starts) is no failure. kill says that in the user's language, so it is run
        without one. "" or what went wrong."""
        problems = []
        for pid in sorted(pids):
            r = self.user.run(["env", "LC_ALL=C", "kill", str(pid)])
            if r.returncode != 0 and "No such process" not in r.stderr:
                problems.append(r.stderr.strip() or f"kill {pid}: exit {r.returncode}")
        return "; ".join(problems)

    def wait(self, seconds):
        time.sleep(seconds)


def restart_tracking(host, wait=30.0, step=0.2):
    """Stop XRService so SteamVR starts a new one, which reads the camera mode: it does after 15 s
    without tracking (its own timer; 16.5 s from the stop, worn 2026-11-23). (restarted, text):
    restarted is None when it wasn't running, False when SteamVR hasn't started a new one yet.
    RateError when XRService couldn't be stopped."""
    old = host.xrservice()
    if not old:
        return None, "The cameras change when SteamVR next starts."
    problem = host.stop(old)
    if problem:
        raise RateError(f"XRService couldn't be stopped ({problem}), so the cameras keep their old "
                        "mode until SteamVR next starts")
    for _ in range(int(wait / step)):
        host.wait(step)
        if host.xrservice() - old:
            return True, "Head tracking restarted with the new camera mode."
    return False, ("SteamVR hasn't restarted head tracking yet: it does when the headset is next worn. "
                   "If the view stays frozen while you wear it, restart SteamVR.")


def switch(on, host=None, conf_path=CONF_PATH, xr_path=XR_PATH):
    """Write the three settings for ON or off; a setting that already reads so is left as it is.
    The cameras go first (a file that doesn't read stops it there, with nothing changed). If
    frametop.conf then can't be written, what was written is put back, so the two don't part,
    and the error says so, or says what stayed changed. With a host, apply them now. The text
    to show; RateError when something wasn't written or applied."""
    before = read_cameras(xr_path)
    write_cameras(HIGH if on else {}, xr_path)
    conf, written = ft_conf.read(conf_path), {}
    try:
        for key, value in (CONF_ON if on else CONF_OFF).items():
            was = conf.get(key, CONF_OFF[key])
            if was != value:  # off with nothing set writes nothing (uninstall.sh)
                ft_conf.set_value(key, value, conf_path)
                written[key] = was
    except OSError as e:
        try:
            write_cameras(before, xr_path)
            for key, was in written.items():
                ft_conf.set_value(key, was, conf_path)
            kept = "the cameras' mode and ft-hands' settings are as they were"
        except (RateError, OSError) as again:
            now = "on" if state(None, xr_path, conf_path)["on"] else "off"
            kept = (f"and what was written couldn't be put back ({again}): the switch reads {now} now, "
                    "so set it again once the files can be written")
        raise RateError(f"{conf_path} couldn't be written ({e}); {kept}") from None
    said = ("60 fps hand tracking on: the controllers don't track." if on
            else "60 fps hand tracking off: the controllers track again.")
    if host is None:
        return said + " It counts from SteamVR's next start."
    problem = host.restart_hands()
    if problem:
        raise RateError(f"The settings are written, but ft-hands couldn't be restarted ({problem}): "
                        "restart SteamVR to apply them")
    try:
        return said + " " + restart_tracking(host)[1]
    except RateError as e:
        raise RateError(f"The settings are written, but {e}") from None


def main(argv, conf_path=CONF_PATH, xr_path=XR_PATH):
    cmd = argv[0] if argv else "status"
    if cmd == "status" and argv[1:] in ([], ["--json"]):
        st = state(None, xr_path, conf_path)
        if "--json" in argv:
            print(json.dumps(st))
        else:
            print("60 fps hand tracking: " + ("on" if st["on"] else "off"))
            if st["note"]:
                print(st["note"])
        return 0
    if cmd in ("on", "off") and argv[1:] in ([], ["--now"]):
        try:
            print(switch(cmd == "on", Host() if "--now" in argv else None, conf_path, xr_path))
        except RateError as e:
            print(f"handsrate: {e}", file=sys.stderr)
            return 1
        return 0
    print("usage: handsrate.py status [--json] | on [--now] | off [--now]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
