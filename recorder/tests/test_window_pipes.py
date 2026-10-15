#!/usr/bin/python3
"""recorder/window.py's reader of hub.py's output (the upload and the login): a child that
writes more to stderr than a pipe holds doesn't stop it, and its last stderr line is the error
when there's no JSON one (2026-10-15). Needs PySide6 (the standalone recorder's venv, or the dev
container).

  python3 recorder/tests/test_window_pipes.py
"""
import os
import subprocess
import sys
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
try:
    from recorder.window import RecorderBackend
except ImportError:   # no PySide6
    RecorderBackend = None


class Lines:
    def __init__(self):
        self.got = []

    def emit(self, obj):
        self.got.append(obj)


def read(code, timeout=30):
    proc = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, bufsize=1)
    lines, got = Lines(), {}
    t = threading.Thread(target=lambda: got.update(last=RecorderBackend._read_lines(
        None, proc, lines, lambda: False, "The upload stopped")), daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        proc.kill()
        raise AssertionError("the reader hung on hub.py's pipes")
    return got["last"], lines.got


@unittest.skipIf(RecorderBackend is None, "needs PySide6")
class ReadLinesTest(unittest.TestCase):
    NOISY = "import sys, json; sys.stderr.write('Retrying in 1s\\n' * 30000); sys.stderr.flush(); "

    def test_noisy_stderr(self):
        last, lines = read(self.NOISY + "print(json.dumps({'phase': 'upload'}), flush=True); "
                                        "print(json.dumps({'done': {'pr_url': 'x'}}), flush=True)")
        self.assertEqual(last, {"done": {"pr_url": "x"}})
        self.assertEqual(lines, [{"phase": "upload"}])

    def test_noisy_stderr_then_a_crash(self):
        last, _ = read(self.NOISY + "sys.stderr.write('OSError: [Errno 104] Connection reset by peer\\n'); sys.exit(1)")
        self.assertEqual(last, {"error": {"kind": "hub", "text": "The upload stopped: OSError: [Errno 104] "
                                                                  "Connection reset by peer"}})

    def test_quiet_exit(self):
        last, _ = read("import sys; sys.exit(3)")
        self.assertEqual(last, {"error": {"kind": "hub", "text": "The upload stopped: exit status 3"}})


if __name__ == "__main__":
    unittest.main()
