#!/usr/bin/env python3
"""Offline test of input/rules-migrate.py, the one-time changes to the input relay's rules: a
key_bindings list of its own gets Meta+Shift+H once (DEE-217), and nothing else in the file
changes. Works on files in a temporary folder, never ~/.config/frametop-input.json.

  input/test/rules-migrate-test.py
"""
import importlib.util
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


def marked(state):
    return (rm.read_state(state) or {}).get(rm.SCREENS_TOGGLE_KEY, False)


def reloads():
    """How many times the relay was told to read its rules again since the last call."""
    n = 0
    while True:
        try:
            n += relay.recv(64) == b"reload"
        except socket.timeout:
            return n


try:
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
finally:
    relay.close()
    for root, dirs, files in os.walk(OUT):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
    shutil.rmtree(OUT, ignore_errors=True)

if failures:
    print(f"FAILED: {', '.join(failures)}")
    sys.exit(1)
print("all passed")
