"""Remote Displays' per-route stream settings (direct/dongle vs network): what the backend
exposes, what setRouteStream sends to ft-layout, and the Kirigami controls on each display's
settings form. HOME is a temp folder and ft-layout/ft-screens are never run. Needs PySide6 and
Kirigami (the image's python3).

  python3 remote-displays/test/test_route_settings.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import ADDRESS, Base, items, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
from PySide6.QtCore import QMetaObject  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402


def make_host(*, kind="vibepollo", direct=None, routes=None):
    disp = {
        "id": "pc-main",
        "client": "frametop-pc-main",
        "app": "apple:physical" if kind == "apple" else "monitor",
        "label": "Main",
        "size": [2560, 1440],
        "fps": 60,
        "bitrate": 20000,
    }
    if routes is not None:
        disp["routes"] = routes
    entry = {"name": "PC", "address": ADDRESS, "kind": kind, "displays": [disp]}
    if direct is not None:
        entry["direct"] = direct
    return entry


class BackendTest(Base):
    def test_hosts_exposes_route_overrides_and_defaults(self):
        set_hosts(make_host(direct=["10.0.0.9"]))
        d = self.b.hosts[0]["displays"][0]
        self.assertEqual((d["directFps"], d["directBitrate"], d["networkFps"], d["networkBitrate"], d["hasRoutes"]),
                         (0, -1, 0, -1, False))

        set_hosts(make_host(direct=["10.0.0.9"], routes={"direct": {"fps": 90, "bitrate": 35000},
                                                         "network": {"bitrate": 0}}))
        d = self.b.hosts[0]["displays"][0]
        self.assertEqual((d["directFps"], d["directBitrate"], d["networkFps"], d["networkBitrate"], d["hasRoutes"]),
                         (90, 35000, 0, 0, True))

    def test_set_route_stream_sends_only_changed_and_reset_keys(self):
        self.b.setRouteStream("pc-main", 90, 35000, -1, 4000)
        self.assertEqual(self.ran, [("remote", "set", "pc-main",
                                     "direct.fps=90", "direct.bitrate=35000", "network.bitrate=4000")])

        self.ran.clear()
        self.b.setRouteStream("pc-main", 0, -1, 0, -1)
        self.assertEqual(self.ran, [("remote", "set", "pc-main",
                                     "direct.fps=", "direct.bitrate=", "network.fps=", "network.bitrate=")])

        self.ran.clear()
        self.b.setRouteStream("pc-main", -1, -2, -1, -2)
        self.assertEqual(self.ran, [])


class PageTest(Base):
    def load_page(self, *hosts):
        set_hosts(*hosts)
        self.b.changed.emit()
        self.settings = ft_settings.Settings(start="remote-displays", backends={"remote": self.b})
        self.engine = QQmlApplicationEngine()
        self.warnings = []
        self.engine.warnings.connect(lambda ws: self.warnings.extend(w.toString() for w in ws))
        if not ft_settings.load(self.engine, self.settings):
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(self.warnings[:3]))
        self.root = self.engine.rootObjects()[0]
        self.addCleanup(self._close_engine)
        spin(self.app, seconds=0.3)

    def _close_engine(self):
        self.engine.deleteLater()
        spin(self.app, seconds=0.05)

    def one(self, name):
        found = [i for i in items(self.root.contentItem()) if i.objectName() == name]
        self.assertEqual(len(found), 1, f"expected one {name}, got {len(found)}")
        return found[0]

    def expand_first_display(self):
        # Toggle the configure ToolButton ("Stream & size settings") to reveal the display form.
        btn = next(i for i in items(self.root.contentItem())
                   if i.property("text") == "Stream & size settings")
        QMetaObject.invokeMethod(btn, "clicked")
        spin(self.app, seconds=0.1)

    def test_route_rows_hidden_without_direct_or_saved_routes(self):
        self.load_page(make_host())
        self.expand_first_display()
        self.assertFalse(self.one("routeRow-direct").property("visible"))
        self.assertFalse(self.one("routeRow-network").property("visible"))
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])

    def test_route_rows_apply_and_reset_for_host_with_direct(self):
        self.load_page(make_host(direct=["10.0.0.9"], routes={"network": {"bitrate": 0}}))
        self.expand_first_display()
        self.assertTrue(self.one("routeRow-direct").property("visible"))
        self.assertTrue(self.one("routeRow-network").property("visible"))
        self.assertTrue(self.one("routeBitrate-direct").property("visible"))
        self.assertFalse(self.one("routeApply").property("enabled"))
        self.assertTrue(self.one("routeReset").property("visible"))

        # Edit direct bitrate (35 Mbps) and network fps (30 fps), leaving untouched network.bitrate=0 intact.
        self.one("routeBitrate-direct").setProperty("value", 35)
        net_fps = self.one("routeFps-network")
        net_fps.setProperty("currentIndex", 1)  # 30 fps (index 0 is Default)
        spin(self.app, seconds=0.1)
        apply_btn = self.one("routeApply")
        self.assertTrue(apply_btn.property("enabled"))
        QMetaObject.invokeMethod(apply_btn, "clicked")
        spin(self.app, seconds=0.1)
        self.assertEqual(self.ran, [("remote", "set", "pc-main", "direct.bitrate=35000", "network.fps=30")])

        # Clear button resets all four per-route keys.
        self.ran.clear()
        QMetaObject.invokeMethod(self.one("routeReset"), "clicked")
        spin(self.app, seconds=0.1)
        self.assertEqual(self.ran, [("remote", "set", "pc-main",
                                     "direct.fps=", "direct.bitrate=", "network.fps=", "network.bitrate=")])
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])

    def test_apple_host_hides_route_bitrate_spinboxes(self):
        self.load_page(make_host(kind="apple", direct=["10.0.0.9"]))
        self.expand_first_display()
        self.assertTrue(self.one("routeRow-direct").property("visible"))
        self.assertFalse(self.one("routeBitrate-direct").property("visible"))
        self.assertFalse(self.one("routeBitrate-network").property("visible"))

        direct_fps = self.one("routeFps-direct")
        direct_fps.setProperty("currentIndex", 2)  # 60 fps
        spin(self.app, seconds=0.1)
        apply_btn = self.one("routeApply")
        self.assertTrue(apply_btn.property("enabled"))
        QMetaObject.invokeMethod(apply_btn, "clicked")
        spin(self.app, seconds=0.1)
        self.assertEqual(self.ran, [("remote", "set", "pc-main", "direct.fps=60")])
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
