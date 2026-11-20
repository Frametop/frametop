"""Display Settings' dashboard tiles (dash_tiles.py, backend.dashTiles): the stay default and the
wall's numbers, read from frametop.conf, written there, clamped to ft-screens' ranges and sent
to it live. HOME is a temp folder and the ask to ft-screens is recorded, not sent. Needs PySide6
(the image's python3).

  python3 display-settings/tests/test_dash_tiles.py
"""
import os
import re
import sys
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-settings-tiles-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import dash_tiles  # noqa: E402
import ft_display_settings as fds  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402

VR = os.path.join(HERE, "..", "..", "screens", "vrtiles.cpp")


class DashTiles(unittest.TestCase):
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
        self.t = self.b.dashTiles

    def conf(self):
        with open(fds.CONF_PATH) as f:
            return f.read()

    def test_defaults(self):
        self.assertTrue(self.t.stay)
        self.assertFalse(self.t.stayTheater)
        self.assertEqual((self.t.deskDeg, self.t.radius, self.t.pxPerDeg, self.t.wallDeg), (-12, 2.7, 48, 80))

    def test_stay_written_and_sent(self):
        self.t.setStay(True)
        self.assertIn("DASH_TILE_STAY=1", self.conf())
        self.assertTrue(self.t.stay)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_STAY 1")
        self.t.setStay(False)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_STAY 0")
        self.assertFalse(self.t.stay)
        self.t.setStayTheater(True)
        self.assertIn("DASH_TILE_STAY_THEATER=1", self.conf())
        self.assertTrue(self.t.stayTheater)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_STAY_THEATER 1")
        self.assertIn("# mine", self.conf())

    def test_look_focus_on_unless_off(self):
        self.assertTrue(self.t.lookFocus)
        self.t.setLookFocus(False)
        self.assertIn("DASH_TILE_LOOK=0", self.conf())
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_LOOK 0")
        self.assertFalse(self.t.lookFocus)

    def test_gamescope_apps_on_unless_off(self):
        self.assertTrue(self.t.gsApps)
        self.t.setGsApps(False)
        self.assertIn("GSAPP_PANELS=0", self.conf())
        self.assertFalse(self.t.gsApps)
        self.assertEqual(self.asked[-1], "handsetting GSAPP_PANELS 0")
        self.t.setGsApps(True)
        self.assertTrue(self.t.gsApps)
        self.assertEqual(self.asked[-1], "handsetting GSAPP_PANELS 1")

    def test_steams_window_off_unless_on(self):
        self.assertFalse(self.t.steamWindows)
        self.t.setSteamWindows(True)
        self.assertIn("STEAM_DASH_PANEL=1", self.conf())
        self.assertTrue(self.t.steamWindows)
        self.assertEqual(self.asked[-1], "handsetting STEAM_DASH_PANEL 1")
        self.t.setSteamWindows(False)
        self.assertFalse(self.t.steamWindows)
        self.assertEqual(self.asked[-1], "handsetting STEAM_DASH_PANEL 0")

    def test_at_steams_bar_on_unless_off(self):
        self.assertTrue(self.t.atBar)
        self.t.setAtBar(False)
        self.assertIn("DASH_TILE_AT_BAR=0", self.conf())
        self.assertFalse(self.t.atBar)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_AT_BAR 0")
        self.t.setAtBar(True)
        self.assertTrue(self.t.atBar)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_AT_BAR 1")

    def test_numbers_written_and_sent(self):
        self.t.setDeskDeg(-20)
        self.t.setRadius(2.25)
        self.t.setPxPerDeg(60)
        self.t.setWallDeg(150)
        self.assertEqual(self.asked, ["handsetting DASH_TILE_DESK_DEG -20", "handsetting DASH_TILE_RADIUS 2.25",
                                      "handsetting DASH_TILE_PX_PER_DEG 60", "handsetting DASH_TILE_WALL_DEG 150"])
        self.assertEqual((self.t.deskDeg, self.t.radius, self.t.pxPerDeg, self.t.wallDeg), (-20, 2.25, 60, 150))

    def test_clamped_to_the_range(self):
        self.t.setRadius(9)
        self.t.setPxPerDeg(1)
        self.t.setDeskDeg(float("nan"))
        self.assertEqual(self.asked, ["handsetting DASH_TILE_RADIUS 6", "handsetting DASH_TILE_PX_PER_DEG 15",
                                      "handsetting DASH_TILE_DESK_DEG -12"])

    def test_bad_conf_value_is_the_default(self):
        with open(fds.CONF_PATH, "a") as f:
            f.write("DASH_TILE_WALL_DEG=400\nDASH_TILE_RADIUS=junk\n")
        self.assertEqual(self.t.wallDeg, 80)
        self.assertEqual(self.t.radius, 2.7)

    def test_not_sent_without_ft_screens(self):
        self.b._running = False
        self.t.setWallDeg(90)
        self.assertEqual(self.asked, [])
        self.assertEqual(self.t.wallDeg, 90)

    def test_changed_reaches_the_qml(self):
        seen = []
        self.t.changed.connect(lambda: seen.append(1))
        self.t.setStay(True)
        self.assertTrue(seen)

    def test_ranges_match_ft_screens(self):
        """The ranges here are TileSettings()'s in screens/vrtiles.cpp."""
        with open(VR) as f:
            src = f.read()
        for key, (_default, lo, hi) in dash_tiles.NUMBERS.items():
            with self.subTest(key=key):
                m = re.search(r'\{"%s", &[\w.]+, ([\w.-]+), ([\d.]+)\}' % key, src)
                self.assertIsNotNone(m, key)
                self.assertEqual((float(m.group(1)), float(m.group(2))), (lo, hi))

    def test_defaults_match_ft_screens(self):
        """The defaults here are tiling::Params' in screens/tiling.h."""
        with open(os.path.join(HERE, "..", "..", "screens", "tiling.h")) as f:
            src = f.read()
        fields = {"DASH_TILE_DESK_DEG": "deskDeg", "DASH_TILE_RADIUS": "radius", "DASH_TILE_PX_PER_DEG": "pxPerDeg",
                  "DASH_TILE_WALL_DEG": "wallDeg"}
        for key, (default, _lo, _hi) in dash_tiles.NUMBERS.items():
            with self.subTest(key=key):
                m = re.search(r"double %s = ([\d.-]+);" % fields[key], src)
                self.assertIsNotNone(m, key)
                self.assertEqual(float(m.group(1)), default)

    def test_page_loads(self):
        from test_grab_bar import page_errors
        errors = page_errors(self.b, 'label: "Dashboard tiles"', 'Each app, screen and remote display remembers')
        if errors is None:
            self.skipTest("Frametop Settings didn't load here (no Kirigami?)")
        self.assertEqual(errors, [], "Display Settings QML failed to load")


if __name__ == "__main__":
    unittest.main()
