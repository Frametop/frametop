#!/usr/bin/env python3
"""Offline test of input/rules-migrate.py, the one-time changes to the input relay's rules: a
key_bindings list of its own gets Meta+Shift+H once (DEE-217) and Ctrl+Alt+Shift+Esc once (DEE-222),
and nothing else in the file changes. Works on files in a temporary folder, never ~/.config/frametop-input.json.

  input/test/rules-migrate-test.py
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import socket
import stat
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rules_migrate", os.path.join(HERE, "..", "rules-migrate.py"))
rm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rm)
rm.RELAY = f"\0ft_rules_migrate_test_{os.getpid()}_relay"  # never the live relay's
relay = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
relay.bind(rm.RELAY)
relay.settimeout(0.05)

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


OUT = tempfile.mkdtemp(prefix="ft-rules-migrate-test-")
LINE = '    "42+125+35": "screens_toggle",\n'
RELEASE_LINE = '    "29+42+56+1": "release_keys",\n'
# As Frametop Input Settings writes it (json.dump, indent 2, no newline at the end).
CUSTOM = {"key_bindings": {"125+36": "gaze_left", "125+37": "gaze_right", "42+125+33": "float_toggle",
                           "97": "dictate", "125+35": "hands_toggle"},
          "devices": {"d1:01": {"role": "combo", "name": "Bluetooth 5.1 Keyboard"}}, "vr_keyboard": "no_keyboard"}


def case(name, content=None, newline=""):
    """A fresh folder with a rules file (content: a dict laid out as Input Settings does, a
    string as it is, None for no file). (rules path, state path)."""
    folder = os.path.join(OUT, name)
    os.makedirs(folder)
    rules = os.path.join(folder, "frametop-input.json")
    if content is not None:
        with open(rules, "w") as f:
            f.write(content if isinstance(content, str) else json.dumps(content, indent=2) + newline)
    return rules, os.path.join(folder, "state", "rules-migrations.json")


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def marked(state, name=rm.SCREENS_TOGGLE_KEY):
    return (rm.read_state(state) or {}).get(name, False)


def reloads():
    """How many times the relay was told to read its rules again since the last call."""
    n = 0
    while True:
        try:
            n += relay.recv(64) == b"reload"
        except socket.timeout:
            return n


ALL_MIGRATIONS = rm.MIGRATIONS
try:
    # Meta+Shift+H alone first (DEE-217): what every change goes through, shown with it.
    rm.MIGRATIONS = ALL_MIGRATIONS[:1]
    # A list of its own without the combination: it's added once, first, and only that line.
    rules, state = case("added", CUSTOM)
    os.chmod(rules, 0o600)
    before = read(rules)
    check("missing: done", rm.main(rules, state), 0)
    after = read(rules)
    check("missing: added", json.loads(after)["key_bindings"].get("42+125+35"), "screens_toggle")
    check("...first in the list, the others in their order", list(json.loads(after)["key_bindings"]),
          ["42+125+35", *CUSTOM["key_bindings"]])
    check("...every other byte as it was", after.decode().replace(LINE, "", 1).encode(), before)
    check("...the rest of the file too", {k: v for k, v in json.loads(after).items() if k != "key_bindings"},
          {k: v for k, v in CUSTOM.items() if k != "key_bindings"})
    check("...backed up first", read(rules + ".bak-screens-toggle-key"), before)
    check("...mode kept", stat.S_IMODE(os.stat(rules).st_mode), 0o600)
    check("...no temporary file left", sorted(os.listdir(os.path.dirname(rules))),
          ["frametop-input.json", "frametop-input.json.bak-screens-toggle-key", "state"])
    check("...marked as done", marked(state), True)
    check("...the relay told to reload", reloads(), 1)
    rm.main(rules, state)
    check("second run: nothing changes", (read(rules), reloads()), (after, 0))

    # The user removes it again (Input Settings writes the list without it): it stays removed.
    with open(rules, "w") as f:
        json.dump(CUSTOM, f, indent=2)
    removed = read(rules)
    rm.main(rules, state)
    check("removed after the migration: not added again", read(rules), removed)

    # The combination there already, as screens_toggle or as something of the user's own.
    for name, action in (("present", "screens_toggle"), ("taken", "command:notify-send hi")):
        rules, state = case(name, {**CUSTOM, "key_bindings": {**CUSTOM["key_bindings"], "42+125+35": action}})
        before = read(rules)
        rm.main(rules, state)
        check(f"combination bound already ({action}): unchanged", read(rules), before)
        check("...no backup", read(rules + ".bak-screens-toggle-key"), None)
        check("...marked as done", marked(state), True)

    # A file written with a newline at the end (voice/voice_settings.py's write_json) keeps it.
    rules, state = case("newline", CUSTOM, "\n")
    before = read(rules)
    rm.main(rules, state)
    check("newline at the end: kept, only the line added", read(rules).decode().replace(LINE, "", 1).encode(), before)

    # An empty list of its own (every default removed): it had the KWin shortcut, so it gets it.
    rules, state = case("empty", {"key_bindings": {}})
    rm.main(rules, state)
    check("empty list: added", json.loads(read(rules))["key_bindings"], {"42+125+35": "screens_toggle"})

    # Nothing to do: no file, no list of its own (the relay's defaults have it), a file that isn't
    # JSON or isn't laid out as Input Settings writes it. Nothing is written, and it's marked done.
    for name, content in (("no-file", None), ("no-list", {"devices": {}}), ("list-not-a-dict", {"key_bindings": []}),
                          ("not-an-object", "[1, 2]"),
                          ("hand-laid-out", json.dumps(CUSTOM)),  # all on one line
                          ("duplicate-keys", '{\n  "key_bindings": {\n    "97": "dictate",\n    "97": "dictate"\n  }\n}')):
        rules, state = case(name, content)
        before = read(rules)
        rm.main(rules, state)
        check(f"{name}: left as it is", (read(rules), read(rules + ".bak-screens-toggle-key")), (before, None))
        check(f"{name}: marked as done", marked(state), True)

    # A state file that isn't JSON counts as nothing done.
    # A file that isn't JSON may be half written (Input Settings doesn't write in one step): it's
    # left alone and tried again next time.
    for name, content in (("not-json", '{"key_bindings": {"97": "dictate",'), ("zero-bytes", "")):
        rules, state = case(name, content)
        before = read(rules)
        check(f"{name}: waits for the next install", rm.main(rules, state), 1)
        check(f"{name}: left as it is, not marked", (read(rules), marked(state)), (before, False))

    # A state file that can't be read: nothing runs, so nothing can run twice.
    rules, state = case("bad-state", CUSTOM)
    before = read(rules)
    os.makedirs(os.path.dirname(state))
    with open(state, "w") as f:
        f.write("not json")
    check("unreadable state: nothing runs", (rm.main(rules, state), read(rules)), (1, before))

    # A rules file made read-only: left as it is, for good.
    rules, state = case("read-only-file", CUSTOM)
    os.chmod(rules, 0o444)
    before = read(rules)
    rm.main(rules, state)
    check("read-only file: left as it is, marked", (read(rules), marked(state)), (before, True))

    # An older backup of another version of the file stays; this one gets a dated name.
    rules, state = case("old-backup", CUSTOM)
    before = read(rules)
    with open(rules + ".bak-screens-toggle-key", "wb") as f:
        f.write(b"older")
    rm.main(rules, state)
    backups = sorted(n for n in os.listdir(os.path.dirname(rules)) if ".bak-" in n)
    check("old backup: kept", read(rules + ".bak-screens-toggle-key"), b"older")
    check("...this one beside it", (len(backups), read(os.path.join(os.path.dirname(rules), backups[-1]))),
          (2, before))

    # A rules file that can't be replaced (its folder read-only; the backup from an earlier try
    # can still be written): not marked, so the next install tries again. Root writes anyway.
    if os.geteuid() != 0:
        rules, _ = case("read-only", CUSTOM)
        state = os.path.join(OUT, "read-only-state", "rules-migrations.json")  # outside the read-only folder
        before = read(rules)
        shutil.copy2(rules, rules + ".bak-screens-toggle-key")
        os.chmod(os.path.dirname(rules), 0o555)
        try:
            status = rm.main(rules, state)
        finally:
            os.chmod(os.path.dirname(rules), 0o755)
        check("can't write: waits for the next install", status, 1)
        check("...unchanged, not marked, no temporary file", (read(rules), marked(state),
                                                              sorted(os.listdir(os.path.dirname(rules)))),
              (before, False, ["frametop-input.json", "frametop-input.json.bak-screens-toggle-key"]))

    # A symlinked rules file: the file it points to changes, the link stays.
    rules, state = case("symlink", CUSTOM)
    link = os.path.join(os.path.dirname(rules), "link.json")
    os.symlink(rules, link)
    rm.main(link, state)
    check("symlink: kept, its target updated", (os.path.islink(link),
                                                json.loads(read(rules))["key_bindings"].get("42+125+35")),
          (True, "screens_toggle"))

    # Ctrl+Alt+Shift+Esc (DEE-222), with every change, as an install runs them.
    rm.MIGRATIONS = ALL_MIGRATIONS
    check("both changes run, in order", [name for name, _ in rm.MIGRATIONS], ["screens-toggle-key", "release-keys-key"])
    # A list that got Meta+Shift+H already (an install of DEE-217): it gets this one, once.
    with_h = {**CUSTOM, "key_bindings": {"42+125+35": "screens_toggle", **CUSTOM["key_bindings"]}}
    rules, state = case("release-after-217", with_h)
    os.makedirs(os.path.dirname(state))
    with open(state, "w") as f:
        json.dump({rm.SCREENS_TOGGLE_KEY: True}, f)
    os.chmod(rules, 0o600)
    before = read(rules)
    reloads()
    check("after DEE-217: done", rm.main(rules, state), 0)
    after = read(rules)
    check("...Ctrl+Alt+Shift+Esc added first, the others in their order", list(json.loads(after)["key_bindings"]),
          ["29+42+56+1", *with_h["key_bindings"]])
    check("...every other byte as it was", after.decode().replace(RELEASE_LINE, "", 1).encode(), before)
    check("...backed up under its own name", (read(rules + ".bak-release-keys-key"),
                                             read(rules + ".bak-screens-toggle-key")), (before, None))
    check("...mode kept", stat.S_IMODE(os.stat(rules).st_mode), 0o600)
    check("...both marked, the relay told once", (marked(state), marked(state, rm.RELEASE_KEYS_KEY), reloads()),
          (True, True, 1))
    rm.main(rules, state)
    check("...second run: nothing changes", (read(rules), reloads()), (after, 0))
    with open(rules, "wb") as f:
        f.write(before)
    rm.main(rules, state)
    check("...removed afterwards: not added again", read(rules), before)

    # A list that had neither (no install since before DEE-217): both, in one run.
    rules, state = case("release-and-217", CUSTOM, "\n")
    before = read(rules)
    check("neither yet: done", rm.main(rules, state), 0)
    after = read(rules)
    check("...both added, Ctrl+Alt+Shift+Esc first", list(json.loads(after)["key_bindings"]),
          ["29+42+56+1", "42+125+35", *CUSTOM["key_bindings"]])
    check("...every other byte as it was",
          after.decode().replace(RELEASE_LINE, "", 1).replace(LINE, "", 1).encode(), before)
    check("...both marked", (marked(state), marked(state, rm.RELEASE_KEYS_KEY)), (True, True))
    check("...the first backup is the file as it was", read(rules + ".bak-screens-toggle-key"), before)
    check("...the second, the file with Meta+Shift+H in it",
          read(rules + ".bak-release-keys-key").decode().replace(LINE, "", 1).encode(), before)
    check("...the relay told after each", reloads(), 2)

    # The second write fails after the first worked: Meta+Shift+H stays and is marked, and the
    # next install adds Ctrl+Alt+Shift+Esc.
    rules, state = case("release-fails", CUSTOM)
    before = read(rules)
    real_write = rm.write_atomic

    def full_disk(path, data, mode=0o644):
        if path == os.path.realpath(rules) and RELEASE_LINE.encode() in data:
            raise OSError(28, "No space left on device")
        real_write(path, data, mode)
    rm.write_atomic = full_disk
    try:
        status = rm.main(rules, state)
    finally:
        rm.write_atomic = real_write
    check("second write fails: waits for the next install", status, 1)
    check("...Meta+Shift+H in and marked, Ctrl+Alt+Shift+Esc neither",
          ("42+125+35" in json.loads(read(rules))["key_bindings"], "29+42+56+1" in json.loads(read(rules))["key_bindings"],
           marked(state), marked(state, rm.RELEASE_KEYS_KEY)), (True, False, True, False))
    check("next install: added", (rm.main(rules, state), list(json.loads(read(rules))["key_bindings"])[:2],
                                  marked(state, rm.RELEASE_KEYS_KEY)), (0, ["29+42+56+1", "42+125+35"], True))
    check("...every other byte as it was",
          read(rules).decode().replace(RELEASE_LINE, "", 1).replace(LINE, "", 1).encode(), before)
    reloads()

    # A file that isn't JSON: said once, not once for each change, and neither is marked.
    rules, state = case("release-not-json", '{"key_bindings": {')
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = rm.main(rules, state)
    check("not JSON, both waiting: said once, nothing marked",
          (status, out.getvalue().count("isn't JSON"), marked(state), marked(state, rm.RELEASE_KEYS_KEY)),
          (1, 1, False, False))

    # The combination bound already, to release_keys or to something of the user's own: kept.
    for name, action in (("release-present", "release_keys"), ("release-taken", "command:notify-send hi")):
        rules, state = case(name, {**with_h, "key_bindings": {**with_h["key_bindings"], "29+42+56+1": action}})
        before = read(rules)
        rm.main(rules, state)
        check(f"Ctrl+Alt+Shift+Esc bound already ({action}): unchanged, no backup, marked",
              (read(rules), read(rules + ".bak-release-keys-key"), marked(state, rm.RELEASE_KEYS_KEY)),
              (before, None, True))

    # No list of its own: the relay's defaults have it. Nothing written.
    rules, state = case("release-defaults", {"devices": {}})
    before = read(rules)
    rm.main(rules, state)
    check("no list of its own: left as it is, marked", (read(rules), marked(state, rm.RELEASE_KEYS_KEY)),
          (before, True))

    # Laid out by hand: left alone, with a note naming the setting to use.
    rules, state = case("release-hand-laid-out", json.dumps(with_h))
    before = read(rules)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rm.main(rules, state)
    check("laid out by hand: left as it is", read(rules), before)
    check("...the note says what to add, and where",
          any("add Ctrl+Alt+Shift+Esc (Release all keys) on its Keyboard page" in line
              for line in out.getvalue().splitlines()), True)
finally:
    rm.MIGRATIONS = ALL_MIGRATIONS
    relay.close()
    for root, dirs, files in os.walk(OUT):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
    shutil.rmtree(OUT, ignore_errors=True)

if failures:
    print(f"FAILED: {', '.join(failures)}")
    sys.exit(1)
print("all passed")
