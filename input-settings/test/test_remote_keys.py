"""Frametop Settings' Remote display keys page (remote_keys.py, RemoteKeysPage.qml), offline: its
list, what an entry can't be, adding one with the keys you press or with other keys, removing
one, and going back to the defaults, each as it lands in the rules file. Then the page itself in
Frametop Settings: it opens without QML warnings and shows the list. The relay is a fake (key
events are handed to the backend as the relay would send them). Needs PySide6 (the page's test:
Kirigami too; the image's python3).
Run: python3 input-settings/test/test_remote_keys.py"""
import json
import os
import socket
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
import ft_remote_keys  # noqa: E402  (ft_input_settings put conf/ on the path)
import ft_settings  # noqa: E402  (Frametop Settings, the window the page is in)
import remote_keys  # noqa: E402
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

META, SHIFT, CTRL, ALT, SPACE, J, ESC, VOLUME_UP = 125, 42, 29, 56, 57, 36, 1, 115
NAMES = {"125": "Meta tap", "125+57": "Meta+Space", "42+125+57": "Meta+Shift+Space", "125+36": "Meta+J"}


def name(combo):
    return NAMES.get(combo, combo)


def sendable(combo):
    return str(VOLUME_UP) not in combo.split("+")


class Logic(unittest.TestCase):
    def test_rows_say_what_the_computer_gets_and_what_frametop_does_elsewhere(self):
        got = remote_keys.rows({"125+57": "125+57", "42+125+57": "125+57"}, {"125+57": "frunner", "125+36": "gaze_left"},
                               name, lambda action: action.upper())
        self.assertEqual([(r["pressedLabel"], r["same"], r["sentLabel"], r["instead"]) for r in got],
                         [("Meta+Shift+Space", False, "Meta+Space", ""), ("Meta+Space", True, "Meta+Space", "FRUNNER")])

    def test_a_combination_passed_as_a_key_has_nothing_of_frametops(self):
        got = remote_keys.rows({"125+57": "125+57"}, {"125+57": "key"}, name, str)
        self.assertEqual(got[0]["instead"], "")

    def test_problems(self):
        bound = {"29+42+56+1": "release_keys"}
        self.assertEqual(remote_keys.problem("125+57", "125+57", bound, sendable, name), "")
        self.assertEqual(remote_keys.problem("42+125+57", "125+57", bound, sendable, name), "")
        self.assertIn("always stays Frametop's", remote_keys.problem("29+42+56+1", "29+42+56+1", bound, sendable, name))
        self.assertIn("can't be sent", remote_keys.problem("125+57", "125+115", bound, sendable, name))
        self.assertNotEqual(remote_keys.problem("57+125", "125+57", bound, sendable, name), "")

    def test_the_first_change_keeps_the_defaults(self):
        rules = {}
        remote_keys.put(rules, "42+125+57", "125+57", sendable)
        self.assertEqual(rules["remote_key_bindings"], {**ft_remote_keys.DEFAULT, "42+125+57": "125+57"})
        rules = {}
        remote_keys.drop(rules, "125+57", sendable)
        want = dict(ft_remote_keys.DEFAULT)
        del want["125+57"]
        self.assertEqual(rules["remote_key_bindings"], want)

    def test_removing_the_last_one_leaves_an_empty_list_not_the_defaults(self):
        rules = {"remote_key_bindings": {"125+57": "125+57"}}
        remote_keys.drop(rules, "125+57", sendable)
        self.assertEqual(rules["remote_key_bindings"], {})
        self.assertEqual(ft_remote_keys.read(rules, sendable), {})


