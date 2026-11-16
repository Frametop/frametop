"""Frunner's key list (the input relay's keys_list, Meta+/): every key combination bound in the
input relay's rules (~/.config/frametop-input.json, docs/frunner.md), as list items. Typing
filters them as any search does. Enter on one the relay can run from here
(ft_input_actions.KEY_LIST_RUNNABLE: Frametop's menu actions and the dashboard tiling keys) does
it; any other says which keys to press. The first item opens Frametop Settings' Keyboard page,
where they're bound. Standard library only, no GLib, so its test runs anywhere.
"""
import json
import os

import spotsearch
from ft_input_actions import (ACTION_LABELS, DEFAULT_KEY_BINDINGS, KEY_LIST_RUNNABLE, VOICE_KEY_BINDINGS, combo_name,
                              renamed_actions)

RULES = os.path.expanduser("~/.config/frametop-input.json")
VOICE_UNIT = os.path.expanduser("~/.config/systemd/user/frametop-voice.service")  # the relay's VOICE_UNIT
REBIND_KEY = "keys:rebind"


def bindings(path=RULES, voice_unit=VOICE_UNIT):
    """The key combinations the relay uses ({combo: action}): the file's own "key_bindings", or with
    none (no file, not JSON, no list) the relay's defaults, as its read_rules does."""
    try:
        with open(path) as f:
            rules = json.load(f)
    except (OSError, ValueError):
        rules = None
    own = rules.get("key_bindings") if isinstance(rules, dict) else None
    if isinstance(own, dict):
        return {k: v for k, v in renamed_actions(own).items() if isinstance(k, str) and isinstance(v, str)}
    out = dict(DEFAULT_KEY_BINDINGS)
    if os.path.exists(voice_unit):
        out.update(VOICE_KEY_BINDINGS)
    return out


def label(action):
    """An action as Frametop Settings names it."""
    for prefix, text in (("profile:", "Open profile "), ("command:", "Run: "), ("run:", "Run command "),
                         ("keys:", "Press ")):
        if action.startswith(prefix):
            rest = action[len(prefix):]
            return text + (combo_name(rest) if prefix == "keys:" else rest)
    return ACTION_LABELS.get(action, action)


def items(table):
    """The list's items, the Rebind one first, then each binding by its action's name. Kind "keybind";
    data: {"combo", "keys", "action", "runnable"}. Pass-through and do-nothing bindings aren't listed."""
    out = [spotsearch.Item("keybind", REBIND_KEY, "Rebind keys in Frametop Settings", "Keyboard page",
                           ("rebind", "settings", "shortcut", "change"), "",
                           {"combo": "", "keys": "", "action": "", "runnable": False})]
    rows = []
    for combo, action in table.items():
        if action in ("key", "none"):
            continue
        keys = combo_name(combo)
        rows.append(spotsearch.Item("keybind", "keys:" + combo, f"{keys}  {label(action)}", "",
                                    (keys, action, *keys.split("+")), "",
                                    {"combo": combo, "keys": keys, "action": action,
                                     "runnable": action in KEY_LIST_RUNNABLE}))
    rows.sort(key=lambda it: (label(it.data["action"]).casefold(), it.data["keys"]))
    return out + rows
