"""A PC app window's scale, without a headset or a PC: where it starts, the panel size it takes, and
`ft-layout window scale`."""
import fcntl
import pathlib
import re
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent), str(HERE.parents[1] / "conf")]
import test_windows  # noqa: E402
import ft_windowscale as S  # noqa: E402

LOW, HIGH = (320, 240), (3840, 2160)


class Fit(unittest.TestCase):
    def fits(self, size, scale):
        return S.fitted(size, scale, LOW, HIGH)

    def test_a_size_that_takes_it_stays(self):
        self.assertEqual(self.fits((1280, 800), 1.6), ((1280, 800), 1.6))
        self.assertEqual(self.fits((1001 // 2 * 2, 600), 1.0), ((1000, 600), 1.0))

    def test_every_step_fits_any_size_within_a_few_pixels(self):
        for size in ((1280, 800), (1634, 1254), (322, 242), (3838, 2158), (1000, 600)):
            for step in S.STEPS:
                (w, h), scale = self.fits(size, step)
                self.assertEqual(scale, step)
                for v, was, low, high in ((w, size[0], *LOW[:1], *HIGH[:1]), (h, size[1], LOW[1], HIGH[1])):
                    self.assertLessEqual(abs(v - was), S.NUDGE)
                    self.assertTrue(v % 2 == 0 and low <= v <= high and v / scale == round(v / scale), (size, step, v))

    def test_a_scale_in_hyprlands_own_steps(self):
        (w, h), scale = self.fits((1280, 800), 1.666667)   # a display's 167%
        self.assertEqual((w % 10, h % 10, round(scale * 3, 9)), (0, 0, 5))

    def test_a_scale_no_size_takes_goes_to_a_step(self):
        self.assertEqual(self.fits((1280, 800), 1.01)[1], 1.0)   # (121/120 at most)
        self.assertEqual(self.fits((1278, 802), 1.758333)[1], 1.75)   # 211/120


    def test_a_size_past_the_limits_stays_there(self):
        for size in ((5120, 1440), (300, 220)):
            (w, h), scale = self.fits(size, 1.5)
            self.assertEqual(scale, 1.5)
            self.assertLessEqual(max(abs(w - size[0]), abs(h - size[1])), S.NUDGE)

    def test_the_menus_steps_are_these(self):
        text = (HERE.parents[1] / "screens" / "appscale.h").read_text()
        percents = re.search(r"kPercents\[\] = \{([^}]*)\}", text).group(1)
        self.assertEqual([int(p) / 100 for p in percents.split(",")], list(S.STEPS))
        self.assertEqual(int(re.search(r"kNudge = (\d+)", text).group(1)), S.NUDGE)

    def test_what_ft_screens_hears(self):
        self.assertEqual(S.told(101, {"scale": 1.5, "size": [1278, 798]}), "remote 101 scale 1.5 1278x798")
        for bad in (None, "x", 9, True):   # (a layout file edited by hand)
            self.assertEqual(S.told(101, {"scale": bad, "size": [1278, 798]}), "remote 101 scale 0")


class Start(unittest.TestCase):
    def host(self, *displays, **windows):
        return {"name": "pc", "windows": {"port": 1, **windows}, "displays": list(displays)}

    def test_never_picked_is_100(self):
        self.assertEqual(S.start(self.host({"app": "display:DP-1"}), "a"), 1.0)
        self.assertEqual(S.start({"name": "pc"}, "a"), 1.0)

    def test_the_displays_scale_a_connected_ones_first(self):
        off, on = {"app": "display:DP-1", "scale": 2.0, "off": True}, {"app": "display:DP-2", "scale": 1.6}
        self.assertEqual(S.start(self.host(off, on), "a"), 1.6)
        self.assertEqual(S.start(self.host(off), "a"), 2.0)
        self.assertEqual(S.start(self.host({"app": "window:0x1", "window": True, "scale": 2.5}), "a"), 1.0)  # another app's

    def test_the_apps_own_first(self):
        h = self.host({"app": "display:DP-1", "scale": 1.6}, scales={"a": 1.25, "b": "x", "c": True})
        self.assertEqual([S.start(h, k) for k in ("a", "b", "c", "")], [1.25, 1.6, 1.6, 1.6])

    def test_kept_per_app_the_oldest_goes(self):
        h = self.host()
        for k in range(S.REMEMBERED + 1):
            S.keep(h, f"app{k}", 1.5)
        S.keep(h, "app1", 2.0)
        self.assertEqual(len(h["windows"]["scales"]), S.REMEMBERED)
        self.assertNotIn("app0", h["windows"]["scales"])
        self.assertEqual(list(h["windows"]["scales"])[-1], "app1")
        S.keep(h, "", 2.0)   # (a window with no app: nothing to keep it by)
        self.assertEqual(len(h["windows"]["scales"]), S.REMEMBERED)

    def test_nothing_kept_for_a_pc_without_one(self):
        for windows in (None, "on", 1):
            h = {"name": "pc", "windows": windows}
            S.keep(h, "a", 2.0)
            self.assertEqual(h["windows"], windows)


class Screens:
    def __init__(self):
        self.asked = []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        return "ok"


class Command(test_windows.WindowTest):
    """`ft-layout window scale`, on the fakes of test_windows."""
    def setUp(self):
        super().setUp()
        self.m.log = lambda *_: None
        self.list = dict(test_windows.WINDOWS, scaling=True)
        self.answers["/api/frametop/windows"] = (200, self.list)

    def opened(self, *args):
        self.run_window("open", "linuxpc", "0x55aa10", *args)
        self.screens, self.no_screens = Screens(), self.m.screens_socket
        self.m.screens_socket = lambda: self.screens
        self.api.clear()
        return str(self.displays()[-1]["screen"])

    def test_a_pc_that_takes_none_has_none(self):
        self.answers["/api/frametop/windows"] = (200, test_windows.WINDOWS)
        n = self.opened()
        self.assertFalse({"scale", "pc_app", "asked_size"} & set(self.displays()[-1]))
        with self.assertRaisesRegex(RuntimeError, "whose PC takes a scale"):
            self.run_window("scale", n, "1.5")
        self.assertEqual(self.api, [])

    def test_launch_asks_for_the_displays_scale(self):
        layout = self.m.load_layout()
        layout["hosts"][0]["displays"][0]["scale"] = 1.5
        self.m.save_layout(layout)
        self.answers["/api/frametop/windows/open"] = (200, {"output": "FT-W2", "address": "0x77", "status": True})
        self.run_window("launch", "linuxpc", "Calc.desktop")
        body = next(c[4] for c in self.api if c[3] == "/api/frametop/windows/open")
        self.assertEqual(body, {"app": "Calc.desktop", "width": 1278, "height": 798, "scale": 1.5})
        d = self.displays()[-1]
        self.assertEqual((d["size"], d["asked_size"], d["scale"], d["pc_app"]), ([1278, 798], [1280, 800], 1.5, "Calc.desktop"))

    def test_the_same_size_changes_live_and_is_remembered(self):
        n = self.opened("--size", "1280x800")
        self.answers["/api/frametop/windows/resize"] = (200, {"status": True, "scale": 1.6})
        self.run_window("scale", n, "1.6")
        self.assertEqual([c[2:] for c in self.api], [("POST", "/api/frametop/windows/resize",
                                                     {"address": "0x55aa10", "width": 1280, "height": 800, "scale": 1.6})])
        self.assertEqual(self.screens.asked, [f"remote {n} scale 1.6 1280x800 answer"])   # (its stream goes on)
        self.assertEqual((self.displays()[-1]["scale"], self.displays()[-1]["size"]), (1.6, [1280, 800]))
        self.assertEqual(self.m.load_layout()["hosts"][0]["windows"], {"port": 48989, "scales": {"foot": 1.6}})

    def test_another_size_starts_the_stream_over(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.5")
        self.assertEqual(self.api, [])   # (ft-stream's open sends both)
        self.assertEqual((self.displays()[-1]["size"], self.displays()[-1]["scale"]), ([1278, 798], 1.5))
        self.assertRegex(self.screens.asked[0], rf"^remote {n} start .* window:0x55aa10 1278x798 ")
        self.assertIn(f"remote {n} scale 1.5 1278x798", self.screens.asked)
        self.run_window("scale", n, "1")   # ... and back to the size it was given, not one nudged from 1278
        self.assertEqual(self.displays()[-1]["size"], [1280, 800])
        self.assertNotIn("asked_size", self.displays()[-1])

    def test_the_pc_refusing_it_keeps_the_old_one(self):
        n = self.opened("--size", "1280x800")
        self.answers["/api/frametop/windows/resize"] = (400, {"error": "no window output for 0x55aa10"})
        with self.assertRaisesRegex(RuntimeError, "no window output"):
            self.run_window("scale", n, "2")
        self.assertEqual((self.displays()[-1]["scale"], self.screens.asked), (1.0, [f"remote {n} scale 1 answer"]))
        self.assertNotIn("scales", self.m.load_layout()["hosts"][0]["windows"])

    def test_a_bad_scale_asks_nothing(self):
        n = self.opened()
        for bad in ("5", "0", "nan", "big", "-1"):
            with self.assertRaisesRegex(RuntimeError, "a scale is 0.5 to 4"):
                self.run_window("scale", n, bad)
        self.assertEqual(self.api, [])

    def test_the_app_opens_at_it_again(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.6")
        self.run_window("close", "linuxpc-window-55aa10")
        self.m.screens_socket = self.no_screens
        self.run_window("open", "linuxpc", "0x55aa10", "--size", "1280x800")
        self.assertEqual(self.displays()[-1]["scale"], 1.6)

    def test_a_corner_dragged_keeps_the_scale(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.5")
        self.run_window("resize", n, "1000x702", "0.8")
        d = self.displays()[-1]
        self.assertEqual((d["size"], d["asked_size"], d["scale"], d["metres"]), ([1002, 702], [1000, 702], 1.5, 0.8016))
        self.assertIn(f"remote {n} scale 1.5 1002x702", self.screens.asked)   # (its stretch waits for that)

    def test_a_small_panel_stays_wide_enough(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.5")
        self.run_window("resize", n, "500x700", "0.15")
        self.assertEqual((self.displays()[-1]["size"], self.displays()[-1]["metres"]), ([498, 702], 0.15))

    def test_a_corner_dragged_past_the_limits(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.5")
        self.run_window("resize", n, "9000x100", "2")
        d = self.displays()[-1]
        self.assertEqual((d["size"], d["scale"]), ([3840, 240], 1.5))   # (which take it)

    def test_the_layout_changed_while_the_pc_took_its_time(self):
        n = self.opened("--size", "1280x800")
        request = self.w.ft_hostapi.post_json

        def slow(address, path, body, **kw):   # (another window's panel opens meanwhile)
            layout = self.m.load_layout()
            layout["hosts"][0]["displays"].append({"id": "other", "client": "c", "app": "window:0x9", "window": True, "size": [800, 600]})
            self.m.save_layout(layout)
            return request(address, path, body, **kw)

        self.w.ft_hostapi.post_json = slow
        self.run_window("scale", n, "1.6")
        self.assertEqual([(d["id"], d.get("scale")) for d in self.displays()[1:]], [("linuxpc-window-55aa10", 1.6), ("other", None)])

    def test_closed_while_the_pc_took_its_time(self):
        n = self.opened("--size", "1280x800")
        request = self.w.ft_hostapi.post_json

        def slow(address, path, body, **kw):
            layout = self.m.load_layout()
            del layout["hosts"][0]["displays"][1:]
            self.m.save_layout(layout)
            return request(address, path, body, **kw)

        self.w.ft_hostapi.post_json = slow
        self.run_window("scale", n, "1.6")
        self.assertEqual(([d["id"] for d in self.displays()], self.screens.asked), (["linuxpc-dp-2"], []))   # (it stays closed)

    def test_one_at_a_time(self):
        n, held = self.opened("--size", "1280x800"), []
        request = self.w.ft_hostapi.post_json

        def checked(*args, **kw):
            with open(f"{self.m.LOCK_PATH}.window-{n}", "w") as other:
                try:
                    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    held.append(True)
            return request(*args, **kw)

        self.w.ft_hostapi.post_json = checked
        self.run_window("scale", n, "1.6")
        self.assertEqual(held, [True])

    def test_a_pulled_window_starts_at_its_scale(self):
        layout = self.m.load_layout()
        layout["hosts"][0]["windows"]["scales"] = {"foot": 1.25}
        n = next(n for n, _, d in self.m.remote_displays(layout) if d["app"] == "display:DP-2")
        self.m.save_layout(layout)
        self.answers["/api/frametop/windows/at"] = (200, {"address": "0x55aa10", "class": "foot", "title": "t", "width": 1000, "height": 600, "grab": [0.5, 0.5]})
        self.run_window("pull", str(n), "0.5", "0.5")
        self.assertEqual((self.displays()[-1]["scale"], self.displays()[-1]["pc_app"]), (1.25, "foot"))

    def test_remote_set_leaves_a_windows_scale_to_this(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.5")
        for kv in ("scale=2", "scale=own", "mode=own"):
            with self.assertRaisesRegex(RuntimeError, "a window"):
                self.m.remote_command(["set", "linuxpc-window-55aa10", kv])
        self.m.remote_command(["set", "linuxpc-window-55aa10", "size=1000x702"])
        d = self.displays()[-1]
        self.assertEqual((d["size"], d["asked_size"], d["scale"]), ([1002, 702], [1000, 702], 1.5))

    def test_the_hosts_port_set_again_keeps_them(self):
        n = self.opened("--size", "1280x800")
        self.run_window("scale", n, "1.6")
        self.m.remote_command(["host", "linuxpc", "windows=48990"])
        self.assertEqual(self.m.load_layout()["hosts"][0]["windows"], {"port": 48990, "scales": {"foot": 1.6}})


if __name__ == "__main__":
    unittest.main()
