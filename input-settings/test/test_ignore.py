"""Frametop Settings' Ignored panels (POINTER_IGNORE): two or more ignored panels,
keys with spaces, quotes and $() in them, are written so that bash sources frametop.conf under
set -eu without running anything, and read back the same by the app and by bash. Before, ", "
joined them and the session ran the second key as a command (exit 127). HOME is a temporary
directory; nothing goes to the host. Needs PySide6 (the image's python3).
Run: python3 input-settings/test/test_ignore.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HOME = tempfile.mkdtemp()
os.environ["HOME"] = HOME
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "input-settings"))
import ft_input_settings as fis  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402


class Done:
    returncode, stdout, stderr = 1, "", ""


fis.host = lambda *cmd, timeout=20: Done()

KEYS = ["vendor.one.overlay", "evil.$(touch canary).x y", "it's;here `touch canary` \"q\""]


class Ignored(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.b = fis.Backend()
        cls.b._send = lambda text, to=fis.RELAY: True

    def setUp(self):
        os.makedirs(os.path.dirname(fis.CONF_PATH), exist_ok=True)
        shutil.copy(ROOT / "session" / "frametop.conf.example", fis.CONF_PATH)
        self.cwd = tempfile.mkdtemp()

    def source(self):
        """POINTER_IGNORE as the session's bash has it after `set -eu; . frametop.conf`."""
        p = subprocess.run(["bash", "-c", 'set -eu; . "$1"; printf %s "$POINTER_IGNORE"', "_", fis.CONF_PATH],
                           capture_output=True, text=True, cwd=self.cwd, check=False)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(os.listdir(self.cwd), [], "sourcing the conf ran a command")
        return p.stdout

    def test_two_or_more_panels_source_and_round_trip(self):
        for key in KEYS:
            self.b.setPanelIgnored(key, True)
        self.b.setAppIgnored("vendor.two", True)
        want = KEYS + ["vendor.two*"]
        self.assertEqual(self.b._ignore_list(), want)
        self.assertEqual(self.source(), ",".join(want))
        line = next(t for t in Path(fis.CONF_PATH).read_text().split("\n") if t.startswith("POINTER_IGNORE="))
        self.assertIn("# overlay keys the cursor passes through", line)   # the example's comment kept
        self.b.removeIgnore(KEYS[1])
        self.assertEqual(self.b._ignore_list(), [KEYS[0], KEYS[2], "vendor.two*"])
        self.assertEqual(self.source(), ",".join([KEYS[0], KEYS[2], "vendor.two*"]))

    def test_plain_keys_stay_bare(self):
        self.b.setPanelIgnored("a.b", True)
        self.b.setAppIgnored("c.d", True)
        self.assertIn("POINTER_IGNORE=a.b,c.d*   #", Path(fis.CONF_PATH).read_text())
        self.assertEqual(self.source(), "a.b,c.d*")

    def test_an_old_comma_space_line_is_replaced(self):
        """A conf the old writer left (bash can't source it): the next change writes it right."""
        old = Path(fis.CONF_PATH).read_text()
        text = old.replace("POINTER_IGNORE=   ", "POINTER_IGNORE=a.b, c.d*", 1)
        self.assertNotEqual(text, old)
        Path(fis.CONF_PATH).write_text(text)
        self.assertEqual(self.b._ignore_list(), [])   # left out with a warning, as bash can't read it
        self.b.setPanelIgnored("e.f", True)
        self.assertEqual(self.source(), "e.f")

    def test_a_key_with_a_comma_is_not_kept(self):
        self.b.setPanelIgnored("a,b", True)
        self.assertEqual(self.b._ignore_list(), [])


if __name__ == "__main__":
    unittest.main()
