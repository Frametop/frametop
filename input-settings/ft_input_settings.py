#!/usr/bin/env python3
"""Frametop Input Settings: choose and map input devices for the universal 3D mouse.

A Kirigami (QML) app with a Python backend. It runs in the dev container and talks
to the input relay over its control socket (@frametop_relay):
  - Devices: every USB/Bluetooth mouse and keyboard, a live activity light to
    identify them, and a role for each (3D pointer, pass through, ignore).
  - Buttons: press a button or key on a pointer device, then pick an action.
  - Controllers: the same for the Frame controllers' buttons, minus the gaze actions (gaze
    mode is a mouse feature). They're read by the pointer helper through SteamVR input
    (@ft_pointer_helper: vrstatus, vrglobal), and a mapped button is taken from games.
  - Keyboard: when Frametop's keyboard opens, and key combinations for any action, a command
    of your own too ("command:CMD"), or a modifier tapped alone (a Meta tap opens Steam's menu
    and Meta+Shift+F floats a window, unless the rules have their own list).
  - Pointer: speed, dot size, distance and the rest, applied live.
  - Ignored panels: SteamVR overlays the pointer passes through (POINTER_IGNORE), by app or
    one by one. The helper lists them (@ft_pointer_helper "overlays").
  - Game optimization: pausing Frametop while a VR game runs, so it leaves the CPU and GPU to the game
    (input/game_pause.py, in the relay: "pause on|off|?"): pause now, pause by itself during VR
    games, the controller gesture that pauses and resumes, what happens to the desktop, a sound.
  - Voice: Frametop Voice's dictation (voice/ft-voiced): on or off, its keys (key combinations
    with the dictation actions), controller buttons, typing, sounds, and the wake word. Settings
    go to ~/.config/frametop-voice.json (voice/voice_settings.py); ft-voiced reads them again
    within 2 s, and answers "status" on its control socket.
  - Gaze: the pointer's gaze mode (@ft_pointer_helper "gaze") and the gaze service
    (gaze/ft-gazed, @ft_gazed: status, forget, reload), with its eye tracker (SteamVR's or
    our own) and eye bias (GAZE_TRACKER, GAZE_EYE in frametop.conf).
  - Hands: when hand tracking runs (HANDS_MODE: with the Frametop desktop, always with SteamVR,
    or off; hands/handsmode.py on the host starts and stops its services), the hand cutouts'
    margin (HANDS_MATTE_MARGIN, sent live to ft-screens: "cutouts margin DEG"), and three hand
    inputs, each on its own (handsmode.METHODS): clicks at the gaze (POINTER_HANDS,
    POINTER_GAZE_PINCH), the hand ray with gaze off (POINTER_HAND_RAY), and direct touch on
    Frametop's panels (HANDS_NEAR, sent live to ft-screens: "handinput on|off").
  - Bluetooth: paired devices, and re-applying the Bluetooth LE fixes after pairing.
  - A warning on every page when SteamVR won't load the ft_pointer driver (blocked after a
    crash, disabled, or SteamVR in safe mode) or hasn't loaded it (@ft_pointer doesn't answer
    while SteamVR runs): the cursor still moves, but no click lands. Checked at startup and
    every 30 minutes.
Rules go to ~/.config/frametop-input.json and pointer settings to
~/.config/frametop.conf; then the relay (and through it the helper) reloads.
Launch with input-settings/ft-input-settings (host wrapper).
"""
import fnmatch
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

from PySide6.QtCore import Property, QObject, QSocketNotifier, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

RULES_PATH = os.path.expanduser("~/.config/frametop-input.json")
CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
RELAY = "\0frametop_relay"
HELPER = "\0ft_pointer_helper"
GAZED = "\0ft_gazed"
DRIVER = "\0ft_pointer"  # the ft_pointer driver's control socket, bound while SteamVR has it loaded
# SteamVR's settings; older installs keep them under Steam's config.
VRSETTINGS_PATHS = [os.path.expanduser("~/.config/openvr/config/steamvr.vrsettings"),
                    os.path.expanduser("~/.steam/steam/config/steamvr.vrsettings")]
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAZE_PROBE = os.path.join(REPO, "gaze", "probe", "ft-gazeprobe")
# Hand tracking's mode and the cutouts' margin (standard library only, so it imports here too).
sys.path.insert(0, os.path.join(REPO, "hands"))
import handsmode  # noqa: E402
HANDSMODE = os.path.join(REPO, "hands", "handsmode.py")
SCREENS = "\0ft_screens"
HANDS_MODES = {"frametop": "With Frametop: while the Frametop desktop runs",
               "always": "Always: whenever SteamVR runs, in games and the SteamVR dashboard too",
               "off": "Off"}
# The Hands page's input switches, each on its own (handsmode.METHODS): POINTER_HANDS with
# POINTER_GAZE_PINCH or POINTER_HAND_RAY for the pointer, HANDS_NEAR for ft-screens.
HAND_METHODS = {"gaze_clicks": "Hand clicks in gaze mode", "ray": "Hand ray with gaze off",
                "touch": "Direct touch"}
# Every hand interaction on Frametop's panels, each switched on and off on its own (Hands page): the
# frametop.conf key ft-screens reads at start, sent live too ("handsetting KEY 0|1"), its default, the
# value written for on, and the page's group, label and tooltip.
HAND_INTERACTIONS = [
    # (key, default on, value written for on, group, label, tooltip)
    ("HANDS_TOUCH_DOT", True, "1", "Tapping", "Fingertip dot",
     "A soft white dot on the screen where a tap would land, frozen as the fingertip comes close."),
    ("HANDS_TAP_LOCK", True, "1", "Tapping", "Lock the aim on the way in",
     "The tap lands where the dot froze 1.5 cm out, not where the fingertip's jitter takes it."),
    ("HANDS_MOTION_PRESS", True, "1", "Tapping", "Taps that stop just short",
     "A quick tap that stops or bounces back just before the surface still clicks."),
    ("HANDS_LOCK_PUSH", True, "0.01", "Tapping", "Push through to tap",
     "Pushing the finger 1 cm through the screen taps at the frozen dot, however slowly."),
    ("HANDS_PINCH_CLICK", True, "1", "Tapping", "Pinch on a screen clicks",
     "Up close, a pinch on a screen's content clicks there, and drags while held."),
    ("HANDS_TWO_FINGER_SCROLL", True, "1", "Tapping", "Two-finger scroll",
     "Index and middle finger together on a screen: swipe to scroll, lift without moving to click."),
    ("HANDS_PINCH_BUTTONS", True, "1", "Tapping", "Pinch the bar's buttons",
     "A pinch at a round button under a screen (curve, reset, dock, close) presses it."),
    ("HANDS_GRAB_ROTATE", True, "1", "Moving screens", "Turn with the wrist",
     "A screen held by its bar or edge turns as the wrist turns."),
    ("HANDS_CARD_PINCH", True, "1", "Moving screens", "Card grab",
     "Index and middle finger (or all fingertips) pinching an edge hold the screen like a card."),
    ("HANDS_FIST_GRAB", True, "1", "Moving screens", "Fists",
     "A fist at a screen holds it; two fists resize it."),
    ("HANDS_FIST_PAIR", True, "1", "Moving screens", "Two fists near a screen resize it",
     "Two closed hands anywhere near one screen start a resize, without a grip at the screen first."),
    ("HANDS_FIST_RESIZE_ONLY", True, "1", "Moving screens", "Fists only resize",
     "Fists don't move or turn screens (the tracking's too rough for placing): one holds still, two resize."),
    ("HANDS_STRETCH_TURN", True, "1", "Moving screens", "Two hands turn a screen",
     "Holding a screen with both hands turns it on every axis with them."),
    ("HANDS_RELEASE_FREEZE", True, "1", "Moving screens", "Let go without a nudge",
     "A held screen stops following as the fingers start to open, so letting go doesn't move it."),
    ("HANDS_GRAB_THAT", True, "1", "Gestures", "Grab that",
     "Reach toward a far screen with an open hand, then pull back into a fist: it comes to arm's reach."),
    ("HANDS_PUSH", True, "1", "Gestures", "Push away",
     "An open palm pushed into a near screen sends it 3 m out."),
    ("HANDS_FLICK", True, "1", "Gestures", "Flick away",
     "A quick outward flick of an open hand at a screen sends it 3 m out."),
    ("HANDS_THROW", True, "1", "Gestures", "Throw",
     "Letting go of a held screen while moving it fast throws it 3 m out along the throw."),
    ("HANDS_SWEEP", True, "1", "Gestures", "Sweep for the lazy susan",
     "An open hand on its edge, palm leading, swept 25 cm sideways, quickly: the next screen on that side "
     "(Meta+Alt+Tab too). The return stroke does nothing: the other hand palms the other way."),
    ("SCREEN_FRONT_AWAY", False, "1", "Gestures", "A brought screen goes home when you look away",
     "Off: it stays in front of you until you move it yourself or the next stroke sends it home."),
    ("HANDS_SWEEP_BACK", False, "1", "Gestures", "Lazy susan on the return stroke too",
     "A stroke with the back of the hand leading counts too (one hand going back and forth)."),
    ("SCREEN_SPIN_BRING", True, "1", "Gestures", "Lazy susan brings screens to you",
     "The next screen comes 1 m in front of you, big enough to read, and takes the typing; the next stroke sends "
     "it home. Grab that pulls it to arm's reach; a push steps it back. Off: the screens turn around you instead."),
    ("HANDS_PALM_MENU", True, "1", "Gestures", "Palm menu",
     "Palm toward your face, away from screens, and a plain pinch: the Steam menu."),
    ("HANDS_TARGET_GLOW", True, "1", "Feedback", "Grab that's glow",
     "A faint white glow round the screen \"grab that\" would take."),
    ("HANDS_EDGE_GLOW", True, "1", "Feedback", "Resize and edge glows",
     "A glow at the corner or edge a pinch there would resize or move."),
    ("POINTER_GAZE_TWO_HAND", True, "1", "Gaze + hands", "Two-hand drag (like Meta+J, then Meta+K)",
     "With gaze on: hold a pinch on one hand (nudge it into place), then pinch with the other: the left button "
     "goes down where the dot is; the drag lasts while either pinch is held, and the hand still pinching drags."),
    ("POINTER_GAZE_RIGHT_DRAG", False, "1", "Gaze + hands", "Right-drag: tap one hand, then pinch the other",
     "With gaze on: an index tap on one hand, then an index pinch on the other within a second, holds the right "
     "button where you look; that hand drags until you let go."),
    ("POINTER_TYPING_HIDE", True, "1.5", "Gaze + hands", "Hide the gaze pointer while typing",
     "The gaze dot and its laser hide for 1.5 s after each key."),
    ("POINTER_PINCH_MAX_SPEED", True, "1.0", "Gaze + hands", "No clicks from moving hands",
     "A pinch that begins while the hand moves fast (a lazy susan stroke, a wave) doesn't click."),
    ("POINTER_GAZE_SCROLL", True, "1", "Gaze + hands", "Three-finger pinch scrolls",
     "With gaze on: thumb on both the index and middle fingertips, then move the hand up, down or across: it "
     "scrolls where you looked."),
    ("POINTER_GAZE_CHORD", False, "1", "Gaze + hands", "Hold both buttons (chord)",
     "With gaze on: while one hand's pinch is held, the other hand's pinch holds the right button too "
     "(replaced by the right-drag)."),
    ("POINTER_RAY_BEAM", False, "1", "Laser", "Show the hand laser's beam",
     "Off: a hand's laser shows only its dot. On: the whole beam from the hand."),
    ("POINTER_RAY_READY", True, "6", "Laser", "Laser only when about to pinch",
     "A hand's laser shows once its thumb and index are within 6 cm, and goes when they open past 10 cm. "
     "Off: whenever the hand is raised and pointing."),
    ("POINTER_RAY_FIST_CURL", True, "1.1", "Laser", "No laser or clicks in a fist",
     "Closing the hand drops its laser, and a pinch made while the hand closes into a fist doesn't click."),
    ("POINTER_PINCH_NO_RAY", False, "1", "Laser", "Pinch with no laser clicks at the pointer",
     "With no laser up, a pinch or a fist presses where the pointer is and drags it (an air mouse). "
     "Off: a pinch with no laser does nothing."),
    ("HANDS_PUSH_GLOW", False, "1", "Feedback", "Push hint glow",
     "A dim glow on a near screen an open palm faces (a push or flick would send it away)."),
    ("HANDS_TRACK_GATE", True, "1", "Feedback", "Steady through tracking glitches",
     "A held screen ignores impossible jumps of the hand and holds still when the hand leaves the cameras' view."),
]
sys.path.insert(0, os.path.join(REPO, "voice"))
import voice_commands  # noqa: E402  Frametop Voice's commands and settings (standard library)
import voice_settings  # noqa: E402

