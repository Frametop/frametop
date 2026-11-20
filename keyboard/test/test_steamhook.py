"""steamhook.py, offline: its thread against a fake Steam session (what evaluate() answers is
scripted), and steamhook.js itself under node against a fake Steam window, when node is
installed (steamhook-test.js)."""
import json
import os
import select
import shutil
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "host"))
import steamhook  # noqa: E402
import steamui  # noqa: E402  (steamhook put steam/ on the path)


class FakeSession:
    """Answers the install with True, each poll with the next scripted state, typing with
    what `typing` says; a poll answer of "gone" is Steam going away."""

    def __init__(self, polls, typing=True):
        self.polls = list(polls)
        self.typing = typing
        self.seen = []
        self.closed = False
        self.lock = threading.Lock()

    def evaluate(self, expression, timeout=2.0):
        with self.lock:
            self.seen.append(expression)
        if expression.startswith("(") and "install(root, leaseMs)" in expression:
            return True
        if ".poll(" in expression:
            state = self.polls.pop(0) if self.polls else {"events": [], "session": None}
            if state == "gone":
                raise steamui.SteamUnreachable("gone")
            return state
        if ".type(" in expression or ".key(" in expression:
            return self.typing
        return None

    def close(self):
        self.closed = True


class SteamHookTest(unittest.TestCase):
    def results(self, hook, want, seconds=3.0):
        out, end = [], time.monotonic() + seconds
        while time.monotonic() < end and len(out) < want:
            if select.select([hook], [], [], 0.05)[0]:
                out += hook.results()
        return out

    def test_installs_polls_and_types(self):
        session = FakeSession([{"events": [{"open": 1, "purpose": "text"}], "session": 1}])
        hook = steamhook.SteamHook(lambda: session, source="(function install(root, leaseMs) {})")
        hook.take = True
        hook.start()
        try:
            got = self.results(hook, 2)
            self.assertEqual(got, [("steam", True), ("event", {"open": 1, "purpose": "text"})])
            self.assertEqual(session.seen[0], "((function install(root, leaseMs) {}))(globalThis, 1500)")
            self.assertIn("poll(true)", session.seen[1])
            done = []
            hook.type(1, 'a"b', done.append)
            hook.key(1, "Backspace", ["Ctrl"], done.append)
            got = self.results(hook, 2)
            for _, callback, ok in got:
                callback(ok)
            self.assertEqual(done, [True, True])
            typed = [e for e in session.seen if ".type(" in e or ".key(" in e]
            self.assertEqual(typed[0], f'{steamhook.GLOBAL}?.type(...{json.dumps([1, "a" + chr(34) + "b"])}) === true')
            self.assertEqual(typed[1], f'{steamhook.GLOBAL}?.key(...{json.dumps([1, "Backspace", ["Ctrl"]])}) === true')
        finally:
            hook.stop()
        self.assertTrue(session.closed)
        self.assertEqual(session.seen[-1], f"{steamhook.GLOBAL}?.dispose()")   # out at once
        with self.assertRaises(OSError):
            os.fstat(hook.fileno())   # its wake pipe is closed

    def test_steam_going_away_fails_typing_and_tries_again(self):
        sessions = []

        def factory():
            if sessions:
                raise steamui.SteamUnreachable("not again yet")
            sessions.append(FakeSession(["gone"]))
            return sessions[-1]
        old = steamhook.RETRY
        steamhook.RETRY = 0.05
        hook = steamhook.SteamHook(factory, source="(function install(root, leaseMs) {})")
        done = []
        hook.type(1, "x", done.append)   # waiting when Steam goes
        hook.start()
        try:
            got = self.results(hook, 3)
            kinds = [g[0] for g in got]
            self.assertEqual(kinds[:2], ["steam", "steam"])
            self.assertEqual([g[1] for g in got[:2]], [True, False])
            for item in got:
                if item[0] == "done":
                    item[1](item[2])
            self.assertEqual(done, [False])
        finally:
            hook.stop()
            steamhook.RETRY = old


@unittest.skipUnless(shutil.which("node"), "steamhook.js's test needs node, which isn't installed here")
class SteamHookJs(unittest.TestCase):
    def test_in_a_fake_steam_window(self):
        done = subprocess.run(["node", str(HERE / "steamhook-test.js"), str(HERE.parent / "host" / "steamhook.js")],
                              capture_output=True, text=True, timeout=60, env=dict(os.environ, NODE_OPTIONS=""))
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
