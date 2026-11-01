"""Input Settings' files: the input relay's rules (~/.config/frametop-input.json) and
frametop.conf. Plain Python (no Qt): ft_input_settings' Backend calls these with its paths,
read at call time, and says in the window what happened. Tests: input-settings/test/test_input_rules.py.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ft_store  # noqa: E402  (Frametop's JSON files, read and written safely)
# Actions renamed since a release (the relay reads them the same way): a rules file saved before
# shows and saves them under their new names.
from ft_input_actions import renamed_actions  # noqa: E402, F401


def read_json(path):
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if isinstance(data, dict):
        return _renamed(data)
    return {}   # a top-level list or value: nothing in it to read


def _renamed(rules):
    """rules with the renamed actions in their tables under their new names."""
    for table in ("buttons", "controller_buttons", "key_bindings"):
        if table in rules:
            rules[table] = renamed_actions(rules[table])
    return rules


def read_conf(path):
    return ft_conf.read(path)


def write_conf_value(key, value, path):
    """Set KEY=value in frametop.conf, keeping comments and the rest of the file (shell-quoted,
    by a temp file and a rename: ft_conf.set_value)."""
    ft_conf.set_value(key, value, path)


def save_rules(rules, path):
    """Write the rules (whole, under the file's lock). Raises OSError or ft_store.StoreError, and
    writes nothing, when the file there doesn't read: read_json gave {} for it, and saving that
    over it would lose everything in it, so it's kept, with a copy."""
    with ft_store.locked(path):
        ft_store.load_json_strict(path, {}, ft_store.is_object)
        ft_store.write_json(path, rules, indent=2, newline=False)


UNCHANGED = object()   # what an edit_rules change returns to leave the file as it is


def edit_rules(path, change):
    """Read the rules, change(rules) them in place and write them back, all under the file's
    lock, so an edit another program saves between the read and the write isn't lost. The rules
    are read as read_json reads them (renamed actions under their new names), but strictly: a
    file that doesn't read raises ft_store.StoreError and is kept, with a copy, as save_rules
    does. Returns what change returned; when that is UNCHANGED nothing is written."""
    with ft_store.locked(path):
        rules = _renamed(ft_store.load_json_strict(path, {}, ft_store.is_object))
        result = change(rules)
        if result is not UNCHANGED:
            ft_store.write_json(path, rules, indent=2, newline=False)
    return result
