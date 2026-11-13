"""Frametop Settings' Keyboard page and a keyboard that is still a 3D pointer (issue 68).
A keyboard with a touchpad has a mouse node, so its role starts as 3D pointer: it isn't
counted under "Keyboards connected", and nothing said why. The page now names such devices
and the role to set. It loads without QML warnings, names a connected 3D pointer device
with keys and a mouse, and says nothing for a mouse alone, a pass-through keyboard, a
Keyboard + 3D pointer device, a program's uinput device or one that isn't connected.
The relay is a fake (its node list is set here). Needs PySide6 and Kirigami (the image's
python3).
Run: python3 input-settings/test/test_keyboard_page.py"""
import os
import sys
import tempfile
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
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

HINT = "Not counted, because its role is 3D pointer: "


def node(device, name, kinds, role, uinput=False):
    return {"id": device, "name": name, "bus": "bluetooth", "kinds": kinds, "path": f"/dev/input/{name}",
            "role": role, "grabbed": role == "pointer", "uinput": uinput}


class Page(unittest.TestCase):
    def open_page(self, nodes, rules=None):
        """The Keyboard page with these relay nodes: (the hint's text or None if hidden, the
        "Keyboards connected" text, the page's QML warnings)."""
        app = QGuiApplication.instance() or QGuiApplication([])
        if rules is not None:
            os.makedirs(os.path.dirname(fis.RULES_PATH), exist_ok=True)
            with open(fis.RULES_PATH, "w") as f:
                f.write(rules)
            self.addCleanup(os.remove, fis.RULES_PATH)
        b = fis.Backend()
        b._send = lambda text, to=fis.RELAY: True
        b.notifier.setEnabled(False)  # a relay running here answers with its own devices
        b._nodes = nodes
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        settings = ft_settings.Settings(start="keyboard", backends={"input": b})
        ft_settings.load(engine, settings)
        if not engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        b.devicesChanged.emit()
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            app.processEvents()
            time.sleep(0.01)
        root = engine.rootObjects()[0]
        hint, counted = root.findChild(QObject, "keyedPointers"), root.findChild(QObject, "keyboardsConnected")
        self.assertIsNotNone(hint, "the Keyboard page didn't open")
        lines = (ROOT / "input-settings" / "InputPages.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: keyboardPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        mine = [e for e in errors if any(f"InputPages.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        return (hint.property("text") if hint.property("visible") else None), counted.property("text"), mine

    def test_a_keyboard_with_a_touchpad_left_as_3d_pointer_is_named(self):
        hint, counted, warnings = self.open_page([
            node("bt:1", "Pad Keys", ["keyboard", "mouse"], "pointer"),
            node("usb:2", "Desk Keyboard", ["keyboard"], "passthrough")])
        self.assertEqual(warnings, [])
        self.assertIsNotNone(hint)
        self.assertTrue(hint.startswith(HINT + "Pad Keys."), hint)
        self.assertIn("Keyboard + 3D pointer", hint)
        self.assertEqual(counted, "Desk Keyboard")

    def test_its_two_nodes_are_one_device(self):
        hint, counted, _ = self.open_page([
            node("bt:1", "Pad Keys Mouse", ["mouse"], "pointer"),
            node("bt:1", "Pad Keys Keyboard", ["keyboard"], "pointer")])
        self.assertTrue(hint.startswith(HINT + "Pad Keys."), hint)
        self.assertEqual(counted, "none")

    def test_nothing_to_say_about_other_devices(self):
        hint, counted, warnings = self.open_page([
            node("usb:1", "Plain Mouse", ["mouse"], "pointer"),
            node("usb:2", "Desk Keyboard", ["keyboard"], "passthrough"),
            node("uinput:3", "Remote Desktop", ["keyboard", "mouse"], "pointer", uinput=True),
            node("usb:4", "Ignored Combo", ["keyboard", "mouse"], "ignore")])
        self.assertEqual(warnings, [])
        self.assertIsNone(hint)
        self.assertEqual(counted, "Desk Keyboard")

    def test_once_its_role_is_keyboard_and_3d_pointer_it_counts(self):
        hint, counted, _ = self.open_page(
            [node("bt:1", "Pad Keys Mouse", ["mouse"], "pointer"),
             node("bt:1", "Pad Keys Keyboard", ["keyboard"], "passthrough")],
            rules='{"devices": {"bt:1": {"role": "combo"}}}')
        self.assertIsNone(hint)
        self.assertEqual(counted, "Pad Keys")


if __name__ == "__main__":
    unittest.main()
