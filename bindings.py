"""Push-to-talk bindings and settings shared by scarlet-voice, scarlet-vr-input and scarlet-config.

~/.config/scarlet-voice/bindings.json (written by the scarlet-config app):
  {"keyboard": ["KEY_RIGHTCTRL", "BTN_SIDE"],
   "controller": [{"device": "/user/hand/left", "input": "/input/dpad_down/click"}],
   "keyboard_mode": "toggle", "controller_mode": "hold",
   "live_typing": true, "sounds": false, "wake_word": true}

keyboard: evdev key or mouse button names, read from every physical keyboard and mouse.
controller: a SteamVR device path and one of its boolean inputs, read from vrserver.
*_mode: "hold" records while held; "toggle" starts on one press and stops on the next.
The options override config.toml.
"""
import json
import os
from pathlib import Path

PATH = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "scarlet-voice/bindings.json"
DEFAULT = {
    "keyboard": ["KEY_RIGHTCTRL"],
    "controller": [{"device": "/user/hand/left", "input": "/input/dpad_down/click"}],
    # Toggle for keyboards: live text can't be typed while a modifier like Ctrl is held.
    "keyboard_mode": "toggle",
    "controller_mode": "hold",
    "live_typing": True,
    "sounds": False,
    "wake_word": True,
}
MODES = ("hold", "toggle")
OPTIONS = ("live_typing", "sounds", "wake_word")
# Held modifiers turn typed letters into shortcuts, so live typing waits until they are released.
MODIFIERS = {"KEY_LEFTCTRL", "KEY_RIGHTCTRL", "KEY_LEFTALT", "KEY_RIGHTALT",
             "KEY_LEFTSHIFT", "KEY_RIGHTSHIFT", "KEY_LEFTMETA", "KEY_RIGHTMETA"}


def load():
    try:
        data = json.loads(PATH.read_text())
    except (FileNotFoundError, ValueError):
        data = {}
    out = json.loads(json.dumps(DEFAULT))
    if isinstance(data.get("keyboard"), list):
        out["keyboard"] = [k for k in data["keyboard"] if isinstance(k, str)]
    if isinstance(data.get("controller"), list):
        out["controller"] = [c for c in data["controller"] if isinstance(c, dict)
                             and isinstance(c.get("device"), str) and isinstance(c.get("input"), str)]
    for key in ("keyboard_mode", "controller_mode"):
        if data.get(key) in MODES:
            out[key] = data[key]
    for key in OPTIONS:
        if isinstance(data.get(key), bool):
            out[key] = data[key]
    return out


def save(data):
    PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n")
    tmp.replace(PATH)


def mtime():
    try:
        return PATH.stat().st_mtime_ns
    except FileNotFoundError:
        return 0
