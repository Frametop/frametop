"""ft_input_actions: the input rules file's shared vocabulary (~/.config/frametop-input.json).

The input relay (input/input-relay.py) acts on these, and Frametop Settings
(input-settings/) and Frametop Voice's settings (voice/voice_settings.py) edit and show them.
One copy here, so the three read the same names and defaults. Standard library only, and
nothing here reads a file: the callers decide which file and when. The two binding tables are
read-only views: one copy is shared by every importer, so a caller copies before changing one.
"""
from types import MappingProxyType

# Frame controller buttons the pointer helper can read (pointer/helper/vrbuttons.h).
VR_BUTTONS = ("left/view", "left/dpad_up", "left/dpad_down", "left/dpad_left", "left/dpad_right", "left/bumper",
              "left/trigger", "left/grip", "left/thumbstick", "right/menu", "right/a", "right/b", "right/x", "right/y",
              "right/bumper", "right/trigger", "right/grip", "right/thumbstick")

# Gaze mode is a mouse and keyboard feature: these never come from a controller button
# (docs/gaze-controllers.md).
GAZE_ACTIONS = ("gaze_toggle", "gaze_precision", "gaze_drag", "gaze_left", "gaze_right", "gaze_quickcal")

# Frametop Voice's dictation actions.
VOICE_ACTIONS = ("dictate", "dictate_hold")

# Key combinations a rules file without "key_bindings" gets: a Meta tap opens Steam's menu, Meta+J
# and Meta+K click at the gaze (free on the Frametop desktop, and apps don't use Meta),
# Meta+Shift+F floats a window, and Meta+Alt+Tab and Meta+Alt+Shift+Tab spin the panels around
# you (ft-screens' lazy susan); not Meta+Tab, which is Cmd+Tab on a Mac reached through a remote
# desktop like RustDesk. Meta+Space opens frunner's search (Plasma's KRunner is Alt+Space), and
# Meta+H turns hand tracking off or on. Meta+Shift+H hides or shows the screens: the relay reads it
# from the keyboard, so it works with the screens hidden too, when the desktop gets no keys.
# Ctrl+Alt+Shift+Esc lets go of every key held anywhere (release_keys), for a modifier stuck in
# KWin (clicks moved windows, as Meta+click does). It has no Meta, the key that sticks,
# and it's free: KWin's Ctrl+Alt+Esc (kill a window) and Windows' Ctrl+Shift+Esc (Task Manager,
# on a remote display) need their modifiers exactly, and four keys don't happen by accident.
DEFAULT_KEY_BINDINGS = MappingProxyType({"125": "steam_menu", "125+36": "gaze_left", "125+37": "gaze_right",
                        "42+125+33": "float_toggle", "56+125+15": "spin_next", "42+56+125+15": "spin_prev",
                        "125+57": "frunner", "125+35": "hands_toggle", "42+125+35": "screens_toggle",
                        "29+42+56+1": "release_keys"})

# With Frametop Voice installed (its unit file), a rules file without "key_bindings" gets these
# too: a Right Ctrl tap, Right Ctrl alone, so a Left Ctrl tap (crouch in many games) doesn't
# start dictation.
VOICE_KEY_BINDINGS = MappingProxyType({"97": "dictate"})

# Actions renamed since a release: rules files saved before still mean them (spotwatch was
# frunner's name until 2026-10-20).
RENAMED_ACTIONS = MappingProxyType({"spotwatch": "frunner"})


def renamed_actions(table):
    """A rules table ({id: action}, or {id: {code: action}}) with renamed actions under their new names."""
    if not isinstance(table, dict):
        return table
    return {k: renamed_actions(v) if isinstance(v, dict) else RENAMED_ACTIONS.get(v, v) if isinstance(v, str) else v
            for k, v in table.items()}
