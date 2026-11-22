"""ft-kbhost's small parts, offline: placement.py's spots and touch points, keys.py's presses,
touch.py's forwarded pointer, relaylink.py against a stand-in relay socket."""
import math
import os
import socket
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import keys  # noqa: E402
import placement  # noqa: E402
import relaylink  # noqa: E402
from touch import Touch  # noqa: E402

IDENTITY = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 1.6], [0.0, 0.0, 1.0, 0.0]]


def column(m, c):
    return [m[r][c] for r in range(3)]


class Placement(unittest.TestCase):
    def test_matrix_takes_twelve_finite_numbers(self):
        self.assertEqual(placement.matrix([str(v) for row in IDENTITY for v in row]), IDENTITY)
        self.assertIsNone(placement.matrix(["1"] * 11))
        self.assertIsNone(placement.matrix(["x"] * 12))
        self.assertIsNone(placement.matrix(["nan"] + ["0"] * 11))
        self.assertIsNone(placement.matrix(["1e9"] + ["0"] * 11))

    def test_head_spot_is_ahead_and_under_the_eyes_facing_them(self):
        world = placement.spot("head", IDENTITY, 0.6, 0.25)
        # OpenVR looks down -z: 0.70 m ahead, 0.35 m under the eyes.
        self.assertAlmostEqual(world[0][3], 0.0)
        self.assertAlmostEqual(world[1][3], 1.6 - placement.BELOW)
        self.assertAlmostEqual(world[2][3], -placement.AHEAD)
        # Its normal (+z) leans up toward the eyes by TILT.
        normal = column(world, 2)
        self.assertAlmostEqual(math.degrees(math.asin(normal[1])), placement.TILT, places=4)
        self.assertAlmostEqual(column(world, 0), [1.0, 0.0, 0.0])

    def test_head_spot_ignores_looking_down_and_rolling(self):
        pitch = math.radians(-40)
        looking_down = [[1, 0, 0, 0], [0, math.cos(pitch), -math.sin(pitch), 1.6],
                        [0, math.sin(pitch), math.cos(pitch), 0]]
        a = placement.spot("head", looking_down, 0.6, 0.25)
        b = placement.spot("head", IDENTITY, 0.6, 0.25)
        for r in range(3):
            for c in range(4):
                self.assertAlmostEqual(a[r][c], b[r][c])

    def test_head_spot_turns_with_the_heading(self):
        yaw = math.radians(90)  # turned left: looking down -x
        turned = [[math.cos(yaw), 0, math.sin(yaw), 0], [0, 1, 0, 1.6], [-math.sin(yaw), 0, math.cos(yaw), 0]]
        world = placement.spot("head", turned, 0.6, 0.25)
        self.assertAlmostEqual(world[0][3], -placement.AHEAD)
        self.assertAlmostEqual(world[2][3], 0.0)

    def test_dash_spot_hangs_under_the_page(self):
        width, height = 0.6, 0.25
        world = placement.spot("dash", IDENTITY, width, height)
        self.assertLess(world[1][3], 1.6 - placement.DASH_GAP)   # under the edge
        self.assertAlmostEqual(world[2][3], placement.DASH_FORWARD)   # toward the viewer
        # Its top edge reaches up to DASH_GAP under the page's bottom edge.
        top = world[1][3] + column(world, 1)[1] * height / 2
        self.assertAlmostEqual(top, 1.6 - placement.DASH_GAP)

    def test_touch_maps_a_point_to_texture_coordinates(self):
        panel = placement.spot("head", IDENTITY, 0.6, 0.25)
        centre = column(panel, 3)
        self.assertEqual(tuple(round(x, 6) for x in placement.touch(centre, panel, 0.6, 0.25)), (0.5, 0.5))
        right_top = [centre[r] + column(panel, 0)[r] * 0.3 + column(panel, 1)[r] * 0.125 for r in range(3)]
        u, v = placement.touch(right_top, panel, 0.6, 0.25)
        self.assertAlmostEqual(u, 1.0)
        self.assertAlmostEqual(v, 0.0)