VOICE_UNIT = "frametop-voice.service"
VOICE_SOCKET = f"/run/user/{os.getuid()}/frametop-voice/control.sock"  # ft-voiced's, owner-only
BTN_MISC = 0x100
# Header names that mark the start of a range, not a real key (BTN_MOUSE == BTN_LEFT).
RANGE_ALIASES = {"BTN_MISC", "BTN_MOUSE", "BTN_JOYSTICK", "BTN_GAMEPAD", "BTN_DIGI", "BTN_WHEEL",
                 "BTN_TRIGGER_HAPPY", "KEY_MIN_INTERESTING", "KEY_MAX", "KEY_CNT", "BTN_A", "BTN_B", "BTN_X", "BTN_Y"}
DEFAULT_BUTTONS = {0x110: "left", 0x111: "right", 0x112: "middle", 0x113: "back", 0x114: "back"}
ACTION_LABELS = {
    "left": "Left click", "right": "Right click", "middle": "Middle click", "back": "Back",
    "scroll_up": "Scroll up", "scroll_down": "Scroll down", "dashboard": "Toggle SteamVR dashboard",
    "steam_menu": "Open Steam menu / close dashboard",
    "recenter": "Recenter pointer", "pointer_toggle": "Pointer on/off",
    "follow_toggle": "Head follow on/off (experimental)", "gaze_toggle": "Gaze pointer on/off (experimental)",
    "gaze_precision": "Gaze precision: hold to steer, release to click",
    "gaze_drag": "Gaze drag: press where you look, steer, release",
    "gaze_left": "Gaze left click: tap, or hold and turn your head to aim",
    "gaze_right": "Gaze right click: tap, or hold and turn your head to aim",
    "gaze_quickcal": "Gaze quick check (one dot)",
    "sens_up": "Faster pointer",
    "sens_down": "Slower pointer", "layout_reset": "Reset desktop screen layout",
    "screens_toggle": "Hide/show desktop screens", "keyboard_toggle": "Open/close keyboard",
    "float_toggle": "Float window in VR / put it back", "dock_all": "Put all floating windows back",
    "pause_toggle": "Pause/resume Frametop (for VR games)",
    "dictate": "Dictation: start or stop",
    "dictate_hold": "Dictation: while held",
    "spin_next": "Spin the panels: next one on the right to the front",
    "spin_prev": "Spin the panels: next one on the left to the front",
    "spotwatch": "Spotwatch: search apps and remote displays",
    "key": "Pass through as key",
    "none": "Do nothing",
}
# When Frametop's keyboard opens ("vr_keyboard" in the rules; the relay's
# VR_KEYBOARD_MODES). "no_keyboard" is the default.
VR_KEYBOARD_MODES = {
    "always": "When a text field is selected",
    "no_keyboard": "When a text field is selected and no keyboard is connected",
    "button": "Only with a mapped mouse or controller button",
    "never": "Never",
}
ROLE_LABELS = {"pointer": "3D pointer", "passthrough": "Pass through", "ignore": "Ignore"}
# Frame controller buttons the pointer helper can read (pointer/helper/vrbuttons.h). The
# system button stays SteamVR's.
CONTROLLER_BUTTONS = {
    "left/view": "Left View", "left/dpad_up": "Left D-pad up", "left/dpad_down": "Left D-pad down",
    "left/dpad_left": "Left D-pad left", "left/dpad_right": "Left D-pad right", "left/bumper": "Left bumper",
    "left/trigger": "Left trigger", "left/grip": "Left grip", "left/thumbstick": "Left stick click",
    "right/menu": "Right Menu", "right/a": "Right A", "right/b": "Right B", "right/x": "Right X", "right/y": "Right Y",
    "right/bumper": "Right bumper", "right/trigger": "Right trigger", "right/grip": "Right grip",
    "right/thumbstick": "Right stick click",
}
# Pausing for VR games (input/game_pause.py): the gesture a rules file without "pause_gesture" gets,
# both thumbsticks clicked together twice, and what can happen to the desktop.
DEFAULT_PAUSE_GESTURE = {"buttons": ["left/thumbstick", "right/thumbstick"], "presses": 2}
PAUSE_DESKTOP_MODES = {"hide": "Hide it and slow it down (windows stay open)",
                       "close": "Close it (windows close; it starts again when you resume)"}
PAUSE_REASONS = {"game": "a VR game started", "gesture": "controller gesture", "command": "ft-pause",
                 "settings": "from here"}
# Gaze mode is a mouse and keyboard feature (docs/gaze-controllers.md; the relay's GAZE_ACTIONS).
GAZE_ACTIONS = ("gaze_toggle", "gaze_precision", "gaze_drag", "gaze_left", "gaze_right", "gaze_quickcal")
# Dictation's controller buttons are on the Voice page: read from SteamVR's web socket, they stay
# the games' too (a Controllers mapping would take them).
VOICE_ACTIONS = voice_settings.DICTATION_ACTIONS
CONTROLLER_ACTIONS = [a for a in ACTION_LABELS if a not in ("key", "none") + GAZE_ACTIONS + VOICE_ACTIONS]
# Key combinations take any action but key and none; the keyboard clicks only work there.
SHORTCUT_ACTIONS = [a for a in ACTION_LABELS if a not in ("key", "none")]
KEYBOARD_ONLY = ("gaze_left", "gaze_right")
# Profiles (docs/profiles.md): "profile:NAME" opens one (the relay runs ft-layout use NAME).
LAYOUT_PATH = os.path.expanduser("~/.config/frametop-layout.json")
PROFILE = "profile:"
# "command:CMD": the relay runs CMD with sh -c (key combinations only, here).
COMMAND = "command:"


def profile_actions():
    """One action per profile (named layout), for buttons, controllers, and key combinations."""
    names = sorted(read_json(LAYOUT_PATH).get("layouts", {}), key=str.casefold)
    return [PROFILE + n for n in names]


def action_label(a):
    if a.startswith(PROFILE):
        return f"Open profile {a[len(PROFILE):]}"
    if a.startswith(COMMAND):
        return f"Run: {a[len(COMMAND):]}"
    return ACTION_LABELS.get(a, a)


def is_profile(a):
    return a.startswith(PROFILE) and len(a) > len(PROFILE)


def is_command(a):
    return a.startswith(COMMAND) and bool(a[len(COMMAND):].strip())


def mappable(a):
    """An action a controller button can have."""
    return a in CONTROLLER_ACTIONS or is_profile(a)


def shortcut_mappable(a):
    """An action a key combination can have: the gaze ones and commands too."""
    return a in SHORTCUT_ACTIONS or is_profile(a) or is_command(a)
