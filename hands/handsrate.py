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
XRService, so SteamVR starts a new one: head tracking drops for a few seconds, and with the
headset off SteamVR starts it when the headset is next worn. Without --now the cameras change
when SteamVR next starts.

Known (2026-11-23): with no dark frames, changing Room View between colour and black and white
restarts the cameras out of step, and head tracking falls to 3DoF until SteamVR restarts
XRService, about 15 s later.

  python3 hands/handsrate.py status [--json]   # on or off, and what doesn't match
  python3 hands/handsrate.py on|off [--now]    # write the three settings, and apply them now

On the host, system Python, standard library only (the settings app runs it through
distrobox-host-exec and imports it to read the state; uninstall.sh runs `off`).
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "conf"))
sys.path.insert(0, HERE)
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ft_store  # noqa: E402  (JSON files read strictly, written by a temp file and a rename)
import handsmode  # noqa: E402  (the units' names, and systemctl --user on the user manager's bus)

XR_PATH = os.path.expanduser("~/.config/openvr/config/cv/xrservice/XRServiceSettings.json")
CONF_PATH = handsmode.CONF_PATH
SECTION, CAMERAS = "captureSessionSettings", "trackingCameras"
HIGH = {"streamingMode": 0, "slamFrameFPSMode": 3}  # 60 Hz, no dark frames
CONF_ON = {"HANDS_MAX_HZ": "60", "HANDS_FULL_RATE": "1"}
CONF_OFF = {"HANDS_MAX_HZ": "30", "HANDS_FULL_RATE": "0"}  # ft-hands' defaults
HANDS_UNIT = handsmode.TRACKING[1]
XRSERVICE = "^XRService --documentsRoot"
WARNING = ("While it's on the controllers don't track: the cameras stop taking the dark frames that "
           "show their lights, and SteamVR says so in the headset. Switching it restarts head tracking, "
           "so the view holds still for a few seconds. Changing Room View between colour and black and "
           "white drops head tracking for about 15 seconds while it's on.")


def check(data):
    """ft_store's check: why the file isn't XRService's settings as this edits them, or None."""
    if not isinstance(data, dict):
        return "it isn't a JSON object"
    section = data.get(SECTION, {})
    if not isinstance(section, dict) or not isinstance(section.get(CAMERAS, {}), dict):
        return f"its {SECTION}.{CAMERAS} isn't a JSON object"
    return None


def cameras_of(data):
    """The camera mode in XRService's settings: the keys of HIGH that are set ({}: SteamVR's own)."""
    cameras = data.get(SECTION, {}).get(CAMERAS, {})
    return {k: cameras[k] for k in HIGH if k in cameras}


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
    """Set the camera mode in XRService's settings under the file's lock. A file that doesn't
    read is left as it is (ft_store.StoreError)."""
    ft_store.update_json(path, lambda data: with_cameras(data, mode), {}, check, indent=4)


def state(conf=None, xr_path=XR_PATH):
    """{"on", "note"}: on while the cameras are in the no-dark mode (what stops the controllers);
    note says what doesn't match it, or why the camera settings can't be read."""
    conf = ft_conf.read(CONF_PATH) if conf is None else conf
    try:
        cameras = cameras_of(ft_store.load_json_strict(xr_path, {}, check))
    except ft_store.StoreError as e:
        return {"on": False, "note": f"SteamVR's camera settings don't read: {e}"}
    on = cameras == HIGH
    tracker = all(conf.get(k, CONF_OFF[k]) == v for k, v in CONF_ON.items())
    note = ""
    if cameras and not on:
        note = (f"The cameras are in another mode ({json.dumps(cameras)}, not this switch's): "
                "turning it on or off replaces it.")
    elif on and not tracker:
        note = ("The cameras are in the 60 Hz mode but ft-hands isn't set to track every frame "
                "(HANDS_MAX_HZ, HANDS_FULL_RATE): turn it off and on again.")
    elif tracker and not on:
        note = ("ft-hands is set to track every frame but the cameras aren't in the 60 Hz mode: "
                "turn it on, or off and on again.")
    return {"on": on, "note": note}


class Host:
    """The processes this restarts. Tests replace it."""

    def __init__(self):
        self.user = handsmode.Host()

    def restart_hands(self):
        """ft-hands again, if it runs: it reads its settings as it starts."""
        self.user.systemctl("try-restart", "--no-block", HANDS_UNIT)

    def xrservice(self):
        """XRService's process id, or None."""
        out = self.user.run(["pgrep", "-f", XRSERVICE]).stdout.split()
        return int(out[0]) if out else None

    def stop(self, pid):
        self.user.run(["kill", str(pid)])

    def wait(self, seconds):
        time.sleep(seconds)


def restart_tracking(host, wait=20.0, step=0.2):
    """Stop XRService so SteamVR starts a new one, which reads the camera mode. The text to show:
    it's back, it wasn't running, or SteamVR hasn't started a new one yet."""
    old = host.xrservice()
    if old is None:
        return "The cameras change when SteamVR next starts."
    host.stop(old)
    for _ in range(int(wait / step)):
        host.wait(step)
        new = host.xrservice()
        if new is not None and new != old:
            return "Head tracking restarted with the new camera mode."
    return ("SteamVR hasn't restarted head tracking yet: it does when the headset is next worn. "
            "If the view stays frozen while you wear it, restart SteamVR.")


def switch(on, host=None, conf_path=CONF_PATH, xr_path=XR_PATH):
    """Write the three settings for ON or off, the cameras first (a file that doesn't read stops
    it there, with nothing changed); a setting that already reads so is left as it is. With a
    host, apply them now. The text to show."""
    write_cameras(HIGH if on else {}, xr_path)
    conf = ft_conf.read(conf_path)
    for key, value in (CONF_ON if on else CONF_OFF).items():
        if conf.get(key, CONF_OFF[key]) != value:  # off with nothing set writes nothing (uninstall.sh)
            ft_conf.set_value(key, value, conf_path)
    said = ("60 fps hand tracking on: the controllers don't track." if on
            else "60 fps hand tracking off: the controllers track again.")
    if host is None:
        return said + " It counts from SteamVR's next start."
    host.restart_hands()
    return said + " " + restart_tracking(host)


def main(argv):
    cmd = argv[0] if argv else "status"
    if cmd == "status" and argv[1:] in ([], ["--json"]):
        st = state()
        if "--json" in argv:
            print(json.dumps(st))
        else:
            print("60 fps hand tracking: " + ("on" if st["on"] else "off"))
            if st["note"]:
                print(st["note"])
        return 0
    if cmd in ("on", "off") and argv[1:] in ([], ["--now"]):
        try:
            print(switch(cmd == "on", Host() if "--now" in argv else None))
        except (ft_store.StoreError, OSError) as e:
            print(f"handsrate: {e}", file=sys.stderr)
            return 1
        return 0
    print("usage: handsrate.py status [--json] | on [--now] | off [--now]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
