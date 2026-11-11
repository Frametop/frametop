"""Moving through the room (together/roompanels/moving.py): where you go to, and that going
there moves your floor in the room and everything that stands here the other way, so the room
stands still. Against the stand-ins for ft-together and ft-screens that answer as they do.

    python3 together/tests/test_roommoving.py
"""
import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fakescreens import Screens  # noqa: E402
from roompanels import keeper, moving, placing  # noqa: E402
from test_roompanels import Together, near, turn_about_up  # noqa: E402


def in_room(floor, at):
    """A place on the floor (x, y, z), in the room."""
    t = placing.turned(floor[2], at)
    return (t[0] + floor[0], t[1], t[2] + floor[1])


class Sums(unittest.TestCase):
    def test_one_floor_on_another_is_both_moves_one_after_the_other(self):
        rng = random.Random(11)
        for _ in range(100):
            outer, inner = [(rng.uniform(-9, 9), rng.uniform(-9, 9), rng.uniform(-3, 3)) for _ in range(2)]
            at = (rng.uniform(-3, 3), rng.uniform(0, 2), rng.uniform(-3, 3))
            self.assertTrue(near(in_room(placing.then(outer, inner), at), in_room(outer, in_room(inner, at)), 1e-9))

    def test_the_way_back_moves_nothing_either_way_round(self):
        rng = random.Random(12)
        for _ in range(100):
            f = (rng.uniform(-9, 9), rng.uniform(-9, 9), rng.uniform(-3, 3))
            for both in (placing.then(f, placing.back(f)), placing.then(placing.back(f), f)):
                self.assertTrue(near(both, (0, 0, 0), 1e-9), both)

    def test_beside_someone_is_in_front_of_them_looking_back(self):
        rng = random.Random(13)
        for _ in range(50):
            eyes, yaw = (rng.uniform(-9, 9), rng.uniform(-9, 9)), rng.uniform(-3, 3)
            x, z, facing = moving.beside(eyes, yaw)
            ahead = placing.turned(yaw, (0, 0, -1))          # the way they look
            self.assertTrue(near((x - eyes[0], z - eyes[1]), (ahead[0] * moving.BESIDE, ahead[2] * moving.BESIDE), 1e-9))
            back = placing.turned(facing, (0, 0, -1))        # the way you look, standing there
            self.assertTrue(near((back[0], back[2]), (-ahead[0], -ahead[2]), 1e-9))

    def test_someone_looking_down_the_rooms_z_is_met_a_step_further_down_it(self):
        self.assertTrue(near(moving.beside((2.0, 3.0), 0.0), (2.0, 1.8, -math.pi), 1e-9))     # 1.2 m: near enough to talk

    def panel(self, at=(0.0, 1.5, -2.0), yaw=0.0, metres=1.5, turn=None):
        return {"at": list(at), "turn": turn or placing.quat(*turn_about_up(yaw)), "metres": metres, "curve": 0.0}

    def test_a_panel_is_read_from_in_front_of_it_facing_it(self):
        x, z, facing = moving.reading(self.panel())            # it faces +z, as its owner at the middle sees it
        away = 0.75 / math.tan(math.radians(30))
        self.assertTrue(near((x, z, facing), (0.0, -2.0 + away, 0.0), 1e-9), (x, z, facing))
        rng = random.Random(14)
        for _ in range(50):
            yaw, at = rng.uniform(-3, 3), (rng.uniform(-5, 5), 1.4, rng.uniform(-5, 5))
            x, z, facing = moving.reading(self.panel(at, yaw))
            front = placing.turned(yaw, (0, 0, 1))
            self.assertTrue(near((x - at[0], z - at[2]), (front[0] * away, front[2] * away), 1e-6))
            look = placing.turned(facing, (0, 0, -1))
            self.assertTrue(near((look[0], look[2]), (-front[0], -front[2]), 1e-6))

    def test_it_fills_the_same_part_of_the_view_whatever_its_size_within_reason(self):
        for metres, want in ((1.0, 0.5 / math.tan(math.radians(30))), (3.0, 1.5 / math.tan(math.radians(30))),
                             (0.15, moving.NEAREST), (0.5, moving.NEAREST), (20.0, moving.FARTHEST), (4.0, moving.FARTHEST)):
            x, z, _ = moving.reading(self.panel(metres=metres))
            self.assertAlmostEqual(z + 2.0, want)
            self.assertAlmostEqual(x, 0.0)

    def test_a_tilted_one_is_read_from_the_floor_in_front_of_it(self):
        tipped = placing.quat((1, 0, 0), (0, math.cos(0.5), math.sin(0.5)), (0, -math.sin(0.5), math.cos(0.5)))   # leaning back
        x, z, facing = moving.reading(self.panel(turn=tipped))
        self.assertTrue(near((x, facing), (0.0, 0.0), 1e-9) and z > -2.0)

    def test_one_lying_flat_is_read_from_its_lower_edge(self):
        flat = placing.quat((1, 0, 0), (0, 0, -1), (0, 1, 0))     # facing up, its top away along -z
        x, z, facing = moving.reading(self.panel(at=(1.0, 0.8, -1.0), turn=flat))
        self.assertTrue(near((x, facing), (1.0, 0.0), 1e-9) and z > -1.0, (x, z, facing))

    def test_no_turn_at_all_is_read_as_facing_its_owner(self):
        self.assertIsNotNone(moving.reading(self.panel(turn=(0, 0, 0, 0))))


