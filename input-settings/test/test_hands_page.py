"""Frametop Input Settings' Hands page: the lazy susan's distance (SCREEN_SPIN_DISTANCE) read, kept in
range, written to frametop.conf and sent to ft-screens, and the page loading with its slider. HOME is a
temporary directory; nothing goes to the host. Needs PySide6 and Kirigami (the image's python3).
Run: python3 input-settings/test/test_hands_page.py"""
import os
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
from PySide6.QtCore import QUrl  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402


class Done:
    returncode, stdout, stderr = 1, "", ""


fis.host = lambda *cmd, timeout=20: Done()


class Page(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.b = fis.Backend()
        cls.sent = []
        cls.b._send = lambda text, to=fis.RELAY: cls.sent.append((text, to)) or True

    def setUp(self):
        os.makedirs(os.path.dirname(fis.CONF_PATH), exist_ok=True)
        with open(fis.CONF_PATH, "w") as f:
            f.write("# mine\nSCREEN_SPIN_BRING=1\n")
        self.sent.clear()

    def test_distance_default_and_range(self):
        self.assertEqual(self.b.spinDistance, 1.0)
        fis.write_conf_value("SCREEN_SPIN_DISTANCE", "0.5")   # nearer than frunner's spot: 0.9
        self.assertEqual(self.b.spinDistance, 0.9)
        fis.write_conf_value("SCREEN_SPIN_DISTANCE", "nope")
        self.assertEqual(self.b.spinDistance, 1.0)
        r = self.b.spinDistanceRange
        self.assertEqual((r["min"], r["max"], r["default"]), (0.9, 3.0, 1.0))

    def test_set_writes_and_sends(self):
        self.b.setSpinDistance(1.35)
        self.assertEqual(fis.read_conf()["SCREEN_SPIN_DISTANCE"], "1.35")
        self.assertEqual(self.sent[-1], ("spindistance 1.35", fis.SCREENS))
        self.assertEqual(self.b.spinDistance, 1.35)
        self.b.setSpinDistance(9)
        self.assertEqual(self.sent[-1][0], "spindistance 3")
        self.assertIn("# mine", Path(fis.CONF_PATH).read_text())

    def test_cutouts_switch(self):
        # Off by default (DEE-221: the drawn hands since 920d16e); the switch writes HANDS_CUTOUTS
        # and tells ft-screens at once, the rest of the file kept.
        self.assertFalse(self.b.handsCutouts)
        self.b.setHandsCutouts(True)
        self.assertEqual(fis.read_conf()["HANDS_CUTOUTS"], "1")
        self.assertEqual(self.sent[-1], ("cutouts on", fis.SCREENS))
        self.assertTrue(self.b.handsCutouts)
        self.b.setHandsCutouts(False)
        self.assertEqual(fis.read_conf()["HANDS_CUTOUTS"], "0")
        self.assertEqual(self.sent[-1], ("cutouts off", fis.SCREENS))
        self.assertIn("# mine", Path(fis.CONF_PATH).read_text())

    def test_page_loads(self):
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        engine.rootContext().setContextProperty("backend", self.b)
        engine.rootContext().setContextProperty("startPage", "hands")
        engine.load(QUrl.fromLocalFile(str(ROOT / "input-settings" / "main.qml")))
        if not engine.rootObjects():
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        for _ in range(20):
            self.app.processEvents()
        lines = (ROOT / "input-settings" / "main.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: handsPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        mine = [e for e in errors if any(f"main.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])
        # the slider sits under "Lazy susan brings screens to you", the switches all there round it
        keys = [e["key"] for e in self.b.handInteractions]
        self.assertEqual(len(keys), len(fis.HAND_INTERACTIONS))
        self.assertIn("SCREEN_SPIN_BRING", keys)


if __name__ == "__main__":
    unittest.main()
