#!/usr/bin/env python3
"""Frametop Tour: a walk through what Frametop does, the first time the desktop starts, and
whenever it's opened again (the app menu, frunner, SteamVR's Launch a program list, the Tour
entry in Frametop Input Settings, or tour/ft-tour).

A Kirigami (QML) app with a Python backend, like Frametop Input Settings, in Frametop's
container. Each page is a topic: what to do and what it does, as a table, buttons that try it or
open its settings, and on the gaze and hands pages a target to practise on. The pages follow
what's installed (gaze mode, hand tracking, voice dictation) and the keyboard page lists the
key combinations the input relay has now (Frametop Input Settings' own key_bindings()).

Done or not is DONE_PATH. The session opens the tour (tour/ft-tour --first-run) when the
desktop starts and it isn't there; closing the tour, skipping it, or finishing it writes it, and
`tour/ft-tour --reset` (or "Show this tour when the desktop starts" on its last page) takes it
away again.
"""
import json
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "input-settings"))

# The flag. Not under XDG_STATE_HOME: the desktop sets that to its own folder.
DONE_PATH = os.path.expanduser("~/.local/state/frametop/tour.json")
TOUR_VERSION = 2  # a tour with new pages can show itself again by raising this (2: videos, 2026-10-20)
UNITS = os.path.expanduser("~/.config/systemd/user")
CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
SOCKETS = {"gazed": "\0ft_gazed", "frunner": "\0ft_frunner", "screens": "\0ft_screens",
           "float": "\0frametop_float"}
INPUT_SETTINGS = os.path.join(REPO, "input-settings", "ft_input_settings.py")
DISPLAY_SETTINGS = os.path.join(REPO, "display-settings", "ft_display_settings.py")
REMOTE_DISPLAYS = os.path.join(REPO, "remote-displays", "ft_remote_displays.py")


# ---------------------------------------------------------------- the flag

def is_done(path=None):
    try:
        with open(path or DONE_PATH) as f:
            return int(json.load(f).get("version", 0)) >= TOUR_VERSION
    except (OSError, ValueError, AttributeError, TypeError):
        return False


def mark_done(path=None, how="finished"):
    path = path or DONE_PATH
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"version": TOUR_VERSION, "how": how, "when": time.strftime("%Y-%m-%d %H:%M:%S")}, f)
        f.write("\n")
    os.replace(tmp, path)


def reset(path=None):
    try:
        os.remove(path or DONE_PATH)
    except FileNotFoundError:
        pass


# ---------------------------------------------------------------- what's here

def read_conf(path=CONF_PATH):
    conf = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def features(units=UNITS, conf=None):
    """What's installed and on: gaze mode, hand tracking (and hand interaction), voice."""
    conf = read_conf() if conf is None else conf
    has = lambda unit: os.path.exists(os.path.join(units, unit))
    return {"gaze": has("frametop-gaze.service"), "gazeOn": conf.get("POINTER_GAZE", "0") == "1",
            "hands": has("frametop-hands.service"), "handsPointer": conf.get("POINTER_HANDS", "0") == "1",
            "voice": has("frametop-voice.service"), "floats": conf.get("FLOAT_SLOTS", "16") != "0"}


MODIFIER_ORDER = (125, 29, 56, 42)  # Meta, Ctrl, Alt, Shift


def combo_name(combo, names, modifiers, right_modifiers):
    """A key_bindings combination as people write it: "Meta+Shift+F", "Meta tap"."""
    parts = [int(c) for c in combo.split("+") if c.isdigit()]
    if len(parts) == 1 and parts[0] in modifiers:
        return modifiers[parts[0]] + " tap"
    if len(parts) == 1 and parts[0] in right_modifiers:
        return right_modifiers[parts[0]] + " tap"
    # Meta first, as the README writes them (Input Settings goes by code: Shift+Meta+F)
    parts.sort(key=lambda c: (MODIFIER_ORDER.index(c) if c in MODIFIER_ORDER else len(MODIFIER_ORDER)))
    return "+".join(modifiers.get(c) or names.get(c, str(c)).removeprefix("KEY_").title() for c in parts)


# Order on the keyboard page: the everyday ones first.
BINDING_ORDER = ("steam_menu", "frunner", "gaze_left", "gaze_right", "float_toggle", "spin_next", "spin_prev",
                 "dictate", "dictate_hold", "pause_toggle", "hands_toggle", "screens_toggle", "layout_reset")


