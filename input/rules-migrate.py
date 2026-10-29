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
  bound to anything keeps it, and so does a file made read-only. The relay is told to read the
  file again. A file that isn't JSON (Input Settings may be halfway through writing it) waits for
  the next install.

  release-keys-key (DEE-222, 2026-10-29): Ctrl+Alt+Shift+Esc lets go of every key, for a modifier
  stuck in KWin ("29+42+56+1": release_keys, a new default). Added the same way, under its own
  name, so a list that got Meta+Shift+H already gets this one too.

  input/rules-migrate.py [RULES [STATE]]
Python 3 standard library only: it runs on the SteamOS host.
"""
import json
import os
import shutil
import socket
import sys
import tempfile
import time

RULES = os.path.expanduser("~/.config/frametop-input.json")
STATE = os.path.expanduser("~/.local/state/frametop/input/rules-migrations.json")
RELAY = "\0frametop_relay"
SCREENS_TOGGLE_KEY = "screens-toggle-key"
SCREENS_COMBO, SCREENS_ACTION = "42+125+35", "screens_toggle"
RELEASE_KEYS_KEY = "release-keys-key"
RELEASE_COMBO, RELEASE_ACTION = "29+42+56+1", "release_keys"


def read_state(path):
    """The changes that ran: {name: true}; {} with no file yet. None if it can't be read: then
    nothing runs, so a change can't run twice."""
    try:
        with open(path) as f:
            state = json.load(f)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError):
        return None
    return state if isinstance(state, dict) else None


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


def add_binding(path, name, combo, action, keys, what, setting):
    """Add combo: action to a key_bindings list of its own that lacks the combination, first in
    the list. keys, what and setting name it in messages ("Meta+Shift+H", "hide or show the
    screens", "Hide/show desktop screens": its name in Input Settings). (done, message): done
    False means try again on the next install."""
    path = os.path.realpath(path)  # a symlinked rules file: write where it points
    try:
        with open(path, "rb") as f:
            raw = f.read()
        mode = os.stat(path).st_mode & 0o7777
    except FileNotFoundError:
        return True, None  # the relay's defaults, the combination included
    except OSError as e:
        return False, f"key combinations: can't read {path} ({e}); {keys} wasn't added"
    try:
        text = raw.decode("utf-8")
        rules = json.loads(text)
    except ValueError:
        # The relay reads it as no rules (the defaults), but it may be half written: next time.
        return False, f"key combinations: {path} isn't JSON; left as it is"
    bindings = rules.get("key_bindings") if isinstance(rules, dict) else None
    if not isinstance(bindings, dict) or combo in bindings:
        return True, None  # the defaults, or the combination is the user's already
    laid_out = json.dumps(rules, indent=2)
    if not text.startswith(laid_out) or text[len(laid_out):].strip():
        return True, (f"key combinations: {path} isn't laid out as Frametop Input Settings writes it, so it's "
                      f"left as it is: add {keys} ({setting}) on its Keyboard page")
    if not mode & 0o200:
        return True, f"key combinations: {path} is read-only, so it's left as it is: {keys} wasn't added"
    rules["key_bindings"] = {combo: action, **bindings}
    backup = path + ".bak-" + name
    try:
        if read(backup) not in (None, raw):  # an earlier try's, of another version of the file
            backup += time.strftime("-%Y%m%d-%H%M%S")
        shutil.copy2(path, backup)
        write_atomic(path, (json.dumps(rules, indent=2) + text[len(laid_out):]).encode(), mode)
    except OSError as e:
        return False, f"key combinations: can't write {path} ({e}); {keys} wasn't added"
    reload_relay()
    return True, f"key combinations: {keys} ({what}) added to your own list (before: {backup})"


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def reload_relay():
    """Have the input relay read its rules again, if it runs."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(b"reload", RELAY)
    except OSError:
        pass  # not running: it reads them when it starts
    finally:
        s.close()


def screens_toggle_key(path):
    return add_binding(path, SCREENS_TOGGLE_KEY, SCREENS_COMBO, SCREENS_ACTION, "Meta+Shift+H",
                       "hide or show the screens", "Hide/show desktop screens")


def release_keys_key(path):
    return add_binding(path, RELEASE_KEYS_KEY, RELEASE_COMBO, RELEASE_ACTION, "Ctrl+Alt+Shift+Esc",
                       "let go of every key", "Release all keys")


MIGRATIONS = ((SCREENS_TOGGLE_KEY, screens_toggle_key), (RELEASE_KEYS_KEY, release_keys_key))


def main(rules=RULES, state_path=STATE):
    """0 when every change has run, 1 when one waits for the next install."""
    state = read_state(state_path)
    if state is None:
        print(f"key combinations: can't read {state_path}, so no one-time change runs")
        return 1
    status, said = 0, set()
    for name, migrate in MIGRATIONS:
        if state.get(name):
            continue
        done, message = migrate(rules)
        if message and message not in said:  # a file that isn't JSON is the same news for each
            print(message)
            said.add(message)
        if not done:
            status = 1
            continue
        state[name] = True
        os.makedirs(os.path.dirname(state_path), exist_ok=True)
        write_atomic(state_path, (json.dumps(state, indent=2) + "\n").encode())
    return status


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
