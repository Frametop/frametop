#!/usr/bin/python3
"""recorder/one_window.py: a second launch closes an idle window, leaves a busy one (and raises
it), SIGTERMs a stuck one, and the Qt listener answers. The listener test needs PySide6 (the
standalone recorder's venv, or the dev container); the rest runs on any Python.

  python3 recorder/tests/test_one_window.py
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
from recorder import one_window  # noqa: E402

try:
    import PySide6  # noqa: F401
    HAVE_QT = True
except ImportError:
    HAVE_QT = False


def name():
    return "ft_test_one_window_%d_%d" % (os.getpid(), time.monotonic_ns())


class FakeWindow:
    """A window's socket on a thread: answers like Listener, closes it on "quit" unless busy."""

    def __init__(self, name, busy=""):
        self.sock = one_window._bind(name)
        self.busy, self.got = busy, []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            cmd = conn.recv(256).decode().strip()
            self.got.append(cmd)
            if cmd == "where":
                conn.sendall(json.dumps({"pid": os.getpid(), "where": "in SteamVR's dashboard",
                                         "busy": self.busy}).encode() + b"\n")
            elif cmd == "quit":
                conn.sendall(b"busy\n" if self.busy else b"ok\n")
                conn.close()
                if not self.busy:
                    self.sock.close()
                    return
                continue
            else:
                conn.sendall(b"ok\n")
            conn.close()


class TakeTest(unittest.TestCase):
    def test_free(self):
        n = name()
        s = one_window.take(n, "ft_handrec.py")
        self.assertIsNone(one_window._bind(n))   # held
        s.close()
        self.assertIsNotNone(one_window._bind(n))

    def test_idle_window_closes(self):
        n = name()
        w = FakeWindow(n)
        s = one_window.take(n, "ft_handrec.py", wait=5)
        self.assertEqual(w.got, ["where", "quit"])
        s.close()

    def test_busy_window_stays(self):
        n = name()
        w = FakeWindow(n, busy="in the middle of a round")
        with self.assertRaises(one_window.Taken) as cm:
            one_window.take(n, "ft_handrec.py", wait=1)
        self.assertIn("in SteamVR's dashboard, in the middle of a round", cm.exception.text)
        self.assertEqual(w.got, ["where", "show"])
        self.assertIsNone(one_window._bind(n))   # still its

    def test_stuck_window_gets_sigterm(self):
        """A process named like the recorder that listens and never answers: SIGTERM ends it."""
        n = name()
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "ft_handrec.py")
            with open(script, "w") as f:
                f.write("import socket, time\n"
                        "s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
                        "s.bind('\\0%s'); s.listen(4)\n"
                        "print('up', flush=True)\n"
                        "time.sleep(60)\n" % n)
            p = subprocess.Popen([sys.executable, script], stdout=subprocess.PIPE)
            try:
                self.assertEqual(p.stdout.readline().strip(), b"up")
                one_window.ask.__defaults__ = (0.5,)
                try:
                    s = one_window.take(n, "ft_handrec.py", wait=5)
                finally:
                    one_window.ask.__defaults__ = (one_window.ASK_S,)
                self.assertEqual(p.wait(5), -15)
                s.close()
            finally:
                p.kill()

    def test_stuck_other_process_isnt_killed(self):
        n = name()
        holder = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        holder.bind("\0" + n)
        holder.listen(4)
        one_window.ask.__defaults__ = (0.5,)
        try:
            with self.assertRaises(one_window.Taken):
                one_window.take(n, "ft_handrec.py", wait=1)   # this test's process isn't ft_handrec.py
        finally:
            one_window.ask.__defaults__ = (one_window.ASK_S,)
            holder.close()


@unittest.skipUnless(HAVE_QT, "needs PySide6")
class ListenerTest(unittest.TestCase):
    def test_answers(self):
        from PySide6.QtCore import QCoreApplication
        app = QCoreApplication.instance() or QCoreApplication([])
        n = name()
        state = {"busy": "exporting a recording", "shown": 0, "quit": 0}

        def quit():
            if state["busy"]:
                return False
            state["quit"] += 1
            return True
        listener = one_window.Listener(one_window.take(n, "ft_handrec.py"), lambda: state["busy"],
                                       lambda: state.__setitem__("shown", state["shown"] + 1), quit)
        answers = []

        def client():
            for cmd in ("where", "show", "quit"):
                answers.append(one_window.ask(n, cmd, timeout=3)[0])
            state["busy"] = ""
            answers.append(one_window.ask(n, "quit", timeout=3)[0])
        t = threading.Thread(target=client)
        t.start()
        end = time.monotonic() + 10
        while t.is_alive() and time.monotonic() < end:
            app.processEvents()
            time.sleep(0.01)
        t.join(1)
        where = json.loads(answers[0])
        self.assertEqual((where["pid"], where["busy"]), (os.getpid(), "exporting a recording"))
        self.assertEqual(answers[1:], ["ok", "busy", "ok"])
        self.assertEqual((state["shown"], state["quit"]), (1, 1))
        listener.sock.close()


if __name__ == "__main__":
    unittest.main()