def bindings(bound, label, name):
    """[{combo, action}] for the keyboard page: bound is {combo: action}."""
    rank = lambda a: BINDING_ORDER.index(a) if a in BINDING_ORDER else len(BINDING_ORDER)
    rows = sorted(bound.items(), key=lambda ca: (rank(ca[1]), ca[1], ca[0]))
    return [{"combo": name(c), "action": label(a)} for c, a in rows if a not in ("none", "key")]


# ---------------------------------------------------------------- the pages

# Short looping videos of each thing in a headset (animated WebP: Qt plays them as it plays GIFs,
# at a fifth of a GIF's size). Cut from a narrated demo of 2026-10-20 by tour/clips/README.md's
# recipe; each page shows its clips one after another.
CLIPS = os.path.join(HERE, "clips")


def clip(name, caption):
    return {"file": os.path.join(CLIPS, name + ".webp"), "caption": caption}


def pages(f, keys):
    """The tour's pages for these features (features()) and key combinations (bindings()). Each:
    id, title, icon, intro, rows, actions [{id, text, icon}], clips [{file, caption}], and maybe
    practice ("gaze" or "hands": a target to click), note, keys (the key table), toggle
    ("dashboardApps"). A row is [do, get], [do, get, action id] (an Open button on the row), or a
    string: a heading over the rows after it."""
    out = [{
        "id": "welcome", "title": "Welcome to Frametop", "icon": "start-here-kde",
        "intro": "Frametop puts a Plasma desktop around you in VR: as many screens as you like, single windows "
                 "floating on their own, and the monitors of your other computers. This tour teaches it a page at a "
                 "time, in about ten minutes. Most pages start with a short video from a headset: watch it, then "
                 "try it. Meta is the Windows key (Cmd on a Mac keyboard).",
        "rows": ["Five things to know first",
                 ["Move the mouse", "A dot moves around you and lands on whatever screen or panel it's over"],
                 ["Meta+Space", "Frunner: type an app's name and press Enter to open it"],
                 ["Tap Meta", "The Steam menu, wherever you are"],
                 ["Meta+Shift+R", "Lost a screen? This puts every screen back in its layout"],
                 ["Click both thumbsticks together, twice", "Pauses Frametop for a game, or brings it back"],
                 "How this tour works",
                 ["The video at the top of a page", "Loops what the page teaches. Click it to pause it; the numbers "
                                                    "under it pick another video"],
                 ["The buttons", "Try it now, or open its settings"],
                 ["The page list at the bottom", "Jumps to any page, so the tour is also a reference to come back to"]],
        "note": "You can take the tour again any time: Frametop Tour is in the app menu, in frunner (Meta+Space), "
                "in SteamVR's Launch a program list, and in Frametop Input Settings.",
        "actions": [], "clips": []}, {
        "id": "screens", "title": "Screens and the 3D mouse", "icon": "video-display",
        "intro": "Each screen is a panel in the room. The mouse moves a dot around you that snaps onto whatever "
                 "panel it's over, so one mouse reaches every screen, floating window, and SteamVR panel.",
        "clips": [clip("mouse", "One mouse reaches every screen"),
                  clip("mouse-tilt", "Drag the bar to carry a screen; hold right too to tilt it"),
                  clip("roll", "The roll button rolls a screen and snaps it level")],
        "rows": ["Pointing",
                 ["Move the mouse", "The dot moves around you and snaps onto the panel under it"],
                 ["Click, right-click, scroll", "Acts on the panel under the dot"],
                 ["Pick up a controller", "It gets its laser back. Move the mouse to take over again"],
                 "Moving a screen",
                 ["Point near the bottom of a screen", "Its controls fade in: the reset button, the bar, and the "
                                                       "curve and roll buttons"],
                 ["Drag the bar", "Carries the screen. Scroll while dragging to push it away or pull it closer"],
                 ["Hold the right button too while dragging the bar", "Tilts the screen as you move the mouse"],
                 ["Press near a corner, where it glows, and drag", "Resizes the screen"],
                 ["Click the curve button", "Curves the screen around you, or flattens it"],
                 ["Drag the roll button sideways, or scroll on it", "Rolls the screen; it snaps level near straight"],
                 "Getting back",
                 ["The reset button (left of the bar), or Meta+Shift+R", "Puts every screen back in its layout"],
                 ["Meta+Shift+H", "Hides or shows all screens"]],
        "actions": [{"id": "display:screens", "text": "Screens settings", "icon": "video-display"}]}]

    out.append({
        "id": "floating", "title": "Floating windows", "icon": "window-new",
        "intro": "Any desktop window can float in VR as a panel of its own. It's still a window of the same desktop, "
                 "so drag and drop and the clipboard work between floating windows and the screens.",
        "clips": [clip("float", "Float a window, then carry it by its bar")],
        "rows": [["The float button, left of Close in a title bar", "Floats the window, or puts it back"],
                 ["Meta+Shift+F over a window", "The same, for any window: Chromium and Electron apps have no float "
                                                "button. Try it on this window"],
                 ["Drag a floating window's bar", "Carries it. Tilt, roll, curve and resize work as on a screen"],
                 ["The dock button, left of its bar", "Puts it back where it came from. The close button by it "
                                                      "closes the window"],
                 ["Meta+scroll over a floating window", "Scales it"],
                 ["Right-click an app in the launcher: Launch as Standalone", "Starts it floating, where it last floated"],
                 ["Meta+Alt+Tab, or Meta+Alt+Shift+Tab", "Spins every panel around you, like a lazy susan, so the next "
                                                         "one comes to the front"]],
        "toggle": "dashboardApps" if f["floats"] else "",
        "actions": [{"id": "spin", "text": "Spin the panels", "icon": "object-rotate-right"}]})

    out.append({
        "id": "frunner", "title": "Frunner: open anything by name", "icon": "search",
        "intro": "Frunner finds apps and remote displays by name, like Spotlight.",
        "clips": [clip("frunner", "Meta+Space, type a name, Enter"),
                  clip("frunner-more", "Shift+Enter: more ways to open it")],
        "rows": [["Meta+Space, on any keyboard", "Opens the search in front of you"],
                 ["Type a name and press Enter", "A running app's window spins to the front; an app that isn't "
                                                 "running opens floating; a remote display connects"],
                 ["Shift+Enter, or a right click", "More ways to open it: a private window, a new window, docked on "
                                                   "this screen, or one of its windows"],
                 ["Esc", "Closes it"]],
        "actions": [{"id": "frunner", "text": "Open frunner", "icon": "search"}]})

    out.append({
        "id": "keys", "title": "Keyboard shortcuts and macros", "icon": "input-keyboard",
        "intro": "These key combinations work on any keyboard, whatever has focus.",
        "keys": keys,
        "rows": [["Meta+Shift+R", "Reset Screen Layout"],
                 ["Meta+Shift+H", "Hide/Show Screens"]],
        "note": "Macros: on the Keyboard page of Frametop Input Settings, any key combination, or a modifier "
                "tapped on its own, can do a Frametop or Steam action, open a profile, or run a command of your "
                "own. The Buttons and Controllers pages map mouse buttons and controller buttons the same way.",
        "actions": [{"id": "input:keyboard", "text": "Keyboard shortcuts", "icon": "input-keyboard"},
                    {"id": "input:buttons", "text": "Mouse buttons", "icon": "input-mouse"},
                    {"id": "input:controllers", "text": "Controller buttons", "icon": "input-gamepad"}],
        "clips": []})

    gaze = {"id": "gaze", "title": "Eye tracking: look and click", "icon": "view-visible",
            "intro": "In gaze mode the pointer goes where you look, and the keyboard or the mouse does the last bit. "
                     "Calibrate first: press Calibrate below, look at each dot in the headset, and click.",
            "clips": [clip("gaze-check", "Calibration: look at each dot, then click"),
                      clip("gaze-nudge", "Hold left and move to nudge; press right too to drag")],
            "rows": ["Clicking",
                     ["Tap Meta+J, or Meta+K", "A left or right click where you look"],
                     ["Hold Meta+J, turn your head onto what you meant, let go", "A click there"],
                     ["Hold the left mouse button, move the mouse onto what you meant, let go",
                      "A click there (the right button: a right click). Each correction teaches the tracker "
                      "where it was off"],
                     "Dragging",
                     ["Hold Meta+J, or the left button, still for half a second", "A press: turning your head, or "
                                                                                  "moving the mouse, drags"],
                     ["Hold left, move onto it, then press right too", "A drag from there, while either button is held"],
                     ["Double right click (or double Meta+K) while dragging a screen's bar", "Pans and tilts the screen"],
                     "Staying accurate",
                     ["Put the headset on", "A quick check: look at the dot, and the pointer lines up again"],
                     ["Correct by more than about half your view", "The quick check opens instead of the tracker "
                                                                   "learning it"]],
            "actions": [{"id": "gaze:calibrate", "text": "Calibrate", "icon": "crosshairs"},
                        {"id": "gaze:quickcal", "text": "Quick check", "icon": "view-visible"},
                        {"id": "input:gaze", "text": "Gaze settings", "icon": "configure"}]}
    if not f["gaze"]:
        gaze.update(intro="In gaze mode the pointer goes where you look, and the keyboard or the mouse does the last "
                          "bit. It isn't installed here: install it with gaze/run.sh install, then our own eye "
                          "tracker with gaze/tracker/install.sh (it needs sudo).",
                    actions=[], practice="")
    else:
        gaze["practice"] = "gaze"
        if not f["gazeOn"]:
            gaze["note"] = "Gaze mode is off now: turn it on, and calibrate, on the Gaze page of Frametop Input Settings."
    out.append(gaze)

    hands = {"id": "hands", "title": "Hand tracking: touch and move screens", "icon": "transform-browse",
             "intro": "Hand tracking uses the headset's own cameras, with no controllers. Up close, your hands work "
                      "on the screens directly. It runs while the Frametop desktop does, and parks while Frametop is "
                      "paused for a VR game.",
             "clips": [clip("hand-move", "Carry a screen by its bar, turn it, throw it"),
                       clip("two-hands", "Both hands: bring it close and resize it"),
                       clip("grab-that", "Grab that: a far screen comes to you")],
             "rows": ["Touching",
                      ["Poke a screen with your index finger", "A click there. A soft dot shows where it will land"],
                      ["Index and middle finger together on a screen, swiped", "Scrolls. Lifted without moving, a click"],
                      ["Pinch on a screen's content", "A click there; held, it drags"],
                      ["Pinch a round button under a screen", "Presses it (reset, curve, dock, close)"],
                      "Moving",
                      ["Pinch a screen's bar or an edge, and move", "Carries it; turn your wrist to turn it"],
                      ["Pinch a corner, where it glows, and move", "Resizes it"],
                      ["Hold an edge like a card (index and middle finger, or all fingertips)", "Carries it too"],
                      ["Both hands on a screen", "Pull apart to stretch it; turn them together to turn it any way"],
                      ["Two fists near a screen", "Resizes it. Fists don't move screens"],
                      ["Let go while moving it fast", "Throws it 3 m out"],
                      "Far away and back",
                      ["Open hand out to a far screen, then a fist pulled back", "\"Grab that\": it comes to arm's "
                                                                                 "reach. A white glow marks which one"],
                      ["An open palm pushed into a near screen, or flicked at it", "Sends it 3 m out"],
                      ["An open hand on its edge, swept sideways quickly", "Lazy susan: the next screen on that side "
                                                                           "comes to you"]],
             "actions": [{"id": "input:hands", "text": "Hand settings", "icon": "configure"}]}
    if not f["hands"]:
        hands.update(intro=hands["intro"] + " It isn't installed here: install it with hands/run.sh install (it "
                     "needs sudo once, for its camera broker).", actions=[], practice="")
    else:
        hands["practice"] = "hands"
        if not f["handsPointer"]:
            hands["note"] = "Hand interaction is off now: switch it on on the Hands page of Frametop Input Settings."
    out.append(hands)

    far = {"id": "hands-far", "title": "Hand tracking: point from a distance", "icon": "transform-browse",
           "intro": "A screen out of reach takes a pinch. " + ("With gaze mode on, you look and pinch; with it off, "
                                                                "a raised, pointing hand shows a laser."
                                                                if f["gaze"] else "A raised, pointing hand shows a laser."),
           "clips": [clip("hand-clicks", "Pinch to click, middle finger to right click")],
           "rows": [], "actions": [{"id": "input:hands", "text": "Hand settings", "icon": "configure"}]}
    if f["gaze"]:
        far["rows"] += ["With gaze mode on",
                        ["Look at something and pinch index finger to thumb", "A click where you look, like a Vision Pro"],
                        ["Pinch your middle finger to your thumb", "A right click"],
                        ["Pinch, hold, and move your hand", "Nudges the pointer; letting go clicks there"],
                        ["Pinch and hold still for half a second", "A press: your hand drags"],
                        ["Thumb on both the index and middle fingertips, then move", "Scrolls where you looked"],
                        ["Hold a pinch on one hand, then pinch with the other", "A drag from the dot, like Meta+J "
                                                                                "then Meta+K"],
                        "With gaze mode off"]
    far["rows"] += [["Raise a hand in front, pointing, and bring thumb and index together", "Its laser shows"],
                    ["Pinch", "A click where the laser points"],
                    ["Pinch your middle finger to your thumb", "A right click. Held and moved, it scrolls"],
                    "Anywhere",
                    ["Turn a palm up toward your face, look at it, pinch the ring", "Opens the Steam menu"],
                    ["Meta+H", "Turns hand tracking off, or back on, until the desktop or SteamVR starts again"]]
    if not f["hands"]:
        far.update(intro=far["intro"] + " Hand tracking isn't installed here: install it with hands/run.sh install.",
                   actions=[])
    else:
        far["practice"] = "hands"
    out.append(far)

    out += [{
        "id": "pinning", "title": "Pinning screens", "icon": "window-pin",
        "intro": "A screen can go with you: on a wrist, or on your head like a HUD.",
        "rows": [["While carrying a screen, sweep its laser across your other controller's ring, then let go",
                  "Pins it to that wrist; it shows while you see its front. Sweep across the ring again to take it off"],
                 ["Grab a pinned screen's bar", "Adjusts it; it stays pinned"],
                 ["On your head (Display Settings, Visibility & pins)", "Pins it to your head where it is"],
                 ["When the screens show (Visibility & pins)", "Always, only with the dashboard open, or while you look "
                                                              "at your wrist"]],
        "actions": [{"id": "display:visibility", "text": "Visibility & pins", "icon": "window-pin"}],
        "clips": []}, {
        "id": "profiles", "title": "Display profiles", "icon": "view-grid",
        "intro": "A profile is a named setup: where the screens are, with their sizes and pins, which ones are "
                 "hidden, and which apps are open and where their windows are.",
        "rows": [["Save as profile… (Display Settings, Layout)", "Saves the current setup under a name"],
                 ["Pick a profile under Arrangement, then Open profile", "The screens move, windows go to their "
                                                                         "places, and missing apps start. Nothing closes"],
                 ["Start in profile (Display Settings, Layout)", "The desktop starts in that profile"],
                 ["Frametop: NAME in SteamVR's Launch a program list", "Starts the desktop in that profile, or "
                                                                      "switches to it"]],
        "note": "A profile can also go on a key combination, a mouse button, or a controller button in Frametop "
                "Input Settings.",
        "actions": [{"id": "display:layout", "text": "Layout & profiles", "icon": "view-grid"}],
        "clips": []}, {
        "id": "pause", "title": "Pause for VR games", "icon": "media-playback-pause",
        "intro": "Frametop pauses while a VR game runs, so the game gets the headset to itself, and comes back a few "
                 "seconds after it ends. Paused, the screens hide, but their windows stay open.",
        "rows": [["Click both thumbsticks together, twice", "Pauses Frametop, or brings it back, in a game or not"],
                 ["With one controller: click its thumbstick twice", "The same"],
                 ["Start a VR game", "Frametop pauses, and comes back 5 seconds after the game ends"]],
        "note": "The gesture, and what the desktop does while paused, are on the Game optimization page of "
                "Frametop Input Settings.",
        "actions": [{"id": "input:games", "text": "Game optimization", "icon": "applications-games"}],
        "clips": []}]

    out.append({
        "id": "settings", "title": "Settings, page by page", "icon": "configure",
        "intro": "Everything in this tour can be changed. Frametop Input Settings has a page for each kind of input, "
                 "and Frametop Display Settings has the screens. Open any page from its row.",
        "rows": ["Frametop Input Settings",
                 ["Devices", "Every mouse and keyboard, and its role: 3D pointer (it moves the dot), Keyboard + 3D "
                             "pointer (a keyboard with a touchpad), Pass through, or Ignore", "input:devices"],
                 ["Buttons", "A mouse's extra buttons as Frametop or Steam actions, profiles, or your own commands",
                  "input:buttons"],
                 ["Controllers", "The same for the Frame controllers' buttons, outside games or in them too",
                  "input:controllers"],
                 ["Game optimization", "Pausing for VR games: on or off, the controller gesture, what the desktop "
                                       "does while paused", "input:games"],
                 ["Keyboard", "When Frametop's VR keyboard shows, and your key combinations and modifier taps",
                  "input:keyboard"],
                 ["Voice", "Dictation: push-to-talk keys and buttons, your language, typing as you talk, wake word "
                           "commands", "input:voice"],
                 ["Pointer", "The dot's speed, size, and distance, head follow, and Recenter. Changes apply live",
                  "input:pointer"],
                 ["Ignored panels", "Panels the mouse passes through, like a performance overlay that follows your "
                                    "view", "input:ignore"],
                 ["Gaze", "Gaze mode on or off, Calibrate, Quick check, Check headset fit, the mouse buttons, and "
                          "which eye tracker", "input:gaze"],
                 ["Hands", "When hand tracking runs, the hand model, the three ways to use your hands, and every "
                           "gesture on or off", "input:hands"],
                 ["Bluetooth", "Paired mice and keyboards, and the fix that makes an LE one reconnect by itself",
                  "input:bluetooth"],
                 "Frametop Display Settings",
                 ["Screens", "How many screens, their resolution, size in VR, scale, and curve", "display:screens"],
                 ["Layout & profiles", "The arrangement, saved profiles, and how the desktop starts", "display:layout"],
                 ["Visibility & pins", "When screens show, pins, hidden screens, notifications, and dashboard apps",
                  "display:visibility"],
                 ["Power", "Turning the displays off when the headset is unused, and staying awake on the charger",
                  "display:power"]],
        "actions": [], "clips": []})

    more = {"id": "more", "title": "And more", "icon": "help-about",
            "intro": "A few more things Frametop can do, each with its own settings.",
            "rows": [["Remote displays", "Another computer's monitors as Frametop screens (Frametop Remote Displays)"],
                     ["Displays off", "The headset's displays turn off when it's unused (Display Settings, Power)"]],
            "actions": [{"id": "remote", "text": "Remote displays", "icon": "network-workgroup"},
                        {"id": "display:power", "text": "Power", "icon": "preferences-system-power-management"}],
            "clips": []}
    if f["voice"]:
        more["rows"].insert(0, ["Dictation: tap Right Ctrl, or hold the left d-pad down",
                                "Speak, and it's typed where you type. Nothing leaves the headset"])
        more["actions"].insert(0, {"id": "input:voice", "text": "Voice", "icon": "audio-input-microphone"})
    out.append(more)
    out.append({"id": "done", "title": "That's the tour", "icon": "checkmark",
                "intro": "Frametop Tour stays in the app menu, frunner, and SteamVR's Launch a program list, for "
                         "when you want it again. The page list at the bottom jumps straight to any topic.",
                "rows": ["If you get stuck",
                         ["A screen is lost or in the way", "Meta+Shift+R, or the reset button under any screen"],
                         ["The dot is gone", "Move the mouse; with gaze mode on, look where you want it"],
                         ["Gaze is off target", "Hold the left button and move the mouse onto it: each correction "
                                                "teaches the tracker. Or run Quick check"],
                         ["The screens vanished", "Frametop may be paused: click both thumbsticks twice. Or "
                                                  "Meta+Shift+H shows hidden screens"]],
                "actions": [], "clips": []})
    for p in out:  # a clip that isn't in this install isn't shown
        p["clips"] = [c for c in p.get("clips", []) if os.path.exists(c["file"])]
    return out


