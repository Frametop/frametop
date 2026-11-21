"""Remote Displays' "Add windows" (issue 81): a KDE PC's open windows from KWin Window Stream.
When the page offers it, what it keeps of the PC's list, what adding one runs, and the dialog.
Same stand-ins as test_app_windows.py (HOME a temp folder, ft-layout not run); the list is a
stand-in for ft_kwinwindows.find_client (tested in layout/tests/test_kwin_windows.py). Needs
PySide6 and Kirigami.

  python3 remote-displays/test/test_kwin_windows.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import ADDRESS, HOSTS, Base, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
import kwin_windows  # noqa: E402
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine, QQmlEngine, QQmlExpression  # noqa: E402
from PySide6.QtQuick import QQuickItem  # noqa: E402

UUID = "{8d3e0c52-5b1f-4c3e-9a57-1f2e3d4c5b6a}"
OTHER = "{0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d}"
KDE = {"name": "KDE", "address": ADDRESS, "kind": "sunshine", "displays": [
    {"id": "kde-desktop", "client": "frametop-kde-desktop", "app": "Desktop", "size": [2560, 1440]},
    {"id": "kde-kate", "client": "frametop-kde-desktop", "app": "kwin:" + OTHER, "label": "Kate",
     "size": [1280, 800]}]}
FROM_PC = [{"id": UUID, "title": "Konsole\x1b[31m", "app": "org.kde.konsole", "width": 1282, "height": 802},
           {"id": OTHER.upper(), "title": "Kate", "app": "org.kde.kate", "width": 1280, "height": 800,
            "minimized": True},
           {"id": "0x55aa10", "title": "not KWin's"}, {"id": UUID, "width": "wide"}]


def pin(key="sha256//kde"):
    """The host's Web UI key, as a sign-in pins it."""
    HOSTS.mkdir(parents=True, exist_ok=True)
    (HOSTS / f"{ADDRESS}.pin").write_text(key + "\n" if key else "")


class KwinBackend(Base):
    def setUp(self):
        super().setUp()
        set_hosts(KDE)
        pin()
        self.ready = []
        self.b.kwinWindows.windowsReady.connect(lambda *a: self.ready.append(a))
        old = kwin_windows.ft_kwinwindows.find_client
        self.asked = []
        kwin_windows.ft_kwinwindows.find_client = lambda host, **kw: (self.asked.append(host["name"]) or
                                                                      ("frametop-kde-desktop", list(FROM_PC)))
        self.addCleanup(setattr, kwin_windows.ft_kwinwindows, "find_client", old)

    def kde(self):
        return next(h for h in self.b.hosts if h["name"] == "KDE")

    def test_offered_for_a_pinned_host_with_a_display(self):
        self.assertTrue(self.kde()["kwinWindows"])
        pin("")
        self.assertFalse(self.kde()["kwinWindows"], "no pinned key: the PC's certificate can't be checked")
        pin()
        set_hosts(dict(KDE, displays=[]))
        self.assertFalse(self.kde()["kwinWindows"], "no display: no pairing to connect with")

    def test_a_window_panel_is_listed_as_a_window(self):
        kinds = {d["id"]: d["kwin"] for d in self.kde()["displays"]}
        self.assertEqual(kinds, {"kde-desktop": False, "kde-kate": True})

    def test_the_list_keeps_only_good_windows_with_printable_titles(self):
        self.b.kwinWindows.listWindows("KDE")
        self.assertTrue(spin(self.app, lambda: self.ready))
        host, listed, client, error = self.ready[0]
        self.assertEqual((host, client, error, self.asked), ("KDE", "frametop-kde-desktop", "", ["KDE"]))
        self.assertEqual([(w["id"], w["title"], w["added"], w["minimized"]) for w in listed],
                         [(UUID, "Konsole[31m", False, False), (OTHER, "Kate", True, True)])

    def test_no_service_says_why(self):
        def refused(host, **kw):
            raise RuntimeError("kwin windows: can't connect to 192.0.2.7:14501")
        kwin_windows.ft_kwinwindows.find_client = refused
        self.b.kwinWindows.listWindows("KDE")
        self.assertTrue(spin(self.app, lambda: self.ready))
        self.assertEqual(self.ready[0], ("KDE", [], "", "kwin windows: can't connect to 192.0.2.7:14501"))

    def test_adding_one_runs_kwin_add_with_the_client_the_pc_took(self):
        self.b.kwinWindows.addWindow("KDE", UUID, "Konsole\x07", "frametop-kde-desktop", 1282, 802)
        self.assertEqual(self.ran, [("remote", "kwin-add", "KDE", UUID, "--label", "Konsole", "--size", "1282x802",
                                     "--client", "frametop-kde-desktop")])


class KwinPage(Base):
    def setUp(self):
        super().setUp()
        set_hosts(KDE)
        pin()
        self.listed = []
        old = kwin_windows.KwinWindows.listWindows
        kwin_windows.KwinWindows.listWindows = lambda s, name: self.listed.append(name)
        self.addCleanup(setattr, kwin_windows.KwinWindows, "listWindows", old)
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

    def test_the_dialog_lists_and_adds_the_ticked_ones(self):
        dialog = self.root.findChild(QObject, "kwinDialog")
        self.assertIsNotNone(dialog, "the windows dialog is on the page")
        dialog.setProperty("host", "KDE")
        dialog.refresh()
        self.assertEqual(self.listed, ["KDE"])
        self.b.kwinWindows.windowsReady.emit("KDE", kwin_windows.offered(KDE, FROM_PC), "frametop-kde-desktop", "")
        self.assertTrue(spin(self.app, lambda: not dialog.property("loading")))
        dialog.tick(UUID, True)
        dialog.tick(OTHER, True)  # added already: the box is off, but a tick still can't add it twice
        self.assertEqual(dialog.property("count"), 1)
        dialog.addTicked()
        self.assertEqual([a[:4] for a in self.ran], [("remote", "kwin-add", "KDE", UUID)])
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])

    def test_titles_from_the_pc_are_plain_text(self):
        dialog = self.root.findChild(QObject, "kwinDialog")
        dialog.setProperty("host", "KDE")
        listed = [{"id": UUID, "title": "<b>Konsole</b><img src=x>", "app": "", "width": 1282, "height": 802}]
        self.b.kwinWindows.windowsReady.emit("KDE", kwin_windows.offered(KDE, listed), "c", "<i>refused</i>")
        self.assertTrue(spin(self.app, lambda: dialog.property("listed")))
        spin(self.app, seconds=0.1)
        items, stack = set(), list(dialog.findChildren(QQuickItem))  # a Repeater's rows: visual children only
        while stack:
            item = stack.pop()
            if item not in items:
                items.add(item)
                stack.extend(item.childItems())
        shown = [o for o in items if "<b>Konsole" in str(o.property("text") or "")
                 or "<i>refused" in str(o.property("text") or "")]
        self.assertEqual(len(shown), 2, "the title and the error are shown")
        for o in shown:
            plain = QQmlExpression(QQmlEngine.contextForObject(o), o, "textFormat === Text.PlainText").evaluate()
            self.assertEqual(plain, (True, False), f"{o.property('text')!r} isn't plain text")


if __name__ == "__main__":
    unittest.main()
