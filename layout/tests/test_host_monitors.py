"""A host's monitors following its display panels (ft_hostmonitors, and ft-layout's hooks): the
plan made from the panels (their stream sizes and scales), what is sent when, and remote host
monitors= / remote set size=, mode= and scale=. The PC is a stand-in for ft_hostapi.request; no headset, no ft-screens.

  python3 layout/tests/test_host_monitors.py
"""
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

LAYOUT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LAYOUT.parent / "conf"))
sys.path.insert(0, str(LAYOUT))
import ft_hostmonitors as hm  # noqa: E402

HZ240 = {"width": 2560, "height": 1600, "refresh_mhz": 240000}
HZ60 = {"width": 2560, "height": 1600, "refresh_mhz": 60000}
UHD = {"width": 3840, "height": 2160, "refresh_mhz": 60000}


def size(w, h):
    return {"width": w, "height": h, "refresh_mhz": 0}


def pc(**over):
    host = {"name": "PC", "address": "192.0.2.7", "monitors": True, "displays": [
        {"id": "pc-edp", "client": "c1", "app": "display:eDP-2", "size": [2560, 1600], "mode": HZ60},
        {"id": "pc-dp", "client": "c2", "app": "display:DP-1", "size": [3840, 2160]},
        {"id": "pc-virtual", "client": "c3", "app": "monitor", "size": [1920, 1080]},
        {"id": "pc-win", "client": "c4", "app": "display:HDMI-A-1", "window": True, "size": [800, 600]}]}
    host.update(over)
    return host


def listed(*ids):
    # eDP-2 on Hyprland (any mode), DP-1 on Windows (only its own), HDMI-A-1 an older build (no sizes)
    known = {"eDP-2": {"id": "eDP-2", "on": True, "mode": HZ240, "modes": [HZ240, HZ60], "scales": [1, 1.25, 2],
                       "any_mode": True},
             "DP-1": {"id": "DP-1", "on": True, "mode": UHD, "modes": [UHD], "any_mode": False},
             "HDMI-A-1": {"id": "HDMI-A-1", "on": True, "mode": UHD, "modes": [UHD]}}
    return {i: known[i] for i in ids or known}


