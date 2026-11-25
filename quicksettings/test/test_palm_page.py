"""Frametop Settings' Quick Settings page itself (QuickSettingsPages.qml, PalmTileRow.qml): each row shows
its own tile's words and action, also after the list of actions changes (a ComboBox given another list
goes to its first entry), and picking or typing in one changes that tile. HOME is a temp folder. Needs
PySide6 and Kirigami (the image's python3).

  python3 quicksettings/test/test_palm_page.py
"""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-palm-page-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
for folder in ("settings", "quicksettings", "conf"):
    sys.path.insert(0, str(ROOT / folder))
import ft_palm_menu as palm  # noqa: E402
import ft_quick_settings as qs  # noqa: E402
import ft_settings  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine, QQmlExpression, qmlContext  # noqa: E402

CONFIG = Path(os.environ["HOME"]) / ".config"


class Page(unittest.TestCase):
    def setUp(self):
        CONFIG.mkdir(parents=True, exist_ok=True)
        self.commands("bbb")
        palm.PATH = str(CONFIG / f"palm-{self._testMethodName}.txt")
        Path(palm.PATH).write_text("menu\t1\tsteam_menu\tSteam\nmenu\t1\trun:bbb\tMine\nmenu\t0\trun:gone\tGone\n"
                                   "quick\t1\tdictate\tTalk\n")
        self.app = QGuiApplication.instance() or QGuiApplication([])
        self.backend = qs.Backend()
        self.warnings = []
        self.engine = QQmlApplicationEngine()
        self.engine.warnings.connect(lambda ws: self.warnings.extend(w.toString() for w in ws))
        self.settings = ft_settings.Settings(start="quick-settings", backends={"quick": self.backend})   # (held)
        ft_settings.load(self.engine, self.settings)
        if not self.engine.rootObjects():
            self.skipTest("Frametop Settings didn't load here (no Kirigami?): " + "; ".join(self.warnings[:3]))
        self.root = self.engine.rootObjects()[0]
        self.spin(0.5)

    def commands(self, *names):
        (CONFIG / "frametop-commands.json").write_text(
            json.dumps({"commands": [{"name": n, "run": "true"} for n in names]}))

    def spin(self, seconds=0.2):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.app.processEvents()
            time.sleep(0.01)

    def parts(self, name):
        """The rows' parts of that objectName, top to bottom (a Repeater's rows aren't QObject children)."""
        found, left = [], [self.root.contentItem()]
        while left:
            item = left.pop()
            if item.objectName() == name:
                found.append(item)
            left.extend(reversed(item.childItems()))
        return found

    def rows(self):
        return [(t.property("text"), c.property("currentValue")) for t, c in
                zip(self.parts("palmLabel"), self.parts("palmAction"), strict=True)]

    def tiles(self):
        return [(e["label"], e["action"]) for p in palm.PAGES for e in palm.of(palm.load(), p)]

    def run_in(self, item, code):
        QQmlExpression(qmlContext(item), item, code).evaluate()
        self.spin()

    def page_warnings(self):
        """(Every section's page is made outside the scene, and says so.)"""
        return [w for w in self.warnings if "/quicksettings/" in w and "not placed in the graphics scene" not in w]

    def test_each_row_shows_its_tile(self):
        self.assertEqual(self.rows(), self.tiles())
        self.assertEqual(len(self.rows()), 5)
        self.assertEqual(self.page_warnings(), [])

    def test_the_list_of_actions_changes(self):
        for names in (("aaa", "bbb"), ("aaa", "bbb", "gone"), ()):
            self.commands(*names)
            self.backend.refresh()
            self.spin()
            self.assertEqual(self.rows(), self.tiles(), names)
        self.backend.setAction("menu", 2, "dictate")   # (its "not available" entry goes)
        self.spin()
        self.assertEqual(self.rows(), self.tiles())

    def test_tiles_come_go_and_move(self):
        for edit in (lambda: self.backend.add("menu", "frunner"), lambda: self.backend.move("menu", 0, 1),
                     lambda: self.backend.remove("menu", 1), lambda: self.backend.restoreDefaults()):
            edit()
            self.spin()
            self.assertEqual(self.rows(), self.tiles())
        self.assertEqual(self.page_warnings(), [])

    def test_picking_and_typing(self):
        picker = self.parts("palmAction")[1]
        self.run_in(picker, "currentIndex = indexOfValue('frunner'); activated(currentIndex)")
        self.assertEqual(self.tiles()[1], ("Mine", "frunner"))
        field = self.parts("palmLabel")[0]
        self.run_in(field, "text = '  Big  '; editingFinished()")
        self.assertEqual(self.tiles()[0], ("Big", "steam_menu"))
        self.run_in(field, "text = ''; editingFinished()")
        self.assertEqual(self.tiles()[0], ("Steam Menu", "steam_menu"))
        self.assertEqual(self.rows(), self.tiles())


if __name__ == "__main__":
    unittest.main()