# Gaze mode settings (pointer helper), like POINTER_SETTINGS.
GAZE_SETTINGS = [
    ("POINTER_GAZE_RETAKE", "Look away to hand back", 5, 1, 45, 0.5, "°"),
    ("POINTER_GAZE_NUDGE_MAX", "Largest correction to learn", 55, 1, 110, 1, "°"),
    ("POINTER_GAZE_HOLD", "Hold still to drag", 0.5, 0.1, 2.0, 0.05, "s"),
    ("POINTER_GAZE_SHOW", "Dot shows after moving", 1.0, 0.0, 5.0, 0.1, "s"),
    # with hand tracking (POINTER_HANDS): how far a held pinch moves before it nudges, and how fast
    ("POINTER_GAZE_PINCH_DEADZONE", "Pinch nudge starts after", 4.0, 1.0, 10.0, 0.5, "°"),
    ("POINTER_GAZE_PINCH_GAIN", "Pinch nudge speed", 0.3, 0.1, 1.0, 0.05, "×"),
]
# In gaze mode, what the mouse's left button does (POINTER_GAZE_MOUSE).
GAZE_MOUSE = {"precision": "Gaze precision: hold to steer with the mouse, release to click",
              "direct": "Click right away where the pointer is"}
# Key combinations ("key_bindings" in the rules): modifiers, either side folded into the left code.
MODIFIER_CODES = {29: 29, 97: 29, 42: 42, 54: 42, 56: 56, 100: 56, 125: 125, 126: 125}
MODIFIER_NAMES = {29: "Ctrl", 42: "Shift", 56: "Alt", 125: "Meta"}
# A tap of a right-hand modifier is bound by its own code: that key alone (the relay's modifier_tap).
RIGHT_MODIFIER_NAMES = {97: "Right Ctrl", 54: "Right Shift", 100: "Right Alt", 126: "Right Meta"}
# What a rules file without "key_bindings" gets (the relay's DEFAULT_KEY_BINDINGS): a Meta tap
# opens Steam's menu, Meta+J and Meta+K click at the gaze, Meta+Shift+F floats a window,
# Meta+Alt+Tab and Meta+Alt+Shift+Tab spin the panels, Meta+Space opens spotwatch.
DEFAULT_KEY_BINDINGS = {"125": "steam_menu", "125+36": "gaze_left", "125+37": "gaze_right",
                        "42+125+33": "float_toggle", "56+125+15": "spin_next", "42+56+125+15": "spin_prev",
                        "125+57": "spotwatch"}


def voice_installed():
    return os.path.exists(os.path.expanduser(f"~/.config/systemd/user/{VOICE_UNIT}"))


def key_bindings(rules):
    """The rules' key combinations, or the defaults if it has none of its own (an empty list
    counts as its own): with Frametop Voice installed, a Right Ctrl tap dictates too."""
    bound = rules.get("key_bindings")
    if isinstance(bound, dict):
        return dict(bound)
    return {**DEFAULT_KEY_BINDINGS, **(voice_settings.VOICE_KEY_BINDINGS if voice_installed() else {})}
# The gaze service's settings (gaze/ft-gazed): whose eye tracking, and the eye bias. GAZE_TRACKER
# can also be auto (the default): ours when it's installed, else SteamVR's.
GAZE_TRACKERS = {"steam": "SteamVR's eye tracker", "own": "our own eye tracker"}
# Our tracker's frame grabber (gaze/tracker/install.sh), on the host: this runs in the dev
# container, which has the host's /etc under /run/host.
EYEGRAB = ("/etc/frametop/ft-eyegrab", "/run/host/etc/frametop/ft-eyegrab")
GAZE_EYES = {"auto": "auto", "left": "left eye", "right": "right eye"}
# Pointer settings: key, label, default, min, max, step, unit.
POINTER_SETTINGS = [
    ("POINTER_SENSITIVITY", "Speed", 0.03, 0.005, 0.12, 0.001, "°/count"),
    ("POINTER_CURSOR_DEG", "Dot size", 0.4, 0.1, 2.0, 0.05, "°"),
    ("POINTER_DISTANCE", "Distance in open space", 1.5, 0.5, 4.0, 0.1, "m"),
    ("POINTER_ORIGIN_FRACTION", "SteamVR dot shrink", 0.95, 0.5, 0.98, 0.01, ""),
    ("POINTER_ORIGIN_MARGIN", "Room for small controls", 0.15, 0.03, 0.5, 0.01, "m"),
    ("POINTER_SCENE_RADIUS", "Dock / window-control reach", 0.5, 0.1, 1.5, 0.05, "m"),
    ("POINTER_EDGE_REACH", "Panel edge reach", 0.3, 0.0, 1.0, 0.05, "m"),
    ("POINTER_LEASH_DEG", "Head follow leash", 10, 0, 60, 1, "°"),
    ("POINTER_LEASH_DELAY", "Head follow delay", 0.2, 0.0, 1.0, 0.05, "s"),
    ("POINTER_LEASH_RETURN", "Head follow catch-up", 0.2, 0.05, 2.0, 0.05, "s"),
    ("POINTER_FOLLOW_REACH", "Head follow reach", 70, 20, 85, 1, "°"),
    ("POINTER_WAKE_COUNTS", "Movement to wake", 40, 5, 200, 5, "counts"),
    ("POINTER_IDLE", "Release after idle", 30, 5, 120, 5, "s"),
    ("POINTER_CONTROLLER_PICKUP", "Controller movement to take over", 1.0, 0.5, 5.0, 0.1, "×"),
]
# Overlay keys (shell patterns) the pointer passes through, comma-separated.
IGNORE_KEY = "POINTER_IGNORE"


def overlay_app(key):
    """The app an overlay key belongs to, by the vendor.app.overlay convention."""
    return ".".join(key.split(".")[:2])


def code_names():
    """evdev key/button code -> name, from the kernel header (first name wins, BTN_ for buttons)."""
    names = {}
    try:
        with open("/usr/include/linux/input-event-codes.h") as f:
            for m in re.finditer(r"#define\s+((?:KEY|BTN)_\w+)\s+(0x[0-9a-fA-F]+|\d+)", f.read()):
                code = int(m.group(2), 0)
                name = m.group(1)
                if name in RANGE_ALIASES:
                    continue
                if code not in names or (code >= BTN_MISC and name.startswith("BTN_") and not names[code].startswith("BTN_")):
                    names[code] = name
    except OSError:
        pass
    return names


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def read_conf():
    conf = {}
    try:
        with open(CONF_PATH) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def write_conf_value(key, value):
    """Set KEY=value in frametop.conf, keeping comments and the rest of the file."""
    try:
        with open(CONF_PATH) as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    for i, line in enumerate(lines):
        if line.split("#", 1)[0].strip().startswith(f"{key}="):
            comment = line[line.index("#"):] if "#" in line else ""
            lines[i] = f"{key}={value}" + (f"   {comment}" if comment else "")
            break
    else:
        lines.append(f"{key}={value}")
    with open(CONF_PATH, "w") as f:
        f.write("\n".join(lines) + "\n")


def host(*cmd):
    """Run a command on the SteamOS host (we live in the dev container)."""
    runner = ["distrobox-host-exec"] if shutil.which("distrobox-host-exec") else []
    try:
        return subprocess.run(runner + list(cmd), capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired) as e:
        return subprocess.CompletedProcess(cmd, 1, "", str(e))


def driver_block():
    """Why SteamVR won't load the ft_pointer driver: "blocked" (safe mode blocked it after a
    crash), "disabled" (turned off in Manage Add-Ons), "safemode" (SteamVR in safe mode, every
    add-on off) or "" (nothing stops it). SteamVR reads these at startup."""
    for path in VRSETTINGS_PATHS:
        if os.path.exists(path):
            settings = read_json(path)
            break
    else:
        return ""
    section = lambda name: settings.get(name) if isinstance(settings.get(name), dict) else {}
    if not isinstance(settings, dict):
        return ""
    driver = section("driver_ft_pointer")
    if driver.get("blocked_by_safe_mode") is True:
        return "blocked"
    if driver.get("enable") is False:
        return "disabled"
    if section("steamvr").get("enableSafeMode") is True:
        return "safemode"
    return ""


