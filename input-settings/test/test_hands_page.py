"""Frametop Settings' Hands page: the lazy susan's distance (SCREEN_SPIN_DISTANCE) read, kept in
range, written to frametop.conf and sent to ft-screens, the 60 fps switch (hands/handsrate.py: its
state and warning, and the command it runs on the host), and the page loading with its slider. HOME is a
temporary directory; nothing goes to the host. Needs PySide6 and Kirigami (the image's python3).
Run: python3 input-settings/test/test_hands_page.py"""
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HOME = tempfile.mkdtemp()
os.environ["HOME"] = HOME
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "input-settings"))
sys.path.insert(0, str(ROOT / "settings"))
import ft_input_settings as fis  # noqa: E402
import ft_settings  # noqa: E402  (Frametop Settings, the window these pages are in)
from PySide6.QtCore import QObject  # noqa: E402
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

    def test_rate_switch_shows_the_camera_mode_and_runs_handsrate(self):
        # Off until the cameras are in the no-dark mode; the warning about the controllers always
        # there. The switch runs handsrate.py on the host with --now, shows what was asked while it
        # runs, says what handsrate.py said, and stays busy for RATE_COOLDOWN after (each switch
        # stops XRService, and SteamVR gives up on one that stops too often).
        self.assertTrue(fis.handsrate.XR_PATH.startswith(HOME + os.sep))   # the files below are the test's
        self.addCleanup(setattr, fis, "RATE_COOLDOWN", fis.RATE_COOLDOWN)
        fis.RATE_COOLDOWN = 0.3
        st = self.b.handsRate
        self.assertEqual((st["on"], st["busy"], st["note"]), (False, False, ""))
        self.assertIn("controllers don't track", st["warning"])
        fis.handsrate.write_cameras(fis.handsrate.HIGH)
        self.addCleanup(fis.handsrate.write_cameras, {})
        self.assertTrue(self.b.handsRate["on"])
        self.assertIn("HANDS_FULL_RATE", self.b.handsRate["note"])   # the cameras alone: it says so

        class Ran:
            returncode, stdout, stderr = 0, "60 fps hand tracking off: the controllers track again.\n", ""
        ran, said, seen = [], [], []

        def host(*cmd, timeout=20):
            ran.append(cmd)
            seen.append(dict(self.b.handsRate, timeout=timeout))   # while it runs
            return Ran()
        old, fis.host = fis.host, host
        self.addCleanup(setattr, fis, "host", old)
        self.b.message.connect(lambda text, bad: said.append((text, bad)))
        self.b.setHandsRate(False)
        for _ in range(200):
            self.app.processEvents()
            if said:
                break
            time.sleep(0.01)
        self.assertEqual(ran, [("python3", fis.HANDSRATE, "off", "--now")])
        self.assertEqual((seen[0]["on"], seen[0]["busy"], seen[0]["switching"]), (False, True, True))
        self.assertEqual(seen[0]["timeout"], 2 * fis.handsrate.RESTART_WAIT)   # room for its wait for SteamVR
        self.assertEqual(said, [("60 fps hand tracking off: the controllers track again.", False)])
        st = self.b.handsRate
        self.assertEqual((st["on"], st["busy"], st["switching"]), (True, True, False))   # what the files say again; still held
        self.b.setHandsRate(False)                               # held: this one doesn't run
        for _ in range(200):
            self.app.processEvents()
            if not self.b.handsRate["busy"]:
                break
            time.sleep(0.01)
        self.assertFalse(self.b.handsRate["busy"])
        self.assertEqual(len(ran), 1)

    def test_rate_switch_says_what_went_wrong(self):
        class Failed:
            returncode, stdout = 1, ""
            stderr = "handsrate: The settings are written, but ft-hands couldn't be restarted (no bus)\n"
        said = []
        old, fis.host, cool, fis.RATE_COOLDOWN = fis.host, lambda *cmd, timeout=20: Failed(), fis.RATE_COOLDOWN, 0
        self.addCleanup(setattr, fis, "host", old)
        self.addCleanup(setattr, fis, "RATE_COOLDOWN", cool)
        self.b.message.connect(lambda text, bad: said.append((text, bad)))
        self.b.setHandsRate(True)
        for _ in range(200):
            self.app.processEvents()
            if said and not self.b.handsRate["busy"]:
                break
            time.sleep(0.01)
        self.assertEqual(said[-1], ("60 fps hand tracking: handsrate: The settings are written, but ft-hands "
                                    "couldn't be restarted (no bus)", True))
        self.assertFalse(self.b.handsRate["on"])   # the fake host wrote nothing: the switch shows the files

    def test_rate_switch_frees_itself_when_the_host_call_breaks(self):
        def host(*cmd, timeout=20):
            raise RuntimeError("no host")
        broke = []
        old, fis.host, hook, threading.excepthook = fis.host, host, threading.excepthook, broke.append
        self.addCleanup(setattr, fis, "host", old)
        self.addCleanup(setattr, threading, "excepthook", hook)
        self.b.setHandsRate(True)
        for _ in range(200):
            self.app.processEvents()
            if broke and not self.b.handsRate["busy"]:
                break
            time.sleep(0.01)
        self.assertEqual(str(broke[0].exc_value), "no host")
        self.assertFalse(self.b.handsRate["busy"])   # not held for good: the switch can be used again

    def test_page_loads(self):
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        settings = ft_settings.Settings(start="hands", backends={"input": self.b})
        ft_settings.load(engine, settings)
        if not engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        for _ in range(20):
            self.app.processEvents()
        lines = (ROOT / "input-settings" / "InputPages.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: handsPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        mine = [e for e in errors if any(f"InputPages.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])
        # The 60 fps switch turns on only where hand tracking is installed and on (the host said
        # nothing here, so it isn't), and turns off wherever the cameras' mode is on.

        window = engine.rootObjects()[0]   # held: PySide drops a found child's wrapper with its parent's

        def enabled(installed, mode, cameras):
            self.b._hands_refreshing = True   # the page's timer doesn't ask the host again meanwhile
            self.b._hands = {"installed": installed}
            fis.write_conf_value("HANDS_MODE", mode)
            fis.handsrate.write_cameras(cameras)
            self.b.handsChanged.emit()
            for _ in range(5):
                self.app.processEvents()
            switch = window.findChild(QObject, "handsRateSwitch")
            self.assertIsNotNone(switch)
            return switch.property("enabled"), switch.property("checked")

        def note():
            return window.findChild(QObject, "handsRateNote").property("text")
        self.addCleanup(fis.handsrate.write_cameras, {})
        self.addCleanup(setattr, self.b, "_hands_refreshing", False)
        self.assertTrue(fis.handsrate.XR_PATH.startswith(HOME + os.sep))
        self.assertEqual(enabled(False, "frametop", {}), (False, False))
        self.assertEqual(note(), "It needs hand tracking, which isn't installed.")
        self.assertEqual(enabled(True, "off", {}), (False, False))
        self.assertEqual(note(), "It needs hand tracking: turn that on above first.")
        self.assertEqual(enabled(True, "frametop", {}), (True, False))
        self.assertEqual(note(), "")
        self.assertEqual(enabled(False, "off", fis.handsrate.HIGH), (True, True))
        # the slider sits under "Lazy susan brings screens to you", the switches all there round it
        keys = [e["key"] for e in self.b.handInteractions]
        self.assertEqual(len(keys), len(fis.HAND_INTERACTIONS))
        self.assertIn("SCREEN_SPIN_BRING", keys)


if __name__ == "__main__":
    unittest.main()
