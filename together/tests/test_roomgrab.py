"""Grabbing the air, the rule (together/roommove/grab.py): which pull keeps the room in two hands
that move, turn, come together, jump, stop being tracked, let go and close again.

    python3 together/tests/test_roomgrab.py
"""
import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fakescreens import Screens  # noqa: E402
from roommove import grab  # noqa: E402
from test_roompanels import near  # noqa: E402


def hands(left, right, height=1.1):
    """Two hands over (x, z) of the floor."""
    return ((left[0], height, left[1]), (right[0], height + 0.1, right[1]))


def carried(pair, yaw, by):
    """Two places turned `yaw` about their middle and moved `by`: hands that move as one."""
    mid = ((pair[0][0] + pair[1][0]) / 2, (pair[0][1] + pair[1][1]) / 2)
    out = []
    for at in pair:
        t = grab.turned(yaw, (at[0] - mid[0], at[1] - mid[1]))
        out.append((t[0] + mid[0] + by[0], t[1] + mid[1] + by[1]))
    return tuple(out)


class Pulls(unittest.TestCase):
    def setUp(self):
        self.grab = grab.Grab()
        self.at = ((-0.3, -0.4), (0.3, -0.4))       # left and right hand, half a step ahead

    def look(self, at=None, both=True):
        return self.grab.step(both, hands(*(at or self.at)))

    def test_one_grip_holds_nothing_and_closing_both_moves_nothing_yet(self):
        self.assertIsNone(self.look(both=False))
        self.assertFalse(self.grab.held)
        self.assertIsNone(self.look())
        self.assertTrue(self.grab.held)

    def test_the_room_goes_where_the_hands_go(self):
        self.look()
        self.assertEqual(self.look(carried(self.at, 0, (0.1, -0.05))), (0.1, -0.05, 0.0))

    def test_how_high_the_hands_are_moves_nothing(self):
        self.look()
        self.assertIsNone(self.grab.step(True, ((-0.3, 0.2, -0.4), (0.3, 1.9, -0.4))))

    def test_hands_turning_about_their_middle_turn_the_room_about_it(self):
        self.look()
        pull = self.look(carried(self.at, math.radians(10), (0, 0)))
        self.assertAlmostEqual(pull[2], 10.0, places=3)
        self.assertTrue(near(grab.moved(pull, (0.0, -0.4)), (0.0, -0.4), 1e-4))      # their middle stays where it is
        self.assertTrue(near(grab.moved(pull, self.at[0]), carried(self.at, math.radians(10), (0, 0))[0], 1e-4))

    def test_to_the_left_is_the_way_a_floor_turns(self):
        self.look()
        pull = self.look(((-0.3, -0.3), (0.3, -0.5)))      # the right hand goes ahead, the left comes back
        self.assertGreater(pull[2], 5)                      # seen from above, counterclockwise: to the left
        screens = Screens()
        screens.at = {1: (0.3, 1.5, -0.4)}
        screens("shift" + grab.words(pull)[4:])                # what ft-roompanels makes of it (roompanels/moving.py)
        self.assertTrue(near(screens.at[1][::2], grab.moved(pull, (0.3, -0.4)), 1e-4))   # and ft-screens moves it the same way

    def test_hands_close_together_only_move_the_room(self):
        near_ = ((-0.05, -0.4), (0.05, -0.4))
        self.grab.step(True, hands(*near_))
        pull = self.grab.step(True, hands(*carried(near_, math.radians(30), (0.02, 0.0))))
        self.assertEqual(pull[2], 0.0)
        self.assertTrue(near(pull[:2], (0.02, 0.0), 1e-5))

    def test_hands_that_closed_near_each_other_turn_it_once_they_are_apart(self):
        near_ = ((-0.05, -0.4), (0.05, -0.4))
        self.grab.step(True, hands(*near_))
        apart = ((-0.2, -0.3), (0.26, -0.5))                # apart, their line turned already, their middle 3 cm on
        pull = self.grab.step(True, hands(*apart))
        self.assertEqual(pull, (0.03, 0.0, 0.0))            # no turn from a line that said nothing
        self.grab.made(pull)
        pull = self.grab.step(True, hands(*carried(apart, math.radians(10), (0, 0))))
        self.assertAlmostEqual(pull[2], 10.0, places=3)     # but from the line they have now
        self.assertTrue(near(grab.moved(pull, (0.03, -0.4)), (0.03, -0.4), 1e-4))     # about their middle

    def test_a_line_that_points_backward_turns_it_like_any_other(self):
        behind = carried(((0.0, -0.7), (0.0, -0.1)), math.radians(-4), (0, 0))      # the right hand behind the left
        self.grab.step(True, hands(*behind))
        pull = self.grab.step(True, hands(*carried(behind, math.radians(8), (0, 0))))
        self.assertAlmostEqual(pull[2], 8.0, places=3)

    def test_hands_coming_together_stop_turning_it(self):
        self.look()
        self.assertEqual(self.look(((-0.03, -0.38), (0.07, -0.42))), (0.02, 0.0, 0.0))

    def test_a_move_too_small_to_see_waits_and_is_not_lost(self):
        self.look()
        for i in range(1, 4):
            self.assertIsNone(self.look(carried(self.at, 0, (0.0006 * i, 0))))
        self.assertEqual(self.look(carried(self.at, 0, (0.0024, 0))), (0.0024, 0.0, 0.0))

    def test_a_small_turn_alone_is_a_pull(self):
        self.look()
        self.assertIsNone(self.look(carried(self.at, math.radians(0.1), (0, 0))))
        self.assertAlmostEqual(self.look(carried(self.at, math.radians(0.2), (0, 0)))[2], 0.2, places=3)

    def test_a_hand_somewhere_else_all_at_once_throws_nobody(self):
        self.look()
        self.assertIsNone(self.look(carried(self.at, 0, (0.6, 0))))                   # further than a hand goes between looks
        self.assertEqual(self.look(carried(self.at, 0, (0.61, 0))), (0.01, 0.0, 0.0))  # held on from there
        self.grab.made((0.01, 0.0, 0.0))
        self.assertIsNone(self.look(carried(self.at, math.radians(50), (0.61, 0))))
        self.assertIsNone(self.look(carried(self.at, math.radians(50), (0.61, 0))))
        self.assertEqual(self.look(carried(self.at, 0, (0.5, 0))), None)              # half a turn back at once is no hand either
        self.assertTrue(self.grab.held)

    def test_just_within_a_hands_move_is_one(self):
        self.look()
        self.assertEqual(self.look(carried(self.at, 0, (0.49, 0))), (0.49, 0.0, 0.0))

    def test_a_hand_that_isnt_tracked_lets_go(self):
        self.look()
        self.assertIsNone(self.grab.step(True, None))
        self.assertFalse(self.grab.held)
        self.assertIsNone(self.look(carried(self.at, 0, (0.3, 0.3))))                 # closing again starts from there
        self.assertEqual(self.look(carried(self.at, 0, (0.31, 0.3))), (0.01, 0.0, 0.0))

    def test_letting_go_and_closing_again_starts_from_where_the_hands_are(self):
        self.look()
        self.look(carried(self.at, 0, (0.2, 0)), both=False)
        self.assertIsNone(self.look(carried(self.at, 0, (0.4, 0))))
        self.assertEqual(self.look(carried(self.at, 0, (0.45, 0))), (0.05, 0.0, 0.0))

    def test_a_pull_that_wasnt_made_starts_afresh(self):
        self.look()
        self.assertIsNotNone(self.look(carried(self.at, 0, (0.1, 0))))
        self.grab.drop()
        self.assertIsNone(self.look(carried(self.at, 0, (0.1, 0))))
        self.assertEqual(self.look(carried(self.at, 0, (0.13, 0))), (0.03, 0.0, 0.0))

    def test_until_a_pull_is_made_it_is_asked_for_again(self):
        self.look()
        self.assertEqual(self.look(carried(self.at, 0, (0.1, 0))), (0.1, 0.0, 0.0))
        self.assertEqual(self.look(carried(self.at, 0, (0.1, 0))), (0.1, 0.0, 0.0))
        self.grab.made((0.1, 0.0, 0.0))
        self.assertIsNone(self.look(carried(self.at, 0, (0.1, 0))))

    def test_what_the_hands_hold_stays_in_them_all_the_way(self):
        rng = random.Random(21)
        held = self.at                      # the two places of the room the hands closed on, wherever they are now
        now = self.at
        self.look()
        for _ in range(2000):
            now = carried(now, rng.uniform(-0.03, 0.03), (rng.uniform(-0.02, 0.02), rng.uniform(-0.02, 0.02)))
            pull = self.look(now)
            if pull is not None:
                self.assertEqual(pull, tuple(float(w) for w in grab.words(pull).split()[1:]))   # as its words say it
                held = (grab.moved(pull, held[0]), grab.moved(pull, held[1]))
                self.grab.made(pull)
            for place, hand in zip(held, now):
                self.assertLess(math.hypot(place[0] - hand[0], place[1] - hand[1]), 0.004)      # never further than what waits

    def test_the_words_carry_every_digit(self):
        self.assertEqual(grab.words((0.12345, -2.0, 7.5)), "pull 0.12345 -2.00000 7.5000")


if __name__ == "__main__":
    unittest.main()
