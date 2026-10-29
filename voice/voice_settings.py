#!/usr/bin/env python3
"""Frametop Voice's settings, shared by ft-voiced, Frametop Input Settings (its Voice page), and
the installer. Standard library only: the installer runs it with the host's python3.

~/.config/frametop-voice.json (written by Frametop Input Settings):
  {"controller": ["left/dpad_down"], "controller_mode": "hold",
   "language": "de", "model": "small",
   "live_typing": true, "sounds": false, "wake_word": false,
   "wake_phrases": ["HEY COMPUTER"],
   "commands": [{"say": ["flag that", "flag"], "run": ["~/.local/bin/my-tool", "flag"]}],
   "advanced": {"dictation": {"min_seconds": 0.5}}}

controller: Frame controller buttons for push-to-talk, as the input relay names them
  ("left/dpad_down"). They're read from SteamVR's web socket, so they still do what they do
  in games.
controller_mode: "hold" records while held; "toggle" starts on one press and stops on the next.
language: what you speak (LANGUAGES): "en", another language's code, or "auto" (whisper tells, per
  piece of speech). English types live with Moonshine; the others with whisper on the GPU.
model: the whisper model (WHISPER_MODELS). One that knows only English is taken as "small" for
  another language. The ones not installed with Frametop Voice are fetched when chosen:
  voice/models.sh model NAME.
wake_phrases: what to say before a command; any English words, spelled out at load time.
commands: wake word commands that run a program (voice_commands.py). Edited by hand; the
  settings app keeps them as they are, and every other key it doesn't manage too.
advanced: overrides of ft-voiced's DEFAULTS, by section (audio, whisper, dictation, wake, sounds).
The keyboard's push-to-talk keys are Frametop key combinations ("key_bindings" in
~/.config/frametop-input.json, actions dictate and dictate_hold), not here.

  voice/voice_settings.py install   on install: carry over frame-voice's settings once, and give
                                    dictation its Right Ctrl tap in a rules file with its own keys
  voice/voice_settings.py model-file   the whisper model file to load (voice/whisper-run)
"""
import json
import os
import re
import socket
import sys
from pathlib import Path

# Not under $XDG_CONFIG_HOME: Frametop's desktop points it at ~/.config/frametop, and the
# service and the settings app must read the same file.
PATH = Path(os.environ.get("FRAMETOP_VOICE_SETTINGS", "~/.config/frametop-voice.json")).expanduser()
RULES_PATH = Path("~/.config/frametop-input.json").expanduser()
FRAME_VOICE_PATH = Path("~/.config/frame-voice/settings.json").expanduser()  # the standalone frame-voice's
DEFAULT = {
    "controller": ["left/dpad_down"],
    "controller_mode": "hold",
    "language": "en",
    "model": "small.en",
    "live_typing": True,
    "sounds": False,
    # Off until you turn it on: it keeps the mic open, and does nothing without commands of your own.
    "wake_word": False,
    "wake_phrases": ["HEY COMPUTER"],
}
MODES = ("hold", "toggle")
MODELS_DIR = Path(os.environ.get("FRAMETOP_VOICE_MODELS", "~/.local/share/frametop/voice/models")).expanduser()
# whisper.cpp's models (ggml, quantized), in voice/models.sh with their sha256: name -> (file,
# MB, languages). small.en comes with Frametop Voice; the others are fetched when chosen.
WHISPER_MODELS = {
    "small.en": ("ggml-small.en-q5_1.bin", 190, "en"),
    "small": ("ggml-small-q5_1.bin", 190, "all"),
    "large-v3-turbo": ("ggml-large-v3-turbo-q5_0.bin", 574, "all"),
}
MULTILINGUAL_DEFAULT = "small"
# Languages to dictate in, by whisper's codes (it knows about 100; these are on the Voice page).
LANGUAGES = {"en": "English", "auto": "Detect it", "de": "Deutsch", "fr": "Fran\u00e7ais", "es": "Espa\u00f1ol",
             "it": "Italiano", "pt": "Portugu\u00eas", "nl": "Nederlands", "pl": "Polski",
             "cs": "\u010ce\u0161tina", "sv": "Svenska", "da": "Dansk", "no": "Norsk", "fi": "Suomi",
             "tr": "T\u00fcrk\u00e7e", "ru": "\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
             "uk": "\u0423\u043a\u0440\u0430\u0457\u043d\u0441\u044c\u043a\u0430",
             "ja": "\u65e5\u672c\u8a9e", "zh": "\u4e2d\u6587", "ko": "\ud55c\uad6d\uc5b4"}
