"""Display Settings' head pin mode (SCREEN_PIN_LEVEL 0, 1 or 2; 2 is DEE-208's walking pin): read
from frametop.conf, written there, and sent to ft-screens live. HOME is a temp folder and the ask
to ft-screens is recorded, not sent. Needs PySide6 (the image's python3).

  python3 display-settings/tests/test_pin_mode.py
"""
import os
import sys
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-settings-pin-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ft_display_settings as fds  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402


class PinMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        os.makedirs(os.path.dirname(fds.CONF_PATH), exist_ok=True)
        with open(fds.CONF_PATH, "w") as f:
            f.write("# mine\n")
        self.b = fds.Backend()
        self.asked = []
        self.b._ask_screens = lambda text, then=None: self.asked.append(text)
        self.b._running = True
        old = fds.ft_layout.backend
        fds.ft_layout.backend = lambda: "screens"
        self.addCleanup(setattr, fds.ft_layout, "backend", old)

    def conf(self):
        with open(fds.CONF_PATH) as f:
            return f.read()

    def test_default_rides_on_the_headset(self):
        self.assertEqual(self.b.pinMode, 0)

    def test_each_mode_written_and_sent(self):
        for mode in (2, 1, 0):
            with self.subTest(mode=mode):
                self.b.setPinMode(mode)
                self.assertIn(f"SCREEN_PIN_LEVEL={mode}", self.conf())
                self.assertEqual(self.b.pinMode, mode)
                self.assertEqual(self.asked[-1], f"handsetting SCREEN_PIN_LEVEL {mode}")
        self.assertIn("# mine", self.conf())

    def test_out_of_range_is_off(self):
        self.b.setPinMode(7)
        self.assertIn("SCREEN_PIN_LEVEL=0", self.conf())
        with open(fds.CONF_PATH, "a") as f:
            f.write("SCREEN_PIN_LEVEL=junk\n")
        self.assertEqual(self.b.pinMode, 0)

    def test_not_sent_without_ft_screens(self):
        self.b._running = False
        self.b.setPinMode(2)
        self.assertEqual(self.asked, [])
        self.assertEqual(self.b.pinMode, 2)


if __name__ == "__main__":
    unittest.main()