class Keys(unittest.TestCase):
    def test_modifiers_wrap_the_key(self):
        self.assertEqual(keys.presses("KeyA", ["Ctrl", "Shift"]), [(29, 1), (42, 1), (30, 1), (30, 0), (42, 0), (29, 0)])
        self.assertEqual(keys.presses("Enter", []), [(28, 1), (28, 0)])
        self.assertEqual(keys.presses("AltRight", ["AltGr"]), [(100, 1), (100, 0)])   # not held twice

    def test_unknown_keys_and_modifiers(self):
        self.assertEqual(keys.presses("Emoji", ["Ctrl"]), [])
        self.assertEqual(keys.presses("KeyB", ["Hyper", "Shift", "Shift"]), [(42, 1), (48, 1), (48, 0), (42, 0)])


class TouchTest(unittest.TestCase):
    GEOMETRY = (placement.spot("head", IDENTITY, 0.6, 0.25), 0.6, 0.25)

    def setUp(self):
        self.sent = []
        self.touch = Touch(lambda op, **f: self.sent.append((op, f)))

    def centre_words(self, side, verb):
        c = column(self.GEOMETRY[0], 3)
        return [side, verb, *(f"{x:.4f}" for x in c)]

    def test_a_press_forwards_then_goes_native_when_idle(self):
        self.touch.poke(0.0, self.centre_words("right", "press"), self.GEOMETRY)
        self.touch.poke(0.1, self.centre_words("right", "up"), self.GEOMETRY)
        ops = [(op, f.get("phase")) for op, f in self.sent]
        self.assertEqual(ops, [("pointer_mode", None), ("pointer", "move"), ("pointer", "down"), ("pointer", "up"),
                               ("pointer_mode", None)])
        self.assertEqual(self.sent[0][1], {"mode": "forwarded"})
        self.assertAlmostEqual(self.sent[2][1]["u"], 0.5, places=3)
        self.assertEqual(self.sent[-1], ("pointer_mode", {"mode": "native"}))   # its X acts now

    def test_a_hover_forwards_then_goes_native_when_idle(self):
        self.touch.poke(0.0, self.centre_words("right", "hover"), self.GEOMETRY)
        self.touch.tick(0.5)
        self.assertEqual(self.sent[-1][0], "pointer")   # not yet idle
        self.touch.tick(0.6)
        self.assertEqual(self.sent[-1], ("pointer_mode", {"mode": "native"}))

    def test_a_release_stays_forwarded_while_the_other_hand_presses(self):
        self.touch.poke(0.0, self.centre_words("left", "press"), self.GEOMETRY)
        self.touch.poke(0.0, self.centre_words("right", "press"), self.GEOMETRY)
        self.touch.poke(0.1, self.centre_words("right", "up"), self.GEOMETRY)
        self.assertNotIn(("pointer_mode", {"mode": "native"}), self.sent)
        self.touch.poke(0.2, self.centre_words("left", "up"), self.GEOMETRY)
        self.assertEqual(self.sent[-1], ("pointer_mode", {"mode": "native"}))
        self.touch.poke(0.3, self.centre_words("left", "press"), self.GEOMETRY)   # the next tap forwards again
        self.assertIn(("pointer", {"hand": "left", "phase": "down", "u": 0.5, "v": 0.5}), self.sent[-3:])

    def test_a_stuck_press_is_cancelled(self):
        self.touch.poke(0.0, self.centre_words("left", "press"), self.GEOMETRY)
        self.touch.tick(1.0)
        self.assertNotIn(("pointer", {"hand": "left", "phase": "cancel"}), self.sent)   # held: 3 s
        self.touch.tick(3.0)
        self.assertIn(("pointer", {"hand": "left", "phase": "cancel"}), self.sent)
        self.assertEqual(self.sent[-1], ("pointer_mode", {"mode": "native"}))

    def test_a_stuck_press_is_cancelled_while_the_other_hand_pokes(self):
        self.touch.poke(0.0, self.centre_words("left", "press"), self.GEOMETRY)   # its up is lost
        for t in (0.5, 1.0, 1.5, 2.0, 2.5, 3.0):
            self.touch.poke(t, self.centre_words("right", "hover"), self.GEOMETRY)
            self.touch.tick(t)
        self.assertIn(("pointer", {"hand": "left", "phase": "cancel"}), self.sent)
        self.assertNotIn(("pointer_mode", {"mode": "native"}), self.sent)   # the right hand is still there
        self.touch.tick(3.0 + 0.6)
        self.assertEqual(self.sent[-1], ("pointer_mode", {"mode": "native"}))

    def test_malformed_pokes_and_no_geometry(self):
        self.touch.poke(0.0, self.centre_words("right", "press"), None)
        self.touch.poke(0.0, ["middle", "press", "0", "0", "0"], self.GEOMETRY)
        self.touch.poke(0.0, ["right", "press", "x", "0", "0"], self.GEOMETRY)
        self.assertEqual(self.sent, [])


