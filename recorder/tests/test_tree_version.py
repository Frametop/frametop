#!/usr/bin/python3
"""recorder/common.tree_version: what a session records as Frametop's version. A checkout gives
its git describe; a release's folder has no .git, so its .frametop-release gives the version and
commit; a checkout's own .frametop-release (only BOX=) or no file at all gives "".

  python3 recorder/tests/test_tree_version.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
from recorder import common  # noqa: E402

RELEASE = ("VERSION=0.3.0-exp.12\nCOMMIT=789bb44a1b2c3d4e5f60718293a4b5c6d7e8f901\nCHANNEL=experimental\n"
           "IMAGE=localhost/frametop:0.3.0-exp.12\nIMAGE_ID=0123\nBOX=frametop-0123\n")


class TreeVersionTest(unittest.TestCase):
    def write(self, d, name, text):
        with open(os.path.join(d, name), "w") as f:
            f.write(text)

    def test_release(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, ".frametop-release", RELEASE)
            self.assertEqual(common.tree_version(d), "v0.3.0-exp.12 release 789bb44")

    def test_release_without_commit(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, ".frametop-release", "VERSION=0.2.2\nBOX=frametop-a79f\n")
            self.assertEqual(common.tree_version(d), "v0.2.2 release")

    def test_checkout_box_only(self):
        # a checkout run in a release's container: .frametop-release names only BOX, and no .git here
        with tempfile.TemporaryDirectory() as d:
            self.write(d, ".frametop-release", "BOX=frametop-7269ba475198\n")
            self.assertEqual(common.tree_version(d), "")

    def test_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(common.tree_version(d), "")

    def test_checkout(self):
        with tempfile.TemporaryDirectory() as d:
            git = ["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.invalid"]
            try:
                subprocess.run(git[:3] + ["init", "-q"], check=True, capture_output=True)
                self.write(d, "a", "a\n")
                subprocess.run(git + ["add", "a"], check=True, capture_output=True)
                subprocess.run(git + ["commit", "-qm", "a"], check=True, capture_output=True)
                subprocess.run(git + ["tag", "v9.9.9"], check=True, capture_output=True)
            except (OSError, subprocess.CalledProcessError):
                self.skipTest("no git")
            # the checkout wins over a .frametop-release next to it (a dev tree's BOX= file)
            self.write(d, ".frametop-release", RELEASE)
            self.assertEqual(common.tree_version(d), "v9.9.9")


if __name__ == "__main__":
    unittest.main()
