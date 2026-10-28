"""Display Settings' preview geometry (ft_layout.preview), without a headset or real configuration."""
import importlib.util
import json
import math
import pathlib
import tempfile
import unittest


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("preview_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    module.ANCHOR_PATH = str(tmp / "state" / "anchor.json")
    return module


def close(a, b, tol=1e-3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


class Preview(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def layout(self, **extra):
        layout = json.loads(json.dumps(self.m.DEFAULTS))
        layout["screens"] = [{"size": [1920, 1080], "metres": 2.0}]
        layout.update(extra)
        return layout

    def world(self, anchor, pos, face, roll=0.0):
        """A panel placed in the anchor's head frame, as ft-screens' world pose."""
        eye, heading = anchor
        x, y, z = self.m.panel_axes((face[0] + heading, face[1]), roll)
        centre = tuple(e + v for e, v in zip(eye, self.m.turn_yaw(pos, heading)))
        return centre, x, y, z

    @staticmethod
    def get_reply(centre, x, y, z, metres, height, curve=0.0):
        nums = [*centre, *x, *y, *z, metres, height, curve]
        return "ok " + " ".join(f"{v:.5f}" for v in nums) + " none"

    def test_axes_read_back_as_the_same_face_and_roll(self):
        for face, roll in (((30.0, 10.0), 5.0), ((109.04, -1.77), 0.06), ((-150.0, -40.0), -12.0)):
            x, _, z = self.m.panel_axes(face, roll)
            got = self.m.relative_pose((0.0, 0.0, -2.0), x, z, (0.0, 0.0, 0.0), 0.0)
            self.assertTrue(close(got["face"], face, 0.02), (got, face))
            self.assertAlmostEqual(got["roll"], roll, delta=0.02)

    def test_a_curved_panel_bends_round_its_front(self):
        p = {"center": (0.0, 0.0, -2.0), "axes": self.m.panel_axes((0.0, 0.0), 0.0), "curve": 2.0}
        left, right = self.m.surface_point(p, -1.5, 0.0), self.m.surface_point(p, 1.5, 0.0)
        self.assertGreater(left[2], -2.0)   # the edges come toward you
        self.assertGreater(right[2], -2.0)
        self.assertLess(right[0] - left[0], 3.0)  # the chord is shorter than the panel
        axis = (0.0, 0.0, 0.0)  # its cylinder's axis: radius 2 in front of its middle
        self.assertAlmostEqual(math.dist((right[0], right[2]), (axis[0], axis[2])), 2.0, places=6)
        flat = dict(p, curve=0.0)
        self.assertTrue(close(self.m.surface_point(flat, 1.5, 0.5), (1.5, 0.5, -2.0)))

    def test_the_layout_alone_is_drawn_from_where_it_is_arranged(self):
        # The user's (2026-10-28): screen 1 off to the left and behind, facing you.
        layout = self.layout(mode="custom")
        layout["screens"][0].update(pos=[-2.0294, -0.0314, 0.8345], face=[109.04, -1.77], roll=0.06, curve=2.034)
        view = self.m.preview(layout)
        self.assertFalse(view["live"])
        self.assertFalse(view["head"]["tracked"])
        (screen,) = view["planned"]
        outline = screen["view"]
        self.assertTrue(all(60 < yaw < 160 for yaw, _ in outline), outline)  # to the left, past the side
        middle = screen["top"][len(screen["top"]) // 2]
        self.assertTrue(close(middle, (-2.0294, 0.8345)), middle)  # its centre, from above
        yaw, pitch = self.m.yaw_pitch((-2.0294, -0.0314, 0.8345))
        self.assertAlmostEqual(yaw, 112.35, places=1)

    def test_a_panel_behind_you_stays_in_one_piece(self):
        p = {"center": (0.0, 0.0, 2.0), "axes": self.m.panel_axes((180.0, 0.0), 0.0), "curve": 0.0,
             "width": 2.0, "height": 1.0}
        yaws = [q[0] for q in self.m.preview_panel(p, (0.0, 0.0, 0.0))["view"]]
        self.assertLess(max(yaws) - min(yaws), 90)
        self.assertTrue(all(abs(abs(y) - 180) < 45 for y in yaws))

    def test_from_where_you_are_is_round_the_way_you_face(self):
        # Turned 170 deg left of where it was arranged: a screen 15 deg to your left is beside
        # where you face, not a turn away at -175.
        p = {"center": tuple(self.m.turn_yaw((0.0, 0.0, -2.0), 185.0)), "axes": self.m.panel_axes((185.0, 0.0), 0.0),
             "curve": 0.0, "width": 0.5, "height": 0.3}
        yaws = [q[0] for q in self.m.preview_panel(p, (0.0, 0.0, 0.0), 170.0)["view"]]
        self.assertTrue(all(170 < y < 200 for y in yaws), yaws)

    def test_live_panels_and_head_are_in_the_layouts_frame(self):
        anchor = ((1.0, 1.6, -0.5), 90.0)  # arranged facing left in the world
        layout = self.layout()
        self.m.save_anchor(*anchor)
        self.assertEqual(self.m.stored_anchor(), anchor)
        layout["hosts"] = [{"name": "oma7", "displays": [{"id": "oma7-dp-2", "label": "DP-2", "screen": 106,
                                                           "size": [5120, 1440], "metres": 5.0}]}]
        screen = self.world(anchor, (0.0, 0.0, -2.0), (0.0, 0.0))
        remote = self.world(anchor, (0.0, 0.5, -3.0), (0.0, 5.0))
        window = self.world(anchor, (1.0, 0.0, -1.0), (-45.0, 0.0))
        replies = {"screens": "ok 1 1:1920x1080:1",
                   "head": "ok 1.5000 1.6000 -0.5000 120.00",  # 0.5 m along world +x, turned 30 deg left
                   "remotes": "ok 1 106:frametop-oma7-dp-2:live:5120x1440",
                   "get 1": self.get_reply(*screen, 2.0, 1.125),
                   "get 106": self.get_reply(*remote, 5.0, 1.4, 2.0),
                   "get 2": self.get_reply(*window, 1.0, 0.75)}
        room = self.m.read_room(replies.get, layout, [(2, "Frametop Display Settings")])
        view = self.m.preview(layout, room)
        self.assertTrue(view["live"])
        self.assertTrue(view["head"]["tracked"])
        # The head: 0.5 m along world +x is, for an anchor facing world -x (heading 90), behind it.
        self.assertTrue(close((view["head"]["x"], view["head"]["z"]), (0.0, 0.5)), view["head"])
        self.assertAlmostEqual(view["head"]["yaw"], 30.0, places=2)
        kinds = {p["kind"]: p for p in view["panels"]}
        self.assertEqual(set(kinds), {"screen", "remote", "window"})
        self.assertEqual(kinds["remote"]["label"], "oma7 DP-2")
        self.assertEqual(kinds["window"]["label"], "Frametop Display Settings")
        top = kinds["screen"]["top"]
        self.assertTrue(close(top[len(top) // 2], (0.0, -2.0)), top)
        # The layout puts screen 1 where it is: no planned outline away from it.
        planned = view["planned"][0]["top"]
        self.assertTrue(close(planned[len(planned) // 2], (0.0, -2.0), 0.01), planned)

    def test_without_a_saved_anchor_screen_1_tells_where_it_was_arranged_from(self):
        anchor = ((-0.3, 1.2, 0.4), -35.0)
        layout = self.layout(mode="custom")
        layout["screens"][0].update(pos=[0.5, 0.1, -1.8], face=[-15.0, 3.0], roll=0.0)
        g = self.m.parse_get(self.get_reply(*self.world(anchor, (0.5, 0.1, -1.8), (-15.0, 3.0)), 2.0, 1.125))
        eye, heading = self.m.inferred_anchor(layout, g)
        self.assertTrue(close(eye, anchor[0]))
        self.assertAlmostEqual(heading, anchor[1], places=2)

    def test_with_the_headset_asleep_you_are_where_it_was_arranged_from(self):
        anchor = ((2.0, 1.5, -1.0), 60.0)
        layout = self.layout()
        self.m.save_anchor(*anchor)
        screen = self.world(anchor, (0.0, 0.0, -2.0), (0.0, 0.0))
        replies = {"screens": "ok 1 1:1920x1080:1", "head": "error no head pose (headset off?)",
                   "remotes": "ok 0", "get 1": self.get_reply(*screen, 2.0, 1.125)}
        view = self.m.preview(layout, self.m.read_room(replies.get, layout))
        self.assertTrue(view["live"])
        self.assertFalse(view["head"]["tracked"])
        self.assertTrue(close((view["head"]["x"], view["head"]["z"], view["head"]["yaw"]), (0.0, 0.0, 0.0)))
        top = view["panels"][0]["top"]
        self.assertTrue(close(top[len(top) // 2], (0.0, -2.0)), top)  # 2 m ahead of where it was arranged from

    def test_without_a_saved_anchor_the_room_is_drawn_from_screen_1(self):
        anchor = ((-0.3, 1.2, 0.4), -35.0)
        layout = self.layout(mode="custom")
        layout["screens"][0].update(pos=[0.5, 0.1, -1.8], face=[-15.0, 3.0], roll=0.0)
        screen = self.world(anchor, (0.5, 0.1, -1.8), (-15.0, 3.0))
        head = self.m.turn_yaw((0.0, 0.0, 0.5), anchor[1])  # half a metre behind where it was arranged from
        head = tuple(a + b for a, b in zip(anchor[0], head))
        replies = {"screens": "ok 1 1:1920x1080:1", "head": "ok %.4f %.4f %.4f %.2f" % (*head, anchor[1] + 10),
                   "remotes": "ok 0", "get 1": self.get_reply(*screen, 2.0, 1.125)}
        view = self.m.preview(layout, self.m.read_room(replies.get, layout))
        self.assertTrue(close((view["head"]["x"], view["head"]["z"]), (0.0, 0.5)), view["head"])
        self.assertAlmostEqual(view["head"]["yaw"], 10.0, places=1)
        self.assertTrue(all(close(a, b) for a, b in zip(view["panels"][0]["top"], view["planned"][0]["top"])))

    def test_with_nothing_to_place_the_room_by_the_layout_alone_is_drawn(self):
        layout = self.layout()
        pinned = self.get_reply(*self.world(((0, 0, 0), 0), (0.0, 0.0, -2.0), (0.0, 0.0)), 2.0, 1.125).replace(" none", " left")
        replies = {"screens": "ok 1 1:1920x1080:1", "head": "error no head pose (headset off?)",
                   "remotes": "ok 0", "get 1": pinned + " 1 0 0 0 0 1 0 0 0 0 1 -0.3"}
        view = self.m.preview(layout, self.m.read_room(replies.get, layout))
        self.assertFalse(view["live"])
        self.assertFalse(self.m.preview(layout, {"head": None, "panels": []})["live"])

    def test_a_moved_screen_is_away_from_where_the_layout_puts_it(self):
        anchor = ((0.0, 1.6, 0.0), 0.0)
        layout = self.layout()
        self.m.save_anchor(*anchor)
        moved = self.world(anchor, (1.0, 0.0, -2.0), (0.0, 0.0))  # a metre to the right of its place
        replies = {"screens": "ok 1 1:1920x1080:1", "head": "ok 0 1.6 0 0", "remotes": "ok 0",
                   "get 1": self.get_reply(*moved, 2.0, 1.125)}
        view = self.m.preview(layout, self.m.read_room(replies.get, layout))
        live, planned = view["panels"][0]["top"], view["planned"][0]["top"]
        self.assertTrue(close(live[len(live) // 2], (1.0, -2.0)), live)
        self.assertTrue(close(planned[len(planned) // 2], (0.0, -2.0)), planned)

    def test_a_panel_that_does_not_answer_leaves_the_rest(self):
        anchor = ((0.0, 1.6, 0.0), 0.0)
        layout = self.layout()
        screen = self.world(anchor, (0.0, 0.0, -2.0), (0.0, 0.0))
        replies = {"screens": "ok 1 1:1920x1080:1", "head": "ok 0 1.6 0 0",
                   "remotes": "ok 2 105:frametop-oma7-edp-2:lost:2560x1600 106:frametop-oma7-dp-2:live:5120x1440",
                   "get 1": self.get_reply(*screen, 2.0, 1.125), "get 105": "error no screen 105",
                   "get 106": self.get_reply(*screen, 5.0, 1.4)}
        room = self.m.read_room(replies.get, layout, [(2, "gone")])
        self.assertEqual([(p["kind"], p["number"]) for p in room["panels"]], [("screen", 1), ("remote", 106)])
        self.assertTrue(self.m.preview(layout, room)["live"])

    def test_capturing_the_screens_keeps_where_they_were_captured_from(self):
        anchor = ((0.5, 1.6, -0.2), 25.0)
        layout = self.layout()
        self.m.save_layout(layout)
        screen = self.world(anchor, (0.0, 0.2, -1.5), (0.0, 0.0))
        replies = {"head": "ok %.4f %.4f %.4f %.2f" % (*anchor[0], anchor[1]), "get 1": self.get_reply(*screen, 2.0, 1.125)}
        self.m.screens_socket = lambda: type("Sock", (), {"ask": staticmethod(replies.get)})()
        self.m.capture_screens()
        self.assertEqual(self.m.stored_anchor(), anchor)
        self.assertTrue(close(self.m.load_layout()["screens"][0]["pos"], (0.0, 0.2, -1.5)))

    def test_desktop_start_forgets_where_the_screens_were_arranged_from(self):
        self.m.save_anchor((1.0, 1.6, 0.0), 30.0)
        self.m.load_layout = lambda: {**self.layout(), "mode": "preset"}  # no start profile: "apply" next
        calls = []
        real_main = self.m.main
        self.m.main = lambda argv: calls.append(argv) if argv[1] == "apply" else real_main(argv)
        real_main(["ft-layout", "start"])
        self.assertIsNone(self.m.stored_anchor())
        self.assertEqual(calls, [["ft-layout", "apply"]])

    def test_a_broken_anchor_file_is_no_anchor(self):
        path = pathlib.Path(self.m.ANCHOR_PATH)
        path.parent.mkdir(parents=True)
        for text in ("{", '{"eye": [1], "heading": 0}', '{"eye": "abc", "heading": 0}', "[]"):
            path.write_text(text)
            self.assertIsNone(self.m.stored_anchor(), text)

    def test_an_anchor_that_cannot_be_saved_only_logs(self):
        logged = []
        self.m.log = logged.append
        blocker = pathlib.Path(self.tmp.name) / "file"
        blocker.write_text("")
        self.m.ANCHOR_PATH = str(blocker / "anchor.json")  # under a file: no folder can be made
        self.m.save_anchor((0.0, 0.0, 0.0), 0.0)
        self.assertEqual(len(logged), 1, logged)
        self.assertIsNone(self.m.stored_anchor())

    def test_no_compositor_no_room(self):
        self.assertIsNone(self.m.read_room(lambda text: None, self.layout()))


if __name__ == "__main__":
    unittest.main()
