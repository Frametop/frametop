#!/usr/bin/env python3
"""One-time changes to the input relay's rules (~/.config/frametop-input.json) for a "key_bindings"
list of its own, which gets no new defaults (input-relay.py's read_rules). scripts/conf-migrate.sh
runs this on every install and update. Each change runs once: STATE records it, so a binding the
user removes afterwards stays removed.

  screens-toggle-key (DEE-217, 2026-10-28): Meta+Shift+H hides and shows the screens. It was a KWin
  shortcut, which a list of its own didn't need to have; now it's the relay's default ("42+125+35":
  screens_toggle). A list without that combination gets it, first in the list, and nothing else in
  the file changes: every other line stays byte for byte, so a file laid out other than as Frametop
  Input Settings writes it (json indent 2) is left alone, with a note. A list with the combination
  bound to anything keeps it. The file is backed up first (RULES.bak-screens-toggle-key) and
  replaced in one step, with its mode.

  input/rules-migrate.py [RULES [STATE]]
Python 3 standard library only: it runs on the SteamOS host.
"""
import json
import os
import shutil
import sys
import tempfile

RULES = os.path.expanduser("~/.config/frametop-input.json")
STATE = os.path.expanduser("~/.local/state/frametop/input/rules-migrations.json")
SCREENS_TOGGLE_KEY = "screens-toggle-key"
SCREENS_COMBO, SCREENS_ACTION = "42+125+35", "screens_toggle"


def read_state(path):
    """The changes that ran: {name: true}. A missing or unreadable file: none."""
    try:
        with open(path) as f:
            state = json.load(f)
    except (OSError, ValueError):
        return {}
    return state if isinstance(state, dict) else {}


def write_atomic(path, data, mode=0o644):
    """Write data (bytes) to path in one step: a temporary file beside it, then a rename."""
    folder = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(dir=folder, prefix="." + os.path.basename(path) + ".")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def screens_toggle_key(path):
    """Add Meta+Shift+H to a key_bindings list of its own that lacks the combination. (done,
    message): done False means try again on the next install."""
    path = os.path.realpath(path)  # a symlinked rules file: write where it points
    try:
        with open(path, "rb") as f:
            raw = f.read()
        mode = os.stat(path).st_mode & 0o7777
    except FileNotFoundError:
        return True, None  # the relay's defaults, Meta+Shift+H included
    except OSError as e:
        return False, f"key combinations: can't read {path} ({e}); Meta+Shift+H wasn't added"
    try:
        text = raw.decode("utf-8")
        rules = json.loads(text)
    except ValueError:
        # The relay reads it as no rules: the defaults, Meta+Shift+H included.
        return True, f"key combinations: {path} isn't JSON; left as it is"
    bindings = rules.get("key_bindings") if isinstance(rules, dict) else None
    if not isinstance(bindings, dict) or SCREENS_COMBO in bindings:
        return True, None  # the defaults, or the combination is the user's already
    laid_out = json.dumps(rules, indent=2)
    if not text.startswith(laid_out) or text[len(laid_out):].strip():
        return True, (f"key combinations: {path} isn't laid out as Frametop Input Settings writes it, so it's "
                      "left as it is: add Meta+Shift+H (Hide/show desktop screens) on its Keyboard page")
    rules["key_bindings"] = {SCREENS_COMBO: SCREENS_ACTION, **bindings}
    backup = path + ".bak-" + SCREENS_TOGGLE_KEY
    try:
        if not os.path.exists(backup):  # an earlier run's is the older one
            shutil.copy2(path, backup)
        write_atomic(path, (json.dumps(rules, indent=2) + text[len(laid_out):]).encode(), mode)
    except OSError as e:
        return False, f"key combinations: can't write {path} ({e}); Meta+Shift+H wasn't added"
    return True, ("key combinations: Meta+Shift+H (hide or show the screens) added to your own list "
                  f"(before: {backup})")


MIGRATIONS = ((SCREENS_TOGGLE_KEY, screens_toggle_key),)


def main(rules=RULES, state_path=STATE):
    state = read_state(state_path)
    for name, migrate in MIGRATIONS:
        if state.get(name):
            continue
        done, message = migrate(rules)
        if message:
            print(message)
        if done:
            state[name] = True
            os.makedirs(os.path.dirname(state_path), exist_ok=True)
            write_atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
