"""A remote display's own settings per route (ft-layout remote set/list), offline: no headset,
ft-screens, host, or user config is touched."""
import contextlib
import copy
import importlib.util
import io
import pathlib
import unittest


def load_layout_module():
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("route_settings_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeScreens:
    """ft-screens' control socket: records what it's asked; its remote screens are `running`."""
    def __init__(self, running):
        self.running = running
        self.asked = []

    def ask(self, text):
        self.asked.append(text)
        if text == "remotes":
            return "ok " + str(len(self.running)) + "".join(f" {n}:client:{s}" for n, s in self.running.items())
        return "ok"


class Harness:
    def __init__(self, running=None, off=False, routes=None):
        self.m = load_layout_module()
        display = {"id": "mac-desktop", "client": "frametop-mac-desktop", "app": "Desktop", "label": "Mac Desktop",
                   "size": [2560, 1440], "fps": 60, "bitrate": 4000, "screen": 101, "metres": 2.8, "curve": 1.9}
        if off:
            display["off"] = True
        if routes is not None:
            display["routes"] = routes
        self.layout = {"hosts": [{"name": "Mac", "address": "mac.local", "direct": ["192.168.0.2"],
                                  "route": "auto", "displays": [display]}]}
        self.saved = []
        self.logs = []
        self.screens = FakeScreens({101: "live"} if running is None else running)
        self.m.load_layout = lambda: copy.deepcopy(self.layout)
        self.m.save_layout = lambda layout: self.saved.append(copy.deepcopy(layout))
        self.m.screens_socket = lambda: self.screens
        self.m.log = lambda *args: self.logs.append(" ".join(map(str, args)))

    def display(self):
        return self.saved[-1]["hosts"][0]["displays"][0]

    def starts(self):
        return [a for a in self.screens.asked if a.startswith("remote 101 ")]


class RouteSettingsTest(unittest.TestCase):
    def test_each_route_keeps_its_own_settings(self):
        h = Harness()
        h.m.remote_command(["set", "mac-desktop", "direct.bitrate=15000", "network.bitrate=4000"])
        d = h.display()
        self.assertEqual(d["routes"], {"direct": {"bitrate": 15000}, "network": {"bitrate": 4000}})
        self.assertEqual((d["fps"], d["bitrate"]), (60, 4000))  # the display's own stay
        self.assertEqual(d["curve"], 1.9)  # and so does everything else

    def test_a_route_change_starts_the_running_stream_over(self):
        h = Harness()
        h.m.remote_command(["set", "mac-desktop", "direct.bitrate=15000"])
        starts = h.starts()
        self.assertEqual(starts[0], "remote 101 stop")
        self.assertTrue(starts[1].startswith("remote 101 start frametop-mac-desktop mac.local Desktop "
                                             "2560x1440 60 4000 "), starts[1])
        self.assertEqual(len(starts), 2)

    def test_the_displays_own_settings_still_leave_restarting_to_ft_screens(self):
        h = Harness()
        h.m.remote_command(["set", "mac-desktop", "bitrate=6000"])
        starts = h.starts()
        self.assertEqual(len(starts), 1)
        self.assertTrue(starts[0].startswith("remote 101 start ") and " 60 6000 " in starts[0], starts[0])

    def test_a_stream_that_isnt_running_just_starts(self):
        h = Harness(running={})
        h.m.remote_command(["set", "mac-desktop", "direct.fps=30"])
        starts = h.starts()
        self.assertEqual(len(starts), 1)
        self.assertTrue(starts[0].startswith("remote 101 start "))

    def test_a_disconnected_display_only_saves(self):
        h = Harness(off=True)
        h.m.remote_command(["set", "mac-desktop", "direct.bitrate=15000"])
        self.assertEqual(h.display()["routes"], {"direct": {"bitrate": 15000}})
        self.assertEqual(h.screens.asked, [])

    def test_empty_goes_back_to_the_displays_own(self):
        h = Harness(routes={"direct": {"fps": 30, "bitrate": 15000}, "network": {"bitrate": 4000}})
        h.m.remote_command(["set", "mac-desktop", "direct.bitrate="])
        self.assertEqual(h.display()["routes"], {"direct": {"fps": 30}, "network": {"bitrate": 4000}})
        h.layout = h.saved[-1]
        h.m.remote_command(["set", "mac-desktop", "direct.fps=", "network.bitrate="])
        self.assertNotIn("routes", h.display())

    def test_bad_values_are_refused_and_nothing_is_saved(self):
        for kv in ("direct.fps=0", "direct.fps=241", "direct.bitrate=500001", "direct.bitrate=-1",
                   "direct.bitrate=fast", "wifi.bitrate=4000", "direct.size=1920x1080", "direct=15000",
                   ".bitrate=15000"):
            with self.subTest(kv=kv):
                h = Harness()
                with self.assertRaises(RuntimeError):
                    h.m.remote_command(["set", "mac-desktop", kv])
                self.assertEqual(h.saved, [])
                self.assertEqual(h.screens.asked, [])

    def test_the_limits_themselves_are_accepted(self):
        h = Harness()
        h.m.remote_command(["set", "mac-desktop", "direct.fps=1", "direct.bitrate=0", "network.fps=240",
                            "network.bitrate=500000"])
        self.assertEqual(h.display()["routes"], {"direct": {"fps": 1, "bitrate": 0},
                                                 "network": {"fps": 240, "bitrate": 500000}})

    def test_a_broken_routes_entry_is_replaced(self):
        h = Harness(routes="fast")
        h.m.remote_command(["set", "mac-desktop", "direct.bitrate=15000"])
        self.assertEqual(h.display()["routes"], {"direct": {"bitrate": 15000}})

    def test_list_shows_each_routes_settings(self):
        h = Harness(routes={"direct": {"bitrate": 15000}, "network": {"fps": 30}})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            h.m.remote_command(["list"])
        line = out.getvalue().strip()
        self.assertIn("2560x1440@60 4000 kbit/s, direct @60 15000 kbit/s, network @30 4000 kbit/s live route auto",
                      line)
        h = Harness(routes={"direct": {"bitrate": 0}})
        h.layout["hosts"][0]["displays"][0]["bitrate"] = 0
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            h.m.remote_command(["list"])
        self.assertIn("2560x1440@60 default bitrate, direct @60 default bitrate live", out.getvalue())

    def test_list_preserves_upstream_host_input_and_microphone_flags(self):
        h = Harness(routes={"direct": {"fps": 30}})
        h.layout["hosts"][0].update(mic=True, input=True)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            h.m.remote_command(["list"])
        self.assertIn("direct @30 4000 kbit/s live route auto mic input", out.getvalue())

    def test_route_changes_work_while_streaming_without_the_desktop(self):
        h = Harness()
        h.m.streams_only = lambda: True
        h.m.nested_env = lambda: self.fail("route changes must not need KWin")
        h.m.remote_command(["set", "mac-desktop", "network.fps=30", "network.bitrate=2000"])
        self.assertEqual(h.display()["routes"], {"network": {"fps": 30, "bitrate": 2000}})
        self.assertEqual(h.starts()[0], "remote 101 stop")
        self.assertTrue(h.starts()[1].startswith("remote 101 start "))

    def test_list_without_routes_is_as_before_plus_the_bitrate(self):
        h = Harness()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            h.m.remote_command(["list"])
        self.assertEqual(out.getvalue().strip(), "101 mac-desktop Mac mac.local Desktop "
                                                 "2560x1440@60 4000 kbit/s live route auto  Mac Desktop")


if __name__ == "__main__":
    unittest.main()
