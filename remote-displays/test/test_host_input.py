"""Remote Displays' "Use this computer's own" Keyboard and Mouse switches: what the host list says
and what each switch sets, leaving the other kind as it was. Same stand-ins as
test_app_windows.py (HOME a temp folder, ft-layout not run). Needs PySide6 and Kirigami.

  python3 remote-displays/test/test_host_input.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import ADDRESS, Base, items, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
from PySide6.QtCore import QMetaObject  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402


def host(value):
    entry = {"name": "PC", "address": ADDRESS, "kind": "vibepollo"}
    if value is not None:
        entry["input"] = value
    return entry


class Backend(Base):
    def test_the_host_list_says_which_kinds_it_lends(self):
        for value, kinds in ((None, (False, False)), (False, (False, False)), (True, (True, True)),
                             ("keyboard", (True, False)), ("mouse", (False, True)), ("both", (False, False))):
            set_hosts(host(value))
            h = self.b.hosts[0]
            self.assertEqual((h["inputKeyboard"], h["inputMouse"]), kinds, value)

    def test_each_switch_leaves_the_other_kind(self):
        for value, kind, on, word in ((None, "keyboard", True, "keyboard"), (None, "mouse", True, "mouse"),
                                      ("keyboard", "mouse", True, "on"), ("mouse", "keyboard", True, "on"),
                                      (True, "mouse", False, "keyboard"), (True, "keyboard", False, "mouse"),
                                      ("keyboard", "keyboard", False, "off"), ("mouse", "mouse", False, "off"),
                                      ("keyboard", "keyboard", True, "keyboard")):
            set_hosts(host(value))
            self.ran.clear()
            self.b.setInput("PC", kind, on)
            self.assertEqual(self.ran, [("remote", "host", "PC", f"input={word}")], (value, kind, on))

    def test_nothing_for_an_unknown_host_or_kind(self):
        self.b.setInput("Nope", "keyboard", True)
        self.b.setInput("PC", "trackpad", True)
        self.assertEqual(self.ran, [])


class Page(Base):
    def setUp(self):
        super().setUp()
        set_hosts(host("keyboard"))
        self.settings = ft_settings.Settings(start="remote-displays", backends={"remote": self.b})
        self.engine = QQmlApplicationEngine()
        self.warnings = []
        self.engine.warnings.connect(lambda ws: self.warnings.extend(w.toString() for w in ws))
        if not ft_settings.load(self.engine, self.settings):
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(self.warnings[:3]))
        self.root = self.engine.rootObjects()[0]
        spin(self.app, seconds=0.3)

    def tearDown(self):
        self.engine.deleteLater()
        spin(self.app, seconds=0.05)

    def switch(self, kind):
        found = [i for i in items(self.root.contentItem()) if i.objectName() == "input-" + kind]
        self.assertEqual(len(found), 1, f"one {kind} switch on the host's card")
        return found[0]

    def test_two_switches_side_by_side(self):
        kb, mouse = self.switch("keyboard"), self.switch("mouse")
        self.assertEqual((kb.property("text"), mouse.property("text")), ("Keyboard", "Mouse"))
        self.assertEqual((kb.property("checked"), mouse.property("checked")), (True, False))
        self.assertEqual(kb.parentItem(), mouse.parentItem(), "in one row")
        self.assertLess(kb.x(), mouse.x(), "keyboard first")
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])

    def test_the_mouse_switch_sets_both(self):
        mouse = self.switch("mouse")
        mouse.setProperty("checked", True)
        QMetaObject.invokeMethod(mouse, "toggled")
        spin(self.app, seconds=0.1)
        self.assertEqual(self.ran, [("remote", "host", "PC", "input=on")])


if __name__ == "__main__":
    unittest.main()
