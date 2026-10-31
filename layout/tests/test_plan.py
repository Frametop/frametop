"""The layout planner (ft_layout.plan, _chain) and named layouts (save_named, use_named,
rename_named, delete_named): pinned as they behave now. Pure math on made-up
layouts; frametop.conf and the layout file are in a temp folder, nothing reaches ft-screens.

  python3 layout/tests/test_plan.py
"""
import importlib.util
import math
import pathlib
import tempfile
import unittest


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("layout_plan_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


def layout(m, count, **preset):
    return {**m.DEFAULTS, "preset": {**m.DEFAULTS["preset"], **preset},
            "screens": [{"size": [1920, 1080]} for _ in range(count)]}


class Plan(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def width(self):
        return self.m.screen_size(layout(self.m, 1), 0)[0]

    def test_one_screen_straight_ahead(self):
        for kind in ("arc", "flat"):
            with self.subTest(kind=kind):
                (p,) = self.m.plan(layout(self.m, 1, kind=kind), 1)
                self.assertEqual(p["roll"], 0.0)
                for got, want in zip(p["pos"], (0.0, 0.0, -2.0)):
                    self.assertAlmostEqual(got, want)
                self.assertAlmostEqual(p["face"][0], 0.0)

    def test_flat_wall_side_by_side_centred(self):
        out = self.m.plan(layout(self.m, 3, kind="flat", gap=0.1, distance=1.5), 3)
        xs = [p["pos"][0] for p in out]
        self.assertAlmostEqual(sum(xs), 0.0)
        for a, b in zip(xs, xs[1:]):
            self.assertAlmostEqual(b - a, self.width() + 0.1)
        self.assertTrue(all(p["pos"][2] == -1.5 and p["face"] == (0.0, 0.0) for p in out))

    def test_arc_hinged_edge_to_edge_and_facing_you(self):
        for count in (2, 3, 4):
            with self.subTest(count=count):
                w, gap = self.width(), 0.05
                out = self.m.plan(layout(self.m, count, gap=gap), count)
                for p in out:  # each screen turned to face the eye
                    x, _, z = p["pos"]
                    self.assertAlmostEqual(p["face"][0], math.degrees(math.atan2(-x, -z)), places=3)
                for p, q in zip(out, reversed(out)):  # mirrored left to right
                    self.assertAlmostEqual(p["pos"][0], -q["pos"][0], places=6)
                    self.assertAlmostEqual(p["face"][0], -q["face"][0], places=6)
                # neighbours' facing edges are a gap apart
                def edge(p, side):
                    yaw = math.radians(p["face"][0])
                    return (p["pos"][0] + side * w / 2 * math.cos(yaw), p["pos"][2] - side * w / 2 * math.sin(yaw))
                for a, b in zip(out, out[1:]):
                    self.assertAlmostEqual(math.dist(edge(a, +1), edge(b, -1)), gap, places=3)

    def test_rows_above_what_the_screens_fill_are_trimmed(self):
        out = self.m.plan(layout(self.m, 4, rows=3), 4)  # cols 2: a third row would be empty
        self.assertEqual(len({round(p["face"][1], 6) for p in out}), 2)

    def test_distance_has_a_floor(self):
        (p,) = self.m.plan(layout(self.m, 1, distance=0.0), 1)
        self.assertAlmostEqual(p["pos"][2], -0.3)

    def test_custom_mode_uses_the_saved_poses(self):
        lay = layout(self.m, 2)
        lay["mode"] = "custom"
        lay["screens"][0].update(pos=[1, 0, -1], face=[30, 5], roll=0)
        lay["screens"][1].update(pos=[0, 1, -2])
        out = self.m.plan(lay, 2)
        self.assertEqual(out[0], {"pos": (1, 0, -1), "face": (30, 5), "roll": 0.0})
        self.assertEqual(out[1]["face"], tuple(self.m.yaw_pitch([0, 1, -2])))
        lay["screens"][1].pop("pos")  # one without a pose: the preset for all
        self.assertAlmostEqual(self.m.plan(lay, 2)[0]["pos"][1], 0.0)


class Named(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.lay = layout(self.m, 2)
        self.lay["screens"][0].update(pos=[1, 0, -1], face=[30, 0], roll=0, scale=2)
        self.lay["screens"][1].update(pos=[-1, 0, -1], face=[-30, 0], roll=0)

    def tearDown(self):
        self.tmp.cleanup()

    def test_save_keeps_the_spatial_keys_and_uses_it(self):
        self.assertEqual(self.m.save_named(self.lay, "  Work   desk "), "Work desk")
        self.assertEqual(self.lay["layouts"]["Work desk"][0], {"pos": [1, 0, -1], "face": [30, 0], "roll": 0})
        self.assertEqual((self.lay["mode"], self.lay["active"]), ("custom", "Work desk"))

    def test_names(self):
        for bad in ("", "   ", "x" * 41):
            with self.assertRaises(RuntimeError):
                self.m.check_name(bad)
        self.assertEqual(self.m.check_name("x" * 40), "x" * 40)
        self.m.save_named(self.lay, "b")
        self.m.save_named(self.lay, "A")
        self.assertEqual(self.m.layout_names(self.lay), ["A", "b"])

    def test_nothing_captured_nothing_saved(self):
        with self.assertRaisesRegex(RuntimeError, "nothing to save"):
            self.m.save_named(layout(self.m, 2), "x")

    def test_use_a_layout_saved_with_fewer_screens(self):
        self.lay["screens"] = self.lay["screens"][:1]
        self.m.save_named(self.lay, "one")
        lay = {**layout(self.m, 2), "layouts": self.lay["layouts"]}
        self.m.use_named(lay, "one")
        self.assertEqual(lay["screens"][0]["pos"], [1, 0, -1])
        preset = self.m.plan(layout(self.m, 2), 2)
        self.assertEqual(lay["screens"][1]["pos"], list(preset[1]["pos"]))  # the other: where the preset puts it
        self.assertEqual(lay["screens"][0]["size"], [1920, 1080])  # what isn't spatial stays
        with self.assertRaisesRegex(RuntimeError, "no layout called"):
            self.m.use_named(lay, "gone")

    def test_rename_and_delete_follow_the_profile(self):
        self.m.save_named(self.lay, "a")
        self.lay["default_profile"] = "a"
        self.m.rename_named(self.lay, "a", "b")
        self.assertEqual((list(self.lay["layouts"]), list(self.lay["profiles"]), self.lay["default_profile"],
                          self.lay["active"]), (["b"], ["b"], "b", "b"))
        self.m.save_named(self.lay, "c")
        with self.assertRaisesRegex(RuntimeError, "already a layout"):
            self.m.rename_named(self.lay, "b", "c")
        self.m.delete_named(self.lay, "b")
        self.assertNotIn("default_profile", self.lay)
        self.assertEqual(list(self.lay["layouts"]), ["c"])
        self.assertEqual(self.lay["mode"], "custom")  # the screens stay where they were
        with self.assertRaises(RuntimeError):
            self.m.delete_named(self.lay, "b")


if __name__ == "__main__":
    unittest.main()