class Plan(unittest.TestCase):
    def test_each_display_panel_names_its_monitor(self):
        self.assertEqual(hm.wishes(pc()), {"eDP-2": {"on": True, "mode": HZ60},
                                           "DP-1": {"on": True, "mode": size(3840, 2160)}},
                         "the virtual display and the window panel aren't monitors; a panel without a "
                         "picked mode asks for its stream size")

    def test_a_disconnected_panel_turns_its_monitor_off(self):
        host = pc()
        host["displays"][0]["off"] = True
        self.assertEqual(hm.wishes(host)["eDP-2"], {"on": False})

    def test_two_panels_of_a_monitor_keep_it_on_while_either_is_connected(self):
        host = pc()
        host["displays"].append({"id": "pc-edp-2", "app": "display:eDP-2", "off": True})
        self.assertTrue(hm.wishes(host)["eDP-2"]["on"])
        host["displays"].insert(0, {"id": "pc-edp-0", "app": "display:eDP-2", "off": True})
        self.assertTrue(hm.wishes(host)["eDP-2"]["on"])

    def test_the_body_turns_off_the_others(self):
        self.assertEqual(hm.plan_body(pc(), listed()),
                         {"monitors": {"eDP-2": {"on": True, "mode": HZ60}, "DP-1": {"on": True, "mode": size(3840, 2160)}},
                          "others_off": True},
                         "HDMI-A-1 has no display panel here: others_off turns it off")

    def test_an_unplugged_monitor_or_an_unlisted_mode_leaves_the_rest(self):
        host = pc()
        host["displays"][1]["mode"] = {"width": 1, "height": 1, "refresh_mhz": 1}
        self.assertEqual(hm.plan_body(host, listed("DP-1")),
                         {"monitors": {"DP-1": {"on": True}}, "others_off": True})

    def test_a_stream_size_goes_where_the_pc_takes_it(self):
        host = pc()
        del host["displays"][0]["mode"]
        host["displays"][0]["size"] = [2560, 1200]
        host["displays"][1]["size"] = [1920, 1080]
        body = hm.plan_body(host, listed())["monitors"]
        self.assertEqual(body["eDP-2"]["mode"], size(2560, 1200), "Hyprland makes a mode of any size")
        self.assertNotIn("mode", body["DP-1"], "a size Windows doesn't list: the stream fits it as it is")
        host["displays"][1]["app"] = "display:HDMI-A-1"
        host["displays"][1]["size"] = [3840, 2160]
        self.assertNotIn("mode", hm.plan_body(host, listed())["monitors"]["HDMI-A-1"], "an older build takes no size")

    def test_a_monitors_largest_size_is_its_widest_and_tallest_mode(self):
        known = listed()
        self.assertEqual(hm.largest_size(known["eDP-2"]), (2560, 1600))
        wide = dict(known["DP-1"], modes=[UHD, {"width": 5120, "height": 1440, "refresh_mhz": 60000}])
        self.assertEqual(hm.largest_size(wide), (5120, 2160))
        self.assertEqual(hm.largest_size(dict(known["DP-1"], sideways=True)), (2160, 3840), "as its stream is")
        self.assertIsNone(hm.largest_size({"id": "DP-2", "modes": []}), "Windows lists none while it's off")
        self.assertIsNone(hm.largest_size({"id": "DP-2", "modes": [{"width": "wide"}]}))

    def test_a_sideways_monitor_is_asked_for_its_size_unrotated(self):
        host = pc()
        host["displays"][1]["size"] = [2160, 3840]
        known = listed()
        known["DP-1"]["sideways"] = True
        self.assertEqual(hm.plan_body(host, known)["monitors"]["DP-1"]["mode"], size(3840, 2160))

    def test_a_mode_the_pc_lists_within_ten_millihertz_is_kept(self):
        host = pc()
        host["displays"][0]["mode"] = dict(HZ240, refresh_mhz=239761)
        known = listed("eDP-2")
        known["eDP-2"]["modes"] = [dict(HZ240, refresh_mhz=239760)]
        self.assertEqual(hm.plan_body(host, known)["monitors"]["eDP-2"]["mode"]["refresh_mhz"], 239761)

    def test_no_connected_panel_sends_nothing(self):
        host = pc()
        for d in host["displays"]:
            d["off"] = True
        self.assertIsNone(hm.plan_body(host, listed()), "the PC's grace runs as it would")
        self.assertIsNone(hm.plan_body(pc(), listed("HDMI-A-1")), "only unplugged monitors have panels on")

    def test_a_picked_scale_goes_in_the_plan_where_the_pc_can_set_it(self):
        host = pc()
        host["displays"][0]["scale"] = 1.25
        host["displays"][1]["scale"] = 2
        self.assertEqual(hm.wishes(host)["eDP-2"], {"on": True, "mode": HZ60, "scale": 1.25})
        body = hm.plan_body(host, listed())
        self.assertEqual(body["monitors"]["eDP-2"], {"on": True, "mode": HZ60, "scale": 1.25})
        self.assertEqual(body["monitors"]["DP-1"], {"on": True, "mode": size(3840, 2160)},
                         "DP-1 lists no scales (Windows, an older build): the rest still takes")
        host["displays"][0]["off"] = True
        self.assertEqual(hm.wishes(host)["eDP-2"], {"on": False}, "off: no scale")

    def test_a_bad_picked_scale_is_its_own(self):
        for bad in ("1.5", True, 0, 0.2, 5, None):
            self.assertIsNone(hm.scale_of({"scale": bad}), bad)
        self.assertEqual(hm.scale_of({"scale": 2}), 2.0)

    def test_a_bad_picked_mode_is_its_own(self):
        self.assertIsNone(hm.mode_of({"mode": {"width": "x"}}))
        self.assertIsNone(hm.mode_of({"mode": {"width": 0, "height": 1, "refresh_mhz": 1}}))
        self.assertEqual(hm.mode_of({"mode": HZ60}), HZ60)