OPTIONS = ("live_typing", "sounds", "wake_word")
# Frame controller buttons (the input relay's VR_BUTTONS; pointer/helper/vrbuttons.h).
VR_BUTTONS = ("left/view", "left/dpad_up", "left/dpad_down", "left/dpad_left", "left/dpad_right", "left/bumper",
              "left/trigger", "left/grip", "left/thumbstick", "right/menu", "right/a", "right/b", "right/x", "right/y",
              "right/bumper", "right/trigger", "right/grip", "right/thumbstick")
# The input relay's key combinations without a list of their own, with Frametop Voice installed
# (its DEFAULT_KEY_BINDINGS and VOICE_KEY_BINDINGS; voice/test checks they match).
DEFAULT_KEY_BINDINGS = {"125": "steam_menu", "125+36": "gaze_left", "125+37": "gaze_right",
                        "42+125+33": "float_toggle", "56+125+15": "spin_next", "42+56+125+15": "spin_prev",
                        "125+57": "frunner", "125+35": "hands_toggle", "42+125+35": "screens_toggle"}
VOICE_KEY_BINDINGS = {"97": "dictate"}
DICTATION_ACTIONS = ("dictate", "dictate_hold")
# frame-voice's keys, by evdev name: the modifiers (a left one taps either side, a right one only
# itself, as the relay reads them) and the keys a push-to-talk key usually is.
KEY_CODES = {"KEY_LEFTCTRL": 29, "KEY_RIGHTCTRL": 97, "KEY_LEFTSHIFT": 42, "KEY_RIGHTSHIFT": 54,
             "KEY_LEFTALT": 56, "KEY_RIGHTALT": 100, "KEY_LEFTMETA": 125, "KEY_RIGHTMETA": 126,
             "KEY_CAPSLOCK": 58, "KEY_SCROLLLOCK": 70, "KEY_PAUSE": 119, "KEY_INSERT": 110, "KEY_COMPOSE": 127,
             **{f"KEY_F{n}": 58 + n for n in range(1, 11)}, "KEY_F11": 87, "KEY_F12": 88,
             **{f"KEY_F{n}": 170 + n for n in range(13, 25)}}


def clean_phrase(text):
    """A wake phrase as the keyword spotter wants it: uppercase words, letters and apostrophes."""
    return " ".join(re.sub(r"[^A-Z' ]", " ", text.upper()).split())


def read_json(path):
    try:
        data = json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def load():
    data = read_json(PATH)
    out = json.loads(json.dumps(DEFAULT))
    if isinstance(data.get("controller"), list):
        out["controller"] = [b for b in data["controller"] if b in VR_BUTTONS]
    if data.get("controller_mode") in MODES:
        out["controller_mode"] = data["controller_mode"]
    if isinstance(data.get("language"), str) and re.fullmatch(r"auto|[a-z]{2,3}", data["language"]):
        out["language"] = data["language"]  # any of whisper's, not only LANGUAGES
    if data.get("model") in WHISPER_MODELS:
        out["model"] = data["model"]
    for key in OPTIONS:
        if isinstance(data.get(key), bool):
            out[key] = data[key]
    if isinstance(data.get("wake_phrases"), list):
        phrases = (clean_phrase(p) for p in data["wake_phrases"] if isinstance(p, str))
        out["wake_phrases"] = list(dict.fromkeys(p for p in phrases if p))
    # Checked by voice_commands.parse(), which warns about each bad entry.
    out["commands"] = data.get("commands", [])
    advanced = data.get("advanced")
    out["advanced"] = {k: v for k, v in advanced.items() if isinstance(v, dict)} if isinstance(advanced, dict) else {}
    return out


def model_for(settings):
    """The whisper model dictation uses: the chosen one, or MULTILINGUAL_DEFAULT when that one
    knows only English and the language isn't English."""
    name = settings["model"]
    if WHISPER_MODELS[name][2] == "en" and settings["language"] != "en":
        return MULTILINGUAL_DEFAULT
    return name


def model_path(name):
    return MODELS_DIR / WHISPER_MODELS[name][0]


def model_installed(name):
    """Fetched and checked: models.sh writes FILE.sha256 once the file matches."""
    return model_path(name).is_file() and Path(f"{model_path(name)}.sha256").is_file()


def model_file():
    """The model file whisper-server loads (voice/whisper-run): model_for() the settings, or
    small.en when that one isn't there (ft-voiced says so in its log)."""
    name = model_for(load())
    return model_path(name if model_installed(name) else "small.en")


def save(data):
    """Write the keys the settings app manages (DEFAULT). Every other key, "commands" and
    "advanced" included, keeps its value from the file, so hand edits survive a save."""
    merged = {**read_json(PATH), **{k: data[k] for k in DEFAULT if k in data}}
    write_json(PATH, merged)


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n")
    tmp.replace(path)


