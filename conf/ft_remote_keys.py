"""ft_remote_keys: the key combinations a remote display's computer gets.

While you type on a remote display, Frametop's own key combinations ("key_bindings" in the input
rules, ~/.config/frametop-input.json) still work: Meta+J clicks where you look, Meta+Space opens
frunner. The rules' "remote_key_bindings" names the combinations that go to the computer instead:

    "remote_key_bindings": {"<pressed>": "<sent>"}

pressed is what you press, written as in key_bindings: evdev codes joined by "+", modifiers first
and in rising order, left-hand codes for either side ("42+125+57": Meta+Shift+Space), or one
modifier alone for a tap ("125": a Meta tap; a right-hand code is that key's own). sent is what
the computer gets: the same combination (it's typed there as it is, and held as long as you hold
it), or another one, which is tapped there once ("125+57": Meta+Space, so Meta+Shift+Space on the
Frame opens the computer's launcher while Meta+Space stays frunner's).

The input relay (input/input-relay.py) acts on the table and Frametop Settings' Remote display
keys page (input-settings/remote_keys.py) edits it; both read it here, so they agree on what a
file means. Standard library only, and nothing here reads a file.
"""
from types import MappingProxyType

from ft_input_actions import DEFAULT_KEY_BINDINGS, GAZE_ACTIONS, TILE_ACTIONS

RULES_KEY = "remote_key_bindings"
META = 125
# Modifiers, each side's code folded into the left one's, as the relay and Input Settings fold them.
MODIFIERS = MappingProxyType({29: 29, 97: 29, 42: 42, 54: 42, 56: 56, 100: 56, 125: 125, 126: 125})
KEY_MAX = 256  # BTN_MISC: keys are below it

# What a rules file without the table gets: Frametop's default Meta combinations go to the
# computer, as every Meta combination did before the table existed (so Super+Space opens the
# computer's launcher and a Meta tap its Start menu), but for the gaze clicks, which are the
# pointer's and belong to whatever you look at. The dashboard tiling keys aren't here either: they
# are ft-screens' only while an app's tile has the focus, and the computer's otherwise anyway.
DEFAULT = MappingProxyType({combo: combo for combo, action in DEFAULT_KEY_BINDINGS.items()
                            if str(META) in combo.split("+") and action not in GAZE_ACTIONS
                            and action not in TILE_ACTIONS})


def codes(combo):
    """A combination's key codes, in its order, or None if it isn't codes joined by "+"."""
    parts = combo.split("+") if isinstance(combo, str) else []
    if not parts or not all(p.isascii() and p.isdecimal() and 0 < int(p) < KEY_MAX for p in parts):
        return None
    return [int(p) for p in parts]


def pressed_ok(combo):
    """Whether the relay can ever see combo pressed: one key or modifier alone, or folded
    modifiers in rising order and then one key that isn't a modifier."""
    keys = codes(combo)
    if keys is None:
        return False
    *mods, last = keys
    if not mods:
        return True
    return last not in MODIFIERS and mods == sorted(set(mods)) and all(MODIFIERS.get(m) == m for m in mods)


def read(rules, sendable):
    """The rules' table as {pressed: sent}, or DEFAULT's when the rules have none of their own (an
    empty one counts as its own). An entry the relay couldn't use is left out: a pressed side it
    never sees, or a sent side that isn't the pressed one and that sendable(sent) refuses (the
    relay's limits for keys it taps)."""
    table = rules.get(RULES_KEY) if isinstance(rules, dict) else None
    if not isinstance(table, dict):
        return dict(DEFAULT)
    return {pressed: sent for pressed, sent in table.items()
            if pressed_ok(pressed) and isinstance(sent, str) and (sent == pressed or sendable(sent))}
