"""Whether a Steam game reads its controller through Steam Input's own actions (vrpad.py asks, for a
game on a Frametop panel).

Such a game ships its action list, controller_config/game_actions_<app id>.vdf in its install
folder, and asks Steam for "the Joystick action" instead of reading a pad's stick. Steam Input
answers only the game gamescope has focused, which a game on a Frametop panel never is (2026-11-27:
Steam Input stayed on Steam's own UI, 769), so every action is idle there. What such a game still
reads from a pad itself is whatever its engine binds to plain buttons: Slay the Spire 2 (2868840,
v0.114.0, Godot 4.5) took Frametop's pad's d-pad and buttons and neither stick nor trigger. Its
SteamControllerInputStrategy runs its Godot fallback, the one that turns a stick into directions,
only while Steam reports no controller at all, and Steam always reports the Frame's.

The folders are Steam's own layout (steamapps/libraryfolders.vdf, appmanifest_<app id>.acf), read
and never written; scripts/update-check.py has the retest hint.
"""

import os
import re

STEAM = "~/.local/share/Steam"


def _values(path, key):
    """Every "key" "value" in a Steam text file ("" when it can't be read)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return re.findall(r'"%s"\s+"([^"]*)"' % re.escape(key), f.read())
    except OSError:
        return []


def install_dir(app, steam=STEAM):
    """The folder Steam installed `app` in, or None."""
    steam = os.path.expanduser(steam)
    for library in [steam, *_values(os.path.join(steam, "steamapps", "libraryfolders.vdf"), "path")]:
        for name in _values(os.path.join(library, "steamapps", f"appmanifest_{int(app)}.acf"), "installdir"):
            path = os.path.join(library, "steamapps", "common", name)
            if os.path.isdir(path):
                return path
    return None


def native(app, steam=STEAM):
    """`app` (a Steam app id) ships a Steam Input action list: it reads its controller through
    Steam Input's actions."""
    folder = install_dir(app, steam) if app else None
    return bool(folder) and os.path.isfile(os.path.join(folder, "controller_config", f"game_actions_{int(app)}.vdf"))