class MoverCase(unittest.TestCase):
    def setUp(self):
        self.screens, self.together = Screens(), Together()
        self.together.floor = (2.0, -1.0, 0.7)
        self.mover = moving.Mover(self.together, self.screens)
        self.screens("remote 116 peer 0 1280x720 1.5 Screen 1")       # someone's panel, standing here
        self.screens("pose 116 " + " ".join(f"{v:.5f}" for v in placing.to_floor(self.together.floor, {"at": (0.0, 1.5, -2.0), "turn": (1.0, 0.0, 0.0, 0.0)})))
        self.before = self.stands()

    def head(self):
        """The head in the room: (x, z, the way it faces)."""
        x, _, z, degrees = (float(w) for w in self.screens.head.split()[1:])
        at = in_room(self.together.floor, (x, 0.0, z))
        return (at[0], at[2], placing.wrap(math.radians(degrees) + self.together.floor[2]))

    def stands(self):
        """Where everything that stands here is in the room: each screen, and the panel."""
        floor = self.together.floor
        got = {n: placing.to_room(floor, placing.parse_get(self.screens(f"get {n}"))) for n in self.screens.at}
        pose = self.screens.panels[116]["pose"]
        got[116] = placing.to_room(floor, {"at": pose[3::4], "x": pose[0::4], "y": pose[1::4], "z": pose[2::4], "metres": 1.5, "curve": 0.0})
        return {n: (g["at"], g["turn"]) for n, g in got.items()}

    def assertRoomStoodStill(self, by=2e-3):
        now = self.stands()
        for n, (at, turn) in self.before.items():
            same_turn = near(now[n][1], turn, by) or near(now[n][1], [-v for v in turn], by)
            self.assertTrue(near(now[n][0], at, by) and same_turn, (n, now[n], (at, turn)))

    def assertFacing(self, got, want, by=1e-3):
        self.assertTrue(abs(placing.wrap(got - want)) < by, (got, want))


