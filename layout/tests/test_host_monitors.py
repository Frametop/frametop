"""A host's monitors following its display panels (ft_hostmonitors, and ft-layout's hooks): the
plan made from the panels, what is sent when, and remote host monitors= / remote set mode=. The
PC is a stand-in for ft_hostapi.request; no headset, no ft-screens.

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


def pc(**over):
    host = {"name": "PC", "address": "192.0.2.7", "monitors": True, "displays": [
        {"id": "pc-edp", "client": "c1", "app": "display:eDP-2", "size": [2560, 1600], "mode": HZ60},
        {"id": "pc-dp", "client": "c2", "app": "display:DP-1", "size": [3840, 2160]},
        {"id": "pc-virtual", "client": "c3", "app": "monitor", "size": [1920, 1080]},
        {"id": "pc-win", "client": "c4", "app": "display:HDMI-A-1", "window": True, "size": [800, 600]}]}
    host.update(over)
    return host


def listed(*ids):
    known = {"eDP-2": {"id": "eDP-2", "on": True, "mode": HZ240, "modes": [HZ240, HZ60]},
             "DP-1": {"id": "DP-1", "on": True, "mode": UHD, "modes": [UHD]},
             "HDMI-A-1": {"id": "HDMI-A-1", "on": True, "mode": UHD, "modes": [UHD]}}
    return {i: known[i] for i in ids or known}


class Plan(unittest.TestCase):
    def test_each_display_panel_names_its_monitor(self):
        self.assertEqual(hm.wishes(pc()), {"eDP-2": {"on": True, "mode": HZ60}, "DP-1": {"on": True}},
                         "the virtual display and the window panel aren't monitors")

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
                         {"monitors": {"eDP-2": {"on": True, "mode": HZ60}, "DP-1": {"on": True}}, "others_off": True},
                         "HDMI-A-1 has no display panel here: others_off turns it off")

    def test_an_unplugged_monitor_or_an_unlisted_mode_leaves_the_rest(self):
        host = pc()
        host["displays"][0]["mode"] = {"width": 1, "height": 1, "refresh_mhz": 1}
        self.assertEqual(hm.plan_body(host, listed("eDP-2")),
                         {"monitors": {"eDP-2": {"on": True}}, "others_off": True})

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
        self.synced, self.released = [], []
        for name, fn in (("sync", lambda h, log, request=None: self.synced.append(h["name"])),
                         ("release", lambda h, log, request=None: self.released.append(h["name"]))):
            old = getattr(hm, name)
            setattr(hm, name, fn)
            self.addCleanup(setattr, hm, name, old)
        old = hm.sync_all
        hm.sync_all = lambda hosts, log, request=None: self.synced.extend(
            h["name"] for h in {id(h): h for h in hosts}.values() if hm.controls(h))
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
        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "pc-dp", "mode=1920x1080"])

    def test_connect_disconnect_and_remove_send_the_plan(self):
        self.m.remote_command(["host", "PC", "monitors=on"])
        self.m.remote_command(["disconnect", "pc-edp"])
        self.assertTrue(self.host()["displays"][0]["off"])
        self.m.remote_command(["connect", "pc-edp"])
        old = self.m.run_stream
        self.m.run_stream = lambda *a, **kw: (0, "unpaired", "")
        self.addCleanup(setattr, self.m, "run_stream", old)
        self.m.remote_command(["remove", "pc-dp"])
        self.assertEqual(self.synced, ["PC"] * 4)

    def test_without_the_switch_nothing_is_sent(self):
        self.m.remote_command(["disconnect", "pc-edp"])
        self.m.remote_command(["set", "pc-dp", "fps=30"])
        self.assertEqual(self.synced, [])


if __name__ == "__main__":
    unittest.main()
