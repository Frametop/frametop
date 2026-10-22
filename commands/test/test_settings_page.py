"""Frametop Input Settings' Commands page: adding, refusing, removing (with the bindings to it),
the run:NAME actions in every picker, and the page itself loading. HOME is a temporary
directory; host() is replaced, so nothing runs on the host. Needs PySide6 and Kirigami (the
image's python3). Run: python3 commands/test/test_settings_page.py"""
import json
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
import ft_commands  # noqa: E402  (ft_input_settings put commands/ on the path)
from PySide6.QtCore import QUrl  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

RULES = Path(fis.RULES_PATH)
ran = []


class Done:
    def __init__(self, code=0, out=""):
        self.returncode, self.stdout, self.stderr = code, out, ""


def fake_host(*cmd, timeout=20):
    ran.append(cmd)
    if cmd[:2] == ("sh", "-c"):
        return Done(0 if cmd[-1] == "taken-on-host" else 1)  # command -v NAME
    return Done(0, f"{cmd[-1]}: started (unit frametop-cmd-x-1)")


fis.host = fake_host


class Page(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.b = fis.Backend()
        cls.messages = []
        cls.b.message.connect(lambda text, err: cls.messages.append((text, err)))

    def setUp(self):
        for p in (ft_commands.COMMANDS_PATH, fis.RULES_PATH):
            if os.path.exists(p):
                os.remove(p)
        self.messages.clear()
        ran.clear()

    def test_add_lists_and_offers_everywhere(self):
        self.assertTrue(self.b.addCommand(" stop-fc ", " killall framecorder "))
        self.assertEqual([(c["name"], c["run"], c["said"]) for c in self.b.commands],
                         [("stop-fc", "killall framecorder", "stop fc")])
        for actions in (self.b.actions, self.b.controllerActions, self.b.shortcutActions):
            self.assertIn({"value": "run:stop-fc", "text": "Run command stop-fc"}, actions)
        self.assertTrue(fis.mappable("run:stop-fc") and fis.shortcut_mappable("run:stop-fc"))
        self.assertTrue(os.path.exists(ft_commands.ALIASES_PATH))
        self.assertIn(ft_commands.BASHRC_MARK, Path(HOME, ".bashrc").read_text())

    def test_refused_names(self):
        self.assertTrue(self.b.addCommand("a", "true"))
        self.assertIn("already", self.b.commandNameProblem("a"))
        self.assertIn("letters, digits", self.b.commandNameProblem("two words"))
        self.assertIn("on the headset", self.b.commandNameProblem("taken-on-host"))
        self.assertFalse(self.b.addCommand("b", "   "))
        self.assertEqual([c["name"] for c in self.b.commands], ["a"])

    def test_remove_drops_its_bindings(self):
        self.b.addCommand("x", "true")
        self.b.addCommand("y", "true")
        RULES.write_text(json.dumps({"controller_buttons": {"right/a": "run:x", "right/b": "run:y"},
                                     "key_bindings": {"125+45": "run:x", "125": "steam_menu"},
                                     "buttons": {"dev1": {"275": "run:x", "276": "back"}}}))
        self.b.removeCommand("x")
        rules = json.loads(RULES.read_text())
        self.assertEqual(rules["controller_buttons"], {"right/b": "run:y"})
        self.assertEqual(rules["key_bindings"], {"125": "steam_menu"})
        self.assertEqual(rules["buttons"]["dev1"], {"276": "back"})
        self.assertIn("and 3 bindings to it", self.messages[-1][0])
        self.assertEqual([c["name"] for c in self.b.commands], ["y"])

    def test_run_goes_through_ft_cmd_on_the_host(self):
        self.b.addCommand("z", "true")
        self.b.runCommand("z")
        self.assertEqual(ran[-1][1:], ("run", "z"))
        self.assertTrue(ran[-1][0].endswith("ft-cmd"))
        self.assertFalse(self.messages[-1][1])

    def test_page_loads(self):
        self.b.addCommand("shown", "echo hi")
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        engine.rootContext().setContextProperty("backend", self.b)
        engine.rootContext().setContextProperty("startPage", "commands")
        engine.load(QUrl.fromLocalFile(str(ROOT / "input-settings" / "main.qml")))
        if not engine.rootObjects():
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(errors[:3]))
        for _ in range(20):
            self.app.processEvents()
        # Warnings from the Commands page's lines (other pages may warn offscreen, e.g. no SteamVR).
        lines = (ROOT / "input-settings" / "main.qml").read_text().split("\n")
        first = next(i for i, t in enumerate(lines) if "id: commandsPage" in t) + 1
        last = next(i for i in range(first, len(lines)) if "// ------" in lines[i]) + 1
        # Every page opened first says "Created graphical object was not placed in the graphics scene".
        mine = [e for e in errors if any(f"main.qml:{n}:" in e for n in range(first - 1, last + 1))
                and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])


if __name__ == "__main__":
    unittest.main()
