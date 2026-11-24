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
    "avatar": ("avatar", "ft_avatar_settings", "AvatarPages.qml"),
    "together": ("together", "ft_together_settings", "TogetherPages.qml"),
    "quick": ("quicksettings", "ft_quick_settings", "QuickSettingsPages.qml"),
}
# The sidebar, in its order: (section name, area, group, title, icon, search words).
SECTIONS = [
    ("screens", "display", "Screens & windows", "Screens", "video-display",
     "display monitor resolution scale size width taskbar wallpaper 3d stereo add restart"),
    ("layout", "display", "Screens & windows", "Layout & profiles", "view-grid",
     "arrange curved flat rows distance gap height preset profile save workspace"),
    ("visibility", "display", "Screens & windows", "Visibility & pins", "view-visible",
     "show hide dashboard look controller wrist pin head tiles anchor games"),
    ("desktopless", "display", "Screens & windows", "Desktopless mode", "preferences-system-windows",
     "desktopless desktop windows apps panels float every window own panel pc frame meta shift d"),
    ("handles", "display", "Screens & windows", "Grab bars & corners", "transform-scale",
     "window controls grab bar move drag zone padding resize corner indicator outline glow haptics color"),

    ("devices", "input", "Controls & devices", "Mice & keyboards", "input-mouse",
     "devices mouse keyboard touchpad usb role pass through ignore"),
    ("bluetooth", "input", "Controls & devices", "Bluetooth", "preferences-system-bluetooth",
     "pair wireless mouse keyboard reconnect le fixes"),
    ("pointer", "input", "Controls & devices", "Pointer", "transform-move",
     "speed dot size distance cursor aim travel mode airplane head follow recenter laser"),
    ("buttons", "input", "Controls & devices", "Mouse buttons", "input-keyboard",
     "buttons mouse side button map remap action"),
    ("controllers", "input", "Controls & devices", "Controllers", "input-gamepad",
     "controller button map remap trigger grip vr"),
    ("keyboard", "input", "Controls & devices", "Keyboard & shortcuts", "input-keyboard-virtual",
     "vr keyboard on-screen shortcut hotkey key combination modifier meta tap layout full keyboard"),
    ("ignore", "input", "Controls & devices", "Click-through panels", "view-hidden",
     "ignored panels overlay fps performance pass through ignore"),

    ("hands", "input", "Hands, eyes & voice", "Hands", "transform-browse",
     "hand tracking gestures pinch model touch typing cutout 60 fps"),
    ("gaze", "input", "Hands, eyes & voice", "Eye tracking", "view-visible",
     "gaze eye tracking calibrate calibration look mouse pupil tracker"),
    ("voice", "input", "Hands, eyes & voice", "Voice dictation", "audio-input-microphone",
     "voice dictation speech talk type wake word microphone"),
    ("appearance", "avatar", "Hands, eyes & voice", "Hands & avatar", "preferences-desktop-theme-global",
     "appearance hand model look skin glove robot ghost solid avatar body wear mirror import vrm gltf glb fbx room view"),

    ("remote-displays", "remote", "PCs & sharing", "Remote displays", "network-workgroup",
     "pc mac computer windows monitor stream moonlight vibepollo sunshine dongle"),
    ("remote-keys", "input", "PCs & sharing", "Remote PC shortcuts", "preferences-desktop-keyboard-shortcut",
     "remote display keys pc mac computer shortcut key combination pass through send super meta launcher"),
    ("together", "together", "PCs & sharing", "Frametop Together", "system-users",
     "room guest friend invite join people share collaborate multiplayer mute"),
    ("remote-access", "access", "PCs & sharing", "Remote access", "preferences-desktop-remote-desktop",
     "vnc krdp tailscale tailnet password screen sharing view headset desktop"),

    ("games", "input", "System", "VR games", "applications-games",
     "game optimization pause vr game performance gesture fps"),
    ("quick-settings", "quick", "System", "Quick Settings", "configure",
     "palm menu quick access toggles resources cpu gpu memory temperature battery"),
    ("power", "display", "System", "Power & startup", "preferences-system-power-management",
     "power display off sleep idle plugged autostart boot start with steamvr"),
    ("steam", "display", "System", "Steam & notifications", "preferences-desktop-notification",
     "notifications popup steam lock screen pin dashboard apps launch program floating"),
    ("commands", "input", "System", "Custom commands", "utilities-terminal",
     "commands shell script alias run terminal"),
]
NAMES = [s[0] for s in SECTIONS]
# Sections only ft-screens has (the gamescope backend has no panels of its own to show, pin or grab).
SCREENS_ONLY = ("visibility", "handles", "steam")


def start_section(argv=None, environ=None):
    """The section to open on: the first argument, else FT_SETTINGS_PAGE, else the first one."""
    argv = sys.argv[1:] if argv is None else argv
    environ = os.environ if environ is None else environ
    for name in (argv[0] if argv else "", environ.get("FT_SETTINGS_PAGE", "")):
        if name in NAMES:
            return name
    return NAMES[0]