class Going(MoverCase):
    def test_to_a_spot_puts_the_head_on_it_facing_its_way(self):
        reply = self.mover.go((-3.0, 4.5, 2.0))
        self.assertTrue(reply.startswith("ok "), reply)
        x, z, facing = self.head()
        self.assertTrue(near((x, z), (-3.0, 4.5), 2e-3), (x, z))
        self.assertFacing(facing, 2.0)
        self.assertRoomStoodStill()

    def test_it_says_where_the_floor_lies_now(self):
        words = self.mover.go((-3.0, 4.5, 2.0)).split()
        floor = self.together.floor
        self.assertTrue(near([float(w) for w in words[1:]], (floor[0], floor[1], math.degrees(floor[2])), 0.06), words)

    def test_the_floor_follows_what_ft_screens_was_told_to_the_last_digit(self):
        was = self.together.floor
        self.mover.go((0.123456789, -7.987654321, 1.23456789))
        dx, dz, yaw = self.screens.shifted[-1]
        self.assertTrue(near(placing.then(self.together.floor, (dx, dz, yaw)), was, 1e-5))   # floor then shift is the floor it was

    def test_to_someone_is_in_front_of_them_facing_them(self):
        self.together.seen[0] = (5.0, 1.7, -2.0, 1.0)
        self.assertTrue(self.mover.go_person(0).startswith("ok "))
        x, z, facing = self.head()
        want = moving.beside((5.0, -2.0), 1.0)
        self.assertTrue(near((x, z), want[:2], 2e-3), ((x, z), want))
        self.assertFacing(facing, want[2])
        self.assertAlmostEqual(math.hypot(x - 5.0, z + 2.0), moving.BESIDE, places=2)
        self.assertRoomStoodStill()

    def test_someone_whose_poses_dont_come_is_met_at_their_spot(self):
        self.assertTrue(self.mover.go_person(0).startswith("ok "))     # ada's spot is the room's middle, facing -z
        x, z, _ = self.head()
        self.assertTrue(near((x, z), (0.0, -moving.BESIDE), 2e-3), (x, z))

    def test_their_spot_is_a_place_on_the_rooms_floor_and_the_way_it_faces(self):
        self.together.ada = [3.0, 0.25, -4.0, 1.5]      # x, up, z, the way it faces
        self.assertTrue(self.mover.go_person(0).startswith("ok "))
        want = moving.beside((3.0, -4.0), 1.5)
        x, z, facing = self.head()
        self.assertTrue(near((x, z), want[:2], 2e-3), ((x, z), want))
        self.assertFacing(facing, want[2])

    def test_rubbish_about_where_someone_is_counts_as_not_known(self):
        self.together.__class__ = type("Odd", (Together,), {"__call__": lambda s, text: "ok 1 0:nan:1:2:3 0:1:2 x" if text == "where" else Together.__call__(s, text)})
        self.assertTrue(self.mover.go_person(0).startswith("ok "))
        self.assertTrue(near(self.head()[:2], (0.0, -moving.BESIDE), 2e-3))

    def test_nobody_goes_to_themselves_or_to_nobody(self):
        self.assertEqual(self.mover.go_person(1), "error that is you")
        self.assertEqual(self.mover.go_person(3), "error nobody is at place 3")
        self.assertEqual(self.screens.shifted, [])

    def test_to_a_panel_is_where_it_reads_well_facing_it(self):
        self.together.panel(0, at=(3.0, 1.4, 1.0), turn=placing.quat(*turn_about_up(-2.0)), metres=2.0)
        self.assertTrue(self.mover.go_panel(0).startswith("ok "))
        want = moving.reading(self.together.panels[0])
        x, z, facing = self.head()
        self.assertTrue(near((x, z), want[:2], 2e-3))
        self.assertFacing(facing, want[2])
        self.assertRoomStoodStill()

    def test_a_panel_that_isnt_in_the_room_takes_nobody_anywhere(self):
        self.assertEqual(self.mover.go_panel(9), "error no panel 9 in the room")
        self.assertEqual(self.screens.shifted, [])

    def test_twice_to_the_same_spot_moves_nothing_the_second_time(self):
        self.mover.go((1.0, 1.0, 0.5))
        self.mover.go((1.0, 1.0, 0.5))
        dx, dz, yaw = self.screens.shifted[-1]
        self.assertTrue(near((dx, dz, yaw), (0, 0, 0), 2e-3))


class Pulling(MoverCase):
    def test_the_room_comes_past_and_you_go_the_other_way_through_it(self):
        self.together.floor = (2.0, -1.0, 0.0)
        self.before = self.stands()
        was = self.head()
        self.assertTrue(self.mover.pull(0.5, -0.25, 0.0).startswith("ok "))
        self.assertTrue(near(self.head(), (was[0] - 0.5, was[1] + 0.25, was[2]), 1e-3))
        self.assertTrue(near(self.screens.at[1], (0.5, 1.5, -2.25), 1e-6))     # what stood ahead came nearer
        self.assertRoomStoodStill()

    def test_on_a_floor_that_lies_turned_in_the_room_it_is_still_your_own_directions(self):
        was = self.head()
        self.mover.pull(0.0, 1.0, 0.0)      # pulled toward you: you go ahead (-z of your floor)
        ahead = placing.turned(self.together.floor[2], (0, 0, -1))
        self.assertTrue(near(self.head()[:2], (was[0] + ahead[0], was[1] + ahead[2]), 1e-3))
        self.assertRoomStoodStill()

    def test_turning_the_room_turns_you_the_other_way(self):
        was = self.head()
        self.mover.pull(0.0, 0.0, 30.0)
        self.assertFacing(self.head()[2], was[2] - math.radians(30))
        self.assertRoomStoodStill()

    def test_a_long_walk_leaves_the_room_where_it_is(self):
        rng = random.Random(15)
        for _ in range(400):
            self.assertTrue(self.mover.pull(rng.uniform(-0.03, 0.03), rng.uniform(-0.03, 0.03), rng.uniform(-1.5, 1.5)).startswith("ok "))
        self.assertRoomStoodStill(5e-3)

    def test_more_than_a_hands_move_is_no_pull(self):
        for dx, dz, degrees in ((moving.MOST_PULL + 0.1, 0, 0), (4, 4, 0), (0, 0, 181), (0, 0, -181)):
            self.assertEqual(self.mover.pull(dx, dz, degrees), "error that is more than a pull")
        self.assertEqual(self.screens.shifted, [])
        self.assertTrue(self.mover.pull(moving.MOST_PULL, 0, 180).startswith("ok "))


