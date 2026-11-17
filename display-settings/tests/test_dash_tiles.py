"""Display Settings' dashboard tiles (dash_tiles.py, backend.dashTiles): the stay default and the
band's numbers, read from frametop.conf, written there, clamped to ft-screens' ranges and sent
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
        self.assertFalse(self.t.stay)
        self.assertEqual((self.t.raiseDeg, self.t.radius, self.t.heightDeg, self.t.maxTopDeg), (0, 1.6, 40, 55))

    def test_stay_written_and_sent(self):
        self.t.setStay(True)
        self.assertIn("DASH_TILE_STAY=1", self.conf())
        self.assertTrue(self.t.stay)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_STAY 1")
        self.t.setStay(False)
        self.assertEqual(self.asked[-1], "handsetting DASH_TILE_STAY 0")
        self.assertIn("# mine", self.conf())

    def test_gamescope_apps_on_unless_off(self):
        self.assertTrue(self.t.gsApps)
        self.t.setGsApps(False)
        self.assertIn("GSAPP_PANELS=0", self.conf())
        self.assertFalse(self.t.gsApps)
        self.assertEqual(self.asked[-1], "handsetting GSAPP_PANELS 0")
        self.t.setGsApps(True)
        self.assertTrue(self.t.gsApps)
        self.assertEqual(self.asked[-1], "handsetting GSAPP_PANELS 1")

    def test_numbers_written_and_sent(self):
        self.t.setRaiseDeg(12)
        self.t.setRadius(2.25)
        self.t.setHeightDeg(45)
        self.t.setMaxTopDeg(50)
        self.assertEqual(self.asked, ["handsetting DASH_TILE_RAISE_DEG 12", "handsetting DASH_TILE_RADIUS 2.25",
                                      "handsetting DASH_TILE_HEIGHT_DEG 45", "handsetting DASH_TILE_MAX_TOP_DEG 50"])
        self.assertEqual((self.t.raiseDeg, self.t.radius, self.t.heightDeg, self.t.maxTopDeg), (12, 2.25, 45, 50))

    def test_clamped_to_the_range(self):
        self.t.setRadius(9)
        self.t.setHeightDeg(1)
        self.t.setRaiseDeg(float("nan"))
        self.assertEqual(self.asked, ["handsetting DASH_TILE_RADIUS 4", "handsetting DASH_TILE_HEIGHT_DEG 12",
                                      "handsetting DASH_TILE_RAISE_DEG 0"])

    def test_bad_conf_value_is_the_default(self):
        with open(fds.CONF_PATH, "a") as f:
            f.write("DASH_TILE_MAX_TOP_DEG=95\nDASH_TILE_RADIUS=junk\n")
        self.assertEqual(self.t.maxTopDeg, 55)
        self.assertEqual(self.t.radius, 1.6)

    def test_not_sent_without_ft_screens(self):
        self.b._running = False
        self.t.setHeightDeg(30)
        self.assertEqual(self.asked, [])
        self.assertEqual(self.t.heightDeg, 30)

    def test_changed_reaches_the_qml(self):
        seen = []
        self.t.changed.connect(lambda: seen.append(1))
        self.t.setStay(True)
        self.assertTrue(seen)

    def test_ranges_match_ft_screens(self):
        """The ranges here are TileSettings()'s in screens/vrtiles.cpp (the height's lower end is
        tiling::Params::minHeightDeg, 12)."""
        with open(VR) as f:
            src = f.read()
        for key, (_default, lo, hi) in dash_tiles.NUMBERS.items():
            with self.subTest(key=key):
                m = re.search(r'\{"%s", &[\w.]+, ([\w.-]+), ([\d.]+)\}' % key, src)
                self.assertIsNotNone(m, key)
                want_lo = 12.0 if m.group(1) == "g_tiles.params.minHeightDeg" else float(m.group(1))
                self.assertEqual((want_lo, float(m.group(2))), (lo, hi))

    def test_page_loads(self):
        from test_grab_bar import page_errors
        errors = page_errors(self.b, 'label: "Dashboard tiles"', 'Each app, screen and remote display remembers')
        if errors is None:
            self.skipTest("Frametop Settings didn't load here (no Kirigami?)")
        self.assertEqual(errors, [], "Display Settings QML failed to load")


if __name__ == "__main__":
    unittest.main()
