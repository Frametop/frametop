"""A flight through the room (together/roompanels/gliding.py): the way to where a panel reads
well, gone in small steps, each a move of the mover's against the stand-ins for ft-together
and ft-screens.

    python3 together/tests/test_roomgliding.py
"""
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from roompanels import gliding, placing  # noqa: E402
from test_roommoving import MoverCase  # noqa: E402
from test_roompanels import near, turn_about_up  # noqa: E402


GLIDE = 0.4       # seconds: what docs/together.md says a flight takes


class Flights(MoverCase):
    def setUp(self):
        super().setUp()
        self.clock = 50.0
        self.glide = gliding.Glide(self.mover, lambda: self.clock)
        self.together.panel(2, at=(4.0, 1.5, -6.0), turn=placing.quat(*turn_about_up(2.0)))
        self.goal = self.mover.panel_spot(2)
        self.start = self.head()

    def fly(self, seconds=GLIDE + gliding.STEP):
        """Steps for that long. Returns where the head was after each."""
        way = []
        for _ in range(round(seconds / gliding.STEP)):
            self.clock += gliding.STEP
            self.glide.step()
            way.append(self.head())
        return way

    def assertOver(self, head, spot, by=2e-3):
        self.assertTrue(near(head[:2], spot[:2], by) and abs(placing.wrap(head[2] - spot[2])) < by, (head, spot))

    def test_it_ends_over_the_spot_the_panel_reads_well_from_facing_it(self):
        self.assertEqual(self.glide.to_panel(2), "ok")
        self.assertTrue(self.glide.going)
        self.fly()
        self.assertFalse(self.glide.going)
        self.assertOver(self.head(), self.goal)
        self.assertRoomStoodStill()

    def test_it_takes_as_long_as_it_says_and_then_asks_nothing_more(self):
        self.glide.to_panel(2)
        self.fly(GLIDE - 2 * gliding.STEP)
        self.assertTrue(self.glide.going)
        self.fly(3 * gliding.STEP)
        self.assertFalse(self.glide.going)
        asked = self.screens.count("shift")
        self.fly(0.5)
        self.assertEqual(self.screens.count("shift"), asked)

    def test_it_goes_straight_there_slow_at_both_ends(self):
        self.glide.to_panel(2)
        way = [self.start] + self.fly()
        whole = math.dist(self.start[:2], self.goal[:2])
        steps = [math.dist(a[:2], b[:2]) for a, b in zip(way, way[1:])]
        self.assertAlmostEqual(sum(steps), whole, delta=0.01)             # no detour
        self.assertLess(max(steps), whole * 1.6 * gliding.STEP / GLIDE)      # no jump: at most one and a half times the even pace
        middle = steps[len(steps) // 2 - 1]
        self.assertTrue(steps[0] < middle / 4 and steps[-2] < middle / 4, steps)
        gone = [math.dist(self.start[:2], at[:2]) for at in way]
        self.assertEqual(gone, sorted(gone))

    def test_halfway_through_it_is_halfway_there(self):
        self.glide.to_panel(2)
        self.fly(GLIDE / 2)
        half = [(a + b) / 2 for a, b in zip(self.start[:2], self.goal[:2])]
        self.assertTrue(near(self.head()[:2], half, 0.02), (self.head(), half))

    def test_it_turns_the_short_way_round(self):
        self.together.floor = (2.0, -1.0, 3.0 - math.radians(90))        # the head faces 3.0 in the room
        self.together.panel(3, at=(2.0, 1.5, -1.0), turn=placing.quat(*turn_about_up(-3.0)))
        self.assertEqual(self.glide.to_panel(3), "ok")
        turned = [placing.wrap(at[2] - 3.0) for at in self.fly()]
        self.assertTrue(all(-1e-6 <= t <= 0.3 for t in turned), turned)   # through half round, never back through nought
        self.assertAlmostEqual(turned[-1], placing.wrap(-3.0 - 3.0), delta=2e-3)

    def test_someone_who_is_there_already_goes_nowhere(self):
        self.mover.go(self.goal)
        asked = self.screens.count("shift")
        self.assertEqual(self.glide.to_panel(2), "ok")
        self.assertFalse(self.glide.going)
        self.fly(0.1)
        self.assertEqual(self.screens.count("shift"), asked)

    def test_turned_away_from_it_on_the_spot_is_not_there(self):
        self.mover.go((self.goal[0], self.goal[1], self.goal[2] + 0.5))
        self.glide.to_panel(2)
        self.assertTrue(self.glide.going)
        self.fly()
        self.assertOver(self.head(), self.goal)

    def test_no_panel_no_flight(self):
        self.assertEqual(self.glide.to_panel(5), "error no panel 5 in the room")
        self.together.there = False
        self.assertEqual(self.glide.to_panel(2), "error no answer from ft-together")
        self.assertFalse(self.glide.going)

    def test_nowhere_to_start_from_no_flight(self):
        self.screens.head = "error no head pose"
        self.assertEqual(self.glide.to_panel(2), "error no head pose (is the headset on?)")
        self.together.floor = None
        self.assertTrue(self.glide.to_panel(2).startswith("error"))
        self.assertFalse(self.glide.going)

    def test_a_step_that_cant_be_made_ends_it_where_it_is(self):
        self.glide.to_panel(2)
        self.fly(GLIDE / 2)
        self.screens.there = False
        self.fly(gliding.STEP)
        self.assertFalse(self.glide.going)
        self.screens.there = True
        asked = self.screens.count("shift")
        self.fly(0.5)
        self.assertEqual(self.screens.count("shift"), asked)
        self.assertRoomStoodStill()

    def test_a_head_that_moves_meanwhile_still_ends_over_the_spot(self):
        self.glide.to_panel(2)
        self.fly(GLIDE / 2)
        self.screens.head = "ok 0.9000 1.6000 -0.3000 60.00"
        self.fly(GLIDE / 2 + gliding.STEP)
        self.assertOver(self.head(), self.goal)

    def test_a_new_flight_takes_over_from_where_the_old_one_is(self):
        self.together.panel(3, at=(-5.0, 1.5, 2.0))
        self.glide.to_panel(2)
        self.fly(GLIDE / 2)
        self.assertEqual(self.glide.to_panel(3), "ok")
        way = [self.head()] + self.fly()
        self.assertOver(self.head(), self.mover.panel_spot(3))
        steps = [math.dist(a[:2], b[:2]) for a, b in zip(way, way[1:])]
        self.assertLess(steps[0], max(steps) / 4)                         # it set off slowly again, with no jump
        self.assertRoomStoodStill()


if __name__ == "__main__":
    unittest.main()
