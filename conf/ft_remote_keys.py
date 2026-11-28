"""ft_remote_keys: the key combinations a remote display's computer gets.

While you type on a remote display, what you type goes to its computer, but no combination with
Meta does on its own: Frametop's ("key_bindings" in the input rules,
~/.config/frametop-input.json) still work, so Meta+J clicks where you look and Meta+Space opens
frunner, and any other is the Frame's own desktop's, as it is off the remote display. The rules'
"remote_key_bindings" names the combinations that go to the computer instead, and it is all of
them (a combination without Meta that Frametop doesn't bind is typed there as ever):

    "remote_key_bindings": {"<pressed>": "<sent>"}

pressed is what you press, written as in key_bindings: evdev codes joined by "+", modifiers first
and in rising order, left-hand codes for either side ("42+125+57": Meta+Shift+Space), or one
modifier alone for a tap ("125": a Meta tap, either side's; a right-hand code is that key's own,
and an entry for the left code leaves a right-hand key that key_bindings binds on its own). sent
is what the computer gets: the same combination (it's typed there as it is, and held as long as
you hold it), or another one, which is tapped there once ("125+57": Meta+Space, so
Meta+Shift+Space on the Frame opens the computer's launcher while Meta+Space stays frunner's).
An entry for a combination Frametop doesn't bind is what sends it ("125+28": Meta+Enter, for a
computer whose window manager opens a terminal with it).

The computer hears nothing of a combination that isn't its own: the relay holds Meta's press
back, and the modifiers pressed after it, until the key they are for says whose it is
(screens/waiting-keys.h). A click or a wheel on the remote display takes them along, so Meta+drag
there is still the computer's.

The input relay (input/input-relay.py) acts on the table and Frametop Settings' Remote display
keys page (input-settings/remote_keys.py) edits it; both read it here, so they agree on what a
file means. Standard library only, and nothing here reads a file.
"""
from types import MappingProxyType

from ft_input_actions import DEFAULT_KEY_BINDINGS, GAZE_ACTIONS, MODIFIERS, TILE_ACTIONS, combo_codes

RULES_KEY = "remote_key_bindings"
META = 125

# What a rules file without the table gets: Frametop's default Meta combinations go to the
# computer, as every Meta combination did before the table existed (so Super+Space opens the
# computer's launcher and a Meta tap its Start menu), but for the gaze clicks, which are the
# pointer's and belong to whatever you look at. The dashboard tiling keys aren't here either: they
# are ft-screens' only while an app's tile has the focus, and the computer's otherwise anyway.
DEFAULT = MappingProxyType({combo: combo for combo, action in DEFAULT_KEY_BINDINGS.items()
                            if str(META) in combo.split("+") and action not in GAZE_ACTIONS
                            and action not in TILE_ACTIONS})


def pressed_ok(combo):
    """Whether the relay can ever see combo pressed: one key or modifier alone, or folded
    modifiers in rising order and then one key that isn't a modifier."""
    keys = combo_codes(combo)
    if keys is None:
        return False
    *mods, last = keys
    if not mods:
        return True
    return last not in MODIFIERS and mods == sorted(set(mods)) and all(MODIFIERS.get(m) == m for m in mods)


def read(rules, sendable):
    """The rules' table as {pressed: sent}, or DEFAULT's when the rules have none of their own (an
    empty one counts as its own). An entry the relay couldn't use is left out: a pressed side it
    never sees, or a sent side that isn't the pressed one and isn't key codes, or that
    sendable(sent) refuses (the relay's limits for keys it taps)."""
    table = rules.get(RULES_KEY) if isinstance(rules, dict) else None
    if not isinstance(table, dict):
        return dict(DEFAULT)
    return {pressed: sent for pressed, sent in table.items()
            if pressed_ok(pressed) and (sent == pressed or (combo_codes(sent) is not None and sendable(sent)))}
