"""Frametop Settings' Pointer page with the travel mode switch: the page loads
without QML warnings, reads SteamVR's setting when it opens, and the switch stays off and
greyed with a note when SteamVR isn't running. The host runner is a fake (SteamVR down), so
vrcmd never runs. Needs PySide6 and Kirigami (the image's python3).
Run: python3 input-settings/test/test_travel_page.py"""
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp()
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "input-settings"))
sys.path.insert(0, str(ROOT / "settings"))
import ft_input_settings as fis  # noqa: E402
import ft_settings  # noqa: E402  (Frametop Settings, the window these pages are in)
import travel_mode  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

RAN = []


class Down:
    returncode, stdout, stderr = travel_mode.NOT_RUNNING, "", ""


def fake_host(*cmd, timeout=20):
    RAN.append(cmd)
    return Down()


fis.host = fake_host


class Page(unittest.TestCase):
    def test_pointer_page_reads_travel_mode(self):
        app = QGuiApplication.instance() or QGuiApplication([])
        b = fis.Backend()
        b._send = lambda text, to=fis.RELAY: True
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        settings = ft_settings.Settings(start="pointer", backends={"input": b})
        ft_settings.load(engine, settings)
        if not engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        deadline = time.monotonic() + 10
        while (b.travelBusy or not b.travelNote) and time.monotonic() < deadline:
            app.processEvents()
            time.sleep(0.01)
        lines = (ROOT / "input-settings" / "InputPages.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: pointerPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        mine = [e for e in errors if any(f"InputPages.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])
        self.assertTrue(any("--settings-bool" in c and travel_mode.KEY in c for c in RAN), RAN)
        self.assertFalse(any("--set-settings-bool" in c for c in RAN))  # opening it changes nothing
        self.assertFalse(b.travelKnown)
        self.assertFalse(b.travelMode)
        self.assertIn("Start SteamVR", b.travelNote)

    def test_the_switch_keeps_the_new_state_while_setting(self):
        app = QGuiApplication.instance() or QGuiApplication([])
        b = fis.Backend()
        gate = threading.Event()
        old = fis.host

        def slow_host(*cmd, timeout=20):
            gate.wait(10)
            return Down()
        fis.host = slow_host
        self.addCleanup(setattr, fis, "host", old)
        b.setTravelMode(True)
        self.assertTrue(b.travelBusy)
        self.assertTrue(b.travelMode)  # not snapped back to off while vrcmd runs
        gate.set()
        deadline = time.monotonic() + 10
        while b.travelBusy and time.monotonic() < deadline:
            app.processEvents()
            time.sleep(0.01)
        self.assertFalse(b.travelBusy)
        self.assertFalse(b.travelMode)  # SteamVR down: it didn't take
        self.assertIn("Start SteamVR", b.travelNote)


if __name__ == "__main__":
    unittest.main()
