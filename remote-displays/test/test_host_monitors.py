"""Remote Displays' "Monitors": a PC's own monitors on or off and in another mode, from Frametop's
build of Vibepollo. What is sent, what is told, how the panels follow, and the page. Same
stand-ins as test_app_windows.py (HOME a temp folder, ft-layout not run); the PC is a stand-in
for ft_hostapi.request. Needs PySide6 and Kirigami.

  python3 remote-displays/test/test_host_monitors.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import ADDRESS, HOSTS, Base, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
import host_monitors  # noqa: E402
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

PC = {"name": "PC", "address": ADDRESS, "kind": "vibepollo", "displays": [
    {"id": "pc-edp", "client": "frametop-pc-edp", "app": "display:eDP-2", "label": "Laptop", "size": [2560, 1600]},
    {"id": "pc-dp", "client": "frametop-pc-dp", "app": "display:DP-1", "label": "Big", "size": [3840, 2160]},
    {"id": "pc-virtual", "client": "frametop-pc-virtual", "app": "monitor", "size": [1920, 1080]},
    {"id": "pc-win", "client": "frametop-pc-win", "app": "display:DP-1", "window": True, "size": [800, 600]}]}
HZ240, HZ60 = ({"width": 2560, "height": 1600, "refresh_mhz": 240000},
               {"width": 2560, "height": 1600, "refresh_mhz": 60000})
UHD = {"width": 3840, "height": 2160, "refresh_mhz": 60000}


def state(edp_wish=None, dp_wish=None):
    return {"status": True, "backend": "hyprland", "streams": 0, "applies": False, "grace_seconds": 120, "error": "",
            "monitors": [{"id": "eDP-2", "name": "<b>Samsung</b>", "on": True, "primary": True, "mode": HZ240,
                          "modes": [HZ240, HZ60], "wish": edp_wish, "changed": False},
                         {"id": "DP-1", "name": "Dell", "on": True, "primary": False, "mode": UHD,
                          "modes": [UHD], "wish": dp_wish, "changed": False}]}


class Words(unittest.TestCase):
    def test_why(self):
        self.assertEqual(host_monitors.why("PC", 200, state()), "")
        self.assertEqual(host_monitors.why("PC", 0, None), "Couldn't reach PC")
        self.assertIn("Sign in to PC again", host_monitors.why("PC", 401, None))
        self.assertEqual(host_monitors.why("PC", 404, None), host_monitors.NO_BUILD)
        self.assertEqual(host_monitors.why("PC", 400, {"status": False, "error": "that would leave no display on"}),
                         "that would leave no display on")
        self.assertEqual(host_monitors.why("PC", 500, "<html>"), "PC answered 500")

    def test_the_body_has_a_mode_only_for_a_monitor_that_is_on(self):
        self.assertEqual(host_monitors.plan_body({"eDP-2": {"on": True, "width": 2560, "height": 1600,
                                                            "refresh_mhz": 60000},
                                                  "DP-1": {"on": False, "width": 3840, "height": 2160},
                                                  "HDMI-A-1": {"on": True, "width": 0}}),
                         {"monitors": {"eDP-2": {"on": True, "mode": HZ60}, "DP-1": {"on": False},
                                       "HDMI-A-1": {"on": True}}})
        self.assertEqual(host_monitors.plan_body({}), {"monitors": {}})


class Panels(unittest.TestCase):
    def test_a_mode_sets_the_size_and_off_disconnects(self):
        parked = set()
        runs = host_monitors.panel_changes(PC, state({"on": True, "mode": {**HZ60, "width": 1920, "height": 1200}},
                                                     {"on": False, "mode": None}), parked)
        self.assertEqual([a for _, a in runs], [("remote", "set", "pc-edp", "size=1920x1200"),
                                                ("remote", "disconnect", "pc-dp")])
        self.assertEqual(parked, {"pc-dp"}, "the window panel and the virtual display are left alone")

    def test_back_on_connects_only_what_was_parked_here(self):
        host = dict(PC, displays=[dict(d, off=True) if d["id"] in ("pc-dp", "pc-edp") else d for d in PC["displays"]])
        parked = {"pc-dp"}
        runs = host_monitors.panel_changes(host, state(), parked)
        self.assertEqual([a for _, a in runs], [("remote", "connect", "pc-dp")],
                         "the laptop's panel was disconnected by the user: it stays so")
        self.assertEqual(parked, set())

    def test_same_size_runs_nothing(self):
        self.assertEqual(host_monitors.panel_changes(PC, state({"on": True, "mode": HZ60}), set()), [])


class Backend(Base):
    def setUp(self):
        super().setUp()
        set_hosts(PC)
        self.sent, self.reply = [], (200, state({"on": True, "mode": {**HZ60, "width": 1920, "height": 1200}}))
        self.ready = []
        old = host_monitors.ft_hostapi.request

        def request(address, method, path, body=None, timeout=2.0, **kw):
            self.sent.append((address, method, path, body))
            return self.reply
        host_monitors.ft_hostapi.request = request
        self.addCleanup(setattr, host_monitors.ft_hostapi, "request", old)
        self.b.hostMonitors.monitorsReady.connect(lambda *a: self.ready.append(a))

    def answer(self):
        self.assertTrue(spin(self.app, lambda: self.ready), "no answer")
        return self.ready.pop()

    def test_refresh_asks_and_the_panels_follow(self):
        self.b.hostMonitors.refresh("PC")
        name, got, error = self.answer()
        self.assertEqual((name, error, len(got["monitors"])), ("PC", "", 2))
        self.assertEqual(self.sent, [(ADDRESS, "GET", host_monitors.PATH, None)])
        self.assertEqual(self.ran, [("remote", "set", "pc-edp", "size=1920x1200")])
        self.assertEqual(self.b.hostMonitors.supported, {"PC": True})

    def test_apply_and_put_back_send_the_plan(self):
        self.b.hostMonitors.apply("PC", {"DP-1": {"on": False}})
        self.answer()
        self.b.hostMonitors.putBack("PC")
        self.answer()
        self.assertEqual([(m, b) for _, m, _, b in self.sent],
                         [("POST", {"monitors": {"DP-1": {"on": False}}}), ("POST", {"monitors": {}})])

    def test_an_old_build_is_known_and_an_old_sign_in_is_not(self):
        self.reply = (401, None)
        self.b.hostMonitors.refresh("PC")
        self.assertIn("Sign in to PC again", self.answer()[2])
        self.assertEqual(self.b.hostMonitors.supported, {}, "still worth the button that says to sign in")
        self.reply = (404, None)
        self.b.hostMonitors.refresh("PC")
        _, got, error = self.answer()
        self.assertEqual((got, error), ({}, host_monitors.NO_BUILD))
        self.assertEqual(self.b.hostMonitors.supported, {"PC": False})
        self.assertEqual(self.ran, [], "no answer: the panels are left as they are")

    def test_a_sunshine_host_is_not_asked(self):
        set_hosts(dict(PC, kind="sunshine"))
        self.b.hostMonitors.refresh("PC")
        self.assertEqual(self.answer()[2], host_monitors.NO_BUILD)
        self.assertEqual(self.sent, [])


class Page(Base):
    def setUp(self):
        super().setUp()
        set_hosts(PC)
        HOSTS.mkdir(parents=True, exist_ok=True)
        (HOSTS / f"{ADDRESS}.token").write_text("tok\n")
        self.asked = []
        old = host_monitors.HostMonitors._ask
        host_monitors.HostMonitors._ask = lambda s, name, method, body=None: self.asked.append((name, method, body))
        self.addCleanup(setattr, host_monitors.HostMonitors, "_ask", old)
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

    def test_the_page_asks_each_vibepollo_host_once(self):
        self.assertEqual(self.asked, [("PC", "GET", None)])
        self.b.hostMonitors._done("PC", 200, state())
        spin(self.app, seconds=0.1)
        self.assertEqual(self.asked, [("PC", "GET", None)], "known now: not asked again")

    def test_the_dialog_sends_what_was_picked(self):
        dialog = self.root.findChild(QObject, "monitorsDialog")
        self.assertIsNotNone(dialog, "the monitors dialog is on the page")
        dialog.setProperty("host", "PC")
        self.b.hostMonitors.monitorsReady.emit("PC", state({"on": True, "mode": HZ60}), "")
        self.assertTrue(spin(self.app, lambda: dialog.property("listed")))
        wishes = dialog.property("wishes").toVariant()
        self.assertEqual(wishes["eDP-2"], {"on": True, **HZ60}, "its wish, not its mode now")
        self.assertEqual(wishes["DP-1"], {"on": True, **UHD})
        dialog.wish("DP-1", {"on": False})
        self.assertTrue(dialog.property("anyOn"))
        dialog.wish("eDP-2", {"on": False})
        self.assertFalse(dialog.property("anyOn"), "Apply is off with every monitor off")
        dialog.wish("eDP-2", {"on": True})
        dialog.send()
        self.assertEqual(self.asked[-1][:2], ("PC", "POST"))
        self.assertEqual(self.asked[-1][2]["monitors"]["DP-1"], {"on": False})
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