class HandleTest(unittest.TestCase):
    PANEL = placement.spot("head", IDENTITY, 0.6, 0.25)
    GEOMETRY = (PANEL, 0.6, 0.25, 0.772)   # the case ends 77% down

    def setUp(self):
        self.sent = []
        self.touch = Touch(lambda op, **f: self.sent.append((op, f)))

    def at(self, side, verb, local_x, local_y, along=0.0):
        """A poke at a panel-local point (m), `along` off the panel's face."""
        m = self.PANEL
        p = [m[r][3] + m[r][0] * local_x + m[r][1] * local_y + m[r][2] * along for r in range(3)]
        return [side, verb, *(f"{x:.4f}" for x in p)]

    def test_the_strip_under_the_case_drags_the_panel_in_its_plane(self):
        y = 0.25 * (0.5 - 0.9)   # v = 0.9: under the case
        self.touch.poke(0.0, self.at("right", "hover", 0.0, y), self.GEOMETRY)
        self.assertEqual(self.sent, [])   # not forwarded: the keyboard's line stays shown
        self.touch.poke(0.1, self.at("right", "press", 0.0, y), self.GEOMETRY)
        self.touch.poke(0.2, self.at("right", "move", 0.10, y + 0.05, along=0.03), self.GEOMETRY)
        self.touch.poke(0.3, self.at("right", "up", 0.10, y + 0.05), self.GEOMETRY)
        poses = [f["matrix"] for op, f in self.sent if op == "set_pose"]
        self.assertEqual(len(poses), 2)
        want = [self.PANEL[r][3] + self.PANEL[r][0] * 0.10 + self.PANEL[r][1] * 0.05 for r in range(3)]
        for r in range(3):
            self.assertAlmostEqual(poses[-1][r][3], want[r], places=3)   # off the face doesn't count
            self.assertEqual(poses[-1][r][:3], self.PANEL[r][:3])        # it only slides
        self.assertNotIn("pointer", [op for op, _ in self.sent])
        self.touch.poke(0.4, self.at("right", "press", 0.0, 0.0), self.GEOMETRY)   # a key, after
        self.assertEqual(self.sent[-1][1]["phase"], "down")

    def test_a_two_hand_turn_turns_it_and_the_slide_goes_on_from_there(self):
        y = 0.25 * (0.5 - 0.9)
        self.touch.poke(0.0, self.at("right", "press", 0.0, y), self.GEOMETRY)
        self.touch.turn(0.1, ["begin"])
        c, s_ = math.cos(0.3), math.sin(0.3)
        p = [self.PANEL[r][3] for r in range(3)]
        # a yaw of 0.3 rad (about its own centre) and 5 cm up
        rot = [[c, 0.0, s_], [0.0, 1.0, 0.0], [-s_, 0.0, c]]
        t = [0.0, 0.05, 0.0]
        move = ["move"] + [f"{v:.5f}" for r in range(3) for v in (*rot[r], t[r])]
        self.touch.turn(0.2, move)
        turned = [f["matrix"] for op, f in self.sent if op == "set_pose"][-1]
        for r in range(3):
            self.assertAlmostEqual(turned[r][3], p[r] + t[r], places=4)   # about its centre, then moved
        self.assertAlmostEqual(turned[0][0], sum(rot[0][k] * self.PANEL[k][0] for k in range(3)), places=4)
        n = len(self.sent)
        self.touch.poke(0.25, self.at("right", "move", 0.2, y), self.GEOMETRY)   # the carry waits meanwhile
        self.assertEqual(len(self.sent), n)
        self.touch.turn(0.3, ["end"])
        self.touch.poke(0.4, self.at("right", "move", 0.3, y), self.GEOMETRY)    # from here: no jump
        after = [f["matrix"] for op, f in self.sent if op == "set_pose"][-1]
        for r in range(3):
            self.assertAlmostEqual(after[r][3], turned[r][3], places=4)

    def test_a_turn_without_a_handle_drag_does_nothing(self):
        self.touch.turn(0.0, ["begin"])
        self.touch.turn(0.1, ["move"] + ["0"] * 12)
        self.assertEqual(self.sent, [])

    def test_a_hide_or_a_lost_up_ends_a_handle_drag(self):
        y = 0.25 * (0.5 - 0.9)
        self.touch.poke(0.0, self.at("right", "press", 0.0, y), self.GEOMETRY)
        self.touch.hidden()
        self.touch.turn(0.1, ["begin"])
        self.touch.turn(0.2, ["move"] + ["1", "0", "0", "0", "0", "1", "0", "0", "0", "0", "1", "0.1"])
        self.assertNotIn("set_pose", [op for op, _ in self.sent])
        self.touch.poke(1.0, self.at("right", "press", 0.0, y), self.GEOMETRY)
        self.touch.tick(1.0 + 3.0)   # its up never came
        self.touch.turn(4.1, ["begin"])
        self.touch.turn(4.2, ["move"] + ["1", "0", "0", "0", "0", "1", "0", "0", "0", "0", "1", "0.1"])
        self.assertNotIn("set_pose", [op for op, _ in self.sent])

    def test_a_key_press_sliding_onto_the_strip_stays_the_keys(self):
        self.touch.poke(0.0, self.at("left", "press", 0.0, 0.0), self.GEOMETRY)
        self.touch.poke(0.1, self.at("left", "move", 0.0, 0.25 * (0.5 - 0.9)), self.GEOMETRY)
        self.assertEqual(self.sent[-1][1].get("phase"), "move")
        self.assertNotIn("set_pose", [op for op, _ in self.sent])

    def test_without_the_case_bottom_there_is_no_handle(self):
        self.touch.poke(0.0, self.at("right", "press", 0.0, 0.25 * (0.5 - 0.9)), self.GEOMETRY[:3])
        self.assertEqual(self.sent[-1][1].get("phase"), "down")