class Refused(MoverCase):
    def assertNothingMoved(self, by=1e-6):
        self.assertEqual(self.together.floor, (2.0, -1.0, 0.7))
        self.assertRoomStoodStill(by)

    def test_outside_a_room_nobody_moves(self):
        self.together.mode = "idle"
        for reply in (self.mover.go((0, 0, 0)), self.mover.pull(0.1, 0, 0)):
            self.assertEqual(reply, "error you aren't in a room")
        self.assertEqual(self.screens.shifted, [])

    def test_before_the_floor_is_laid_nobody_moves(self):
        self.together.floor = None
        self.assertEqual(self.mover.go((0, 0, 0)), "error your floor isn't laid in the room yet")
        self.assertEqual(self.mover.pull(0.1, 0, 0), "error your floor isn't laid in the room yet")
        self.assertEqual(self.screens.shifted, [])

    def test_with_no_head_pose_there_is_nowhere_to_go_from(self):
        self.screens.head = "error no head pose (headset off?)"
        self.assertTrue(self.mover.go((0, 0, 0)).startswith("error no head pose"))
        self.assertNothingMoved()
        self.assertTrue(self.mover.pull(0.1, 0, 0).startswith("ok "))      # a pull needs none

    def test_further_than_a_floor_may_lie_is_too_far(self):
        self.assertEqual(self.mover.go((moving.FURTHEST + 5, 0, 0)), "error that is too far")
        self.assertEqual(self.screens.shifted, [])
        self.assertNothingMoved()

    def test_when_ft_screens_says_nothing_the_floor_stays(self):
        self.screens.there = False
        self.assertEqual(self.mover.pull(0.1, 0, 0), "error no answer from ft-screens")
        self.screens.there = True
        self.assertEqual(self.together.count("floor move"), 0)
        self.assertNothingMoved()

    def test_when_ft_screens_refuses_its_reason_is_passed_on(self):
        self.screens.__class__ = type("Odd", (Screens,), {"__call__": lambda s, text: "error no SteamVR (--no-vr)" if text.startswith("shift") else Screens.__call__(s, text)})
        self.assertEqual(self.mover.pull(0.1, 0, 0), "error no SteamVR (--no-vr)")
        self.assertEqual(self.together.count("floor move"), 0)

    def test_when_the_floor_wont_move_the_panels_are_put_back(self):
        self.together.fixed = True
        self.assertEqual(self.mover.go((-3.0, 4.5, 2.0)), "error no floor is laid to move")
        self.assertEqual(len(self.screens.shifted), 2)
        self.assertNothingMoved(1e-4)      # (as near as the words to ft-screens say it)

    def test_when_ft_together_says_nothing_the_panels_are_put_back(self):
        class Quiet(Together):
            def __call__(self, text):
                return None if text.startswith("floor move") else Together.__call__(self, text)
        self.together.__class__ = Quiet
        self.assertEqual(self.mover.pull(0.5, 0.5, 20), "error no answer from ft-together")
        self.assertEqual(len(self.screens.shifted), 2)
        self.assertRoomStoodStill(1e-3)
        self.together.there = False
        self.assertEqual(self.mover.pull(0.5, 0.5, 20), "error no answer from ft-together")
        self.assertEqual(self.mover.go_person(0), "error no answer from ft-together")
        self.assertEqual(self.mover.go_panel(0), "error no answer from ft-together")


