"""Display Settings' head pin mode (SCREEN_PIN_LEVEL 0, 1 or 2; 2 is the walking pin): read
from frametop.conf, written there, and sent to ft-screens live. And the per-screen pin, to the
dashboard too. HOME is a temp folder and the ask
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


class PinTo(unittest.TestCase):
    """The per-screen pin, the dashboard one included, and what it says when ft-screens is still
    waiting for the dashboard to open."""

    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        self.b = fds.Backend()
        self.asked = []
        self.b._ask_screens = lambda text, then=None: self.asked.append(text)
        self.b._check_running = lambda: None
        self.b._running = True
        self.said = []
        self.b.message.connect(lambda text, error: self.said.append((text, error)))

    def test_pin_and_unpin_sent(self):
        self.b.pin("2", "dashboard")
        self.b.pin("2", "none")
        self.assertEqual(self.asked, ["pin 2 dashboard", "unpin 2"])

    def test_waiting_for_the_dashboard_said(self):
        self.b._pinned("1", "dashboard", "ok waiting for the dashboard to open")
        self.assertEqual(len(self.said), 1)
        self.assertIn("until the dashboard first opens", self.said[0][0])
        self.assertFalse(self.said[0][1])

    def test_all_on_the_dashboard_said(self):
        self.b._pinned("all", "dashboard", "ok")
        self.assertIn("on the dashboard", self.said[0][0])

    def test_failure_said(self):
        self.b._pinned("1", "dashboard", "error no such screen")
        self.assertTrue(self.said[0][1])


if __name__ == "__main__":
    unittest.main()
