"""Frametop Settings' list of a profile's windows (Layout & profiles, Apps): the settings apps
Frametop Settings replaced aren't listed, however they were saved, and Leave out removes the
window it's next to, not the one at that place in the saved list. The layout is a dict here,
never the real ~/.config/frametop-layout.json. Needs PySide6 (the image's python3).

  python3 display-settings/tests/test_profile_windows.py
"""
import copy
import os
import sys
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-settings-profile-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ft_display_settings as fds  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402

WINDOWS = [
    {"app": "ft-input-settings", "screen": 1},
    {"app": "org.kde.dolphin", "screen": 1},
    # Remote Access (GTK) matched no desktop file, so it was saved by command line and class.
    {"cmd": ["/usr/bin/python3", "/home/me/frametop/remote/ft-remote-settings"], "class": "org.frametop.RemoteAccess",
     "screen": 2},
    {"app": "firefox", "screen": 2, "maximized": True},
]


class ProfileWindows(unittest.TestCase):
    def setUp(self):
        QGuiApplication.instance() or QGuiApplication([])
        self.layout = {"profiles": {"Work": {"windows": copy.deepcopy(WINDOWS)}}}
        old = fds.ft_layout.load_layout
        fds.ft_layout.load_layout = lambda: self.layout
        self.addCleanup(setattr, fds.ft_layout, "load_layout", old)
        self.b = fds.Backend()
        self.b._save = lambda layout: True

    def test_retired_apps_are_not_listed(self):
        listed = self.b.profileWindows("Work")
        self.assertEqual([w["app"] for w in listed], ["dolphin", "firefox"])
        self.assertEqual([w["where"] for w in listed], ["screen 1", "screen 2, maximized"])

    def test_leave_out_removes_that_window(self):
        firefox = self.b.profileWindows("Work")[1]
        self.b.removeProfileWindow("Work", firefox["index"])
        left = self.layout["profiles"]["Work"]["windows"]
        self.assertNotIn("firefox", [w.get("app") for w in left])
        self.assertIn("org.kde.dolphin", [w.get("app") for w in left])
        self.assertEqual([w["app"] for w in self.b.profileWindows("Work")], ["dolphin"])

    def test_retired_entry(self):
        retired = fds.ft_layout.retired_entry
        self.assertTrue(retired({"app": "ft-remote-displays"}))
        self.assertTrue(retired({"cmd": ["python3", "x/remote/ft-remote-settings"], "class": ""}))
        self.assertTrue(retired({"cmd": ["python3", "x"], "class": "org.frametop.RemoteAccess"}))
        self.assertFalse(retired({"app": "org.kde.dolphin"}))
        self.assertFalse(retired({"cmd": ["python3", "x/myapp.py"], "class": "myapp"}))


if __name__ == "__main__":
    unittest.main()