def mtime():
    try:
        return PATH.stat().st_mtime_ns
    except FileNotFoundError:
        return 0


def from_frame_voice(old):
    """frame-voice's settings.json as ours, and its keyboard keys as key combinations
    ({combo: action}). Returns (settings, bindings, notes about what didn't carry over)."""
    notes = []
    new = {k: old[k] for k in (*OPTIONS, "wake_phrases", "commands", "controller_mode") if k in old}
    buttons = []
    for b in old.get("controller", []) if isinstance(old.get("controller"), list) else []:
        side = str(b.get("device", "")).rsplit("/", 1)[-1] if isinstance(b, dict) else ""
        name = str(b.get("input", "")).removeprefix("/input/").removesuffix("/click") if isinstance(b, dict) else ""
        if f"{side}/{name}" in VR_BUTTONS:
            buttons.append(f"{side}/{name}")
        else:
            notes.append(f"controller button {b!r}: not a Frame controller button by side; set it on the Voice page")
    if "controller" in old:
        new["controller"] = buttons
    action = "dictate_hold" if old.get("keyboard_mode") == "hold" else "dictate"
    bindings = {}
    for name in old.get("keyboard", []) if isinstance(old.get("keyboard"), list) else []:
        if name in KEY_CODES:
            bindings[str(KEY_CODES[name])] = action
        else:
            notes.append(f"push-to-talk {name}: set it on Frametop Input Settings' Keyboard page "
                         "(mouse buttons: Buttons page)")
    return new, bindings, notes


def set_dictation_keys(rules, bindings):
    """Make bindings ({combo: action}) dictation's keys in the rules, replacing its others. A
    rules file without key combinations of its own keeps the defaults when they're the same."""
    own = rules.get("key_bindings")
    if not isinstance(own, dict):
        if bindings == VOICE_KEY_BINDINGS:
            return False
        own = {**DEFAULT_KEY_BINDINGS, **VOICE_KEY_BINDINGS}
    keys = {c: a for c, a in own.items() if a not in DICTATION_ACTIONS}
    keys.update(bindings)
    if keys == own and isinstance(rules.get("key_bindings"), dict):
        return False
    rules["key_bindings"] = keys
    return True


def install():
    """Run by voice/run.sh install, on the host, after the units are in place. Says what it did."""
    carry_over()
    relay = relay_actions()
    if relay is None:
        print("voice: the input relay isn't running, and it types the text (desktops.sh relay install)")
    elif "dictate" not in relay:
        print("voice: the input relay running now is older than Frametop Voice, so dictation keys and typing\n"
              "       wait until it restarts: with SteamVR after a reboot, or now with\n"
              "       systemctl --user restart frametop-input-relay (in VR the mouse stops for a moment)")


def relay_actions():
    """Have the input relay read its rules again (its default key combinations include
    dictation's once Frametop Voice is installed), then the actions it knows; None when it
    doesn't answer."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.bind("")  # an address to reply to
        sock.settimeout(2)
        sock.sendto(b"reload", "\0frametop_relay")
        sock.recv(4096)
        sock.sendto(b"devices", "\0frametop_relay")
        return json.loads(sock.recv(65536)).get("actions", [])
    except (OSError, ValueError):
        return None
    finally:
        sock.close()


def carry_over():
    """frame-voice's settings, once; or dictation's key in a rules file with keys of its own."""
    rules = read_json(RULES_PATH)
    if not PATH.exists() and FRAME_VOICE_PATH.exists():
        new, bindings, notes = from_frame_voice(read_json(FRAME_VOICE_PATH))
        write_json(PATH, new)
        print(f"voice: carried over frame-voice's settings ({FRAME_VOICE_PATH} -> {PATH})")
        for note in notes:
            print(f"voice: {note}")
        if set_dictation_keys(rules, bindings or {}):
            write_json(RULES_PATH, rules)
            print(f"voice: dictation keys are frame-voice's: {', '.join(sorted(bindings)) or 'none'}")
        return
    own = rules.get("key_bindings")
    if isinstance(own, dict) and not any(a in DICTATION_ACTIONS for a in own.values()) and "97" not in own:
        # A list of its own doesn't get the relay's defaults, so give dictation its key once.
        own.update(VOICE_KEY_BINDINGS)
        write_json(RULES_PATH, rules)
        print("voice: added Right Ctrl tap (dictate) to your key combinations")


if __name__ == "__main__":
    if sys.argv[1:] == ["install"]:
        install()
    elif sys.argv[1:] == ["model-file"]:
        print(model_file())
    else:
        sys.exit(__doc__)