class RelayLinkTest(unittest.TestCase):
    def setUp(self):
        self.name = f"\0ft-kbhost-test-{os.getpid()}".encode()
        self.relay = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.relay.bind(self.name)
        self.relay.settimeout(0.5)
        self.link = relaylink.RelayLink(self.name)

    def tearDown(self):
        self.link.close()
        self.relay.close()

    def said(self):
        data, addr = self.relay.recvfrom(256)
        return data.decode(), addr

    def test_word_from_the_relay_only(self):
        self.link.hello(True)
        text, addr = self.said()
        self.assertEqual(text, "kbhello 1")
        self.relay.sendto(b"kb 1 1 0", addr)
        self.relay.sendto(b"other words", addr)
        stranger = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        stranger.bind("")
        stranger.sendto(b"kb show", addr)
        stranger.close()
        import time
        time.sleep(0.05)
        self.assertEqual(self.link.receive(10.0), [["1", "1", "0"]])
        self.assertTrue(self.link.fresh(10.0 + relaylink.STALE - 0.1))
        self.assertFalse(self.link.fresh(10.0 + relaylink.STALE))

    def test_typing_only_while_fresh_and_releases_owed(self):
        self.assertFalse(self.link.text(0.0, "a"))   # never heard from it
        self.link.heard_at = 0.0
        self.assertTrue(self.link.text(1.0, "hé"))
        self.assertEqual(self.said()[0], "kbtext hé")
        self.link.address = b"\0ft-kbhost-test-nobody"
        self.assertFalse(self.link.key(1.0, 30, True))    # a press that doesn't go is dropped
        self.assertFalse(self.link.key(1.0, 30, False))   # a release is owed
        self.assertEqual(self.link.owed, [30])
        self.link.address = self.name
        self.link.flush()
        self.assertEqual(self.said()[0], "kbkey 30 0")
        self.assertEqual(self.link.owed, [])


if __name__ == "__main__":
    unittest.main()
