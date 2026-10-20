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
TOUR_VERSION = 1  # a tour with new pages can show itself again by raising this
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

def pages(f, keys):
    """The tour's pages for these features (features()) and key combinations (bindings()). Each:
    id, title, icon, intro, rows [[do, get]], actions [{id, text, icon}], and maybe practice
    ("gaze" or "hands": a target to click), note, keys (the key table), toggle ("dashboardApps")."""
    out = [{
        "id": "welcome", "title": "Welcome to Frametop", "icon": "start-here-kde",
        "intro": "Frametop puts a Plasma desktop around you in VR: as many screens as you like, single windows "
                 "floating on their own, and the monitors of your other computers. This tour takes a few minutes. "
                 "Each page has buttons to try things or open their settings.",
        "note": "You can take the tour again any time: Frametop Tour is in the app menu, in frunner (Meta+Space), "
                "in SteamVR's Launch a program list, and in Frametop Input Settings.",
        "rows": [], "actions": []}, {
        "id": "screens", "title": "Screens and the 3D mouse", "icon": "video-display",
        "intro": "Each screen is a panel in the room. Your mouse moves a dot around you that snaps onto whatever "
                 "panel it's over, so one mouse reaches every screen, window, and SteamVR panel.",
        "rows": [["Move the mouse", "The dot moves around you and snaps onto the panel under it"],
                 ["Point near the bottom of a screen", "Its bar, curve and roll buttons fade in"],
                 ["Drag the bar under a screen", "Moves it. Scroll while dragging to push it away or pull it in"],
                 ["Press near a corner, where it glows, and drag", "Resizes the screen"],
                 ["Click the curve button, next to the bar", "Curves the screen around you, or flattens it"],
                 ["Pick up a controller", "It gets its laser back. Move the mouse to take over again"],
                 ["Meta+Shift+R", "Puts the screens back in their layout"],
                 ["Meta+Shift+H", "Hides or shows all screens"]],
        "actions": [{"id": "display:screens", "text": "Screens settings", "icon": "video-display"}]}, {
        "id": "keys", "title": "Keyboard shortcuts and macros", "icon": "input-keyboard",
        "intro": "These key combinations work on any keyboard, whatever has focus. Meta is the Windows key "
                 "(Cmd on a Mac keyboard).",
        "keys": keys,
        "rows": [["Meta+Shift+R", "Reset Screen Layout"],
                 ["Meta+Shift+H", "Hide/Show Screens"]],
        "note": "Macros: on the Keyboard page of Frametop Input Settings, any key combination, or a modifier "
                "tapped on its own, can do a Frametop or Steam action, open a profile, or run a command of your "
                "own. The Buttons and Controllers pages map mouse buttons and controller buttons the same way.",
        "actions": [{"id": "input:keyboard", "text": "Keyboard shortcuts", "icon": "input-keyboard"},
                    {"id": "input:buttons", "text": "Mouse buttons", "icon": "input-mouse"},
                    {"id": "input:controllers", "text": "Controller buttons", "icon": "input-gamepad"}]}]

    gaze = {"id": "gaze", "title": "Eye tracking: Meta+J and Meta+K", "icon": "view-visible",
            "intro": "In gaze mode the pointer goes where you look, and the keyboard or the mouse does the last bit.",
            "rows": [["Tap Meta+J, or Meta+K", "A left or right click where you look"],
                     ["Hold Meta+J, turn your head onto what you meant, let go", "A click there"],
                     ["Hold Meta+J still for half a second", "A press: turning your head drags"],
                     ["Hold the left mouse button, move the mouse onto it, let go",
                      "A click there. Each correction teaches the tracker where it was off"],
                     ["Double Meta+K while dragging a screen's bar", "Pans and tilts the screen"],
                     ["Put the headset on", "A quick check: look at the dot, and the pointer lines up again"]],
            "actions": [{"id": "gaze:calibrate", "text": "Calibrate", "icon": "crosshairs"},
                        {"id": "gaze:quickcal", "text": "Quick check", "icon": "view-visible"},
                        {"id": "input:gaze", "text": "Gaze settings", "icon": "configure"}]}
    if not f["gaze"]:
        gaze.update(intro=gaze["intro"] + " It isn't installed here: install it with gaze/run.sh install, then our "
                    "own eye tracker with gaze/tracker/install.sh (it needs sudo).",
                    actions=[], practice="")
    else:
        gaze["practice"] = "gaze"
        if not f["gazeOn"]:
            gaze["note"] = "Gaze mode is off now: turn it on, and calibrate, on the Gaze page of Frametop Input Settings."
    out.append(gaze)

    hands = {"id": "hands", "title": "Hand tracking", "icon": "transform-browse",
             "intro": "Hand tracking uses the headset's own cameras, with no controllers. It runs while the Frametop "
                      "desktop does, and parks while Frametop is paused for a VR game.",
             "rows": [["Poke a screen with your index finger", "A click there"],
                      ["Pinch a screen's bar, edge, or corner, and move", "Moves or resizes it"],
                      ["Hold a screen like a card, or in a fist", "Carries it; turn your wrist to turn it. Flick it away, "
                                                                  "or let go fast to throw it"],
                      ["Both hands on a screen, pulled apart", "Stretches it"],
                      ["Open hand out to a far screen, then a fist pulled back", "\"Grab that\": brings it to arm's reach"],
                      ["Palm up toward your face, look at it, pinch the ring", "Opens the Steam menu"]],
             "actions": [{"id": "input:hands", "text": "Hand settings", "icon": "configure"}]}
    if f["hands"] and f["gaze"]:
        hands["rows"] += [["With gaze mode: look at something and pinch", "A click where you look, like a Vision Pro"],
                          ["Pinch, hold and move your hand", "Nudges the pointer; letting go clicks there"],
                          ["Pinch and hold still for half a second", "A press: your hand drags"],
                          ["Pinch your middle finger to your thumb", "A right click"],
                          ["Close your hand", "Presses, and drags what the pointer is on"]]
    if not f["hands"]:
        hands.update(intro=hands["intro"] + " It isn't installed here: install it with hands/run.sh install (it "
                     "needs sudo once, for its camera broker).", actions=[], practice="")
    else:
        hands["practice"] = "hands"
        if not f["handsPointer"]:
            hands["note"] = "Hand interaction is off now: switch it on on the Hands page of Frametop Input Settings."
    out.append(hands)

    out += [{
        "id": "frunner", "title": "Frunner", "icon": "search",
        "intro": "Frunner finds apps and remote displays by name, like Spotlight.",
        "rows": [["Meta+Space, on any keyboard", "Opens the search in front of you"],
                 ["Type a name and press Enter", "A running app's window spins to the front; an app that isn't "
                                                 "running opens floating; a remote display connects"],
                 ["Shift+Enter, or a right click", "More: a private window, a new window, docked on this screen"],
                 ["Esc", "Closes it"]],
        "actions": [{"id": "frunner", "text": "Open frunner", "icon": "search"}]}, {
        "id": "floating", "title": "Floating windows", "icon": "window-new",
        "intro": "Any desktop window can float in VR as a panel of its own. It's still a window of the same desktop, "
                 "so drag and drop and the clipboard work between floating windows and the screens.",
        "rows": [["Meta+Shift+F over a window", "Floats it, or puts it back on its screen. Try it on this window"],
                 ["The float button, left of Close in a title bar", "The same"],
                 ["Right-click an app in the launcher: Launch as Standalone", "Starts it floating, where it last floated"],
                 ["Meta+scroll over a floating window", "Scales it"],
                 ["Meta+Alt+Tab, or Meta+Alt+Shift+Tab", "Spins every panel around you, like a lazy susan, so the next "
                                                         "one comes to the front"]],
        "toggle": "dashboardApps" if f["floats"] else "",
        "actions": [{"id": "spin", "text": "Spin the panels", "icon": "object-rotate-right"}]}, {
        "id": "pinning", "title": "Pinning screens", "icon": "window-pin",
        "intro": "A screen can go with you: on a wrist, or on your head like a HUD.",
        "rows": [["While carrying a screen, sweep its laser across your other controller's ring, then let go",
                  "Pins it to that wrist; it shows while you see its front. Sweep across the ring again to take it off"],
                 ["Grab a pinned screen's bar", "Adjusts it; it stays pinned"],
                 ["On your head (Display Settings, Visibility & pins)", "Pins it to your head where it is"],
                 ["When the screens show (Visibility & pins)", "Always, only with the dashboard open, or while you look "
                                                              "at your wrist"]],
        "actions": [{"id": "display:visibility", "text": "Visibility & pins", "icon": "window-pin"}]}, {
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
        "actions": [{"id": "display:layout", "text": "Layout & profiles", "icon": "view-grid"}]}, {
        "id": "pause", "title": "Pause for VR games", "icon": "media-playback-pause",
        "intro": "Frametop pauses while a VR game runs, so the game gets the headset to itself, and comes back a few "
                 "seconds after it ends. Paused, the screens hide, but their windows stay open.",
        "rows": [["Click both thumbsticks together, twice", "Pauses Frametop, or brings it back, in a game or not"],
                 ["With one controller: click its thumbstick twice", "The same"],
                 ["Start a VR game", "Frametop pauses, and comes back 5 seconds after the game ends"]],
        "actions": [{"id": "input:games", "text": "Game optimization", "icon": "applications-games"}]}]

    more = {"id": "more", "title": "And more", "icon": "help-about",
            "intro": "A few more things Frametop can do, each with its own settings.",
            "rows": [["Remote displays", "Another computer's monitors as Frametop screens (Frametop Remote Displays)"],
                     ["Displays off", "The headset's displays turn off when it's unused (Display Settings, Power)"]],
            "actions": [{"id": "remote", "text": "Remote displays", "icon": "network-workgroup"},
                        {"id": "display:power", "text": "Power", "icon": "preferences-system-power-management"}]}
    if f["voice"]:
        more["rows"].insert(0, ["Dictation: tap Right Ctrl, or hold the left d-pad down",
                                "Speak, and it's typed where you type. Nothing leaves the headset"])
        more["actions"].insert(0, {"id": "input:voice", "text": "Voice", "icon": "audio-input-microphone"})
    out.append(more)
    out.append({"id": "done", "title": "That's the tour", "icon": "checkmark",
                "intro": "Frametop Tour stays in the app menu, frunner, and SteamVR's Launch a program list, for "
                         "when you want it again.",
                "rows": [], "actions": []})
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
