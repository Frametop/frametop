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
# Meta+W closes the app you're typing in (close_app: Alt+F4 where typing goes; user, 2026-11-23).
#
# The dashboard tiles (screens/dashtiles.h; docs/design.md, Dashboard tiles): Meta+Shift+arrows move
# the focused app's tile (user, 2026-11-20), Meta+Ctrl+arrows size it, Meta+/ lists every key
# combination in frunner. The relay hands a tiling key to ft-screens only while an anchored app's
# tile has the focus (ft-screens' "tilefocus": a floating window, a gamescope app or a PC's app
# window); otherwise it goes to the desktop as it always did. A screen's and a remote display's
# tile are dragged, never moved by a key (user, 2026-11-23).
TILE_KEY_BINDINGS = MappingProxyType({"42+125+105": "tile_left", "42+125+106": "tile_right", "42+125+103": "tile_up",
                                      "42+125+108": "tile_down", "29+125+106": "tile_wider", "29+125+105": "tile_narrower",
                                      "29+125+103": "tile_taller", "29+125+108": "tile_shorter", "125+53": "keys_list"})
# Meta+arrows alone move an app's tile too while it has the focus, as Meta+Shift+arrows do, unless
# the rules bind that combination to something. On the desktop they're KWin's quick tile, which only
# halves a window alone on its own output: it looked like a resize where a move was meant (user,
# 2026-11-23). With no app's tile focused they stay the desktop's and a PC's own (Windows' snap).
APP_TILE_KEY_BINDINGS = MappingProxyType({"125+105": "tile_left", "125+106": "tile_right", "125+103": "tile_up",
                                          "125+108": "tile_down"})
DEFAULT_KEY_BINDINGS = MappingProxyType({"125": "steam_menu", "125+36": "gaze_left", "125+37": "gaze_right",
                        "42+125+33": "float_toggle", "56+125+15": "spin_next", "42+56+125+15": "spin_prev",
                        "125+57": "frunner", "125+35": "hands_toggle", "42+125+35": "screens_toggle",
                        "42+125+32": "desktopless_toggle", "29+42+56+1": "release_keys", "125+17": "close_app",
                        **TILE_KEY_BINDINGS})

# The tiling actions: ft-screens' command for each ("tile KEY" on the focused tile; anchor_toggle
# anchors it, or lets it go, as its anchor button does).
TILE_ACTIONS = MappingProxyType({"tile_left": "tile left", "tile_right": "tile right", "tile_up": "tile up",
                                 "tile_down": "tile down", "tile_wider": "tile wider", "tile_narrower": "tile narrower",
                                 "tile_taller": "tile taller", "tile_shorter": "tile shorter",
                                 "anchor_toggle": "anchor focused toggle"})
# The ones that act only on an anchored panel with the focus: the key goes to the desktop otherwise.
TILE_FOCUS_ACTIONS = tuple(a for a in TILE_ACTIONS if a != "anchor_toggle")

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

# Frametop's own actions in frunner's search, and its OS menu (the os_menu action: frunner opens
# with these listed): the input relay's action of that name runs ("action NAME" on its control
# socket, frunner/ft_frunner.py), and only these run that way. Each is an action that works
# without pointer mode; keyboard_toggle isn't one, since frunner opens and closes Frametop's keyboard
# itself. (name, title, keywords), in the menu's order.
MENU_ACTIONS = (
    ("steam_menu", "Steam menu", ("dashboard", "steamos", "steam", "open", "close")),
    ("screens_toggle", "Hide or show the screens", ("desktop", "screens", "hide", "show")),
    ("spin_next", "Spin to the next panel", ("spin", "next", "right", "lazy susan")),
    ("spin_prev", "Spin to the previous panel", ("spin", "previous", "left", "lazy susan")),
    ("dock_all", "Put every floating window back", ("dock", "float", "windows")),
    ("desktopless_toggle", "Desktopless mode on or off", ("desktopless", "desktop", "windows", "apps", "panels")),
    ("hands_toggle", "Hand tracking on or off", ("hands", "tracking")),
    ("pause_toggle", "Pause for a VR game", ("pause", "game")),
    ("release_keys", "Let go of every key", ("release", "stuck", "keys", "modifier")),
)
MENU_ACTION_NAMES = tuple(name for name, _, _ in MENU_ACTIONS)
# What frunner's key list (keys_list) may run with Enter, besides those: the tiling keys.
KEY_LIST_RUNNABLE = MENU_ACTION_NAMES + tuple(TILE_ACTIONS)

