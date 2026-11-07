"""Frametop Settings' sections and areas (settings/ft_settings.py), without Qt: the tour
(tour/ft_tour.py) and the tests read them too.

A section is one page of the app, in the sidebar; its area is the backend and pages file it
belongs to. Section names are what ft-settings SECTION and FT_SETTINGS_PAGE take.
"""
import os
import sys

# Each area: its folder, the module with its Backend, and its pages (a QML file whose root has
# backend and window properties and a page(name) function).
AREAS = {
    "input": ("input-settings", "ft_input_settings", "InputPages.qml"),
    "display": ("display-settings", "ft_display_settings", "DisplayPages.qml"),
    "remote": ("remote-displays", "ft_remote_displays", "RemoteDisplayPages.qml"),
    "access": ("remote", "remote_access", "RemoteAccessPages.qml"),
}
# The sidebar, in its order: (section name, area, group, title, icon, search words).
SECTIONS = [
    ("screens", "display", "Screens", "Screens", "video-display",
     "display monitor resolution scale size width taskbar wallpaper stereo add restart"),
    ("layout", "display", "Screens", "Layout & profiles", "view-grid",
     "arrange curved flat rows distance gap height preset profile save"),
    ("visibility", "display", "Screens", "Visibility & pins", "view-visible",
     "show hide dashboard look controller wrist pin head notifications launch program floating"),
    ("remote-displays", "remote", "Screens", "Remote displays", "network-workgroup",
     "pc mac computer monitor stream moonlight vibepollo sunshine dongle"),
    ("devices", "input", "Input", "Devices", "input-mouse",
     "mouse keyboard touchpad usb role pass through ignore"),
    ("buttons", "input", "Input", "Buttons", "input-keyboard", "mouse button map action"),
    ("controllers", "input", "Input", "Controllers", "input-gamepad", "controller button map trigger grip"),
    ("keyboard", "input", "Input", "Keyboard", "input-keyboard-virtual",
     "shortcut key combination modifier meta tap layout full keyboard"),
    ("commands", "input", "Input", "Commands", "utilities-terminal", "shell command alias run"),
    ("pointer", "input", "Input", "Pointer", "transform-move",
     "speed dot size distance cursor travel mode laser"),
    ("ignore", "input", "Input", "Ignored panels", "view-hidden", "overlay pass through ignore"),
    ("bluetooth", "input", "Input", "Bluetooth", "preferences-system-bluetooth", "pair fixes"),
    ("gaze", "input", "Eyes, hands & voice", "Gaze", "view-visible",
     "eye tracking calibrate calibration look mouse"),
    ("hands", "input", "Eyes, hands & voice", "Hands", "transform-browse",
     "hand tracking pinch model touch typing cutout"),
    ("voice", "input", "Eyes, hands & voice", "Voice", "audio-input-microphone",
     "dictation speech wake word microphone"),
    ("games", "input", "System", "Game optimization", "applications-games",
     "pause vr game performance gesture"),
    ("power", "display", "System", "Power", "preferences-system-power-management",
     "display off sleep idle plugged autostart start with steamvr"),
    ("remote-access", "access", "System", "Remote access", "preferences-desktop-remote-desktop",
     "vnc tailscale tailnet password screen sharing"),
]
NAMES = [s[0] for s in SECTIONS]


def start_section(argv=None, environ=None):
    """The section to open on: the first argument, else FT_SETTINGS_PAGE, else the first one."""
    argv = sys.argv[1:] if argv is None else argv
    environ = os.environ if environ is None else environ
    for name in (argv[0] if argv else "", environ.get("FT_SETTINGS_PAGE", "")):
        if name in NAMES:
            return name
    return NAMES[0]
