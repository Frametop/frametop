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

    def test_list_failure_forgets_nothing(self):
        self.run_window("open", "oma7", "0x55aa10")
        self.answers["/api/frametop/windows"] = (0, None)  # oma7 asleep
        with self.assertRaises(RuntimeError):
            self.run_window("list", "oma7")
        self.assertEqual(len([d for d in self.displays() if d.get("window")]), 1)

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

    def test_remote_host_sets_the_port(self):
        self.m.remote_command(["host", "oma7", "windows=49989"])
        self.assertEqual(self.m.load_layout()["hosts"][0]["windows"], {"port": 49989})
        self.m.remote_command(["host", "oma7", "windows=off"])
        self.assertNotIn("windows", self.m.load_layout()["hosts"][0])


if __name__ == "__main__":
    unittest.main()