class Backend(unittest.TestCase):
    """RemoteKeys inside ft_input_settings' Backend, with the rules in a temp HOME."""
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        os.makedirs(os.path.dirname(fis.RULES_PATH), exist_ok=True)
        with open(fis.RULES_PATH, "w") as f:
            json.dump({"key_bindings": {"125+57": "frunner", "125+36": "gaze_left", "29+42+56+1": "release_keys"}}, f)
        self.addCleanup(lambda: os.path.exists(fis.RULES_PATH) and os.remove(fis.RULES_PATH))
        self.b = fis.Backend()
        self.sent = []
        self.b._send = lambda text, to=fis.RELAY: self.sent.append(text) or True
        self.b.notifier.setEnabled(False)   # a relay running here says nothing to this one
        self.said = []
        self.b.message.connect(lambda text, error: self.said.append((text, error)))
        self.keys = self.b.remoteKeys

    def rules(self):
        with open(fis.RULES_PATH) as f:
            return json.load(f)

    def press(self, *codes):
        """A combination pressed and let go, as the relay's watch events."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as relay:
            for value, order in ((1, codes), (0, codes[::-1])):
                for code in order:
                    event = {"t": "event", "type": "key", "id": "usb:test", "code": code, "value": value}
                    relay.sendto(json.dumps(event).encode(), self.b.sock.getsockname())
        self.b._read()

    def test_the_list_starts_as_the_defaults(self):
        self.assertTrue(self.keys.isDefault)
        self.assertEqual({r["pressed"]: r["sent"] for r in self.keys.rows}, dict(ft_remote_keys.DEFAULT))
        space = next(r for r in self.keys.rows if r["pressed"] == "125+57")
        self.assertEqual((space["pressedLabel"], space["same"]), ("Meta+Space", True))
        self.assertIn("Frunner", space["instead"])

    def test_add_a_combination_sent_as_other_keys(self):
        self.keys.capture("pressed")
        self.assertEqual((self.keys.capturing, self.sent[-1]), ("pressed", "watch 60"))
        self.press(META, SHIFT, SPACE)
        self.assertEqual((self.keys.capturing, self.keys.pressed, self.keys.pressedLabel, self.keys.sentLabel),
                         ("", "42+125+57", "Shift+Meta+Space", "The same keys"))
        self.keys.capture("sent")
        self.press(META, SPACE)
        self.assertEqual((self.keys.sentLabel, self.keys.sendsOther, self.keys.problem), ("Meta+Space", True, ""))
        self.keys.add()
        self.assertEqual(self.rules()["remote_key_bindings"]["42+125+57"], "125+57")
        self.assertEqual(self.rules()["remote_key_bindings"]["125+57"], "125+57", "the defaults are kept")
        self.assertEqual((self.keys.pressed, self.keys.isDefault), ("", False))
        self.assertIn("reload", self.sent)
        self.assertEqual(self.said[-1], ("Shift+Meta+Space goes to the computer as Meta+Space", False))

    def test_add_a_combination_as_it_is_and_a_tap(self):
        self.keys.capture("pressed")
        self.press(META, J)
        self.keys.add()
        self.assertEqual(self.rules()["remote_key_bindings"]["125+36"], "125+36")
        self.keys.remove("125")
        self.assertNotIn("125", self.rules()["remote_key_bindings"])
        self.keys.capture("pressed")
        self.press(META)
        self.assertEqual((self.keys.pressed, self.keys.pressedLabel), ("125", "Meta tap"))
        self.keys.add()
        self.assertEqual(self.rules()["remote_key_bindings"]["125"], "125")

    def test_the_same_keys_again_after_other_keys(self):
        self.keys.capture("pressed")
        self.press(META, SPACE)
        self.keys.capture("sent")
        self.press(CTRL, SPACE)
        self.assertTrue(self.keys.sendsOther)
        self.keys.sendSame()
        self.assertEqual((self.keys.sendsOther, self.keys.sentLabel), (False, "The same keys"))
        self.keys.capture("sent")
        self.press(META, SPACE)
        self.assertFalse(self.keys.sendsOther, "the pressed keys captured as the sent ones are the same keys")

    def test_release_keys_and_volume_keys_are_refused(self):
        before = self.rules()
        self.keys.capture("pressed")
        self.press(CTRL, ALT, SHIFT, ESC)
        self.assertIn("always stays Frametop's", self.keys.problem)
        self.keys.add()
        self.assertEqual(self.rules(), before)
        self.keys.capture("pressed")
        self.press(META, SPACE)
        self.keys.capture("sent")
        self.press(META, VOLUME_UP)
        self.assertIn("can't be sent", self.keys.problem)
        self.keys.add()
        self.assertEqual(self.rules(), before)

    def test_cancel_and_one_capture_at_a_time(self):
        self.keys.capture("pressed")
        self.keys.cancelCapture()
        self.press(META, SPACE)
        self.assertEqual((self.keys.capturing, self.keys.pressed), ("", ""))
        # The Keyboard page's capture takes over: this page's is given up, and the keys are its.
        self.keys.capture("pressed")
        self.b.startShortcutCapture("float_toggle")
        self.assertEqual((self.keys.capturing, self.b.capturingShortcut), ("", True))
        self.press(META, SHIFT, J)
        self.assertEqual((self.keys.pressed, self.rules()["key_bindings"]["42+125+36"]), ("", "float_toggle"))
        self.assertFalse(self.b.capturingShortcut)
        # And the other way round.
        self.b.startShortcutCapture("float_toggle")
        self.keys.capture("pressed")
        self.assertEqual((self.keys.capturing, self.b.capturingShortcut), ("pressed", False))
        self.press(META, SPACE)
        self.assertEqual((self.keys.pressed, self.rules()["key_bindings"]["125+57"]), ("125+57", "frunner"))

    def test_remove_and_back_to_the_defaults(self):
        self.keys.remove("125+57")
        self.assertNotIn("125+57", self.rules()["remote_key_bindings"])
        self.assertFalse(self.keys.isDefault)
        self.assertEqual(self.said[-1], ("Meta+Space stays Frametop's on a remote display", False))
        self.keys.reset()
        self.assertNotIn("remote_key_bindings", self.rules())
        self.assertTrue(self.keys.isDefault)

    def test_a_binding_changed_on_the_keyboard_page_shows_here(self):
        seen = []
        self.keys.changed.connect(lambda: seen.append(1))
        self.b.removeShortcut("125+57")
        self.assertTrue(seen)
        self.assertEqual(next(r for r in self.keys.rows if r["pressed"] == "125+57")["instead"], "")


class Page(unittest.TestCase):
    def test_it_opens_and_shows_the_list(self):
        app = QGuiApplication.instance() or QGuiApplication([])
        os.makedirs(os.path.dirname(fis.RULES_PATH), exist_ok=True)
        with open(fis.RULES_PATH, "w") as f:
            json.dump({"remote_key_bindings": {}}, f)
        self.addCleanup(os.remove, fis.RULES_PATH)
        b = fis.Backend()
        b._send = lambda text, to=fis.RELAY: True
        b.notifier.setEnabled(False)
        errors = []
        engine = QQmlApplicationEngine()
        engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
        settings = ft_settings.Settings(start="remote-keys", backends={"input": b})
        ft_settings.load(engine, settings)
        if not engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(errors[:3]))

        def settle():
            deadline = time.monotonic() + 0.5
            while time.monotonic() < deadline:
                app.processEvents()
                time.sleep(0.01)
        settle()
        root = engine.rootObjects()[0]
        none, add = root.findChild(QObject, "remoteKeysNone"), root.findChild(QObject, "remoteKeysAdd")
        self.assertIsNotNone(none, "the Remote display keys page didn't open")
        self.assertTrue(none.property("visible"), "an empty list of its own says so")
        self.assertFalse(add.property("enabled"), "nothing to add before any keys are pressed")
        b.remoteKeys.reset()
        settle()
        self.assertFalse(none.property("visible"), "the defaults are listed")
        mine = [e for e in errors if "RemoteKeysPage.qml" in e and "not placed in the graphics scene" not in e]
        self.assertEqual(mine, [])


if __name__ == "__main__":
    unittest.main()