class Sending(unittest.TestCase):
    def setUp(self):
        self.sent, self.said = [], []
        self.replies = {"GET": (200, {"status": True, "monitors": list(listed().values())}),
                        "POST": (200, {"status": True})}

    def request(self, address, method, path, body=None, timeout=2.0, port=0):
        self.sent.append((address, method, path, body, port))
        return self.replies[method]

    def test_only_a_host_with_the_switch_on_is_asked(self):
        hm.sync(pc(monitors=False), self.said.append, self.request)
        hm.sync(pc(kind="sunshine"), self.said.append, self.request)
        self.assertEqual(self.sent, [])

    def test_its_question_outlasts_a_wifi_stall(self):
        waits = []
        hm.sync(pc(), self.said.append, lambda *a, timeout=2.0, **kw: (waits.append(timeout), self.request(*a, **kw))[1])
        self.assertGreaterEqual(min(waits), 10.0, "a 3 s question left a pick unsent on the Frame's Wi-Fi")

    def test_it_asks_then_sends_the_plan(self):
        hm.sync(pc(), self.said.append, self.request)
        self.assertEqual([(m, b) for _, m, _, b, _ in self.sent],
                         [("GET", None), ("POST", hm.plan_body(pc(), listed()))])
        self.assertEqual(self.said, [])

    def test_a_pc_that_cant_is_logged_and_nothing_else(self):
        self.replies["GET"] = (404, None)
        hm.sync(pc(), self.said.append, self.request)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.said, ["PC: monitors not changed (it answered 404)"])

    def test_a_refused_plan_says_why(self):
        self.replies["POST"] = (400, {"status": False, "error": "that would leave no display on"})
        hm.sync(pc(), self.said.append, self.request)
        self.assertEqual(self.said, ["PC: monitors not changed: that would leave no display on"])

    def test_a_size_its_monitor_refused_is_logged(self):
        said = "Samsung can't be 2560x1800: it stays 2560x1600"
        self.replies["POST"] = (200, {"status": True, "applies": True, "error": said})
        hm.sync(pc(), self.said.append, self.request)
        self.assertEqual(self.said, [f"PC: {said}"])
        self.replies["POST"] = (200, {"status": True, "applies": False, "error": said})
        hm.sync(pc(), self.said.append, self.request)
        self.assertEqual(len(self.said), 1, "not applied now: the error is an earlier change's")

    def test_it_never_raises(self):
        def broken(*a, **kw):
            raise OSError("no route")
        hm.sync(pc(), self.said.append, broken)
        hm.release(pc(), self.said.append, broken)
        self.assertEqual(len(self.said), 2)

    def test_the_port_is_its_displays(self):
        host = pc()
        host["displays"][1]["port"] = 49989
        hm.sync(host, self.said.append, self.request)
        self.assertEqual({p for *_, p in self.sent}, {49989})

    def test_the_port_is_a_display_panels_not_a_window_panels(self):
        host = pc()
        host["displays"].insert(0, host["displays"].pop(3))  # a window panel listed first
        host["displays"][0]["port"] = 49990  # its Frametop windows instance
        host["displays"][2]["port"] = 49989
        hm.sync(host, self.said.append, self.request)
        self.assertEqual({p for *_, p in self.sent}, {49989})

    def test_a_release_that_didnt_take_is_sent_again(self):
        owed = pc(monitors=False, monitors_release=True)
        self.replies["POST"] = (0, None)
        self.assertFalse(hm.release(owed, self.said.append, self.request))
        self.assertEqual(hm.sync_all([owed], self.said.append, self.request), set())
        self.replies["POST"] = (200, {"status": True})
        self.assertEqual(hm.sync_all([owed], self.said.append, self.request), {"PC"})
        self.assertEqual([(m, b) for _, m, _, b, _ in self.sent], [("POST", {"monitors": {}})] * 3)
        self.sent.clear()
        hm.sync_all([pc(monitors=True, monitors_release=True)], self.said.append, self.request)
        self.assertEqual([m for _, m, *_ in self.sent], ["GET", "POST"], "switched on again: it owes nothing")

    def test_release_sends_no_plan(self):
        hm.release(pc(monitors=False), self.said.append, self.request)
        self.assertEqual([(m, b) for _, m, _, b, _ in self.sent], [("POST", {"monitors": {}})])

    def test_sync_all_asks_each_controlled_host_once(self):
        a, b = pc(), pc(name="Other", address="192.0.2.8", monitors=False)
        hm.sync_all([a, a, b], self.said.append, self.request)
        self.assertEqual([(ad, m) for ad, m, *_ in self.sent], [("192.0.2.7", "GET"), ("192.0.2.7", "POST")])


