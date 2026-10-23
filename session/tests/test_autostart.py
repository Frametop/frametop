#!/usr/bin/env python3
"""Tests for session/autostart.sh's reading of AUTOSTART (DEE-190): off when the key is missing,
0 or anything but 1, on for 1 with or without a comment, and the last line wins, as when the
session sources the file. FT_AUTOSTART_DRY stops it before it waits for SteamVR or starts
anything, and FT_CONF points it at a temporary file, so the real settings aren't read.

  python3 session/tests/test_autostart.py
"""
import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.path.join(HERE, "..", "autostart.sh")


def run(conf_text):
    with tempfile.TemporaryDirectory() as d:
        conf = os.path.join(d, "frametop.conf")
        if conf_text is not None:
            with open(conf, "w") as f:
                f.write(conf_text)
        env = dict(os.environ, FT_CONF=conf, FT_AUTOSTART_DRY="1")
        p = subprocess.run(["bash", SCRIPT], env=env, capture_output=True, text=True, timeout=10)
        return p.returncode, p.stdout


class Autostart(unittest.TestCase):
    def assertOn(self, text):
        code, out = run(text)
        self.assertEqual(code, 0)
        self.assertIn("would start", out)

    def assertOff(self, text):
        code, out = run(text)
        self.assertEqual(code, 0)
        self.assertIn("is off", out)

    def test_no_file(self):
        self.assertOff(None)

    def test_missing_key(self):
        self.assertOff("BACKEND=screens\nREMOTE=1\n")

    def test_example_default(self):
        with open(os.path.join(HERE, "..", "frametop.conf.example")) as f:
            self.assertOff(f.read())

    def test_on(self):
        self.assertOn("AUTOSTART=1\n")

    def test_on_with_comment(self):
        self.assertOn("AUTOSTART=1        # 1 = the desktop starts whenever SteamVR does\n")

    def test_other_values_are_off(self):
        for v in ("0", "yes", "11", ""):
            with self.subTest(v=v):
                self.assertOff(f"AUTOSTART={v}\n")

    def test_commented_out(self):
        self.assertOff("#AUTOSTART=1\n")

    def test_last_line_wins(self):
        self.assertOff("AUTOSTART=1\nAUTOSTART=0\n")
        self.assertOn("AUTOSTART=0\nAUTOSTART=1\n")


if __name__ == "__main__":
    unittest.main()
