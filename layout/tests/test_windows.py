"""ft-layout window, without a headset or a host: a window becomes a remote display of its host's
Frametop windows instance (its own port and client from the host's pool), launch opens the app
first, and close drops the panel and puts the window back. ft-stream and the host's API are fakes
that record."""
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stdout


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("windows_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


WINDOWS = {"windows": [
    {"address": "0x55aa10", "class": "foot", "title": "a terminal", "width": 1001, "height": 600, "output": None},
    {"address": "0x55aa20", "class": "gimp", "title": "GIMP", "width": 2000, "height": 1200, "output": "FT-W1"},
], "outputs": []}


class Windows(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.streams, self.api = [], []
        self.answers = {}

        def fake_stream(*args, timeout=60, stdin=None):
            self.streams.append(args)
            return 0, "paired", ""

        def fake_request(address, method, path, body=None, timeout=2.0, port=0, **kw):
            self.api.append((address, port, method, path, body))
            return self.answers.get(path, (200, {"status": True}))

        def fake_post(address, path, body, timeout=20.0, port=0, **kw):
            return fake_request(address, "POST", path, body, timeout, port)

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.run_stream = fake_stream
        self.m.ft_hostapi.request = fake_request
        self.m.ft_hostapi.post_json = fake_post
        self.m.screens_socket = no_screens
        self.answers["/api/frametop/windows"] = (200, WINDOWS)
        self.m.save_layout({"hosts": [{"name": "oma7", "address": "192.0.2.30", "windows": {"port": 48989},
                                       "displays": [{"id": "oma7-dp-2", "client": "frametop-oma7-dp-2",
                                                     "app": "display:DP-2", "size": [5120, 1440]}]}]})

    def tearDown(self):
        self.tmp.cleanup()

    def run_window(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.window_command(list(args))
        return out.getvalue()

    def displays(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"][0]["displays"]

    def test_list_says_when_the_host_is_locked(self):
        self.assertNotIn("locked", self.run_window("list", "oma7"))
        self.answers["/api/frametop/windows"] = (200, dict(WINDOWS, locked=True))
        out = self.run_window("list", "oma7")
        self.assertIn("oma7 is locked", out)
        self.assertIn("0x55aa10 1001x600 - - foot: a terminal", out)

    def test_open_makes_a_window_panel(self):
        self.assertEqual(self.run_window("open", "oma7", "0x55aa10").split()[-1], "oma7-window-55aa10")  # its id, last
        d = self.displays()[-1]
        self.assertEqual((d["app"], d["port"], d["window"], d["client"]),
                         ("window:0x55aa10", 48989, True, "frametop-oma7-win1"))
        self.assertEqual(d["size"], [1000, 600])  # the window's size, even
        self.assertEqual(d["label"], "a terminal")
        self.assertEqual(self.streams, [("pair", "192.0.2.30:48989", "--id", "frametop-oma7-win1", )])
        self.assertTrue(all(port == 48989 for _, port, *_ in self.api))

    def test_each_window_its_own_client(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.run_window("open", "oma7", "0x55aa20", "--size", "1600x900")
        clients = [d["client"] for d in self.displays() if d.get("window")]
        self.assertEqual(clients, ["frametop-oma7-win1", "frametop-oma7-win2"])
        self.assertEqual(self.displays()[-1]["size"], [1600, 900])

    def test_open_caps_a_big_window(self):
        # a window tiled on an ultrawide: the panel starts at most 1920x1200, and the PC resizes it
        self.answers["/api/frametop/windows"] = (200, dict(WINDOWS, windows=[dict(WINDOWS["windows"][0], width=5096, height=1390)]))
        self.run_window("open", "oma7", "0x55aa10")
        self.assertEqual(self.displays()[-1]["size"], [1920, 1200])

    def test_open_twice_is_one_panel(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.run_window("open", "oma7", "0x55aa10")
        self.assertEqual(len([d for d in self.displays() if d.get("window")]), 1)

    def test_a_disconnected_window_panel_is_forgotten(self):
        # (Daenon, 2026-11-07: an "off" entry from an older build kept Brave from being pulled again)
        self.run_window("open", "oma7", "0x55aa10")
        layout = self.m.load_layout()
        next(d for d in layout["hosts"][0]["displays"] if d.get("window"))["off"] = True
        self.m.save_layout(layout)
        said = []
        self.m.log = said.append
        self.run_window("list", "oma7")
        self.assertFalse(any(d.get("window") for d in self.displays()))
        self.assertIn("forgot oma7-window-55aa10: it was disconnected on oma7", said)

    def test_open_again_starts_a_stopped_panel(self):
        self.run_window("open", "oma7", "0x55aa10")
        started, running = [], {}

        class Screens:
            def ask(self, text, timeout=10.0):
                if text == "remotes":
                    return "ok remotes " + " ".join(f"{n}:x:{s}" for n, s in running.items())
                raise self_m.NoAnswer("not this one")

        self_m = self.m
        self.m.screens_socket = Screens
        self.m.start_remotes_now = lambda layout, only: started.append(set(only))
        layout = self.m.load_layout()
        self.m.window_add(layout, layout["hosts"][0], 48989, "0x55aa10", "foot", (800, 600))
        self.assertEqual(started, [{"oma7-window-55aa10"}])   # not running: started
        n = next(d["screen"] for d in self.displays() if d.get("window"))
        running[n] = "live"
        layout = self.m.load_layout()
        self.m.window_add(layout, layout["hosts"][0], 48989, "0x55aa10", "foot", (800, 600))
        self.assertEqual(len(started), 1)   # running: left alone (not placed again)

    def test_no_such_window(self):
        with self.assertRaisesRegex(RuntimeError, "no window 0x99"):
            self.run_window("open", "oma7", "0x99")
        self.assertEqual(self.streams, [])

    def test_bad_address_never_asks(self):
        with self.assertRaisesRegex(RuntimeError, "ADDRESS is a window's address"):
            self.run_window("open", "oma7", "address:0x1; rm")
        self.assertEqual(self.api, [])

    def test_launch_opens_the_app_first(self):
        self.answers["/api/frametop/windows/open"] = (200, {"output": "FT-W2", "address": "0x77", "status": True})
        self.run_window("launch", "oma7", "org.gnome.Calculator.desktop")
        opened = [c for c in self.api if c[3] == "/api/frametop/windows/open"]
        self.assertEqual(opened[0][4], {"app": "org.gnome.Calculator.desktop", "width": 1280, "height": 800})
        self.assertEqual(self.displays()[-1]["app"], "window:0x77")

    def test_launch_takes_a_windows_start_menu_shortcut(self):
        self.answers["/api/frametop/windows/open"] = (200, {"output": "window:0x14801fc", "address": "0x14801fc", "status": True})
        self.run_window("launch", "oma7", "Accessories/System Tools/Character Map.lnk")
        opened = [c for c in self.api if c[3] == "/api/frametop/windows/open"]
        self.assertEqual(opened[0][4]["app"], "Accessories/System Tools/Character Map.lnk")
        self.assertEqual((self.displays()[-1]["app"], self.displays()[-1]["label"]), ("window:0x14801fc", "Character Map"))

    def test_launch_takes_a_windows_store_app_named_by_its_window(self):
        aumid = "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"
        self.answers["/api/frametop/windows/open"] = (200, {"output": "window:0x2a0f3c", "address": "0x2a0f3c",
                                                            "title": "Calculator", "status": True})
        self.run_window("launch", "oma7", aumid)
        opened = [c for c in self.api if c[3] == "/api/frametop/windows/open"]
        self.assertEqual(opened[0][4]["app"], aumid)
        self.assertEqual(self.displays()[-1]["label"], "Calculator")

    def test_a_store_app_without_a_title_is_named_by_its_package(self):
        self.assertEqual(self.m.window_app_title("Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"), "Microsoft.WindowsCalculator")
        self.assertEqual(self.m.window_app_title("x.lnk", {"title": "  "}), "x")
        self.assertEqual(self.m.window_app_title("x.lnk", {"title": "a\nb\x1b" + "c" * 100}), "ab" + "c" * 78)

    def test_launch_refuses_what_is_no_app_id(self):
        for app in ("x.exe", "C:/Windows/notepad.lnk", "a\nb.desktop", "Calc_8wekyb3d8bbwe!App;rm", "Calc!App"):
            with self.assertRaisesRegex(RuntimeError, "APP is an app's id"):
                self.run_window("launch", "oma7", app)
        self.assertEqual(self.api, [])

    def test_launch_failure_says_why(self):
        self.answers["/api/frametop/windows/open"] = (400, {"error": "no app x.desktop"})
        with self.assertRaisesRegex(RuntimeError, "no app x.desktop"):
            self.run_window("launch", "oma7", "x.desktop")
        self.assertFalse(any(d.get("window") for d in self.displays()))

    def test_close_releases_the_window(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.run_window("close", "oma7-window-55aa10")
        self.assertFalse(any(d.get("window") for d in self.displays()))
        self.assertEqual(self.api[-1][2:], ("POST", "/api/frametop/windows/release", {"address": "0x55aa10"}))

    def test_close_after_the_window_left(self):
        # closed on the PC (or back after the grace period): the host has no output for it
        self.run_window("open", "oma7", "0x55aa10")
        self.answers["/api/frametop/windows/release"] = (400, {"error": "no window output for 0x55aa10"})
        said = []
        self.m.log = said.append
        self.run_window("close", "oma7-window-55aa10")
        self.assertEqual(said, ["closed oma7-window-55aa10: the window had already left it"])
        self.assertFalse(any(d.get("window") for d in self.displays()))

    def test_list_forgets_panels_of_closed_windows(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.run_window("open", "oma7", "0x55aa20")
        layout = self.m.load_layout()
        layout["profiles"] = {"Work": {"remote": {"oma7-window-55aa10": {"pos": [1, 2, 3]}}}}
        self.m.save_layout(layout)
        self.answers["/api/frametop/windows"] = (200, dict(WINDOWS, windows=WINDOWS["windows"][1:]))  # foot closed
        said = []
        self.m.log = said.append
        out = self.run_window("list", "oma7")
        self.assertEqual(said, ["forgot oma7-window-55aa10: its window closed on oma7"])
        self.assertEqual([d["id"] for d in self.displays() if d.get("window")], ["oma7-window-55aa20"])
        self.assertEqual(self.m.load_layout()["profiles"]["Work"]["remote"], {})
        self.assertIn("FT-W1 oma7-window-55aa20 gimp", out)
        self.assertEqual(len(self.displays()), 2)  # the display panel stays

    def test_open_reuses_a_closed_windows_client(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.answers["/api/frametop/windows"] = (200, dict(WINDOWS, windows=WINDOWS["windows"][1:]))
        self.m.log = lambda *_: None
        self.run_window("open", "oma7", "0x55aa20")
        self.assertEqual([d["client"] for d in self.displays() if d.get("window")], ["frametop-oma7-win1"])

    def test_launch_reuses_a_closed_windows_client(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.answers["/api/frametop/windows"] = (200, dict(WINDOWS, windows=WINDOWS["windows"][1:]))
        self.answers["/api/frametop/windows/open"] = (200, {"output": "FT-W2", "address": "0x77", "status": True})
        self.m.log = lambda *_: None
        self.run_window("launch", "oma7", "foot.desktop")
        self.assertEqual([(d["app"], d["client"]) for d in self.displays() if d.get("window")],
                         [("window:0x77", "frametop-oma7-win1")])

    def test_catalog_for_frunner(self):
        self.answers["/api/frametop/windows/apps"] = (200, {"apps": [{"id": "foot.desktop", "name": "Foot", "icon": "foot",
                                                                      "exec": "foot"}]})
        self.run_window("open", "oma7", "0x55aa20")
        out = json.loads(self.run_window("catalog", "oma7"))
        self.assertEqual(out["apps"], [{"id": "foot.desktop", "name": "Foot", "icon": "foot"}])  # no exec line
        self.assertEqual([(w["address"], w["panel"], w["screen"]) for w in out["windows"]],
                         [("0x55aa10", None, None), ("0x55aa20", "oma7-window-55aa20", 102)])
        self.assertEqual((out["host"], out["locked"]), ("oma7", False))

    def test_list_failure_forgets_nothing(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.answers["/api/frametop/windows"] = (0, None)  # oma7 asleep
        with self.assertRaises(RuntimeError):
            self.run_window("list", "oma7")
        self.assertEqual(len([d for d in self.displays() if d.get("window")]), 1)

    def test_disconnect_sends_the_window_back(self):
        # Remote Displays' Disconnect (a host's, or the panel's own): no minute's grace on the PC
        self.run_window("open", "oma7", "0x55aa10")
        said = []
        self.m.log = said.append
        self.m.remote_command(["disconnect", "oma7-dp-2", "oma7-window-55aa10"])
        self.assertEqual(self.api[-1][2:], ("POST", "/api/frametop/windows/release", {"address": "0x55aa10"}))
        self.assertEqual([d["id"] for d in self.displays()], ["oma7-dp-2"])
        self.assertTrue(self.displays()[0]["off"])  # a display keeps its place, switched off
        self.assertIn("disconnected oma7-window-55aa10", said)

    def test_close_forgets_the_panel_in_profiles(self):
        self.run_window("open", "oma7", "0x55aa10")
        layout = self.m.load_layout()
        layout["profiles"] = {"Work": {"remote": {"oma7-window-55aa10": {"pos": [1, 2, 3]}, "oma7-dp-2": {}}}}
        self.m.save_layout(layout)
        self.run_window("close", "oma7-window-55aa10")
        self.assertEqual(self.m.load_layout()["profiles"]["Work"]["remote"], {"oma7-dp-2": {}})

    def test_list_strips_control_characters(self):
        windows = {"windows": [dict(WINDOWS["windows"][0], title="evil\x1b]0;x\x07page")], "outputs": []}
        self.answers["/api/frametop/windows"] = (200, windows)
        self.assertIn("foot: evil ]0;x page", self.run_window("list", "oma7"))

    def test_close_is_for_windows_only(self):
        with self.assertRaisesRegex(RuntimeError, "isn't a window"):
            self.run_window("close", "oma7-dp-2")

    def test_host_without_windows(self):
        layout = self.m.load_layout()
        layout["hosts"][0].pop("windows")
        self.m.save_layout(layout)
        with self.assertRaisesRegex(RuntimeError, "no Frametop windows instance"):
            self.run_window("list", "oma7")

    # Window drag (DEE-345): a window panel dropped onto one of its PC's displays, a window pulled off one.
    def test_drop_puts_the_window_onto_that_display(self):
        self.run_window("open", "oma7", "0x55aa10")
        numbers = {d["id"]: d["screen"] for d in self.displays()}
        said = []
        self.m.log = said.append
        self.run_window("drop", str(numbers["oma7-window-55aa10"]), str(numbers["oma7-dp-2"]), "0.25", "0.5", "0.5", "0.02")
        self.assertEqual(self.api[-1][1:], (48989, "POST", "/api/frametop/windows/release",
                                            {"address": "0x55aa10", "monitor": "DP-2", "x": 0.25, "y": 0.5,
                                             "grab_x": 0.5, "grab_y": 0.02}))
        self.assertEqual([d["id"] for d in self.displays()], ["oma7-dp-2"])  # the panel's gone, the display stays
        self.assertEqual(said, ["dropped oma7-window-55aa10 onto DP-2 at 0.25,0.50"])

    def test_drop_only_onto_its_own_pcs_display(self):
        self.run_window("open", "oma7", "0x55aa10")
        layout = self.m.load_layout()
        layout["hosts"].append({"name": "dody", "address": "192.0.2.40",
                                "displays": [{"id": "dody-1", "client": "c", "app": "display:{1}", "size": [2560, 1440]}]})
        self.m.save_layout(layout)
        layout = self.m.load_layout()
        numbers = {d["id"]: n for n, _, d in self.m.remote_displays(layout)}
        calls = len(self.api)
        with self.assertRaisesRegex(RuntimeError, "isn't one of oma7's displays"):
            self.run_window("drop", str(numbers["oma7-window-55aa10"]), str(numbers["dody-1"]), "0.5", "0.5", "0.5", "0.5")
        with self.assertRaisesRegex(RuntimeError, "isn't a window"):
            self.run_window("drop", str(numbers["oma7-dp-2"]), str(numbers["oma7-dp-2"]), "0.5", "0.5", "0.5", "0.5")
        with self.assertRaisesRegex(RuntimeError, "from 0 to 1"):
            self.run_window("drop", str(numbers["oma7-window-55aa10"]), str(numbers["oma7-dp-2"]), "2", "0.5", "0.5", "0.5")
        self.assertEqual(len(self.api), calls)  # nothing asked of the PC
        self.assertEqual(len([d for d in self.displays() if d.get("window")]), 1)

    def test_resize_starts_the_stream_over_at_that_size(self):
        self.run_window("open", "oma7", "0x55aa10")
        told = []

        class Screens:
            def ask(self, text, timeout=10.0):
                told.append(text)
                return "ok"

        self.m.screens_socket = Screens
        said = []
        self.m.log = said.append
        d = next(d for d in self.displays() if d.get("window"))
        self.run_window("resize", str(d["screen"]), "1601x903", "1.25")
        d = next(d for d in self.displays() if d.get("window"))
        self.assertEqual((d["size"], d["metres"]), ([1600, 902], 1.25))   # (even: hosts' encoders)
        self.assertTrue(told[-1].startswith(f"remote {d['screen']} start {d['client']} 192.0.2.30:48989 window:0x55aa10 1600x902 "),
                        told[-1])
        self.assertEqual(said, [f"resized {d['id']} to 1600x902, 1.25 m wide"])

    def test_resize_keeps_to_the_limits(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.m.log = lambda *_: None
        d = next(d for d in self.displays() if d.get("window"))
        self.run_window("resize", str(d["screen"]), "9000x100", "2")   # (no desktop: saved for the next start)
        self.assertEqual(next(e for e in self.displays() if e.get("window"))["size"], [3840, 240])
        for size, metres, why in (("16x9x2", "1", "WxH"), ("1280x720", "40", "0.15 to 12"), ("1280x720", "nan", "0.15 to 12")):
            with self.assertRaisesRegex(RuntimeError, why):
                self.run_window("resize", str(d["screen"]), size, metres)
        with self.assertRaisesRegex(RuntimeError, "isn't a window"):
            self.run_window("resize", "101", "1280x720", "1")   # (oma7-dp-2: a whole display)

    def test_pull_opens_the_window_under_the_point(self):
        self.answers["/api/frametop/windows/at"] = (200, {"address": "0x55aa20", "class": "gimp", "title": "GIMP",
                                                          "width": 2000, "height": 1200, "grab": [0.25, 0.75], "status": True})
        told, m = [], self.m

        class Screens:  # takes "pulled"; starting the stream finds no desktop (the test has none)
            def ask(self, text, timeout=10.0):
                told.append(text)
                if not text.startswith("pulled "):
                    raise m.NoAnswer("no desktop here")
                return "ok"

        self.m.screens_socket = Screens
        self.m.log = lambda *_: None
        number = 101  # oma7-dp-2, the first remote screen
        out = self.run_window("pull", str(number), "0.4", "0.6", "7")
        at = [c for c in self.api if c[3] == "/api/frametop/windows/at"]
        self.assertEqual(at[0][1:], (48989, "POST", "/api/frametop/windows/at", {"monitor": "DP-2", "x": 0.4, "y": 0.6}))
        d = self.displays()[-1]
        self.assertEqual((d["app"], d["size"], d["label"]), ("window:0x55aa20", [1920, 1200], "GIMP"))  # capped
        self.assertEqual(out.split()[-1], d["id"])
        self.assertEqual(told[-1], f"pulled {d['screen']} 0.2500 0.7500 7")

    def test_pull_takes_an_odd_answer_as_the_defaults(self):
        self.answers["/api/frametop/windows/at"] = (200, {"address": "0x55aa30", "class": "x", "width": "wide",
                                                          "height": None, "grab": ["a", 2], "status": True})
        told, m = [], self.m

        class Screens:  # as above: takes "pulled" only
            def ask(self, text, timeout=10.0):
                told.append(text)
                if not text.startswith("pulled "):
                    raise m.NoAnswer("no desktop here")
                return "ok"

        self.m.screens_socket = Screens
        self.m.log = lambda *_: None
        self.run_window("pull", "101", "0.5", "0.5", "3")
        d = self.displays()[-1]
        self.assertEqual(d["app"], "window:0x55aa30")
        self.assertTrue(told[-1].startswith("pulled ") and told[-1].endswith(" 0.5000 1.0000 3"), told[-1])
        with self.assertRaisesRegex(RuntimeError, "SEQ is a number"):
            self.run_window("pull", "101", "0.5", "0.5", "-1")

    def test_pull_needs_a_window_there(self):
        self.answers["/api/frametop/windows/at"] = (400, {"error": "no window there"})
        with self.assertRaisesRegex(RuntimeError, "no window there"):
            self.run_window("pull", "101", "0.5", "0.5")
        self.assertFalse(any(d.get("window") for d in self.displays()))

    def test_pull_is_from_a_display(self):
        self.run_window("open", "oma7", "0x55aa10")
        n = next(d["screen"] for d in self.displays() if d.get("window"))
        with self.assertRaisesRegex(RuntimeError, "isn't a PC's display"):
            self.run_window("pull", str(n), "0.5", "0.5")

    def test_remote_host_sets_the_port(self):
        self.m.remote_command(["host", "oma7", "windows=49989"])
        self.assertEqual(self.m.load_layout()["hosts"][0]["windows"], {"port": 49989})
        self.m.remote_command(["host", "oma7", "windows=off"])
        self.assertNotIn("windows", self.m.load_layout()["hosts"][0])


if __name__ == "__main__":
    unittest.main()
