"""Display Settings' "Hide Frametop while Steam is locked" (issue 66): it sets STEAM_PIN_LOCK in
frametop.conf (docs/steam-pin-lock.md), refuses the gamescope backend the session would refuse,
says a restart is due when it differs from what the desktop started with, and its page
(Steam & notifications, SteamPage.qml) loads without QML errors. No headset, no Steam, no desktop.

  python3 display-settings/tests/test_steam_pin_lock.py
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-pinlock-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_grab_bar import fds, page_errors  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402


class SteamPinLock(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        Path(fds.CONF_PATH).parent.mkdir(parents=True, exist_ok=True)
        Path(fds.CONF_PATH).write_text("# keep this\nSCREENS=2\n")
        self.kind = "screens"
        backend = patch.object(fds.ft_layout, "backend", side_effect=lambda: self.kind)
        backend.start()
        self.addCleanup(backend.stop)
        self.b = fds.Backend()
        self.messages = []
        self.b.message.connect(lambda text, error: self.messages.append((text, error)))

    def conf(self):
        return Path(fds.CONF_PATH).read_text()

    def test_off_by_default_and_on_writes_the_setting(self):
        self.assertFalse(self.b.steamPinLock)
        self.b.setSteamPinLock(True)
        self.assertTrue(self.b.steamPinLock)
        self.assertIn("STEAM_PIN_LOCK=1", self.conf())
        self.assertIn("# keep this", self.conf())
        self.b.setSteamPinLock(False)
        self.assertFalse(self.b.steamPinLock)
        self.assertIn("STEAM_PIN_LOCK=0", self.conf())

    def test_the_gamescope_backend_is_refused(self):
        # frametop-session.sh exits when protection is on with any other backend.
        self.kind = "gamescope"
        self.b.setSteamPinLock(True)
        self.assertFalse(self.b.steamPinLock)
        self.assertNotIn("STEAM_PIN_LOCK", self.conf())
        self.assertEqual(self.messages, [("Steam PIN protection needs the screens backend", True)])
        Path(fds.CONF_PATH).write_text("STEAM_PIN_LOCK=1\n")
        self.b.setSteamPinLock(False)  # turning it off is always allowed
        self.assertIn("STEAM_PIN_LOCK=0", self.conf())

    def test_a_change_waits_for_the_next_desktop_start(self):
        self.b._running, self.b._started = True, self.b._conf()
        self.assertFalse(self.b.steamPinLockRestartNeeded)
        self.b.setSteamPinLock(True)
        self.assertTrue(self.b.steamPinLockRestartNeeded)
        self.b._running = False
        self.assertFalse(self.b.steamPinLockRestartNeeded, "nothing to restart")

    def test_its_section_loads(self):
        errors = page_errors(self.b, "steam", "SteamPage.qml")
        if errors is None:
            self.skipTest("Frametop Settings doesn't load here (no Kirigami)")
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