# The palm menu's Quick Settings toggles (screens/quicksettings.h keeps the same list, in the same
# order: quicksettings/test checks it): (name, label). Which ones it shows is QUICK_SETTINGS in
# frametop.conf (Frametop Settings' Quick Settings page; unset: all). The input relay runs these from
# ft-screens as it does the menu actions; keyboard_toggle and dictate aren't menu actions (frunner
# has its own keyboard item, and dictation is a hold-or-press key there).
QUICK_SETTINGS_ACTIONS = (
    ("screens_toggle", "Screens"),
    ("keyboard_toggle", "Keyboard"),
    ("desktopless_toggle", "Desktopless"),
    ("hands_toggle", "Hand Tracking"),
    ("pause_toggle", "Game Pause"),
    ("dictate", "Dictation"),
)
QUICK_SETTINGS_KEY = "QUICK_SETTINGS"

# Each action's name where people pick or read one: Input Settings' lists and frunner's key list.
ACTION_LABELS = MappingProxyType({
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
    "carry_toggle": "Carry the panel under the pointer / carry all",
    "frunner": "Frunner: search apps and remote displays",
    "os_menu": "OS menu: Frametop's actions in frunner",
    "hands_toggle": "Hand tracking on/off",
    "release_keys": "Release all keys (a stuck modifier)",
    "desktopless_toggle": "Desktopless mode on/off",
    "close_app": "Close app",
    "tile_left": "Dashboard tile: move left", "tile_right": "Dashboard tile: move right",
    "tile_up": "Dashboard tile: move up", "tile_down": "Dashboard tile: move down",
    "tile_wider": "Dashboard tile: wider", "tile_narrower": "Dashboard tile: narrower",
    "tile_taller": "Dashboard tile: taller", "tile_shorter": "Dashboard tile: shorter",
    "anchor_toggle": "Anchor the focused panel to the dashboard / let it go",
    "keys_list": "List the key combinations (frunner)",
    "key": "Pass through as key",
    "none": "Do nothing",
})

# Key names for a combination ("29+125+106": Ctrl+Meta+Right) where the kernel's header isn't at
# hand (frunner, on the host): the keys people bind, else "code N".
KEY_NAMES = MappingProxyType({
    1: "Esc", 14: "Backspace", 15: "Tab", 28: "Enter", 57: "Space", 12: "-", 13: "=", 26: "[", 27: "]",
    39: ";", 40: "'", 41: "`", 43: "\\", 51: ",", 52: ".", 53: "/", 97: "Right Ctrl", 54: "Right Shift",
    100: "Right Alt", 126: "Right Meta", 29: "Ctrl", 42: "Shift", 56: "Alt", 125: "Meta",
    103: "Up", 108: "Down", 105: "Left", 106: "Right", 102: "Home", 107: "End", 104: "Page Up", 109: "Page Down",
    110: "Insert", 111: "Delete", 99: "Print",
    **{2 + i: str((i + 1) % 10) for i in range(10)},
    **{c: ch for row, first in (("QWERTYUIOP", 16), ("ASDFGHJKL", 30), ("ZXCVBNM", 44)) for c, ch in
       ((first + i, ch) for i, ch in enumerate(row))},
    **{59 + i: f"F{i + 1}" for i in range(10)}, 87: "F11", 88: "F12",
    **{183 + i: f"F{i + 13}" for i in range(12)},
})


def combo_name(combo):
    """A key_bindings combination as people read it: "29+125+106" -> "Ctrl+Meta+Right"."""
    parts = combo.split("+") if isinstance(combo, str) else []
    if not parts or not all(p.isascii() and p.isdecimal() for p in parts):
        return str(combo)
    return "+".join(KEY_NAMES.get(int(p), f"code {p}") for p in parts)