class Asked(MoverCase):
    def test_the_questions_reach_the_moves(self):
        self.together.panel(0, at=(3.0, 1.4, 1.0))
        for text in ("go 1 2 30", "go -0.5 0.25 -170.5", "go person 0", "go panel 0", "pull 0.1 -0.1 2"):
            self.assertTrue(moving.answer(self.mover, text.split()).startswith("ok "), text)
        self.assertEqual(len(self.screens.shifted), 5)
        self.assertRoomStoodStill(5e-3)

    def test_a_pull_asked_is_a_pull_made(self):
        self.together.floor = (2.0, -1.0, 0.0)
        self.before = self.stands()
        self.assertTrue(moving.answer(self.mover, "pull 0.5 -0.25 10".split()).startswith("ok "))
        self.assertTrue(near(self.screens.shifted[-1], (0.5, -0.25, math.radians(10)), 1e-6), self.screens.shifted)
        self.assertRoomStoodStill()

    def test_the_floor_is_told_what_the_words_to_ft_screens_say_not_what_was_meant(self):
        self.together.floor = (1.0, 2.0, 0.0)
        self.mover.pull(0.123456789, 0.0, 0.0)          # ft-screens hears 0.12346
        self.assertAlmostEqual(self.together.floor[0], 1.0 - 0.12346, places=7)

    def test_go_says_degrees_to_the_left_of_the_rooms_minus_z(self):
        moving.answer(self.mover, "go 1 2 90".split())
        x, z, facing = self.head()
        self.assertTrue(near((x, z), (1.0, 2.0), 2e-3))
        self.assertFacing(facing, math.pi / 2)

    def test_what_isnt_a_move_is_refused_and_moves_nothing(self):
        for text in ("go", "go 1 2", "go 1 2 3 4", "go a b c", "go nan 0 0", "go 0 inf 0", "go person", "go person ada", "go person -1",
                     "go person 1234", "go panel 0.5", "go there 1", "pull", "pull 1 2", "pull x 0 0", "pull 0 0 nan", "pull 1 2 3 4"):
            self.assertEqual(moving.answer(self.mover, text.split()), moving.USAGE, text)
        for text in ("go 1001 0 0", "go 0 -1001 0", "go 0 0 361"):
            self.assertEqual(moving.answer(self.mover, text.split()), "error that is too far", text)
        self.assertEqual(self.screens.shifted, [])

    def test_other_questions_are_not_its_own(self):
        for text in ("state", "share 1", "going 1 2 3", "pulled 1 2 3", ""):
            self.assertIsNone(moving.answer(self.mover, text.split()), text)


class WithTheKeeper(MoverCase):
    """The two together, as ft-roompanels has them."""

    def setUp(self):
        super().setUp()
        self.screens.panels.clear()      # the keeper makes the room's panels itself
        self.clock = 100.0
        self.keeper = keeper.Keeper(self.together, self.screens, lambda: self.clock)

    def ticks(self, count=3):
        for _ in range(count):
            self.keeper.tick()
            self.clock += 0.25

    def test_what_you_share_stays_where_the_room_has_it_while_you_move(self):
        self.ticks(1)
        self.keeper.share(1)
        self.ticks()
        told = dict(self.together.panels[8])
        for move in (lambda: self.mover.go((-3.0, 4.5, 2.0)), lambda: self.mover.pull(0.4, -0.2, 15), lambda: self.mover.go_person(0)):
            self.assertTrue(move().startswith("ok "))
            self.ticks()
            now = self.together.panels[8]
            self.assertTrue(near(now["at"], told["at"], 2e-3) and (near(now["turn"], told["turn"], 2e-3) or near(now["turn"], [-v for v in told["turn"]], 2e-3)), (now, told))

    def test_someone_elses_panel_stays_where_the_room_has_it_and_is_not_placed_again(self):
        self.together.panel(2, at=(1.0, 1.5, -3.0))
        self.ticks()
        placed = self.screens.count("pose 116")
        self.before = self.stands()
        self.assertTrue(self.mover.go((-3.0, 4.5, 2.0)).startswith("ok "))
        self.ticks()
        self.assertEqual(self.screens.count("pose 116"), placed)
        self.assertRoomStoodStill()
        want = placing.to_floor(self.together.floor, self.together.panels[2])
        self.assertTrue(near(self.screens.panels[116]["pose"], want, 2e-3))      # where the keeper would put it now

    def test_leaving_the_room_puts_your_screens_back_where_they_stood(self):
        stood = dict(self.screens.at)
        self.ticks()
        self.mover.go((-3.0, 4.5, 2.0))
        self.mover.pull(0.4, -0.2, 15)
        self.assertFalse(near(self.screens.at[1], stood[1], 0.5))
        self.together.mode = "idle"
        self.ticks()
        self.assertTrue(all(near(self.screens.at[n], stood[n], 1e-3) for n in stood), self.screens.at)


if __name__ == "__main__":
    unittest.main()
