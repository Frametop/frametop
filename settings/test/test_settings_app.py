"""Frametop Settings (settings/), offline: the window loads, every section opens its page without QML
warnings, an area's backend is made only when one of its sections opens, the search finds
sections by their words, and the section to start on comes from the argument or FT_SETTINGS_PAGE.
HOME is a temp folder, and every command for the host is a fake: nothing is read from or sent to
the host, ft-screens or the relay. Needs PySide6 and Kirigami (the image's python3).

  python3 settings/test/test_settings_app.py
"""
import os
import re
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-settings-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "settings"))
import ft_settings  # noqa: E402
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

HOST_CMDS = []


def fake_host_command(*cmd):
    """Every area's host commands: noted, and run as `true` (nothing reaches the host)."""
    HOST_CMDS.append(cmd)
    return ["true"]


def patched_backend(area):
    module = ft_settings.AREAS[area][1]
    folder = str(ROOT / ft_settings.AREAS[area][0])
    if folder not in sys.path:
        sys.path.insert(0, folder)
    mod = __import__(module)
    if hasattr(mod, "host_command"):
        mod.host_command = fake_host_command
    if hasattr(mod, "host"):  # input-settings' own runner
        mod.host = lambda *cmd, timeout=20: (HOST_CMDS.append(cmd), _Done())[1]
    b = mod.Backend()
    if hasattr(b, "_send"):  # the relay, the pointer helper, the gaze service: never asked
        b._send = lambda *a, **k: False
    return b


class _Done:
    returncode, stdout, stderr = 1, "", "fake"


class Window:
    """The app's window in an engine of its own, with the warnings it printed."""

    def __init__(self, test, start="screens", kind="screens"):
        self.app = QGuiApplication.instance() or QGuiApplication([])
        self.made = []
        old = ft_settings.make_backend
        ft_settings.make_backend = lambda area: (self.made.append(area), patched_backend(area))[1]
        test.addCleanup(setattr, ft_settings, "make_backend", old)
        self.settings = ft_settings.Settings(start=start, kind=kind)
        self.engine = QQmlApplicationEngine()
        self.warnings = []
        self.engine.warnings.connect(lambda ws: self.warnings.extend(w.toString() for w in ws))
        if not ft_settings.load(self.engine, self.settings):
            test.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(self.warnings[:3]))
        self.root = self.engine.rootObjects()[0]
        self.spin()

    def spin(self, seconds=0.2):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.app.processEvents()
            time.sleep(0.01)

    def show(self, name):
        self.root.showSection(name)
        self.spin()

    def page_title(self):
        return self.root.property("pageTitle")

    def own_warnings(self):
        """Ours: not the "not placed in the graphics scene" ones Qt prints for offscreen popups."""
        return [w for w in self.warnings if "not placed in the graphics scene" not in w]


class App(unittest.TestCase):
    def test_every_section_opens_its_page(self):
        w = Window(self)
        for name, area, group, title, icon, words in ft_settings.SECTIONS:
            with self.subTest(section=name):
                w.show(name)
                self.assertEqual(w.root.property("current"), name)
                self.assertEqual(w.page_title(), title)
        self.assertEqual(w.own_warnings(), [])
        self.assertEqual(sorted(set(w.made)), sorted(ft_settings.AREAS))
        self.assertEqual(len(w.made), len(ft_settings.AREAS), "each backend is made once")

    def test_backends_are_made_when_their_page_opens(self):
        w = Window(self, start="remote-access")
        self.assertEqual(w.made, ["access"])
        self.assertEqual(w.page_title(), "Remote access")
        w.show("pointer")
        self.assertEqual(w.made, ["access", "input"])
        self.assertEqual(w.own_warnings(), [])

    def test_gamescope_has_no_visibility_page(self):
        w = Window(self, kind="gamescope")
        names = [s["name"] for s in w.settings.sections]
        self.assertNotIn("visibility", names)
        self.assertIn("screens", names)

    def test_search(self):
        w = Window(self)
        search = w.root.findChild(QObject, "search")
        sections = w.root.findChild(QObject, "sections")
        self.assertIsNotNone(search)
        self.assertEqual(sections.property("count"), len(ft_settings.SECTIONS))
        for text, expect in (("vnc", ["remote-access"]), ("moonlight", ["remote-displays"]),
                             ("EYE calibrate", ["gaze"]), ("zzz", [])):
            with self.subTest(text=text):
                search.setProperty("text", text)
                w.spin(0.05)
                found = [s["name"] for s in sections.property("model")]
                self.assertEqual(found, expect)
        # Typing only filters: no page opens until Enter (one would take the keyboard mid-word).
        search.setProperty("text", "gaz")
        w.spin(0.6)
        self.assertEqual(w.root.property("current"), "screens")
        search.setProperty("text", "")

    def test_start_section(self):
        self.assertEqual(ft_settings.start_section(["pointer"], {}), "pointer")
        self.assertEqual(ft_settings.start_section([], {"FT_SETTINGS_PAGE": "gaze"}), "gaze")
        self.assertEqual(ft_settings.start_section(["nope"], {"FT_SETTINGS_PAGE": "power"}), "power")
        self.assertEqual(ft_settings.start_section([], {}), ft_settings.NAMES[0])

    def test_sections_are_named_once_and_each_area_has_its_pages(self):
        self.assertEqual(len(ft_settings.NAMES), len(set(ft_settings.NAMES)))
        for area, (folder, module, qml) in ft_settings.AREAS.items():
            text = (ROOT / folder / qml).read_text()
            self.assertTrue((ROOT / folder / (module + ".py")).is_file(), module)
            self.assertIn("function page(name)", text, qml)
            for name, a, *_ in ft_settings.SECTIONS:
                if a == area:
                    self.assertTrue(re.search(rf'(\b{re.escape(name)}\b\s*:|"{re.escape(name)}")', text),
                                    f"{qml} has no page for {name}")


if __name__ == "__main__":
    unittest.main()