def load_layout_module(tmp):
    spec = importlib.util.spec_from_file_location("host_monitors_layout", LAYOUT / "ft_layout.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class Layout(unittest.TestCase):
    """ft-layout remote host NAME monitors=, remote set ID mode=, and when the plan is sent."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")
        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": [pc(monitors=False)]})
        self.synced, self.released, self.release_takes = [], [], True
        self.meanwhile = lambda: None  # what else runs while a release waits for its PC
        for name, fn in (("sync", lambda h, log, request=None: self.synced.append(h["name"])),
                         ("release", lambda h, log, request=None: self.released.append(h["name"]) or self.meanwhile() or self.release_takes)):
            old = getattr(hm, name)
            setattr(hm, name, fn)
            self.addCleanup(setattr, hm, name, old)
        old = self.real_sync_all = hm.sync_all
        hm.sync_all = lambda hosts, log, request=None: self.synced.extend(
            h["name"] for h in {id(h): h for h in hosts}.values() if hm.controls(h)) or set()
        self.addCleanup(setattr, hm, "sync_all", old)

    def tearDown(self):
        self.tmp.cleanup()

    def host(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"][0]

    def test_the_switch_is_saved_and_sends_or_releases(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.assertIs(self.host()["monitors"], True)
        self.assertEqual(self.synced, ["PC"])
        self.m.remote_command(["host", "PC", "monitors=off"])
        self.assertIs(self.host()["monitors"], False)
        self.assertEqual(self.released, ["PC"])
        with self.assertRaises(RuntimeError):
            self.m.remote_command(["host", "PC", "monitors=maybe"])

    def test_a_release_that_didnt_take_is_kept_until_it_does(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.release_takes = False
        self.m.remote_command(["host", "PC", "monitors=off"])
        self.assertIs(self.host().get("monitors_release"), True)
        hm.sync_all = self.real_sync_all  # (its release is the stand-in)
        layout = self.m.load_layout()
        self.m.sync_monitors(layout["hosts"])
        self.assertIs(self.host().get("monitors_release"), True, "still not taken")
        self.release_takes = True
        self.m.sync_monitors(layout["hosts"])
        self.assertNotIn("monitors_release", self.host())
        self.assertNotIn("monitors_release", layout["hosts"][0], "nor in the caller's layout")
        self.assertEqual(self.released, ["PC"] * 3)
        self.release_takes = False
        self.m.remote_command(["host", "PC", "monitors=off"])
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.assertNotIn("monitors_release", self.host(), "switched on again: nothing owed")

    def test_a_change_saved_while_a_release_waits_is_kept(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.release_takes = False
        self.meanwhile = lambda: self.m.remote_command(["disconnect", "pc-edp"])
        self.m.remote_command(["host", "PC", "monitors=off"])
        self.assertIs(self.host().get("monitors_release"), True)
        self.assertTrue(self.host()["displays"][0].get("off"), "the disconnect saved meanwhile stays")

    def test_a_flag_that_cant_be_saved_is_logged(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.release_takes = False
        said = []
        old_save, old_log = self.m.save_layout, self.m.log
        self.m.log = said.append

        def locked(layout):
            if layout.get("hosts", [{}])[0].get("monitors_release"):
                raise self.m.LayoutFileError("locked")
            old_save(layout)
        self.m.save_layout = locked
        self.addCleanup(setattr, self.m, "save_layout", old_save)
        self.addCleanup(setattr, self.m, "log", old_log)
        self.m.remote_command(["host", "PC", "monitors=off"])  # doesn't raise
        self.assertIs(self.host()["monitors"], False)
        self.assertTrue(any("not saved" in line for line in said))

    def test_a_mode_is_saved_with_its_size(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.m.remote_command(["set", "pc-dp", "mode=1920x1080@60000"])
        d = self.host()["displays"][1]
        self.assertEqual((d["mode"], d["size"]), ({"width": 1920, "height": 1080, "refresh_mhz": 60000}, [1920, 1080]))
        self.m.remote_command(["set", "pc-dp", "mode=1920x1080@60000", "size=1080x1920"])
        self.assertEqual(self.host()["displays"][1]["size"], [1080, 1920], "turned a quarter: the stream is tall")
        self.m.remote_command(["set", "pc-dp", "mode=own"])
        self.assertNotIn("mode", self.host()["displays"][1])
        self.assertEqual(self.synced, ["PC"] * 4, "each change sends the plan")
        self.m.remote_command(["set", "pc-dp", "fps=30"])
        self.assertEqual(len(self.synced), 4, "the plan doesn't change with the frame rate")
        self.m.remote_command(["set", "pc-dp", "size=2560x1200"])
        self.assertEqual(len(self.synced), 5, "a stream size is its monitor's: the plan is sent")
        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "pc-dp", "mode=1920x1080"])

    def test_a_scale_is_saved_and_sent_without_restarting_the_stream(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.with_screens()
        self.synced.clear()
        self.m.remote_command(["set", "pc-edp", "scale=1.25"])
        self.assertEqual(self.host()["displays"][0]["scale"], 1.25)
        self.assertEqual(self.synced, ["PC"], "the plan is sent; no start: the stream goes on")
        self.m.remote_command(["set", "pc-edp", "scale=own"])
        self.assertNotIn("scale", self.host()["displays"][0])
        for bad in ("scale=5", "scale=0.1", "scale=big"):
            with self.assertRaises(RuntimeError):
                self.m.remote_command(["set", "pc-edp", bad])

    def with_screens(self):
        """ft-screens as a stand-in: what's started and stopped goes in self.synced, in order with
        the plans sent."""
        events = self.synced

        class Sock:
            def ask(self, line):
                events.append(line)
                return "ok"
        self.m.screens_socket = Sock
        self.m.remotes_running = lambda sock: {}
        self.m.start_remote = lambda sock, n, host, d: events.append(f"start {d['id']}") or "ok"
        self.m.remote_anchor = lambda sock, layout: ((0, 0, 0), 0)
        self.m.place_remotes = lambda *a, **kw: None

    def test_connect_sends_before_the_stream_and_disconnect_after(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.with_screens()
        self.synced.clear()
        self.m.remote_command(["disconnect", "pc-edp"])
        self.assertTrue(self.host()["displays"][0]["off"])
        self.assertEqual([e for e in self.synced if not e.startswith("remote ")], ["PC"])
        self.assertTrue(self.synced[0].startswith("remote ") and self.synced[0].endswith(" stop"),
                        "the stream stops before its monitor goes off")
        self.synced.clear()
        self.m.remote_command(["connect", "pc-edp"])
        self.assertEqual(self.synced, ["PC", "start pc-edp"], "its monitor comes back before the stream starts")

    def test_remove_sends_the_plan(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        old = self.m.run_stream
        self.m.run_stream = lambda *a, **kw: (0, "unpaired", "")
        self.addCleanup(setattr, self.m, "run_stream", old)
        self.synced.clear()
        self.m.remote_command(["remove", "pc-dp"])
        self.assertEqual(self.synced, ["PC"])

    def test_starting_streams_sends_for_the_hosts_started(self):
        other = pc(name="Other", address="192.0.2.8", monitors=True)
        for d in other["displays"]:
            d["id"] = "other-" + d["id"]
        self.m.save_layout({"hosts": [pc(monitors=True), other]})
        self.with_screens()
        self.m.start_remotes(self.m.screens_socket(), self.m.load_layout(), only={"other-pc-dp"})
        self.assertEqual(self.synced[0], "Other", "only the host of the display started")
        self.synced.clear()
        self.m.start_remotes(self.m.screens_socket(), self.m.load_layout())
        self.assertEqual(sorted(e for e in self.synced if not e.startswith("start ")), ["Other", "PC"])
        self.assertTrue(all(e.startswith("start ") for e in self.synced[2:]), "plans first, then the streams")

    def test_without_the_switch_nothing_is_sent(self):
        self.m.remote_command(["disconnect", "pc-edp"])
        self.m.remote_command(["set", "pc-dp", "fps=30"])
        self.assertEqual(self.synced, [])


if __name__ == "__main__":
    unittest.main()
