"""Remote Displays' "Let Frametop control NAME's displays" switch and the display panels' scale
pickers and stream size lists (host_monitors.py, MonitorScalePicker.qml, StreamSizePicker.qml). What is asked, what is run, how the panels'
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
QHD = {"width": 2560, "height": 1440, "refresh_mhz": 60000}


def pc(monitors=True):
    return {"name": "PC", "address": ADDRESS, "kind": "vibepollo", "monitors": monitors, "displays": [
        {"id": "pc-edp", "client": "frametop-pc-edp", "app": "display:eDP-2", "label": "Laptop", "size": [2560, 1600],
         "mode": HZ60},
        {"id": "pc-dp", "client": "frametop-pc-dp", "app": "display:DP-1", "label": "Big", "size": [3840, 2160]},
        {"id": "pc-virtual", "client": "frametop-pc-virtual", "app": "monitor", "size": [1920, 1080]}]}


def js(value):
    """A QML var as Python (a JS object comes as a QJSValue)."""
    return value.toVariant() if hasattr(value, "toVariant") else value


def state(dp_sideways=False, error="", refused=()):
    return {"status": True, "backend": "hyprland", "streams": 0, "applies": False, "grace_seconds": 120,
            "error": error, "others_off": True,
            "monitors": [{"id": "eDP-2", "name": "<b>Samsung</b>", "on": True, "primary": True, "mode": HZ240,
                          "modes": [HZ240, HZ60], "changed": False, "sideways": False, "any_mode": True,
                          "refused": [{"width": w, "height": h} for w, h in refused],
                          "scale": 1.25, "scales": [1, 1.25, 1.6, 2, 2.5]},
                         {"id": "DP-1", "name": "Dell", "on": True, "primary": False, "mode": UHD,
                          "modes": [UHD, QHD], "changed": False, "sideways": dp_sideways, "scale": 1, "scales": []}]}


class Words(unittest.TestCase):
    def test_why(self):
        self.assertEqual(host_monitors.why("PC", 200, state()), "")
        self.assertEqual(host_monitors.why("PC", 0, None), "Couldn't reach PC")
        self.assertIn("Sign in to PC again", host_monitors.why("PC", 401, None))
        self.assertEqual(host_monitors.why("PC", 404, None), host_monitors.NO_BUILD)
        self.assertEqual(host_monitors.why("PC", 400, {"status": False, "error": "that would leave no display on"}),
                         "that would leave no display on")
        self.assertEqual(host_monitors.why("PC", 500, "<html>"), "PC answered 500")

    def test_scale_arg(self):
        self.assertEqual(host_monitors.scale_arg(1.25), "1.25")
        self.assertEqual(host_monitors.scale_arg(2), "2")
        self.assertEqual(host_monitors.scale_arg(None), "own")


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

    def test_refresh_lists_the_monitors(self):
        self.b.hostMonitors.refresh("PC")
        got = self.answer()
        self.assertEqual((got["note"], len(got["monitors"])), ("", 2))
        self.assertEqual([m["largest"] for m in got["monitors"]], [[2560, 1600], [2160, 3840]],
                         "the most each takes, as its stream is (DP-1 is turned a quarter)")
        self.assertEqual([m["sizes"] for m in got["monitors"]], [[[2560, 1600]], [[2160, 3840], [1440, 2560]]])
        self.assertEqual(self.sent, [(ADDRESS, "GET", host_monitors.ft_hostmonitors.PATH, None)])
        self.assertEqual(self.ran, [], "a panel's stream size is the user's: the monitor follows it, not the other way")
        self.assertEqual(self.b.hostMonitors.supported, {"PC": True})

    def test_the_pcs_last_error_is_its_note(self):
        self.reply = (200, state(error="DP-1 refused the mode"))
        self.b.hostMonitors.refresh("PC")
        self.assertEqual(self.answer()["note"], "DP-1 refused the mode")

    def test_an_old_build_is_known_and_an_old_sign_in_is_not(self):
        self.reply = (401, None)
        self.b.hostMonitors.refresh("PC")
        got = self.answer()
        self.assertIn("Sign in to PC again", got["note"])
        self.assertTrue(got["stale"], "the page says to sign in again")
        self.assertEqual(self.b.hostMonitors.supported, {}, "it may well can, once signed in again")
        self.reply = (404, None)
        self.b.hostMonitors.refresh("PC")
        got = self.answer()
        self.assertEqual(got["note"], host_monitors.NO_BUILD)
        self.assertFalse(got["stale"])
        self.assertEqual(self.b.hostMonitors.supported, {"PC": False})
        self.assertEqual(self.ran, [], "no answer: the panels are left as they are")

    def test_the_switch_runs_ft_layout_then_asks_again(self):
        self.b.hostMonitors.setControl("PC", True)
        self.b.hostMonitors.setControl("PC", False)
        self.assertEqual(self.ran, [("remote", "host", "PC", "monitors=on"), ("remote", "host", "PC", "monitors=off")])
        self.thens[0]()
        self.answer()
        self.assertEqual(self.sent[-1][1], "GET")

    def test_a_scale_pick_saves_only_the_scale(self):
        self.b.hostMonitors.pickScale("PC", "pc-edp", 1.6)
        self.b.hostMonitors.pickScale("PC", "pc-edp", None)
        self.assertEqual(self.ran, [("remote", "set", "pc-edp", "scale=1.6"), ("remote", "set", "pc-edp", "scale=own")],
                         "no stream size: a scale changes the PC's desktop, not the stream")
        self.assertTrue(all(self.thens[-2:]), "each asks the PC again after")

    def test_a_new_stream_size_asks_the_pc_again_for_its_note(self):
        self.b.setStream("pc-edp", 2560, 1200, 60, 0)
        self.assertEqual(self.ran[-1][:4], ("remote", "set", "pc-edp", "size=2560x1200"))
        self.thens[-1]()
        self.answer()
        self.assertEqual(self.sent[-1][1], "GET", "a size its monitor refused is the PC's note")
        self.b.setStream("pc-virtual", 1920, 1200, 60, 0)
        self.thens[-1]()
        set_hosts(pc(monitors=False))
        self.b.setStream("pc-edp", 2560, 1600, 60, 0)
        self.thens[-1]()
        spin(self.app, seconds=0.1)
        self.assertEqual(len(self.sent), 1, "a virtual display, or the switch off: no monitor follows it")

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
        olds = (host_monitors.HostMonitors.refresh, host_monitors.HostMonitors.pickScale)
        host_monitors.HostMonitors.refresh = lambda s, name: self.asked.append(name)
        host_monitors.HostMonitors.pickScale = lambda s, name, display_id, scale: self.picked.append(
            ("scale", name, display_id, scale))
        self.addCleanup(setattr, host_monitors.HostMonitors, "refresh", olds[0])
        self.addCleanup(setattr, host_monitors.HostMonitors, "pickScale", olds[1])
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

    def boxes(self, name="monitorScale"):
        """Monitor id -> its display's scale picker (the virtual display's has no monitor)."""
        pickers = [(js(b.parentItem().property("monitor")), b) for b in self.found(name)]
        return {m["id"]: b for m, b in pickers if m}

    def part(self, display, name):
        """The item called `name` in a display's settings."""
        return next(i for d in self.found(prop="ownSize") if js(d.property("modelData"))["id"] == display
                    for i in items(d) if i.objectName() == name)

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
        self.assertEqual(switch().property("text"), "Let Frametop control PC's displays")
        self.assertTrue(self.found("ownMonitorsHelp")[0].property("text").startswith(
            "Lets Frametop turn off or resize PC's displays while connected:"))
        self.b.hostMonitors._done("PC", 404, None)
        spin(self.app, seconds=0.1)
        self.assertFalse(switch().property("visible"), "an older build can't")
        self.assertEqual(self.bad_warnings(), [])

    def test_a_sign_in_from_before_says_to_sign_in_again(self):
        hint = lambda: self.found("ownMonitorsSignIn")[0]  # noqa: E731
        self.assertFalse(hint().property("visible"), "not known yet")
        self.b.hostMonitors._done("PC", 403, {"status": False, "error": "Forbidden"})
        spin(self.app, seconds=0.1)
        self.assertTrue(hint().property("visible"))
        self.assertFalse(self.found("ownMonitors")[0].property("visible"), "no switch until it can")
        self.assertIn("sign in again", hint().property("text"))
        self.answered()
        self.assertFalse(hint().property("visible"), "signed in again: the switch instead")
        self.assertTrue(self.found("ownMonitors")[0].property("visible"))
        self.assertEqual(self.bad_warnings(), [])

    def test_the_switch_sits_right_above_the_displays_it_acts_on(self):
        self.answered()
        y = lambda name: self.found(name)[0].mapToItem(self.root.contentItem(), 0, 0).y()  # noqa: E731
        self.assertGreater(y("ownMonitors"), y("shareClipboard"), "after the host's other switches")
        self.assertLess(y("ownMonitors"), min(d.mapToItem(self.root.contentItem(), 0, 0).y()
                                              for d in self.found(prop="ownSize")))

    def test_a_monitor_keeps_its_size_boxes_and_says_what_the_size_does(self):
        self.answered()
        self.assertEqual(self.own_size(), [("pc-dp", True), ("pc-edp", True), ("pc-virtual", True)])
        helps = lambda: {js(d.property("modelData"))["id"]: next(  # noqa: E731
            h.property("text") for h in items(d) if h.objectName() == "streamHelp") for d in self.found(prop="ownSize")}
        self.assertIn('turn on "Let Frametop control PC\'s displays" above', helps()["pc-edp"],
                      "off: a size only crops or fits, and the page says where to change that")
        self.switch_on()
        self.answered()
        self.assertEqual(self.own_size(), [("pc-dp", True), ("pc-edp", True), ("pc-virtual", True)],
                         "on: the size boxes set the monitor")
        self.assertIn("PC's monitor switches to this size while it streams", helps()["pc-edp"])
        self.assertEqual(self.bad_warnings(), [])

    def test_the_size_boxes_stop_at_the_monitors_largest_size(self):
        tops = lambda: {js(d.property("modelData"))["id"]: tuple(  # noqa: E731
            next(b.property("to") for b in items(d) if b.objectName() == name) for name in ("streamWidth", "streamHeight"))
            for d in self.found(prop="ownSize")}
        self.answered()
        self.assertEqual(set(tops().values()), {(7680, 4320)}, "the switch off: a size only crops or fits")
        self.switch_on()
        self.answered()
        self.assertEqual(tops(), {"pc-edp": (2560, 1600), "pc-dp": (3840, 2160), "pc-virtual": (7680, 4320)},
                         "a bigger size than its panel has wouldn't take")
        help_of = lambda: next(  # noqa: E731
            h.property("text") for d in self.found(prop="ownSize") if js(d.property("modelData"))["id"] == "pc-edp"
            for h in items(d) if h.objectName() == "streamHelp")
        self.assertNotIn("Its largest size", help_of(), "its sizes are in the list")
        self.part("pc-edp", "streamCustom").parentItem().customPicked.emit(True)
        spin(self.app, seconds=0.1)
        self.assertIn("Its largest size is 2560 × 1600.", help_of())
        self.assertEqual(self.bad_warnings(), [])

    def test_a_followed_monitor_picks_its_stream_size_from_the_sizes_it_lists(self):
        shown = lambda display, name: self.part(display, name).property("visible")  # noqa: E731
        self.answered()
        for d in self.found(prop="ownSize"):
            d.setProperty("expanded", True)
        spin(self.app, seconds=0.1)
        self.assertEqual([shown("pc-dp", n) for n in ("streamSize", "streamWidth", "streamCustom", "streamCustomWarning")],
                         [False, True, False, False], "the switch off: a size only crops or fits, so any size")
        self.switch_on()
        self.answered()
        for d in self.found(prop="ownSize"):
            d.setProperty("expanded", True)
        spin(self.app, seconds=0.1)
        self.assertEqual([shown("pc-dp", n) for n in ("streamSize", "streamWidth", "streamCustom", "streamCustomWarning")],
                         [True, False, True, False])
        self.assertEqual([shown("pc-virtual", n) for n in ("streamSize", "streamWidth", "streamCustom")],
                         [False, True, False], "a virtual display is made in any size")
        box = self.part("pc-dp", "streamSize")
        self.assertEqual(js(box.property("model")), ["3840 × 2160", "2560 × 1440"])
        self.assertEqual(box.property("currentIndex"), 0, "the panel's size")
        picker = box.parentItem()
        self.assertFalse(picker.property("changed"))
        box.setProperty("currentIndex", 1)
        self.assertEqual((picker.property("pickedWidth"), picker.property("pickedHeight"), picker.property("changed")),
                         (2560, 1440, True), "what Apply sends")

    def test_custom_size_shows_the_boxes_with_a_warning_and_stays_on(self):
        shown = lambda name: self.part("pc-dp", name).property("visible")  # noqa: E731
        self.switch_on()
        self.answered()
        self.part("pc-dp", "streamSize").parentItem().customPicked.emit(True)
        for d in self.found(prop="ownSize"):
            d.setProperty("expanded", True)
        spin(self.app, seconds=0.1)
        self.assertEqual([shown(n) for n in ("streamSize", "streamWidth", "streamCustom", "streamCustomWarning")],
                         [False, True, True, True])
        self.assertTrue(self.part("pc-dp", "streamCustom").property("checked"))
        self.assertEqual(self.part("pc-dp", "streamCustomWarning").property("text"),
                         "A custom size may not work: it depends on the sizes PC's monitor accepts. If the monitor "
                         "doesn't take it, it stays as it is and the stream crops or fits it.")
        self.b.changed.emit()  # the list is made again (Apply)
        spin(self.app, seconds=0.1)
        self.assertTrue(shown("streamWidth"), "still custom")
        self.part("pc-dp", "streamSize").parentItem().customPicked.emit(False)
        spin(self.app, seconds=0.1)
        self.assertEqual([shown(n) for n in ("streamSize", "streamWidth")], [True, False])
        self.assertEqual(self.bad_warnings(), [])

    def test_a_size_its_monitor_doesnt_list_starts_as_custom(self):
        host = pc()
        host["displays"][0]["size"] = [2560, 1200]  # a size Hyprland makes a mode for
        set_hosts(host)
        self.b.changed.emit()
        self.answered()
        for d in self.found(prop="ownSize"):
            d.setProperty("expanded", True)
        spin(self.app, seconds=0.1)
        self.assertTrue(self.part("pc-edp", "streamWidth").property("visible"))
        self.assertTrue(self.part("pc-edp", "streamCustom").property("checked"))
        picker = self.part("pc-edp", "streamSize").parentItem()
        picker.customPicked.emit(False)
        spin(self.app, seconds=0.1)
        box = self.part("pc-edp", "streamSize")
        self.assertEqual((box.property("currentIndex"), box.property("displayText")), (-1, "Pick a size"))
        self.assertFalse(picker.property("changed"), "nothing to apply until a size is picked")

    def test_a_size_its_monitor_refused_is_said_under_the_size_boxes(self):
        helps = lambda: {js(d.property("modelData"))["id"]: next(  # noqa: E731
            h.property("text") for h in items(d) if h.objectName() == "streamHelp") for d in self.found(prop="ownSize")}
        host = pc()
        host["displays"][0]["size"] = [2560, 1200]  # a size Hyprland makes a mode for
        set_hosts(host)
        self.b.changed.emit()
        self.answered()
        self.assertNotIn("can't be", helps()["pc-edp"])
        self.answered(state(error="Samsung can't be 2560x1200", refused=[(2560, 1200)]))
        self.assertEqual(helps()["pc-edp"], "PC's monitor can't be 2560 × 1200, so the stream crops or fits it as it is."
                                            " Sizes it has: 2560 × 1600.")
        self.assertNotIn("can't be", helps()["pc-dp"], "another monitor's panel")
        self.assertEqual(self.bad_warnings(), [])

    def test_the_scale_picker_shows_where_the_pc_can_set_it(self):
        self.switch_on()
        self.answered()
        for disp in self.found(prop="ownSize"):
            disp.setProperty("expanded", True)  # its settings open
        spin(self.app, seconds=0.1)
        boxes = self.boxes("monitorScale")
        texts = lambda box: [c["text"] for c in js(box.property("entries"))]  # noqa: E731
        self.assertEqual(texts(boxes["eDP-2"]), [
            "Its own scale (125%)", "100%", "125%", "150% (160% on this monitor)", "175% (160% on this monitor)",
            "200%", "250%"], "125, 150, 175 and 200 always; one it doesn't list says what the PC fits it to")
        self.assertFalse(boxes["DP-1"].parentItem().property("visible"), "it lists none (a Windows PC)")
        self.assertTrue(boxes["eDP-2"].parentItem().property("visible"))
        boxes["eDP-2"].parentItem().choose(3)
        boxes["eDP-2"].parentItem().choose(0)  # already its own: nothing to send
        self.assertEqual(self.picked, [("scale", "PC", "pc-edp", 1.5)])
        self.assertEqual(self.bad_warnings(), [])

    def test_a_picked_scale_shows_even_if_not_listed(self):
        hosts = pc()
        hosts["displays"][0]["scale"] = 1.5
        set_hosts(hosts)
        self.b.changed.emit()
        spin(self.app, seconds=0.1)
        texts = lambda: [c["text"] for c in js(self.boxes("monitorScale")["eDP-2"].property("entries"))]  # noqa: E731
        self.answered()
        entries = texts()
        self.assertNotIn("160%", entries, "it stands in for 150% and 175%")
        self.assertEqual(entries[0], "Its own scale (125%)", "not applied yet (no stream): its scale is its own")
        self.assertEqual(entries[self.boxes("monitorScale")["eDP-2"].property("currentIndex")], "150% (160% on this monitor)")
        applied = state()
        applied["monitors"][0].update(scale=1.5, changed=True, own_scale=1.25)
        self.answered(applied)
        self.assertEqual(texts()[0], "Its own scale (125%)", "applied: the one it goes back to, from the PC")
        del applied["monitors"][0]["own_scale"]  # a PC from before own_scale
        self.answered(applied)
        self.assertEqual(texts()[0], "Its own scale", "its scale now isn't its own")

    def test_a_picked_scale_that_stands_in_for_another_still_shows(self):
        hosts = pc()
        hosts["displays"][0]["scale"] = 1.6
        set_hosts(hosts)
        self.b.changed.emit()
        self.answered()
        box = self.boxes("monitorScale")["eDP-2"]
        entries = [c["text"] for c in js(box.property("entries"))]
        self.assertEqual(entries[box.property("currentIndex")], "160%")
        listed = state()
        listed["monitors"][0]["scales"] = [1, 1.25, 1.5, 1.75, 2]  # a mode that takes all four
        self.answered(listed)
        box = self.boxes("monitorScale")["eDP-2"]
        self.assertEqual([c["text"] for c in js(box.property("entries"))],
                         ["Its own scale (125%)", "100%", "125%", "150%", "160% (150% on this monitor)", "175%", "200%"])

    def test_its_settings_stay_open_when_the_list_is_made_again(self):
        self.switch_on()
        self.answered()
        laptop = lambda: next(d for d in self.found(prop="ownSize") if d.property("modelData")["id"] == "pc-edp")  # noqa: E731
        laptop().setProperty("expanded", True)
        hosts = pc()
        hosts["displays"][0]["scale"] = 1.6  # a scale pick (or Apply) saves the layout
        set_hosts(hosts)
        self.b.changed.emit()
        spin(self.app, seconds=0.1)
        self.assertTrue(laptop().property("expanded"), "still open")
        others = [d.property("expanded") for d in self.found(prop="ownSize") if d.property("modelData")["id"] != "pc-edp"]
        self.assertEqual(others, [False, False])

    def test_the_pcs_note_shows_with_the_switch_on(self):
        self.switch_on()
        self.answered(state(error="DP-1 refused the mode"))
        note = self.found("ownMonitorsNote")[0]
        self.assertTrue(note.property("visible"))
        self.assertEqual(note.property("text"), "DP-1 refused the mode")


if __name__ == "__main__":
    unittest.main()
