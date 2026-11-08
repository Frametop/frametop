"""Frametop Settings' window drag (Keyboard page, DEE-345): WINDOW_DRAG_* read with their defaults,
each picker's word written to frametop.conf and sent to ft-screens ("windowdrag MOD LEFT RIGHT MIDDLE"),
KWin's bindings set through session/window-drag.sh --live, and the page loading with its pickers.
HOME is a temporary directory; nothing goes to the host (its runner is a fake that records). Needs
PySide6 and Kirigami (the image's python3).
Run: python3 input-settings/test/test_window_drag.py"""
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
sys.path.insert(0, str(ROOT / "settings"))
import ft_input_settings as fis  # noqa: E402
import ft_settings  # noqa: E402  (Frametop Settings, the window these pages are in)
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402


class Done:
    def __init__(self, code=0, err=""):
        self.returncode, self.stdout, self.stderr = code, "", err


ran = []
answer = {"done": Done()}


def fake_host(*cmd, timeout=20):
    ran.append(cmd)
    return answer["done"]


fis.host = fake_host


class WindowDrag(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.b = fis.Backend()
        cls.sent = []
        cls.b._send = lambda text, to=fis.RELAY: cls.sent.append((text, to)) or True
        cls.said = []
        cls.b.message.connect(lambda text, error: cls.said.append((text, error)))

    def setUp(self):
        os.makedirs(os.path.dirname(fis.CONF_PATH), exist_ok=True)
        with open(fis.CONF_PATH, "w") as f:
            f.write("# mine\nFLOAT_SLOTS=16\n")
        self.sent.clear(), ran.clear(), self.said.clear()
        answer["done"] = Done()

    def test_defaults(self):
        self.assertEqual(self.b.windowDrag, {"mod": "meta", "left": "move", "right": "resize", "middle": "none"})

    def test_a_bad_or_old_word_reads_as_its_default(self):
        fis.write_conf_value("WINDOW_DRAG_MOD", "super")   # (ft-screens and the script take it as meta)
        fis.write_conf_value("WINDOW_DRAG_LEFT", "throw")
        self.assertEqual(self.b.windowDrag["mod"], "meta")
        self.assertEqual(self.b.windowDrag["left"], "move")

    def test_set_writes_sends_and_sets_kwin(self):
        self.b.setWindowDrag("mod", "alt")
        self.assertEqual(fis.read_conf()["WINDOW_DRAG_MOD"], "alt")
        self.assertEqual(self.sent[-1], ("windowdrag alt move resize none", fis.SCREENS))
        self.assertEqual(ran[-1], (fis.WINDOW_DRAG_SCRIPT, "--live", "alt", "move", "resize", "none"))
        self.b.setWindowDrag("middle", "move")
        self.assertEqual(self.sent[-1][0], "windowdrag alt move resize move")
        self.assertEqual(self.said[-1], ("Alt + a button in a window: left moves, right resizes, middle moves", False))
        self.assertIn("# mine", Path(fis.CONF_PATH).read_text())

    def test_off(self):
        self.b.setWindowDrag("mod", "off")
        self.assertEqual(self.sent[-1][0], "windowdrag off move resize none")
        self.assertEqual(self.said[-1], ("Window drag off", False))

    def test_a_word_outside_the_list_changes_nothing(self):
        self.b.setWindowDrag("left", "throw")
        self.b.setWindowDrag("side", "move")
        self.assertNotIn("WINDOW_DRAG_LEFT", fis.read_conf())
        self.assertEqual((self.sent, ran), ([], []))

    def test_kwin_failing_says_so(self):
        answer["done"] = Done(2, "window drag: a button does move, resize or none")
        self.b.setWindowDrag("right", "none")
        self.assertEqual(fis.read_conf()["WINDOW_DRAG_RIGHT"], "none")   # saved all the same
        text, error = self.said[-1]
        self.assertTrue(error)
        self.assertIn("didn't take it", text)

    def test_the_script_is_there(self):
        self.assertTrue(os.access(fis.WINDOW_DRAG_SCRIPT, os.X_OK))

    def test_page_loads(self):
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        settings = ft_settings.Settings(start="keyboard", backends={"input": self.b})
        ft_settings.load(engine, settings)
        if not engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        for _ in range(20):
            self.app.processEvents()
        lines = (ROOT / "input-settings" / "InputPages.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: keyboardPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        mine = [e for e in errors if any(f"InputPages.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])


if __name__ == "__main__":
    unittest.main()
