#!/usr/bin/env python3
"""DESKTOP_CURSOR in frametop.conf (issue 76): 1 has KWin draw its own cursor into the
screens' pictures (KWIN_FORCE_SW_CURSOR=1), for input that isn't the 3D mouse or a laser,
like Deskflow's. Off by default, and anything but 1 leaves KWin as it was. The script's own
default and its DESKTOP_CURSOR line run here; nothing else of the session starts.

  python3 session/tests/test_desktop_cursor.py
"""
import os
import re
import subprocess
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
with open(os.path.join(HERE, "..", "frametop-session.sh")) as f:
    SCRIPT = f.read()
DEFAULTS = re.search(r"^NOTIFY_FORWARD=.*\bDESKTOP_CURSOR=\S+.*$", SCRIPT, re.MULTILINE).group(0)
SWITCH = re.search(r'^\[ "\$DESKTOP_CURSOR" .*KWIN_FORCE_SW_CURSOR.*$', SCRIPT, re.MULTILINE).group(0)


def kwin_env(conf=""):
    """What KWin gets as KWIN_FORCE_SW_CURSOR ("unset" for none) with this line in the conf."""
    script = "set -eu\n" + DEFAULTS + "\n" + conf + "\n" + SWITCH + '\necho "${KWIN_FORCE_SW_CURSOR-unset}"\n'
    env = {k: v for k, v in os.environ.items() if k != "KWIN_FORCE_SW_CURSOR"}
    p = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env, timeout=10)
    assert p.returncode == 0, p.stderr
    return p.stdout.strip()


class DesktopCursor(unittest.TestCase):
    def test_off_unless_asked(self):
        self.assertEqual(kwin_env(), "unset")
        self.assertEqual(kwin_env("DESKTOP_CURSOR=0"), "unset")

    def test_on(self):
        self.assertEqual(kwin_env("DESKTOP_CURSOR=1"), "1")

    def test_only_one_turns_it_on(self):
        for value in ("yes", "true", "2", "''"):
            self.assertEqual(kwin_env("DESKTOP_CURSOR=" + value), "unset", value)

    def test_the_cursor_is_set_before_kwin_starts(self):
        self.assertLess(SCRIPT.index(SWITCH), SCRIPT.index("startplasma-wayland"))


if __name__ == "__main__":
    unittest.main()
