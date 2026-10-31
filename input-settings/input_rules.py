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

# Actions renamed since a release (the relay's RENAMED_ACTIONS): a rules file saved before shows
# and saves them under their new names.
RENAMED_ACTIONS = {"spotwatch": "frunner"}


def renamed_actions(table):
    if not isinstance(table, dict):
        return table
    return {k: renamed_actions(v) if isinstance(v, dict) else RENAMED_ACTIONS.get(v, v) if isinstance(v, str) else v
            for k, v in table.items()}


def read_json(path):
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if isinstance(data, dict):
        for table in ("buttons", "controller_buttons", "key_bindings"):
            if table in data:
                data[table] = renamed_actions(data[table])
        return data
    return {}   # a top-level list or value: nothing in it to read (DEE-296)


def read_conf(path):
    return ft_conf.read(path)


def write_conf_value(key, value, path):
    """Set KEY=value in frametop.conf, keeping comments and the rest of the file (shell-quoted,
    by a temp file and a rename: ft_conf.set_value)."""
    ft_conf.set_value(key, value, path)


def save_rules(rules, path):
    """Write the rules (whole, under the file's lock). Raises OSError or ft_store.StoreError, and
    writes nothing, when the file there doesn't read: read_json gave {} for it, and saving that
    over it would lose everything in it, so it's kept, with a copy (DEE-300)."""
    with ft_store.locked(path):
        ft_store.load_json_strict(path, {}, ft_store.is_object)
        ft_store.write_json(path, rules, indent=2, newline=False)