class Backend(QObject):
    devicesChanged = Signal()
    mappingsChanged = Signal()
    pointerChanged = Signal()
    bluetoothChanged = Signal()
    controllersChanged = Signal()
    gazeChanged = Signal()
    driverChanged = Signal()
    panelsChanged = Signal()
    pauseChanged = Signal()
    handsChanged = Signal()
    voiceChanged = Signal()
    activity = Signal(str)  # device id
    captured = Signal(int, str)  # code, name
    capturedController = Signal(str, str)  # button, label
    shortcutCaptureChanged = Signal()
    message = Signal(str, bool)  # text, is error

    def __init__(self):
        super().__init__()
        self._names = code_names()
        self._nodes = []
        self._pointer_mode = False
        self._capture_id = ""
        self._bluetooth = []
        self._relay_ok = False
        self._capture_vr = False
        self._capture_combo = ""  # the action a key combination is being captured for
        self._combo_mods = set()
        self._combo_tap = None  # a modifier pressed alone, nothing since: released, it's a tap
        self._combo_tap_code = None  # that modifier's key as it is (Right Ctrl: 97)
        self._voice = {}  # ft-voiced's status, {} when it isn't running
        self._voice_enabled = False  # its service starts at login (systemctl, refreshVoice)
        self._vr = {}  # the helper's vrstatus, {} when it doesn't answer
        self._vr_at = 0.0
        self._gaze = {}  # ft-gazed's status, {} when it isn't running
        self._gaze_prev = None  # the status before, for rates
        self._gaze_at = 0.0
        self._gaze_mode = None  # the helper's gaze mode: True, False, None (no answer)
        self._check_asked = 0.0  # when quickcal or calibrate went to the gaze service (its errors)
        self._driver_block = ""  # set by _check_driver
        self._panels = None  # SteamVR's overlays, from the helper; None until it answers
        self._pause = {}  # the relay's pause state ("pause ?"), {} when it doesn't answer
        self._pause_at = 0.0
        self._hands = {}  # handsmode.py's status, {} until it answers
        self._hands_lock = threading.Lock()  # one handsmode.py at a time
        self._hands_refreshing = False
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("")  # autobind an abstract address the relay can reply to
        self.sock.setblocking(False)
        self.notifier = QSocketNotifier(self.sock.fileno(), QSocketNotifier.Read)
        self.notifier.activated.connect(self._read)
        self.poll = QTimer(interval=2000, timeout=self._refresh)
        self.poll.start()
        self.rewatch = QTimer(interval=50000, timeout=lambda: self._send("watch 60"))
        self.rewatch.start()
        self.reload_timer = QTimer(singleShot=True, interval=400, timeout=lambda: self._send("reload"))
        # The driver only changes with a SteamVR restart, so a rare check is enough. The first one
        # waits a moment for the helper's vrstatus answer, which tells us SteamVR is up.
        self.driver_timer = QTimer(interval=30 * 60 * 1000, timeout=self._check_driver)
        self.driver_timer.start()
        QTimer.singleShot(3000, self._check_driver)
        self._refresh()
        self._send("watch 60")
        self.refreshBluetooth()

    # --- relay socket ---
    def _send(self, text, to=RELAY):
        try:
            self.sock.sendto(text.encode(), to)
            return True
        except OSError:
            if to == RELAY and self._relay_ok:
                self._relay_ok = False
                self.devicesChanged.emit()
            return False

    def _refresh(self):
        self._send("devices")
        self._send("pause ?")
        self._send("vrstatus", HELPER)
        self._send("gaze ?", HELPER)
        if not self._send("status", GAZED) and self._gaze:
            self._gaze, self._gaze_prev = {}, None
            self.gazeChanged.emit()
        now = time.monotonic()
        # No answer for a while: that side isn't running (any more).
        if self._vr and now - self._vr_at > 5:
            self._vr = {}
            self.controllersChanged.emit()
        if self._gaze_mode is not None and now - self._gaze_at > 5:
            self._gaze_mode = None
            self.gazeChanged.emit()
        if self._pause and now - self._pause_at > 5:
            self._pause = {}
            self.pauseChanged.emit()
        voice = self._voice_status() if voice_installed() else {}
        if voice != self._voice:
            self._voice = voice
            self.voiceChanged.emit()

    def _check_driver(self):
        block = driver_block()
        if not block and self._vr and not self._send("ping", DRIVER):
            block = "unloaded"  # SteamVR runs (the helper answers) without the driver
        if block != self._driver_block:
            self._driver_block = block
            self.driverChanged.emit()

    def _read(self):
        while True:
            try:
                data = self.sock.recv(65536)
            except BlockingIOError:
                return
            text = data.decode(errors="replace")
            if text.startswith("error ") and self._check_asked and time.monotonic() - self._check_asked < 3:
                self._check_asked = 0.0  # the gaze service's answer to quickcal or calibrate
                self.message.emit("Gaze check: " + text[6:], True)
                continue
            if text in ("ok on", "ok off"):  # the helper's answer to "gaze ?" (from an unbound socket)
                self._gaze_mode = text == "ok on"
                self._gaze_at = time.monotonic()
                self.gazeChanged.emit()
                continue
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if isinstance(msg, dict) and "samples" in msg and "t" not in msg:  # ft-gazed's status
                self._gaze_status(msg)
                continue
            if not isinstance(msg, dict):
                continue
            t = msg.get("t")
            if t == "devices":
                self._nodes = msg.get("nodes", [])
                self._pointer_mode = bool(msg.get("pointer_mode"))
                was_ok, self._relay_ok = self._relay_ok, True
                if not was_ok:
                    self.pauseChanged.emit()  # (pauseStatus' "old")
                self.devicesChanged.emit()
            elif t == "overlays":
                panels = [o for o in msg.get("list", []) if isinstance(o, dict) and isinstance(o.get("key"), str)]
                if panels != self._panels:
                    self._panels = panels
                    self.panelsChanged.emit()
            elif t == "pause":
                self._pause_at = time.monotonic()
                if msg != self._pause:
                    self._pause = msg
                    self.pauseChanged.emit()
            elif t == "vrstatus":
                self._vr = msg
                self._vr_at = time.monotonic()
                self.controllersChanged.emit()
            elif t == "event" and msg.get("type") == "vr":
                self.activity.emit(msg["id"])
                if self._capture_vr and msg["value"] == 1 and msg["code"] in CONTROLLER_BUTTONS:
                    self._capture_vr = False
                    self._send("vrcapture 0")
                    self.capturedController.emit(msg["code"], CONTROLLER_BUTTONS[msg["code"]])
            elif t == "event" and self._capture_combo and msg.get("type") == "key":
                self.activity.emit(msg["id"])
                code, value = int(msg["code"]), msg["value"]
                if code in MODIFIER_CODES:
                    mod = MODIFIER_CODES[code]
                    if value == 1:
                        self._combo_tap = None if self._combo_mods else mod
                        self._combo_tap_code = code
                    (self._combo_mods.add if value else self._combo_mods.discard)(mod)
                    if value == 0 and self._combo_tap == mod:
                        # The relay's modifier tap: a right-hand key by its own code, that key alone.
                        tapped = self._combo_tap_code
                        self._end_shortcut_capture(str(tapped if tapped in RIGHT_MODIFIER_NAMES else mod))
                elif value == 1 and code < BTN_MISC:
                    self._end_shortcut_capture("+".join(str(c) for c in sorted(self._combo_mods) + [code]))
                elif value == 1:
                    self._combo_tap = None  # a mouse button: no tap
            elif t == "event":
                self.activity.emit(msg["id"])
                if (self._capture_id and msg["id"] == self._capture_id and msg["type"] == "key"
                        and msg["value"] == 1):
                    code = int(msg["code"])
                    self._capture_id = ""
                    self.captured.emit(code, self.codeName(code))

    # --- SteamVR driver ---
    @Property(str, notify=driverChanged)
    def driverBlock(self):
        """driver_block(), or "unloaded": SteamVR runs without the driver (unblocked since it
        started, or not installed)."""
        return self._driver_block

    # --- devices ---
    @Property(bool, notify=devicesChanged)
    def relayRunning(self):
        return self._relay_ok

    @Property(bool, notify=devicesChanged)
    def pointerMode(self):
        return self._pointer_mode

    @Property("QVariantList", notify=devicesChanged)
    def devices(self):
        """Connected devices, plus devices with saved rules or bindings that aren't connected
        (a sleeping Bluetooth mouse), so their bindings can still be edited and removed."""
        all_rules = read_json(RULES_PATH)
        rules = all_rules.get("devices", {})
        bindings = all_rules.get("buttons", {})
        grouped = {}
        for n in self._nodes:
            d = grouped.setdefault(n["id"], {"id": n["id"], "name": n["name"], "bus": n["bus"], "kinds": [],
                                             "nodes": [], "role": n["role"], "grabbed": False,
                                             "uinput": n.get("uinput", False)})
            d["nodes"].append(n["path"])
            d["kinds"] = sorted(set(d["kinds"]) | set(n["kinds"]))
            d["grabbed"] = d["grabbed"] or n["grabbed"]
            if "mouse" in n["kinds"]:
                d["name"] = n["name"].replace(" Mouse", "")  # the Z3's nodes are "Z3 Mouse"/"Z3 Keyboard"
        for d in grouped.values():
            d["connected"] = True
        for device_id in set(rules) | {i for i, b in bindings.items() if b}:
            if device_id in grouped:
                continue
            rule = rules.get(device_id, {})
            # Not connected: the relay's default for a device we have bindings for is "pointer".
            grouped[device_id] = {"id": device_id, "name": rule.get("name", device_id), "bus": "",
                                  "kinds": [], "nodes": [], "role": rule.get("role", "pointer"),
                                  "grabbed": False, "connected": False}
        for d in grouped.values():
            d["explicit"] = d["id"] in rules and "role" in rules[d["id"]]
            d["roleLabel"] = ROLE_LABELS.get(d["role"], d["role"])
        return sorted(grouped.values(), key=lambda d: (not d["connected"], d["role"] != "pointer", d["name"].lower()))

    @Property("QVariantList", constant=True)
    def roles(self):
        return [{"value": k, "text": v} for k, v in ROLE_LABELS.items()]

    @Slot(str, str, str)
    def setRole(self, device_id, role, name):
        rules = read_json(RULES_PATH)
        rules.setdefault("devices", {})[device_id] = {"role": role, "name": name}
        self._save_rules(rules)
        self.message.emit(f"{name}: {ROLE_LABELS.get(role, role)}", False)

    @Slot(str)
    def resetRole(self, device_id):
        rules = read_json(RULES_PATH)
        rules.get("devices", {}).get(device_id, {}).pop("role", None)
        self._save_rules(rules)

    @Slot(str)
    def forgetDevice(self, device_id):
        """Drop everything saved for a device: role, name, and bindings."""
        rules = read_json(RULES_PATH)
        name = rules.get("devices", {}).pop(device_id, {}).get("name", device_id)
        rules.get("buttons", {}).pop(device_id, None)
        self._save_rules(rules)
        self.message.emit(f"Forgot {name}", False)

    def _remember_name(self, rules, device_id):
        """Keep the device's name with its rules, so it can be listed while disconnected."""
        entry = rules.setdefault("devices", {}).setdefault(device_id, {})
        if "name" not in entry:
            for d in self.devices:
                if d["id"] == device_id:
                    entry["name"] = d["name"]

    def _save_rules(self, rules):
        os.makedirs(os.path.dirname(RULES_PATH), exist_ok=True)
        with open(RULES_PATH, "w") as f:
            json.dump(rules, f, indent=2)
        self._send("reload")
        QTimer.singleShot(300, self._refresh)
        self.mappingsChanged.emit()
        self.devicesChanged.emit()

    # --- buttons ---
    @Slot(int, result=str)
    def codeName(self, code):
        return self._names.get(code, f"code {code}")

    @Property("QVariantList", constant=True)
    def actions(self):
        return [{"value": k, "text": v} for k, v in ACTION_LABELS.items() if k not in KEYBOARD_ONLY] + \
            [{"value": a, "text": action_label(a)} for a in profile_actions()]

    @Slot(str, result="QVariantList")
    def mappings(self, device_id):
        custom = read_json(RULES_PATH).get("buttons", {}).get(device_id, {})
        rows = {}
        for code, action in DEFAULT_BUTTONS.items():
            rows[code] = {"code": code, "action": action, "custom": False}
        for code, action in custom.items():
            rows[int(code)] = {"code": int(code), "action": action, "custom": True}
        out = []
        for code in sorted(rows):
            r = rows[code]
            r["isDefault"] = code in DEFAULT_BUTTONS  # a built-in binding (left/right/middle/side/extra)
            r["name"] = self.codeName(code)
            r["actionLabel"] = action_label(r["action"])
            out.append(r)
        return out

    @Slot(str)
    def startCapture(self, device_id):
        self._capture_id = device_id
        self._send("watch 60")

    @Slot()
    def cancelCapture(self):
        self._capture_id = ""

    @Slot(str, int, str)
    def setMapping(self, device_id, code, action):
        rules = read_json(RULES_PATH)
        rules.setdefault("buttons", {}).setdefault(device_id, {})[str(code)] = action
        self._remember_name(rules, device_id)
        self._save_rules(rules)
        self.message.emit(f"{self.codeName(code)} → {action_label(action)}", False)

    @Slot(str, int)
    def removeMapping(self, device_id, code):
        """Drop a custom binding: built-in buttons go back to their default, others pass through."""
        rules = read_json(RULES_PATH)
        rules.get("buttons", {}).get(device_id, {}).pop(str(code), None)
        self._save_rules(rules)
        default = DEFAULT_BUTTONS.get(code)
        self.message.emit(f"{self.codeName(code)}: " + (f"back to {ACTION_LABELS[default]}" if default
                                                         else "binding removed"), False)

    @Slot(str, int)
    def unbind(self, device_id, code):
        """Make a button do nothing (also works for the built-in bindings)."""
        self.setMapping(device_id, code, "none")

    @Slot(str)
    def clearMappings(self, device_id):
        rules = read_json(RULES_PATH)
        removed = len(rules.get("buttons", {}).pop(device_id, {}) or {})
        self._save_rules(rules)
        self.message.emit(f"Removed {removed} binding{'s' if removed != 1 else ''}; defaults restored", False)

    # --- pointer settings ---
    @Property("QVariantList", notify=pointerChanged)
    def pointerSettings(self):
        conf = read_conf()
        out = []
        for key, label, default, lo, hi, step, unit in POINTER_SETTINGS:
            try:
                value = float(conf.get(key, default))
            except ValueError:
                value = default
            out.append({"key": key, "label": label, "value": value, "min": lo, "max": hi, "step": step,
                        "unit": unit, "default": default})
        return out

    @Slot(str, float)
    def setPointerSetting(self, key, value):
        integer = key in ("POINTER_WAKE_COUNTS", "POINTER_IDLE", "POINTER_LEASH_DEG", "POINTER_FOLLOW_REACH")
        write_conf_value(key, str(int(round(value))) if integer else f"{value:.3f}".rstrip("0").rstrip("."))
        self.reload_timer.start()  # debounce slider drags
        self.pointerChanged.emit()

    @Property(bool, notify=pointerChanged)
    def pointerFollow(self):
        return read_conf().get("POINTER_FOLLOW", "0") not in ("", "0")

    @Slot(bool)
    def setPointerFollow(self, on):
        write_conf_value("POINTER_FOLLOW", "1" if on else "0")
        self.reload_timer.start()
        self.pointerChanged.emit()

    @Slot(result=bool)
    def recenter(self):
        return self._send("recenter", HELPER)

    # --- ignored panels ---
    @staticmethod
    def _ignore_list():
        return [p.strip() for p in read_conf().get(IGNORE_KEY, "").split(",") if p.strip()]

    def _save_ignore(self, entries, text):
        write_conf_value(IGNORE_KEY, ", ".join(entries))
        # Only the helper reads it; the relay passes "reload" on only in pointer mode.
        self._send("reload", HELPER)
        self.panelsChanged.emit()
        self.message.emit(text, False)

    def _open_panels(self):
        """The helper's overlays, minus Frametop's own: ignoring a screen would leave nothing
        to click this app on with the mouse."""
        return [o for o in self._panels or [] if not o["key"].startswith("frametop.")]

    @Slot()
    def refreshPanels(self):
        """Ask the helper for the overlay list; it answers once it has listed them again."""
        self._send("overlays", HELPER)

    @Property(bool, notify=panelsChanged)
    def panelsLoaded(self):
        return self._panels is not None

    @Property("QVariantList", notify=panelsChanged)
    def panelGroups(self):
        """Open overlays by app: {app, title, appIgnored, anyVisible, anyIgnored, panels: [{key,
        name, visible, ignoredBy}]}. ignoredBy is the entry that ignores it ("" if none)."""
        entries = self._ignore_list()
        groups = {}
        for o in self._open_panels():
            key = o["key"]
            app = overlay_app(key)
            g = groups.setdefault(app, {"app": app, "title": app, "panels": []})
            name = o.get("name") or key
            if key == app:
                g["title"] = name
            g["panels"].append({"key": key, "name": name, "visible": bool(o.get("visible")),
                                "ignoredBy": next((p for p in entries if fnmatch.fnmatchcase(key, p)), "")})
        for g in groups.values():
            g["appIgnored"] = g["app"] + "*" in entries
            g["anyVisible"] = any(p["visible"] for p in g["panels"])
            g["anyIgnored"] = any(p["ignoredBy"] for p in g["panels"])
            g["panels"].sort(key=lambda p: (not p["visible"], p["key"]))
        return sorted(groups.values(), key=lambda g: (not g["anyVisible"], g["title"].lower()))

    @Property("QVariantList", notify=panelsChanged)
    def ignoreOrphans(self):
        """Entries that match no open overlay (the app isn't running), so they can be removed."""
        keys = [o["key"] for o in self._open_panels()]
        return [p for p in self._ignore_list() if not any(fnmatch.fnmatchcase(k, p) for k in keys)]

    @Slot(str, bool)
    def setPanelIgnored(self, key, on):
        entries = [p for p in self._ignore_list() if p != key]
        if on:
            entries.append(key)
        self._save_ignore(entries, f"{key}: " + ("the pointer passes through it" if on else "the pointer lands on it again"))

    @Slot(str, bool)
    def setAppIgnored(self, app, on):
        pattern = app + "*"
        entries = [p for p in self._ignore_list() if p != pattern]
        if on:
            entries.append(pattern)
        self._save_ignore(entries, f"{app}: " + ("the pointer passes through all its panels" if on
                                                 else "no longer ignored as a whole"))

    @Slot(str)
    def removeIgnore(self, pattern):
        self._save_ignore([p for p in self._ignore_list() if p != pattern], f"{pattern}: no longer ignored")

    # --- Frametop's keyboard ---
    @Property("QVariantList", constant=True)
    def vrKeyboardModes(self):
        return [{"value": k, "text": v} for k, v in VR_KEYBOARD_MODES.items()]

    @Property(str, notify=mappingsChanged)
    def vrKeyboard(self):
        mode = read_json(RULES_PATH).get("vr_keyboard")
        return mode if mode in VR_KEYBOARD_MODES else "no_keyboard"

    @Property(bool, notify=mappingsChanged)
    def vrKeyboardPersist(self):
        return bool(read_json(RULES_PATH).get("vr_keyboard_persist", True))

    @Slot(bool)
    def setVrKeyboardPersist(self, on):
        rules = read_json(RULES_PATH)
        rules["vr_keyboard_persist"] = bool(on)
        self._save_rules(rules)
        self.message.emit("Keyboard: " + ("stays open until you hide it" if on else "closes with the text field"), False)

    @Slot(str)
    def setVrKeyboard(self, mode):
        if mode not in VR_KEYBOARD_MODES:
            return
        rules = read_json(RULES_PATH)
        rules["vr_keyboard"] = mode
        self._save_rules(rules)
        self.message.emit(f"Keyboard: {VR_KEYBOARD_MODES[mode].lower()}", False)

    # --- controllers ---
    @Property("QVariantList", constant=True)
    def controllerActions(self):
        return [{"value": a, "text": action_label(a)} for a in CONTROLLER_ACTIONS + profile_actions()]

    @Property("QVariantList", constant=True)
    def controllerButtons(self):
        return [{"value": b, "text": label} for b, label in CONTROLLER_BUTTONS.items()]

    @Property("QVariantList", notify=mappingsChanged)
    def controllerMappings(self):
        mapped = read_json(RULES_PATH).get("controller_buttons", {})
        return [{"button": b, "label": label, "action": mapped[b],
                 "actionLabel": action_label(mapped[b])}
                for b, label in CONTROLLER_BUTTONS.items() if b in mapped]

    @Property("QVariantMap", notify=controllersChanged)
    def controllerStatus(self):
        """helper: answering; manifest: its SteamVR input is set up; global: SteamVR's
        "Enable global input from overlays"; active: mapped buttons SteamVR delivers now."""
        vr = self._vr
        return {"helper": bool(vr), "manifest": bool(vr.get("manifest")), "global": bool(vr.get("global")),
                "inGame": bool(vr.get("in_game")), "bound": vr.get("bound", []), "active": vr.get("active", [])}

    @Property(bool, notify=mappingsChanged)
    def controllerInGames(self):
        return bool(read_json(RULES_PATH).get("controller_in_games"))

    @Slot(bool)
    def setControllerInGames(self, on):
        rules = read_json(RULES_PATH)
        rules["controller_in_games"] = bool(on)
        self._save_rules(rules)
        self.message.emit("Mapped controller buttons: " + ("taken in games too" if on else "left to games"), False)

    @Slot(str, str)
    def setControllerMapping(self, button, action):
        if button not in CONTROLLER_BUTTONS or not mappable(action):
            return
        rules = read_json(RULES_PATH)
        rules.setdefault("controller_buttons", {})[button] = action
        self._save_rules(rules)
        self.message.emit(f"{CONTROLLER_BUTTONS[button]} → {action_label(action)}", False)

    @Slot(str)
    def removeControllerMapping(self, button):
        rules = read_json(RULES_PATH)
        rules.get("controller_buttons", {}).pop(button, None)
        self._save_rules(rules)
        self.message.emit(f"{CONTROLLER_BUTTONS.get(button, button)}: back to games", False)

    @Slot()
    def clearControllerMappings(self):
        rules = read_json(RULES_PATH)
        removed = len(rules.pop("controller_buttons", {}) or {})
        self._save_rules(rules)
        self.message.emit(f"Removed {removed} controller binding{'s' if removed != 1 else ''}", False)

    @Slot()
    def startControllerCapture(self):
        self._capture_vr = True
        self._send("watch 60")
        self._send("vrcapture 30")

    @Slot()
    def cancelControllerCapture(self):
        if self._capture_vr:
            self._capture_vr = False
            self._send("vrcapture 0")

    @Slot(bool)
    def setGlobalInput(self, on):
        """SteamVR's "Enable global input from overlays (Experimental)", which the helper needs
        to get controller buttons while a game or the dashboard has focus."""
        if self._send(f"vrglobal {'on' if on else 'off'}", HELPER):
            self.message.emit(f"SteamVR global input from overlays {'on' if on else 'off'}", False)
        else:
            self.message.emit("The pointer helper isn't running (frametop-pointer.service)", True)

    # --- pausing for VR games ---
    @Property("QVariantMap", notify=pauseChanged)
    def pauseStatus(self):
        """The relay's pause state, and a line saying it ("relay": False when it doesn't answer)."""
        st = self._pause
        if not st:
            # "old": the relay answers, but not about pausing (it predates it).
            return {"relay": False, "old": self._relay_ok, "paused": False,
                    "summary": "Unknown: the input relay " + ("can't pause" if self._relay_ok else "isn't answering")}
        if st.get("paused"):
            reason = PAUSE_REASONS.get(st.get("reason"), st.get("reason") or "")
            summary = f"Paused since {time.strftime('%H:%M', time.localtime(st.get('since') or time.time()))}"
            summary += f" ({reason})" if reason else ""
            if st.get("ends_with_game"):
                summary += ", until the game ends"
        else:
            summary = "Running" + (", with a VR game" if st.get("game") else "")
        if st.get("busy"):
            summary += "; working on it…"
        return {"relay": True, "paused": bool(st.get("paused")), "summary": summary,
                "gestureReader": bool(st.get("gesture_reader"))}

    @Slot(bool)
    def setPaused(self, on):
        if self._send(f"pause {'on' if on else 'off'} settings"):
            QTimer.singleShot(300, self._refresh)
        else:
            self.message.emit("The input relay isn't running (frametop-input-relay.service)", True)

    def _set_rule(self, key, value, text):
        rules = read_json(RULES_PATH)
        rules[key] = value
        self._save_rules(rules)
        self.message.emit(text, False)

    @Property(bool, notify=mappingsChanged)
    def pauseAuto(self):
        return read_json(RULES_PATH).get("pause_auto", True) is not False

    @Slot(bool)
    def setPauseAuto(self, on):
        self._set_rule("pause_auto", bool(on), "VR games pause Frametop" if on else "VR games no longer pause Frametop")

    @Property("QVariantList", constant=True)
    def pauseDesktopModes(self):
        return [{"value": k, "text": v} for k, v in PAUSE_DESKTOP_MODES.items()]

    @Property(str, notify=mappingsChanged)
    def pauseDesktop(self):
        mode = read_json(RULES_PATH).get("pause_desktop")
        return mode if mode in PAUSE_DESKTOP_MODES else "hide"

    @Slot(str)
    def setPauseDesktop(self, mode):
        if mode in PAUSE_DESKTOP_MODES:
            self._set_rule("pause_desktop", mode, "Paused, the desktop: " + PAUSE_DESKTOP_MODES[mode].split(" (")[0].lower())

    @Property(bool, notify=mappingsChanged)
    def pauseSound(self):
        return read_json(RULES_PATH).get("pause_sound", True) is not False

    @Slot(bool)
    def setPauseSound(self, on):
        self._set_rule("pause_sound", bool(on), "A sound on pause and resume" if on else "No sound on pause and resume")

    @Property("QVariantList", constant=True)
    def gestureButtons(self):
        return [{"value": "", "text": "None"}] + [{"value": b, "text": label} for b, label in CONTROLLER_BUTTONS.items()]

    @Property("QVariantMap", notify=mappingsChanged)
    def pauseGesture(self):
        """{"first", "second" ("" for none), "presses"} (the relay's game_pause.gesture_of)."""
        spec = read_json(RULES_PATH).get("pause_gesture", DEFAULT_PAUSE_GESTURE)
        buttons = [b for b in (spec or {}).get("buttons", []) if b in CONTROLLER_BUTTONS][:2] \
            if isinstance(spec, dict) and isinstance(spec.get("buttons"), list) else []
        presses = 1 if len(buttons) == 2 and spec.get("presses") == 1 else 2
        return {"first": buttons[0] if buttons else "", "second": buttons[1] if len(buttons) > 1 else "",
                "presses": presses}

    @Slot(str, str, int)
    def setPauseGesture(self, first, second, presses):
        """No first button: no gesture. One button always takes two presses."""
        buttons = list(dict.fromkeys(b for b in (first, second) if b in CONTROLLER_BUTTONS))
        if not buttons:
            self._set_rule("pause_gesture", None, "No controller gesture pauses Frametop")
            return
        presses = 1 if len(buttons) == 2 and presses == 1 else 2
        self._set_rule("pause_gesture", {"buttons": buttons, "presses": presses},
                       "Pause gesture: " + self.gestureText(buttons, presses))

    @staticmethod
    def gestureText(buttons, presses):
        names = " and ".join(CONTROLLER_BUTTONS[b] for b in buttons)
        together = " together" if len(buttons) == 2 else ""
        return f"{names}{together}, {'twice' if presses == 2 else 'once'}"

    @Property(str, notify=mappingsChanged)
    def pauseGestureText(self):
        g = self.pauseGesture
        buttons = [b for b in (g["first"], g["second"]) if b]
        return self.gestureText(buttons, g["presses"]) if buttons else "None"

    # --- gaze ---
    def _gaze_status(self, status):
        now = time.monotonic()
        prev = self._gaze_prev
        # Rates over the last poll: samples per second, and the share with only one eye.
        if prev and now - prev[0] > 0.5 and status["samples"] >= prev[1]["samples"]:
            n = status["samples"] - prev[1]["samples"]
            status["rate"] = n / (now - prev[0])
            status["one_eye_share"] = (status["one_eye"] - prev[1]["one_eye"]) / n if n else 0.0
            for key in ("lost_left", "lost_right"):
                if key in status and key in prev[1]:
                    status[key + "_share"] = (status[key] - prev[1][key]) / n if n else 0.0
        elif self._gaze:
            for key in ("rate", "one_eye_share", "lost_left_share", "lost_right_share"):
                status.setdefault(key, self._gaze.get(key))
        if not prev or now - prev[0] > 0.5:
            self._gaze_prev = (now, status)
        self._gaze = status
        self.gazeChanged.emit()

    @Property("QVariantMap", notify=gazeChanged)
    def gazeStatus(self):
        return self._gaze

    @Property(bool, notify=gazeChanged)
    def gazeServiceRunning(self):
        return bool(self._gaze)

    @Property(bool, notify=gazeChanged)
    def gazeServiceInstalled(self):
        return os.path.exists(os.path.expanduser("~/.config/systemd/user/frametop-gaze.service"))

    @Property(int, notify=gazeChanged)
    def gazeMode(self):
        """The helper's gaze mode now: 1 on, 0 off, -1 no answer (helper not running)."""
        return -1 if self._gaze_mode is None else int(self._gaze_mode)

    @Property(bool, notify=gazeChanged)
    def gazeDefault(self):
        return read_conf().get("POINTER_GAZE", "0") not in ("", "0")

    @Slot(bool)
    def setGazeMode(self, on):
        """On or off now and from now on (POINTER_GAZE); a mapped button toggles it until restart."""
        write_conf_value("POINTER_GAZE", "1" if on else "0")
        self._send(f"gaze {'on' if on else 'off'}", HELPER)
        self.reload_timer.start()
        self.gazeChanged.emit()

    @Property(str, notify=pointerChanged)
    def gazeMouse(self):
        v = read_conf().get("POINTER_GAZE_MOUSE", "precision")
        return v if v in GAZE_MOUSE else "precision"

    @Property("QVariantList", constant=True)
    def gazeMouseChoices(self):
        return [{"value": k, "text": v} for k, v in GAZE_MOUSE.items()]

    @Slot(str)
    def setGazeMouse(self, mode):
        if mode in GAZE_MOUSE:
            write_conf_value("POINTER_GAZE_MOUSE", mode)
            self.reload_timer.start()
            self.pointerChanged.emit()
            self.message.emit(f"Mouse in gaze mode: {GAZE_MOUSE[mode].lower()}", False)

    @Property(bool, notify=pointerChanged)
    def gazeMouseHeld(self):
        return read_conf().get("POINTER_GAZE_MOUSE_MOVE", "held") != "free"

    @Slot(bool)
    def setGazeMouseHeld(self, on):
        write_conf_value("POINTER_GAZE_MOUSE_MOVE", "held" if on else "free")
        self.reload_timer.start()
        self.pointerChanged.emit()
        self.message.emit("Mouse in gaze mode: " + ("moves the pointer only while a button is held" if on
                                                    else "moves the pointer any time"), False)

    @Property(bool, notify=pointerChanged)
    def gazeDotAlways(self):
        return read_conf().get("POINTER_GAZE_DOT", "always") != "moving"

    @Slot(bool)
    def setGazeDotAlways(self, on):
        write_conf_value("POINTER_GAZE_DOT", "always" if on else "moving")
        self.reload_timer.start()
        self.pointerChanged.emit()
        self.message.emit("Gaze dot: " + ("always shown" if on else "shown only while the mouse moves it"), False)

    # --- key combinations ("key_bindings") ---
    def comboName(self, combo):
        parts = [int(c) for c in combo.split("+") if c.isdigit()]
        if len(parts) == 1 and parts[0] in MODIFIER_NAMES:
            return MODIFIER_NAMES[parts[0]] + " tap"
        if len(parts) == 1 and parts[0] in RIGHT_MODIFIER_NAMES:
            return RIGHT_MODIFIER_NAMES[parts[0]] + " tap"
        return "+".join(MODIFIER_NAMES.get(c) or self.codeName(c).removeprefix("KEY_").title() for c in parts)

    @Property("QVariantList", notify=mappingsChanged)
    def keyShortcuts(self):
        bound = key_bindings(read_json(RULES_PATH))
        return [{"combo": c, "label": self.comboName(c), "action": a, "actionLabel": action_label(a)}
                for c, a in sorted(bound.items())]

    @Property("QVariantList", constant=True)
    def shortcutActions(self):
        return [{"value": a, "text": action_label(a)} for a in SHORTCUT_ACTIONS + profile_actions()] + \
            [{"value": COMMAND, "text": "Run a command…"}]

    @Property(bool, notify=shortcutCaptureChanged)
    def capturingShortcut(self):
        return bool(self._capture_combo)

    @Slot(str)
    def startShortcutCapture(self, action):
        if shortcut_mappable(action):
            self._capture_combo = action
            self._combo_mods = set()
            self._combo_tap = None
            self._send("watch 60")
            self.shortcutCaptureChanged.emit()

    @Slot()
    def cancelShortcutCapture(self):
        self._capture_combo = ""
        self.shortcutCaptureChanged.emit()

    def _end_shortcut_capture(self, combo):
        self._save_shortcut(combo, self._capture_combo)
        self._capture_combo = ""
        self._combo_mods = set()
        self._combo_tap = None
        self.shortcutCaptureChanged.emit()

    def _save_shortcut(self, combo, action):
        rules = read_json(RULES_PATH)
        rules["key_bindings"] = dict(key_bindings(rules), **{combo: action})
        self._save_rules(rules)
        self.message.emit(f"{self.comboName(combo)} → {action_label(action)}", False)

    @Slot(str)
    def removeShortcut(self, combo):
        rules = read_json(RULES_PATH)
        rules["key_bindings"] = key_bindings(rules)
        rules["key_bindings"].pop(combo, None)
        self._save_rules(rules)
        self.message.emit(f"{self.comboName(combo)} removed", False)

    # --- voice dictation (Frametop Voice) ---
    def _voice_status(self):
        """ft-voiced's "status" ({} when it isn't running)."""
        try:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(0.3)
                s.connect(VOICE_SOCKET)
                s.sendall(b"status\n")
                return json.loads(s.makefile().readline())
        except (OSError, ValueError):
            return {}

    @Slot()
    def refreshVoice(self):
        """Whether the service starts at login: asked of the host's systemd, so only when the
        Voice page opens and after a change, not every 2 s."""
        self._voice_enabled = host("systemctl", "--user", "is-enabled", VOICE_UNIT).stdout.strip() == "enabled"
        self.voiceChanged.emit()

    @Property(bool, notify=voiceChanged)
    def voiceInstalled(self):
        return voice_installed()

    @Property(bool, notify=voiceChanged)
    def voiceEnabled(self):
        return self._voice_enabled

    @Property("QVariantMap", notify=voiceChanged)
    def voiceStatus(self):
        v = self._voice
        return {"running": bool(v), "dictating": bool(v.get("dictating")), "wake": bool(v.get("wake_enabled")),
                "mic": bool(v.get("mic_open"))}

    @Slot(bool)
    def setVoiceOn(self, on):
        result = host("systemctl", "--user", "enable" if on else "disable", "--now", VOICE_UNIT)
        if result.returncode == 0:
            self.message.emit("Voice dictation " + ("on" if on else "off, and it won't start at login"), False)
        else:
            self.message.emit("Couldn't turn voice dictation " + ("on: " if on else "off: ")
                              + (result.stderr.strip() or "systemctl failed")[:160], True)
        self.refreshVoice()

    @Property("QVariantMap", notify=voiceChanged)
    def voiceSettings(self):
        s = voice_settings.load()
        return {k: s[k] for k in voice_settings.DEFAULT}

    def _set_voice(self, key, value, text):
        s = voice_settings.load()
        s[key] = value
        voice_settings.save(s)
        self.message.emit(text, False)
        self.voiceChanged.emit()

    @Slot(str, bool)
    def setVoiceOption(self, key, on):
        texts = {"live_typing": ("Typing as you talk", "Typing when you stop talking"),
                 "sounds": ("A tone when dictation starts and stops", "No dictation sounds"),
                 "wake_word": ("Listening for the wake word", "Wake word off")}
        if key in texts:
            self._set_voice(key, bool(on), texts[key][0 if on else 1])

    @Property("QVariantList", constant=True)
    def voiceModes(self):
        return [{"value": "hold", "text": "Hold to talk"},
                {"value": "toggle", "text": "Press to start, press again to stop"}]

    @Slot(str)
    def setVoiceControllerMode(self, mode):
        if mode in voice_settings.MODES:
            self._set_voice("controller_mode", mode, "Controller: " + ("hold to talk" if mode == "hold"
                                                                     else "press to start and stop"))

    @Property("QVariantList", notify=voiceChanged)
    def voiceButtons(self):
        return [{"button": b, "label": CONTROLLER_BUTTONS.get(b, b)} for b in voice_settings.load()["controller"]]

    @Slot(str)
    def addVoiceButton(self, button):
        buttons = voice_settings.load()["controller"]
        if button in CONTROLLER_BUTTONS and button not in buttons:
            self._set_voice("controller", buttons + [button], f"Dictation: {CONTROLLER_BUTTONS[button]}")

    @Slot(str)
    def removeVoiceButton(self, button):
        buttons = voice_settings.load()["controller"]
        if button in buttons:
            self._set_voice("controller", [b for b in buttons if b != button],
                            f"{CONTROLLER_BUTTONS.get(button, button)}: no longer dictation")

    @Slot(str)
    def setWakePhrases(self, text):
        phrases = list(dict.fromkeys(p for p in (voice_settings.clean_phrase(t) for t in re.split(r"[,\n]", text)) if p))
        if phrases and phrases != voice_settings.load()["wake_phrases"]:
            self._set_voice("wake_phrases", phrases, "Wake word: " + ", ".join(p.title() for p in phrases))

    @Property("QVariantList", notify=mappingsChanged)
    def dictationKeys(self):
        return [{"combo": c, "label": self.comboName(c), "actionLabel": action_label(a)}
                for c, a in sorted(key_bindings(read_json(RULES_PATH)).items()) if a in VOICE_ACTIONS]

    @Property("QVariantList", notify=voiceChanged)
    def voiceCommands(self):
        commands, _ = voice_commands.parse(voice_settings.load()["commands"])
        return [{"say": " / ".join(p for p, _ in c.phrases), "run": " ".join(c.run)} for c in commands]

    @Property(str, notify=gazeChanged)
    def gazeTracker(self):
        """Whose eye tracking the gaze service uses: "steam" or "own" (GAZE_TRACKER; auto is
        ours when it's installed)."""
        v = read_conf().get("GAZE_TRACKER", "auto")
        if v in GAZE_TRACKERS:
            return v
        return "own" if self.gazeOwnInstalled else "steam"

    @Property(bool, notify=gazeChanged)
    def gazeTrackerAuto(self):
        """GAZE_TRACKER is auto (or missing): ours once it's installed, without a choice here."""
        return read_conf().get("GAZE_TRACKER", "auto") not in GAZE_TRACKERS

    @Property(bool, notify=gazeChanged)
    def gazeOwnInstalled(self):
        """Is our own tracker installed? The gaze service says (it also checks ft-eyes' Python in
        its checkout); without it, whether the frame grabber is."""
        if "own_installed" in self._gaze:
            return bool(self._gaze["own_installed"])
        return any(os.path.exists(p) for p in EYEGRAB)

    @Property(str, notify=gazeChanged)
    def gazeEye(self):
        """The eye bias: "auto", "left" or "right" (GAZE_EYE)."""
        v = read_conf().get("GAZE_EYE", "auto")
        return v if v in GAZE_EYES else "auto"

    @Slot(str)
    def setGazeTracker(self, tracker):
        if tracker in GAZE_TRACKERS:
            self._set_gaze("GAZE_TRACKER", tracker, f"Gaze from {GAZE_TRACKERS[tracker]}")

    @Slot(str)
    def setGazeEye(self, eye):
        if eye in GAZE_EYES:
            self._set_gaze("GAZE_EYE", eye, f"Eye bias: {GAZE_EYES[eye]}")

    def _set_gaze(self, key, value, done):
        """ft-gazed reads these from frametop.conf; "reload" makes it do so now."""
        write_conf_value(key, value)
        running = self._send("reload", GAZED)
        self.message.emit(done if running else f"{done} (the gaze service isn't running: it takes it when it starts)",
                          False)
        self.gazeChanged.emit()

    @Property("QVariantList", notify=pointerChanged)
    def gazeSettings(self):
        conf = read_conf()
        out = []
        for key, label, default, lo, hi, step, unit in GAZE_SETTINGS:
            try:
                value = float(conf.get(key, default))
            except ValueError:
                value = default
            out.append({"key": key, "label": label, "value": value, "min": lo, "max": hi, "step": step,
                        "unit": unit, "default": default})
        return out

    @Slot()
    def forgetGazeLessons(self):
        if self._send("forget", GAZED):
            self.message.emit("Forgot what the pointer's nudges taught; the calibration stays", False)
        else:
            self.message.emit("The gaze service isn't running (frametop-gaze.service)", True)

    @Slot()
    def reloadGazeCalibration(self):
        if self._send("reload", GAZED):
            self.message.emit("The gaze service read the calibration again", False)
        else:
            self.message.emit("The gaze service isn't running (frametop-gaze.service)", True)

    @Slot()
    def keepGazeAwake(self):
        """The Gaze page is open: the gaze service doesn't idle meanwhile (its status stays live,
        and a check opens without waiting for the tracker). The page renews this every 10 s."""
        self._send("wake 30", GAZED)

    @Slot()
    def gazeQuickCheck(self):
        """The gaze service's one-dot check, in the panel fixed to the headset."""
        self._gaze_check("quickcal", "Quick check: look at the dot in front of you")

    @Slot()
    def gazeCalibrate(self):
        """The full calibration in the panel fixed to the headset (gaze/gazecheck.py)."""
        self._gaze_check("calibrate", "Calibration: look at each dot in the headset; right click or Meta+K stops")

    def _gaze_check(self, command, done):
        if self._send(command, GAZED):
            self._check_asked = time.monotonic()
            self.message.emit(done, False)
        else:
            self.message.emit("The gaze service isn't running (frametop-gaze.service)", True)

    @Slot()
    def gazeFitCheck(self):
        """The headset fit check, in the panel fixed to the headset (gaze/gazecheck.py)."""
        self._gaze_check("fitcheck", "Headset fit: in the headset, adjust it while you watch; right click or Meta+K closes it")

    @Slot()
    def openGazeProbe(self):
        """ft-gazeprobe, the gaze tracking's development tool (a GTK app on the host, fullscreen on
        a Frametop screen). Day to day, the checks and the calibration run in the headset panel."""
        self._open_probe([], "Opening the gaze probe (a development tool)")

    def _open_probe(self, args, done):
        runner = ["distrobox-host-exec"] if shutil.which("distrobox-host-exec") else []
        env = [f"{k}={os.environ[k]}" for k in ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS")
               if os.environ.get(k)]
        try:
            subprocess.Popen(runner + ["env"] + env + [GAZE_PROBE] + args, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            self.message.emit(done, False)
        except OSError as e:
            self.message.emit(f"Couldn't open the gaze probe: {e}", True)

    # --- hands ---
    @Property("QVariantList", constant=True)
    def handsModes(self):
        return [{"value": k, "text": v} for k, v in HANDS_MODES.items()]

    @Property(str, notify=handsChanged)
    def handsMode(self):
        return handsmode.mode_of(read_conf())

    @Property("QVariantMap", notify=handsChanged)
    def handsStatus(self):
        """handsmode.py's status: "summary", "problem" (why it can't run, or ""), and the rest."""
        return self._hands

    def _hands_run(self, *args):
        """handsmode.py on the host (systemctl, getcap and the cameras are there), off the UI
        thread: distrobox-host-exec takes a moment. Then its status."""
        with self._hands_lock:
            result = host("python3", HANDSMODE, *args) if args else None
            st = host("python3", HANDSMODE, "status", "--json")
            try:
                self._hands = json.loads(st.stdout)
            except ValueError:
                self._hands = {"summary": "", "problem": "Couldn't ask the host about hand tracking: "
                               + (st.stderr.strip() or st.stdout.strip() or "no answer")[:160]}
            self._hands_refreshing = False
        self.handsChanged.emit()
        return result

    @Slot()
    def refreshHands(self):
        if not self._hands_refreshing:
            self._hands_refreshing = True
            threading.Thread(target=self._hands_run, daemon=True).start()

    @Slot(str)
    def setHandsMode(self, mode):
        """Written here so the page follows at once; handsmode.py set writes the same and applies it
        (starts and stops the services), and the input relay rereads it (games keep the hands with
        HANDS_MODE=always)."""
        if mode not in HANDS_MODES:
            return
        write_conf_value("HANDS_MODE", mode)
        self.handsChanged.emit()
        self.reload_timer.start()

        def job():
            result = self._hands_run("set", mode)
            if result.returncode != 0:
                self.message.emit("Couldn't apply it: " + (result.stderr.strip() or "no answer")[:160], True)
            else:
                self.message.emit("Hand tracking: " + HANDS_MODES[mode].split(":")[0], False)
        threading.Thread(target=job, daemon=True).start()

    @Property("QVariantMap", notify=handsChanged)
    def handMethods(self):
        """{method: on} for the Hands page's input switches (handsmode.methods_of)."""
        return handsmode.methods_of(read_conf())

    @Slot(str, bool)
    def setHandMethod(self, method, on):
        """One hand input on or off, the other two left as they are (handsmode.method_writes). The
        helper and the relay reread frametop.conf; ft-screens hears direct touch now."""
        if method not in HAND_METHODS:
            return
        writes = handsmode.method_writes(read_conf(), method, on)
        for key, value in writes.items():
            write_conf_value(key, value)
        if "HANDS_NEAR" in writes:
            self._send("handinput " + ("off" if writes["HANDS_NEAR"] == "0" else "on"), SCREENS)
        self.reload_timer.start()
        self.handsChanged.emit()
        self.message.emit(f"{HAND_METHODS[method]}: {'on' if on else 'off'}", False)

    @Property("QVariantList", notify=handsChanged)
    def handInteractions(self):
        """The Hands page's interaction switches, in order: key, label, tip, group (the first of each
        group carries it, for the section's label), on."""
        conf, out, last = read_conf(), [], None
        for key, default, _, group, label, tip in HAND_INTERACTIONS:
            raw = conf.get(key, "").strip()
            on = default if raw == "" else raw not in ("0", "0.0", "off", "no")
            out.append({"key": key, "label": label, "tip": tip, "group": group if group != last else "", "on": on})
            last = group
        return out

    @Slot(str, bool)
    def setHandInteraction(self, key, on):
        """One interaction on or off: frametop.conf (ft-screens reads it at start) and ft-screens now
        (the pointer helper's POINTER_ ones: it reloads its settings)."""
        entry = next((e for e in HAND_INTERACTIONS if e[0] == key), None)
        if entry is None:
            return
        write_conf_value(key, entry[2] if on else "0")
        if key.startswith("POINTER_"):   # the pointer helper's: it re-reads frametop.conf
            self._send("reload", HELPER)
        else:
            self._send(f"handsetting {key} {1 if on else 0}", SCREENS)
        self.handsChanged.emit()
        self.message.emit(f"{entry[4]}: {'on' if on else 'off'}", False)

    @Property("QVariantMap", constant=True)
    def handsMarginRange(self):
        return {"min": handsmode.MARGIN_MIN, "max": handsmode.MARGIN_MAX, "step": handsmode.MARGIN_STEP,
                "default": handsmode.MARGIN_DEFAULT}

    @Property(float, notify=handsChanged)
    def handsMargin(self):
        return handsmode.margin_of(read_conf())

    @Slot(float)
    def setHandsMargin(self, value):
        """HANDS_MATTE_MARGIN, and to ft-screens now (only the still margin: its motion part stays).
        Without the desktop running it takes it when it starts."""
        value = format(round(min(max(value, 0.0), 10.0), 2), "g")
        write_conf_value("HANDS_MATTE_MARGIN", value)
        self._send(f"cutouts margin {value}", SCREENS)
        self.handsChanged.emit()

    # --- bluetooth ---
    @Property("QVariantList", notify=bluetoothChanged)
    def bluetooth(self):
        return self._bluetooth

    @Slot()
    def refreshBluetooth(self):
        devices = []
        listing = host("bluetoothctl", "devices", "Paired")
        for line in listing.stdout.splitlines():
            parts = line.split(" ", 2)
            if len(parts) < 3 or parts[0] != "Device":
                continue
            info = host("bluetoothctl", "info", parts[1]).stdout
            battery = re.search(r"Battery Percentage: \S+ \((\d+)\)", info)
            devices.append({"address": parts[1], "name": parts[2],
                            "connected": "Connected: yes" in info,
                            "battery": int(battery.group(1)) if battery else -1})
        self._bluetooth = devices
        self.bluetoothChanged.emit()

    @Slot()
    def applyBluetoothFixes(self):
        if not os.path.exists("/run/host/etc/steamframe/bt-fixups.sh") and not os.path.exists("/etc/steamframe/bt-fixups.sh"):
            # The unit outlives the script: SteamOS updates keep /etc's units, not what they run.
            if any(os.path.exists(r + "/etc/systemd/system/steamframe-bt-fixups.service") for r in ("/run/host", "")):
                self.message.emit("A SteamOS update deleted the Bluetooth fixes: reinstall them with "
                                  "setup/bluetooth/install.sh install", True)
            else:
                self.message.emit("Bluetooth fixes aren't installed (setup/bluetooth/install.sh)", True)
            return
        result = host("pkexec", "/etc/steamframe/bt-fixups.sh")
        if result.returncode == 0:
            self.message.emit("Bluetooth fixes applied: " + " ".join(result.stdout.split())[:120], False)
        else:
            self.message.emit("Couldn't apply the Bluetooth fixes: " + (result.stderr.strip() or "cancelled")[:160], True)
        self.refreshBluetooth()


def main():
    app = QGuiApplication(sys.argv)
    app.setApplicationName("ft-input-settings")
    app.setApplicationDisplayName("Frametop Input Settings")
    app.setDesktopFileName("ft-input-settings")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle("org.kde.desktop")
    engine = QQmlApplicationEngine()
    backend = Backend()
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", os.environ.get("FT_INPUT_PAGE", "devices"))
    engine.load(QUrl.fromLocalFile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.qml")))
    if not engine.rootObjects():
        sys.exit(1)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
