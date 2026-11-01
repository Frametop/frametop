"""Frametop Input Settings' rules file (~/.config/frametop-input.json): one that doesn't
read is kept, with a .bad- copy, and never saved over; read_json gives {} for it, and before,
the next change saved that {} over everything in it. HOME is a temporary directory; nothing
goes to the host. Needs PySide6 (the image's python3).
Run: python3 input-settings/test/test_rules_file.py"""
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
from PySide6.QtGui import QGuiApplication  # noqa: E402


class Done:
    returncode, stdout, stderr = 1, "", ""


fis.host = lambda *cmd, timeout=20: Done()


class RulesFile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])
        cls.b = fis.Backend()
        cls.sent = []
        cls.b._send = lambda text, to=fis.RELAY: cls.sent.append(text) or True
        cls.said = []
        cls.b.message.connect(lambda text, error: cls.said.append((text, error)))

    def setUp(self):
        self.tmp = tempfile.mkdtemp(dir=HOME)
        fis.RULES_PATH = os.path.join(self.tmp, "frametop-input.json")
        self.path = Path(fis.RULES_PATH)
        self.said.clear()
        self.sent.clear()

    def backups(self):
        return [n for n in os.listdir(self.tmp) if n.startswith("frametop-input.json.bad-")]

    def check_kept(self, text):
        self.path.write_text(text)
        self.b.setRole("dev1", "pointer", "Mouse")
        self.b._set_rule("pause_auto", False, "Pause: off")
        self.assertEqual(self.path.read_text(), text)  # not saved over
        self.assertEqual(len(self.backups()), 1)       # one copy, however often it's tried
        self.assertEqual((Path(self.tmp) / self.backups()[0]).read_text(), text)
        self.assertTrue(self.said and all(error for _, error in self.said), self.said)  # no "saved" after it
        self.assertIn("won't save over it", self.said[0][0])
        self.assertEqual(self.sent, [])  # the relay isn't told to reload

    def test_bad_json_is_kept(self):
        self.check_kept('{"devices": {"dev0": {"role": "keyboard"}},}')

    def test_top_level_list_is_kept(self):
        self.check_kept('[{"devices": {}}]')

    def test_a_good_file_saves_and_keeps_the_rest(self):
        self.path.write_text(json.dumps({"key_bindings": {"125": "screens_toggle"}, "pause_auto": True}))
        self.b.setRole("dev1", "pointer", "Mouse")
        saved = json.loads(self.path.read_text())
        self.assertEqual(saved["devices"], {"dev1": {"role": "pointer", "name": "Mouse"}})
        self.assertEqual(saved["key_bindings"], {"125": "screens_toggle"})
        self.assertEqual(self.said, [("Mouse: " + fis.ROLE_LABELS["pointer"], False)])
        self.assertEqual(self.sent, ["reload"])
        self.assertEqual(sorted(os.listdir(self.tmp)), [".frametop-input.json.lock", "frametop-input.json"])

    def test_no_file_yet(self):
        self.b._set_rule("pause_auto", False, "Pause: off")
        self.assertEqual(json.loads(self.path.read_text()), {"pause_auto": False})
        self.assertEqual(self.said, [("Pause: off", False)])

    def test_fixed_file_saves_again(self):
        self.path.write_text("not json")
        self.b.setRole("dev1", "pointer", "Mouse")
        self.path.write_text("{}")
        self.b.setRole("dev1", "pointer", "Mouse")
        self.assertEqual(json.loads(self.path.read_text())["devices"]["dev1"]["role"], "pointer")


    def test_keys_action_on_a_controller_button(self):
        # "keys:CODES" (the relay taps that combination): saved and named, a bad one isn't saved.
        self.b.setControllerMapping("right/b", "keys:125+32")
        self.b.setControllerMapping("right/a", "keys:125+d")
        self.assertEqual(json.loads(self.path.read_text())["controller_buttons"], {"right/b": "keys:125+32"})
        self.assertEqual([m["actionLabel"] for m in self.b.controllerMappings], ["Press Meta+D"])
        self.assertEqual(fis.action_label("keys:29+42+1"), "Press Ctrl+Shift+Esc")
        self.assertTrue(fis.shortcut_mappable("keys:125+32"))
        self.assertFalse(fis.mappable("keys:") or fis.mappable("keys:+32"))


if __name__ == "__main__":
    unittest.main()