# ---------------------------------------------------------------- the app

def send(name, text):
    """A datagram to one of Frametop's sockets; False if nothing's there."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(text.encode(), SOCKETS[name])
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    import ft_input_settings as fis  # the input relay's key combinations and their names
    from PySide6.QtCore import (
        Property,
        QObject,
        QProcess,
        QProcessEnvironment,
        QUrl,
        Signal,
        Slot,
    )
    from PySide6.QtGui import QGuiApplication, QIcon
    from PySide6.QtQml import QQmlApplicationEngine
    from PySide6.QtQuickControls2 import QQuickStyle

    class Backend(QObject):
        changed = Signal()
        message = Signal(str, bool)  # text, is error

        def __init__(self):
            super().__init__()
            self._names = fis.code_names()
            self._again = False

        def _keys(self):
            bound = fis.key_bindings(fis.read_json(fis.RULES_PATH))
            return bindings(bound, fis.action_label,
                            lambda c: combo_name(c, self._names, fis.MODIFIER_NAMES, fis.RIGHT_MODIFIER_NAMES))

        @Property("QVariantList", notify=changed)
        def pages(self):
            return pages(features(), self._keys())

        @Property(bool, notify=changed)
        def showAtStart(self):
            return self._again

        @Slot(bool)
        def setShowAtStart(self, on):
            """The last page's "Show this tour when the desktop starts"."""
            self._again = on
            self.changed.emit()

        @Slot(str)
        def finish(self, how):
            """Closed, skipped, or finished: the desktop's start doesn't open it again, unless
            the last page's box is ticked."""
            reset() if self._again else mark_done(how=how)

        @Property(str, notify=changed)
        def dashboardApps(self):
            return read_conf().get("DASHBOARD_APPS", "steamvr")

        @Slot(str)
        def setDashboardApps(self, mode):
            if mode in ("steamvr", "frametop"):
                fis.write_conf_value("DASHBOARD_APPS", mode)
                self.changed.emit()

        @Slot(str)
        def act(self, what):
            if what.startswith("input:"):
                self._open(INPUT_SETTINGS, "FT_INPUT_PAGE", what[6:], "Frametop Input Settings")
            elif what.startswith("display:"):
                self._open(DISPLAY_SETTINGS, "FT_DISPLAY_PAGE", what[8:], "Frametop Display Settings")
            elif what == "remote":
                self._open(REMOTE_DISPLAYS, None, None, "Frametop Remote Displays")
            elif what in ("gaze:calibrate", "gaze:quickcal"):
                ok = send("gazed", what[5:])
                self.message.emit(("Calibration: look at each dot in the headset; right click or Meta+K stops"
                                   if what == "gaze:calibrate" else "Quick check: look at the dot in front of you")
                                  if ok else "The gaze service isn't running (gaze mode may be off)", not ok)
            elif what == "frunner":
                ok = send("frunner", "open mouse")
                self.message.emit("Frunner is open in front of you: type an app's name" if ok
                                  else "Frunner isn't running (it runs with the Frametop desktop)", not ok)
            elif what == "spin":
                ok = send("screens", "spin next")
                self.message.emit("Spun to the next panel. Meta+Shift+R puts the screens back" if ok
                                  else "The Frametop desktop isn't running", not ok)

        def _open(self, script, var, page, name):
            proc = QProcess(self)
            env = QProcessEnvironment.systemEnvironment()
            if var:
                env.insert(var, page)
            proc.setProcessEnvironment(env)
            proc.setProgram(sys.executable)
            proc.setArguments([script])
            if not proc.startDetached():
                self.message.emit(f"Couldn't start {name}", True)

    app = QGuiApplication(sys.argv)
    app.setApplicationName("ft-tour")
    app.setApplicationDisplayName("Frametop Tour")
    app.setDesktopFileName("ft-tour")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle("org.kde.desktop")
    engine = QQmlApplicationEngine()
    backend = Backend()
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", os.environ.get("FT_TOUR_PAGE", ""))
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, "main.qml")))
    if not engine.rootObjects():
        sys.exit(1)
    sys.exit(app.exec())


def cli(args):
    """`done` (exit 0 if the tour is done, 1 if not: the wrapper's --first-run, on the host's
    Python without starting the container) and `reset`."""
    if args == ["done"]:
        return 0 if is_done() else 1
    if args == ["reset"]:
        reset()
        print("The tour opens again when the Frametop desktop starts next")
        return 0
    sys.exit("usage: ft_tour.py [done|reset]")


if __name__ == "__main__":
    sys.exit(cli(sys.argv[1:])) if sys.argv[1:] else main()
