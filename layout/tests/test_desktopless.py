"""ft-layout desktopless (docs/desktopless.md), without a headset or a host: on hides the screens
and the Linux PC's displays, floats the Frame's windows and opens the PC's as panels (shown ones
first, then by recent use, while panels are free), fanned in front; off puts everything back
and keeps where each PC app's panel was. ft-screens, ft-floatd, ft-stream and the host's API are
fakes that record."""
import importlib.util
import math
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parents[1]


def load_modules(tmp):
    spec = importlib.util.spec_from_file_location("desktopless_layout", HERE / "ft_layout.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    m.LAYOUT_PATH = str(tmp / "layout.json")
    sys.path.insert(0, str(HERE))
    import ft_desktopless
    d = importlib.reload(ft_desktopless)
    d.L = m
    d.SEEN_PATH = str(tmp / "seen.json")
    return m, d


def window(address, cls, focus, shown=False, workspace="12", app=None, width=1200, height=800):
    return {"address": address, "class": cls, "title": cls, "width": width, "height": height, "output": None,
            "workspace": workspace, "focus": focus, "shown": shown, "floating": False, "at": [0, 0], "app": app}


class Screens:
    """ft-screens: answers what desktopless asks, records the rest."""
    def __init__(self):
        self.asked = []

    def ask(self, text):
        self.asked.append(text)
        w = text.split()
        if w[0] == "head":
            return "ok 0 1.6 0 0"
        if w[0] == "remotes":
            return "ok remotes"
        if w[0] == "get":
            # facing you 1.3 m ahead, 0.9 m wide, 0.5 m tall
            return "ok " + " ".join(map(str, [0, 1.5, -1.3, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.9, 0.5, 0])) + " none"
        return "ok"


class Desktopless(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m, self.d = load_modules(pathlib.Path(self.tmp.name))
        self.api, self.floats = [], []
        self.windows = [window("0x10", "slack", 3, app="slack.desktop"),
                        window("0x20", "brave", 1, shown=True, app="brave.desktop"),
                        window("0x30", "foot", 0, shown=True),
                        window("0x40", "scratch", 2, workspace="special:magic")]
        self.screens = Screens()

        def fake_request(address, method, path, body=None, timeout=2.0, port=0, **kw):
            self.api.append((method, path, body))
            if path == "/api/frametop/windows" and method == "GET":
                return 200, {"windows": self.windows, "outputs": [], "locked": False}
            if path == "/api/frametop/windows/open":
                return 200, {"address": "0x99", "output": "FT-W1"}
            return 200, {"status": True}

        self.m.ft_hostapi.request = fake_request
        self.m.ft_hostapi.post_json = lambda a, p, b, timeout=20.0, port=0, **kw: fake_request(a, "POST", p, b, port=port)
        self.m.run_stream = lambda *a, timeout=60, stdin=None: (0, "paired", "")
        self.m.screens_socket = lambda: self.screens

        def fake_float(text, timeout=6.0):
            self.floats.append(text)
            return "ok 21" if text == "float all" else "ok"

        self.m.ask_float = fake_float
        self.m.streams_only = lambda: False
        self.d.start_watch = lambda: None
        self.d.stop_watch = lambda: None
        self.m.save_layout({"mode": "custom", "screens": [{}, {"hidden": True}, {}],
                            "hosts": [{"name": "oma7", "address": "192.0.2.30", "windows": {"port": 48989},
                                       "displays": [{"id": "oma7-dp-2", "client": "frametop-oma7-dp-2",
                                                     "app": "display:DP-2", "size": [5120, 1440]}]},
                                      {"name": "dody", "address": "192.0.2.40",
                                       "displays": [{"id": "dody-1", "client": "frametop-dody-1",
                                                     "app": "display:1", "size": [3840, 2160]}]}]})
        self.m.screen_count = lambda layout=None: 3
        self.m.native_count = lambda layout=None: 3

    def tearDown(self):
        sys.path.remove(str(HERE))
        self.tmp.cleanup()

    def panels(self, layout=None):
        layout = layout or self.m.load_layout()
        return [d for h in layout["hosts"] for d in h.get("displays", []) if d.get("window")]

    def test_the_fan_is_in_front_and_faces_you(self):
        poses = self.d.arc([(0.9, 0.5)] * 3)
        xs = [t["pos"][0] for t in poses]
        self.assertAlmostEqual(xs[1], 0.0, places=6)               # the middle one straight ahead
        self.assertAlmostEqual(xs[0], -xs[2], places=6)            # the others either side
        for t in poses:
            self.assertLess(t["pos"][2], 0)                        # in front
            dist = math.dist((0, 0, 0), t["pos"])
            self.assertAlmostEqual(dist, self.d.ARC_DISTANCE, delta=0.2)
            self.assertAlmostEqual(t["face"][1], self.d.ARC_PITCH)  # one row, a little below the eyes
        self.assertLess(poses[0]["face"][0] * poses[2]["face"][0], 0)  # turned towards you

    def test_a_long_fan_goes_on_in_a_row_above(self):
        poses = self.d.arc([(0.6, 0.4)] * 8)   # 5 to a row at 1.3 m
        pitches = sorted({round(t["face"][1], 3) for t in poses})
        self.assertEqual(len(pitches), 2)
        self.assertEqual(pitches[0], self.d.ARC_PITCH)
        self.assertGreater(pitches[1], pitches[0] + 17)   # (0.4 m tall: about 17.5 degrees)

    def test_shown_windows_first_then_the_most_recently_used(self):
        ranked = sorted(self.windows[:3], key=self.d.rank)
        self.assertEqual([w["address"] for w in ranked], ["0x30", "0x20", "0x10"])

    def test_on_hides_the_desktops_and_brings_every_window_out(self):
        self.d.command(["on"])
        layout = self.m.load_layout()
        self.assertTrue(all(s.get("hidden") for s in layout["screens"]))
        oma7, dody = layout["hosts"]
        self.assertTrue(oma7["displays"][0].get("hidden"))        # the Linux PC's display: hidden
        self.assertFalse(dody["displays"][0].get("hidden"))       # one without a windows instance: as it was
        self.assertEqual(layout["desktopless"]["before"], {"screens": [1], "remotes": []})
        for n in (1, 2, 3, 101):
            self.assertIn(f"conceal {n}", self.screens.asked)
        self.assertEqual(self.floats, ["float all"])
        # the special workspace's window stays; the rest come out shown first, then by recent use
        self.assertEqual([d["app"] for d in self.panels(layout)], ["window:0x30", "window:0x20", "window:0x10"])
        self.assertEqual([d["key"] for d in self.panels(layout)], ["oma7|foot", "oma7|brave.desktop", "oma7|slack.desktop"])
        # the Frame's floating panel 21 is fanned out with them
        self.assertTrue(any(a.startswith("place 21 ") for a in self.screens.asked))
        self.assertEqual(self.m.load_layout()["desktopless"]["on"], True)
        self.assertEqual(self.d.read_seen(), {"oma7": {"0x10", "0x20", "0x30"}})

    def test_on_opens_only_as_many_as_there_are_panels(self):
        self.m.REMOTE_MAX = 4   # 2 displays + 2 window panels
        self.d.command(["on"])
        self.assertEqual([d["app"] for d in self.panels()], ["window:0x30", "window:0x20"])

    def test_one_you_sent_back_stays_on_the_pc(self):
        self.d.command(["on"])
        layout = self.m.load_layout()
        host = layout["hosts"][0]
        self.m.window_close(layout, host, next(d for d in host["displays"] if d.get("app") == "window:0x20"))
        self.windows.append(window("0x50", "gimp", 0, shown=True))
        layout = self.m.load_layout()
        seen = self.d.read_seen()
        self.assertEqual(self.d.open_windows(layout, self.screens, (0, 1.6, 0), 0.0, seen), 1)  # only the new one
        self.assertEqual(sorted(d["app"] for d in self.panels()), ["window:0x10", "window:0x30", "window:0x50"])

    def test_off_puts_everything_back_and_keeps_the_places(self):
        self.d.command(["on"])
        self.screens.asked.clear()
        self.api.clear()
        self.d.command(["off"])
        layout = self.m.load_layout()
        self.assertEqual(self.panels(layout), [])
        released = sorted(b["address"] for m, p, b in self.api if p == "/api/frametop/windows/release")
        self.assertEqual(released, ["0x10", "0x20", "0x30"])
        self.assertEqual([bool(s.get("hidden")) for s in layout["screens"]], [False, True, False])  # as before
        self.assertFalse(layout["hosts"][0]["displays"][0].get("hidden"))
        self.assertIn("reveal 101", self.screens.asked)
        self.assertNotIn("reveal 2", self.screens.asked)
        self.assertEqual(self.floats[-1], "dock all")
        self.assertFalse(layout["desktopless"]["on"])
        self.assertEqual(sorted(layout["desktopless"]["places"]), ["oma7|brave.desktop", "oma7|foot", "oma7|slack.desktop"])

    def test_an_app_that_was_not_out_keeps_its_place(self):
        layout = self.m.load_layout()
        layout["desktopless"] = {"on": False, "places": {"oma7|gimp.desktop": [{"pos": [0, 0, -1]}]}}
        self.m.save_layout(layout)
        self.d.command(["on"])
        self.d.command(["off"])
        self.assertIn("oma7|gimp.desktop", self.m.load_layout()["desktopless"]["places"])

    def test_next_time_an_app_opens_where_it_was(self):
        self.d.command(["on"])
        self.d.command(["off"])
        layout = self.m.load_layout()
        layout["desktopless"]["places"]["oma7|slack.desktop"] = [{"pos": [1.0, 0.2, -0.8], "face": [-50, 5],
                                                                  "roll": 0, "metres": 1.5}]
        self.m.save_layout(layout)
        self.d.command(["on"])
        slack = next(d for d in self.panels() if d["app"] == "window:0x10")
        self.assertEqual((slack["pos"], slack["metres"]), ([1.0, 0.2, -0.8], 1.5))

    def profile(self, name, **kw):
        layout = self.m.load_layout()
        layout.setdefault("layouts", {})[name] = []
        layout.setdefault("profiles", {})[name] = dict(kw)
        self.m.save_layout(layout)

    def test_a_profile_keeps_whether_it_was_desktopless_and_its_pc_apps(self):
        self.d.command(["on"])
        layout = self.m.load_layout()
        profile = {}
        self.d.capture(layout, profile)
        self.assertTrue(profile["desktopless"]["on"])
        self.assertEqual(sorted(profile["desktopless"]["places"]), ["oma7|brave.desktop", "oma7|foot", "oma7|slack.desktop"])
        self.d.command(["off"])
        profile = {}
        self.d.capture(self.m.load_layout(), profile)
        self.assertEqual(profile["desktopless"], {"on": False})

    def test_using_a_desktopless_profile_hides_the_desktops_before_they_are_arranged(self):
        self.profile("Work", hidden=[], desktopless={"on": True, "places": {}})
        layout = self.m.load_layout()
        self.assertTrue(self.d.use_profile(layout, "Work"))
        self.assertTrue(all(s.get("hidden") for s in layout["screens"]))
        self.assertTrue(layout["hosts"][0]["displays"][0].get("hidden"))
        self.assertEqual(layout["desktopless"]["before"], {"screens": [1], "remotes": []})
        self.assertEqual(self.screens.asked, [])   # (nothing shown or hidden yet: the arrangement does it)

    def test_its_pc_apps_that_are_not_open_start_where_they_were(self):
        place = {"pos": [1.0, 0.2, -0.8], "face": [-50, 5], "roll": 0, "metres": 1.5}
        self.profile("Work", desktopless={"on": True, "places": {"oma7|gimp.desktop": [place],
                                                                 "oma7|slack.desktop": [place],
                                                                 "oma7|foot": [place]}})
        layout = self.m.load_layout()
        self.d.use_profile(layout, "Work")
        self.m.save_layout(layout)
        self.d.resume("Work")
        opened = [b["app"] for m, p, b in self.api if p == "/api/frametop/windows/open"]
        self.assertEqual(opened, ["gimp.desktop"])   # slack is open; foot has no desktop file
        gimp = next(d for d in self.panels() if d["app"] == "window:0x99")
        self.assertEqual((gimp["key"], gimp["pos"], gimp["metres"]), ("oma7|gimp.desktop", [1.0, 0.2, -0.8], 1.5))
        self.assertIn("0x99", self.d.read_seen()["oma7"])   # (the watcher leaves it alone)

    def test_using_a_profile_that_was_not_desktopless_puts_the_windows_back(self):
        self.d.command(["on"])
        self.profile("Home", hidden=[3], desktopless={"on": False})
        layout = self.m.load_layout()
        self.m.use_hidden(layout, "Home")
        self.assertFalse(self.d.use_profile(layout, "Home"))
        self.m.save_layout(layout)
        layout = self.m.load_layout()
        self.assertEqual(self.panels(layout), [])
        self.assertFalse(layout["desktopless"]["on"])
        self.assertEqual([bool(s.get("hidden")) for s in layout["screens"]], [False, False, True])  # the profile's
        self.floats.clear()
        self.d.resume("Home")
        self.assertEqual(self.floats, ["dock all"])

    def test_a_profile_from_before_desktopless_mode_keeps_the_mode(self):
        self.d.command(["on"])
        self.profile("Old", hidden=[])
        layout = self.m.load_layout()
        self.m.use_hidden(layout, "Old")     # (shows every screen)
        self.assertTrue(self.d.use_profile(layout, "Old"))
        self.assertTrue(all(s.get("hidden") for s in layout["screens"]))

    def test_the_desktop_starts_desktopless_again(self):
        self.d.command(["on"])
        self.floats.clear()
        watched = []
        self.d.start_watch = lambda: watched.append(1)
        self.d.resume(wait=0)
        self.assertEqual(self.floats, ["windows", "float all"])
        self.assertEqual(watched, [1])
        self.d.command(["off"])
        self.floats.clear()
        self.d.resume(wait=0)
        self.assertEqual(self.floats, [])   # off, and no profile: nothing to do

    def test_toggle_and_status(self):
        self.d.command(["toggle"])
        self.assertTrue(self.m.load_layout()["desktopless"]["on"])
        self.d.command(["toggle"])
        self.assertFalse(self.m.load_layout()["desktopless"]["on"])
        with self.assertRaises(RuntimeError):
            self.d.command(["sideways"])


if __name__ == "__main__":
    unittest.main()
