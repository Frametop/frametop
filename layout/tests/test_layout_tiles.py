"""A profile's dashboard tiles (layout/layout_tiles.py, ft_layout's capture_tiles and use_tiles):
read from ft-screens' "tiles" by what each panel is, and put back with "tiles set" commands that
fit its buffer, the profile's unanchored screens off first. The layout file is in a temp folder;
ft-screens is faked.

  python3 layout/tests/test_layout_tiles.py
"""
import importlib.util
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import layout_tiles  # noqa: E402

TILE = "tile 0 0 0 1 1.000 1.000 0.00"
SAT = "sat 1 " + " ".join(["0.5000"] * 12)


def load_layout_module(tmp):
    spec = importlib.util.spec_from_file_location("layout_tiles_layout", HERE / "ft_layout.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    module.ANCHOR_PATH = str(tmp / "anchor.json")
    module.log = lambda text: None
    return module


class Fake:
    def __init__(self, tiles="ok"):
        self.tiles, self.asked = tiles, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if text == "tiles":
            if not self.tiles.startswith("ok"):
                raise RuntimeError(self.tiles)
            return self.tiles
        return "ok 1 set"


class Pure(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(layout_tiles.parse(f"ok 2 {TILE}; 101 {SAT}"), [(2, TILE), (101, SAT)])
        self.assertEqual(layout_tiles.parse("ok"), [])
        with self.assertRaises(ValueError):
            layout_tiles.parse("error unknown command")

    def test_capture_keeps_screens_and_remotes_only(self):
        fake = Fake(f"ok 17 {TILE}; 2 {TILE}; 101 {SAT}")   # 17: a floating window
        got = layout_tiles.capture(fake.ask, {1: "screen:1", 2: "screen:2", 101: "remote:tv"})
        self.assertEqual(got, [["screen:2", TILE], ["remote:tv", SAT]])

    def test_commands_by_what_each_panel_is(self):
        saved = [["remote:tv", TILE], ["screen:1", SAT]]
        panels = {1: "screen:1", 2: "screen:2", 103: "remote:tv"}   # the TV's number moved
        self.assertEqual(layout_tiles.commands(saved, panels), [f"tiles set 2 off; 103 {TILE}; 1 {SAT}"])

    def test_gone_panels_left_out(self):
        self.assertEqual(layout_tiles.commands([["remote:gone", TILE]], {1: "screen:1"}), ["tiles set 1 off"])
        self.assertEqual(layout_tiles.commands([], {}), [])

    def test_long_lists_split_to_fit(self):
        saved = [[f"screen:{i}", SAT] for i in range(1, 13)]
        cmds = layout_tiles.commands(saved, {i: f"screen:{i}" for i in range(1, 13)})
        self.assertGreater(len(cmds), 1)
        self.assertTrue(all(len(c) <= layout_tiles.MAX_COMMAND and c.startswith("tiles set ") for c in cmds))
        items = [i for c in cmds for i in c[len("tiles set "):].split("; ")]
        self.assertEqual(items, [f"{i} {SAT}" for i in range(1, 13)])   # in order


class Profiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.m.streams_only = lambda: False

    def tearDown(self):
        self.tmp.cleanup()

    def test_saved_into_the_profile(self):
        fake = Fake(f"ok 1 {TILE}")
        self.m.screens_socket = lambda: fake
        layout = {"screens": [{}, {}]}
        profile = {}
        self.m.capture_tiles(layout, profile, "Work")
        self.assertEqual(profile["tiles"], [["screen:1", TILE]])

    def test_older_ft_screens_keeps_what_was_saved(self):
        fake = Fake("error unknown command")
        self.m.screens_socket = lambda: fake
        profile = {"tiles": [["screen:2", TILE]]}
        self.m.capture_tiles({"screens": [{}, {}]}, profile, "Work")
        self.assertEqual(profile["tiles"], [["screen:2", TILE]])

    def test_put_back(self):
        fake = Fake()
        self.m.screens_socket = lambda: fake
        layout = {"screens": [{}, {}], "profiles": {"Work": {"tiles": [["screen:2", TILE], ["junk"]]}}}
        self.m.use_tiles(layout, "Work")
        self.assertEqual(fake.asked, [f"tiles set 1 off; 2 {TILE}"])

    def test_profile_from_before_the_tiles_leaves_them(self):
        fake = Fake()
        self.m.screens_socket = lambda: fake
        self.m.use_tiles({"screens": [{}, {}], "profiles": {"Old": {"hidden": []}}}, "Old")
        self.assertEqual(fake.asked, [])


if __name__ == "__main__":
    unittest.main()
