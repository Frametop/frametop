"""Remote Displays' "Turn off and set its own monitors" switch and the display panels' mode
pickers (host_monitors.py, MonitorModePicker.qml). What is asked, what is run, how the panels'
sizes follow, and the page. Same stand-ins as test_app_windows.py (HOME a temp folder, ft-layout
not run); the PC is a stand-in for ft_hostapi.request. Needs PySide6 and Kirigami.

  python3 remote-displays/test/test_host_monitors.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import ADDRESS, HOSTS, Base, items, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
import host_monitors  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

HZ240, HZ60 = ({"width": 2560, "height": 1600, "refresh_mhz": 240000},
               {"width": 2560, "height": 1600, "refresh_mhz": 60000})
UHD = {"width": 3840, "height": 2160, "refresh_mhz": 60000}


def pc(monitors=True):
    return {"name": "PC", "address": ADDRESS, "kind": "vibepollo", "monitors": monitors, "displays": [
        {"id": "pc-edp", "client": "frametop-pc-edp", "app": "display:eDP-2", "label": "Laptop", "size": [2560, 1600],
         "mode": HZ60},
        {"id": "pc-dp", "client": "frametop-pc-dp", "app": "display:DP-1", "label": "Big", "size": [3840, 2160]},
        {"id": "pc-virtual", "client": "frametop-pc-virtual", "app": "monitor", "size": [1920, 1080]}]}


def js(value):
    """A QML var as Python (a JS object comes as a QJSValue)."""
    return value.toVariant() if hasattr(value, "toVariant") else value


def state(dp_sideways=False, error=""):
    return {"status": True, "backend": "hyprland", "streams": 0, "applies": False, "grace_seconds": 120,
            "error": error, "others_off": True,
            "monitors": [{"id": "eDP-2", "name": "<b>Samsung</b>", "on": True, "primary": True, "mode": HZ240,
                          "modes": [HZ240, HZ60], "changed": False, "sideways": False},
                         {"id": "DP-1", "name": "Dell", "on": True, "primary": False, "mode": UHD,
                          "modes": [UHD], "changed": False, "sideways": dp_sideways}]}


class Words(unittest.TestCase):
    def test_why(self):
        self.assertEqual(host_monitors.why("PC", 200, state()), "")
        self.assertEqual(host_monitors.why("PC", 0, None), "Couldn't reach PC")
        self.assertIn("Sign in to PC again", host_monitors.why("PC", 401, None))
        self.assertEqual(host_monitors.why("PC", 404, None), host_monitors.NO_BUILD)
        self.assertEqual(host_monitors.why("PC", 400, {"status": False, "error": "that would leave no display on"}),
                         "that would leave no display on")
        self.assertEqual(host_monitors.why("PC", 500, "<html>"), "PC answered 500")

    def test_mode_arg(self):
        self.assertEqual(host_monitors.mode_arg(HZ60), "2560x1600@60000")
        self.assertEqual(host_monitors.mode_arg(None), "own")


class Sizes(unittest.TestCase):
    def test_a_panel_streams_at_its_picked_mode_or_its_monitors(self):
        host = pc()
        host["displays"][0]["size"] = [1920, 1080]
        host["displays"][1]["size"] = [1920, 1080]
        self.assertEqual([a for _, a in host_monitors.size_changes(host, state()["monitors"])],
                         [("remote", "set", "pc-edp", "size=2560x1600"), ("remote", "set", "pc-dp", "size=3840x2160")])

    def test_same_size_runs_nothing(self):
        self.assertEqual(host_monitors.size_changes(pc(), state()["monitors"]), [])

    def test_a_monitor_turned_a_quarter_streams_tall(self):
        self.assertEqual([a for _, a in host_monitors.size_changes(pc(), state(dp_sideways=True)["monitors"])],
                         [("remote", "set", "pc-dp", "size=2160x3840")])

    def test_without_the_switch_the_panels_keep_their_size(self):
        self.assertEqual(host_monitors.size_changes(pc(monitors=False), state(dp_sideways=True)["monitors"]), [])

    def test_a_monitor_that_is_off_has_no_mode_to_follow(self):
        listed = state()["monitors"]
        listed[1].update(on=False, mode=None)
        host = pc()
        host["displays"][1]["size"] = [1920, 1080]
        self.assertEqual(host_monitors.size_changes(host, listed), [])


class Backend(Base):
    def setUp(self):
        super().setUp()
        set_hosts(pc())
        self.sent, self.reply = [], (200, state(dp_sideways=True))
        old = host_monitors.ft_hostmonitors.ft_hostapi.request

        def request(address, method, path, body=None, timeout=2.0, **kw):
            self.sent.append((address, method, path, body))
            return self.reply
        host_monitors.ft_hostmonitors.ft_hostapi.request = request
        self.addCleanup(setattr, host_monitors.ft_hostmonitors.ft_hostapi, "request", old)
        self.thens = []
        self.b._run = lambda label, *args, stdin=None, then=None: (self.ran.append(args), self.thens.append(then))
        self.listed = []
        self.b.hostMonitors.listedChanged.connect(lambda: self.listed.append(self.b.hostMonitors.listed))

    def answer(self):
        self.assertTrue(spin(self.app, lambda: self.listed), "no answer")
        return self.listed.pop()["PC"]

    def test_refresh_lists_the_monitors_and_the_panels_follow(self):
        self.b.hostMonitors.refresh("PC")
        got = self.answer()
        self.assertEqual((got["note"], len(got["monitors"])), ("", 2))
        self.assertEqual(self.sent, [(ADDRESS, "GET", host_monitors.ft_hostmonitors.PATH, None)])
        self.assertEqual(self.ran, [("remote", "set", "pc-dp", "size=2160x3840")])
        self.assertEqual(self.b.hostMonitors.supported, {"PC": True})

    def test_the_pcs_last_error_is_its_note(self):
        self.reply = (200, state(error="DP-1 refused the mode"))
        self.b.hostMonitors.refresh("PC")
        self.assertEqual(self.answer()["note"], "DP-1 refused the mode")

    def test_an_old_build_is_known_and_an_old_sign_in_is_not(self):
        self.reply = (401, None)
        self.b.hostMonitors.refresh("PC")
        self.assertIn("Sign in to PC again", self.answer()["note"])
        self.assertEqual(self.b.hostMonitors.supported, {}, "it may well can, once signed in again")
        self.reply = (404, None)
        self.b.hostMonitors.refresh("PC")
        self.assertEqual(self.answer()["note"], host_monitors.NO_BUILD)
        self.assertEqual(self.b.hostMonitors.supported, {"PC": False})
        self.assertEqual(self.ran, [], "no answer: the panels are left as they are")

    def test_the_switch_runs_ft_layout_then_asks_again(self):
        self.b.hostMonitors.setControl("PC", True)
        self.b.hostMonitors.setControl("PC", False)
        self.assertEqual(self.ran, [("remote", "host", "PC", "monitors=on"), ("remote", "host", "PC", "monitors=off")])
        self.thens[0]()
        self.answer()
        self.assertEqual(self.sent[-1][1], "GET")

    def test_a_pick_saves_the_mode_and_its_stream_size(self):
        self.b.hostMonitors.refresh("PC")
        self.answer()
        self.ran.clear()
        self.b.hostMonitors.pickMode("PC", "pc-dp", UHD)
        self.b.hostMonitors.pickMode("PC", "pc-edp", None)
        self.assertEqual(self.ran, [("remote", "set", "pc-dp", "mode=3840x2160@60000", "size=2160x3840"),
                                    ("remote", "set", "pc-edp", "mode=own")])
        self.assertTrue(all(self.thens[-2:]), "each asks the PC again after")

    def test_a_sunshine_host_is_not_asked(self):
        set_hosts(dict(pc(), kind="sunshine"))
        self.b.hostMonitors.refresh("PC")
        self.assertEqual(self.answer()["note"], host_monitors.NO_BUILD)
        self.assertEqual(self.sent, [])


class Page(Base):
    def setUp(self):
        super().setUp()
        set_hosts(pc(monitors=False))
        HOSTS.mkdir(parents=True, exist_ok=True)
        (HOSTS / f"{ADDRESS}.token").write_text("tok\n")
        self.asked, self.picked = [], []
        olds = (host_monitors.HostMonitors.refresh, host_monitors.HostMonitors.pickMode)
        host_monitors.HostMonitors.refresh = lambda s, name: self.asked.append(name)
        host_monitors.HostMonitors.pickMode = lambda s, name, display_id, mode: self.picked.append((name, display_id, mode))
        self.addCleanup(setattr, host_monitors.HostMonitors, "refresh", olds[0])
        self.addCleanup(setattr, host_monitors.HostMonitors, "pickMode", olds[1])
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

    def found(self, name=None, prop=None):
        return [i for i in items(self.root.contentItem())
                if (name is None or i.objectName() == name) and (prop is None or i.property(prop) is not None)]

    def switch_on(self, on=True):
        set_hosts(pc(monitors=on))
        self.b.changed.emit()
        spin(self.app, seconds=0.1)

    def answered(self, answer=None):
        self.b.hostMonitors._done("PC", 200, answer or state())
        spin(self.app, seconds=0.1)

    def own_size(self):
        return sorted((i.property("modelData")["id"], i.property("ownSize")) for i in self.found(prop="ownSize"))

    def boxes(self):
        """Monitor id -> its display's mode picker (the virtual display's has no monitor)."""
        pickers = [(js(b.parentItem().property("monitor")), b) for b in self.found("monitorMode")]
        return {m["id"]: b for m, b in pickers if m}

    def bad_warnings(self):
        return [w for w in self.warnings if "not placed in the graphics scene" not in w]

    def test_the_page_asks_a_vibepollo_host_whether_it_can_once(self):
        self.assertEqual(self.asked, ["PC"])
        self.answered()
        self.b.changed.emit()
        spin(self.app, seconds=0.1)
        self.assertEqual(self.asked, ["PC"], "known now, switch off: not asked again")
        self.switch_on()
        self.assertEqual(self.asked, ["PC", "PC"], "switch on: asked for the modes each time")

    def test_the_switch_shows_once_it_can_and_says_it_acts_on_the_pc(self):
        def switch():
            found = self.found("ownMonitors")
            self.assertEqual(len(found), 1)
            return found[0]
        self.assertFalse(switch().property("visible"), "not known yet")
        self.answered()
        self.assertTrue(switch().property("visible"))
        self.assertEqual(switch().property("text"), "Turn off and set PC's own monitors")
        self.assertTrue(self.found("ownMonitorsHelp")[0].property("text").startswith("On PC itself:"))
        self.b.hostMonitors._done("PC", 404, None)
        spin(self.app, seconds=0.1)
        self.assertFalse(switch().property("visible"), "an older build can't")
        self.assertEqual(self.bad_warnings(), [])

    def test_with_the_switch_on_a_monitor_picks_a_mode_instead_of_a_size(self):
        self.answered()
        self.assertEqual(self.own_size(), [("pc-dp", True), ("pc-edp", True), ("pc-virtual", True)])
        self.switch_on()
        self.answered()
        self.assertEqual(self.own_size(), [("pc-dp", False), ("pc-edp", False), ("pc-virtual", True)])
        self.assertEqual(self.bad_warnings(), [])

    def test_the_picker_lists_its_own_mode_and_the_monitors(self):
        self.switch_on()
        self.answered()
        boxes = self.boxes()
        self.assertEqual(sorted(boxes), ["DP-1", "eDP-2"])
        edp, dp = boxes["eDP-2"], boxes["DP-1"]
        texts = lambda box: [c["text"] for c in js(box.property("entries"))]  # noqa: E731
        self.assertEqual(texts(dp), ["Its own mode (3840 × 2160 at 60 Hz)", "3840 × 2160 at 60 Hz"])
        self.assertEqual(texts(edp), ["Its own mode", "2560 × 1600 at 240 Hz", "2560 × 1600 at 60 Hz"],
                         "a mode is picked: its mode now isn't its own")
        self.assertEqual(edp.property("currentIndex"), 2, "the picked 60 Hz")
        self.assertEqual(dp.property("currentIndex"), 0)
        edp.parentItem().choose(0)
        dp.parentItem().choose(0)  # already its own: nothing to send
        self.assertEqual(self.picked, [("PC", "pc-edp", None)])
        self.assertEqual(self.bad_warnings(), [])

    def test_a_picked_mode_it_doesnt_list_still_shows(self):
        self.switch_on()
        listed = state()
        listed["monitors"][0].update(on=False, mode=None, modes=[])  # a Windows PC, the monitor off
        self.answered(listed)
        box = self.boxes()["eDP-2"]
        self.assertEqual([c["text"] for c in js(box.property("entries"))],
                         ["Its own mode", "2560 × 1600 at 60 Hz"])
        self.assertEqual(box.property("currentIndex"), 1)

    def test_the_pcs_note_shows_with_the_switch_on(self):
        self.switch_on()
        self.answered(state(error="DP-1 refused the mode"))
        note = self.found("ownMonitorsNote")[0]
        self.assertTrue(note.property("visible"))
        self.assertEqual(note.property("text"), "DP-1 refused the mode")


if __name__ == "__main__":
    unittest.main()
