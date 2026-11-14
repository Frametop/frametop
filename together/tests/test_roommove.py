"""What ft-roommove does look by look (together/roommove/puller.py), against stand-ins that
answer as ft-together and ft-screens do, with ft-roompanels' own mover making the pulls and the
grips as the test says. The pull's rule is test_roomgrab.py's, SteamVR's side test_roomgrips.py's.

    python3 together/tests/test_roommove.py
"""
import json
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fakescreens import Screens  # noqa: E402
from roommove.grips import Told  # noqa: E402
from roommove.puller import ROOM_EVERY, STEP, Puller  # noqa: E402
from roompanels import moving  # noqa: E402
from test_roomgrab import carried, hands  # noqa: E402
from test_roompanels import Together, near  # noqa: E402


class Looks(unittest.TestCase):
    """The puller between stand-ins, with ft-roompanels' own mover making the pulls."""

    def setUp(self):
        self.screens, self.together, self.grips = Screens(), Together(), Told()
        self.together.floor = (2.0, -1.0, 0.7)
        self.mover = moving.Mover(self.together, self.screens)
        self.pulled = []

        def panels(text):
            self.pulled.append(text)
            return moving.answer(self.mover, text.split())

        self.panels = panels
        self.puller = Puller(self.together, self.screens, lambda text: self.panels(text), self.grips)
        self.at = ((-0.3, -0.4), (0.3, -0.4))
        self.screens.hands = hands(*self.at)
        self.screens.at = {1: (-0.3, 1.5, -0.4), 2: (0.3, 1.5, -0.4)}      # a screen at each hand, to see what the hands hold
        self.now = 100.0

    def look(self, after=STEP * 1.25):
        self.now += after
        self.puller.look(self.now)

    def move(self, yaw, by):
        self.at = carried(self.at, yaw, by)
        self.screens.hands = hands(*self.at)

    def in_hand(self, by=0.004):
        return all(near(self.screens.at[n][::2], self.at[n - 1], by) for n in (1, 2))

    def test_the_room_follows_the_hands_while_both_grips_are_down(self):
        self.grips.both = True
        self.look()
        rng = random.Random(22)
        for _ in range(300):
            self.move(rng.uniform(-0.02, 0.02), (rng.uniform(-0.02, 0.02), rng.uniform(-0.02, 0.02)))
            self.look()
            self.assertTrue(self.in_hand(), (self.screens.at, self.at))
        self.assertGreater(self.puller.pulls, 250)
        self.assertEqual(self.puller.refused, 0)

    def test_and_you_go_the_other_way_through_it(self):
        self.together.floor = (2.0, -1.0, 0.0)
        self.grips.both = True
        self.look()
        self.move(0, (0.0, 0.25))                      # pulled toward you
        self.look()
        self.assertTrue(near(self.together.floor, (2.0, -1.25, 0.0), 1e-5), self.together.floor)   # your floor went ahead in the room

    def test_with_one_grip_or_none_nothing_moves(self):
        self.look()
        self.move(0, (0.1, 0))
        self.look()
        self.assertEqual((self.pulled, self.screens.count("reach")), ([], 0))

    def test_letting_go_leaves_the_room_where_it_is(self):
        self.grips.both = True
        self.look()
        self.move(0, (0.1, 0))
        self.look()
        self.grips.both = False
        self.look()
        self.move(0, (0.3, 0))
        self.look()
        self.grips.both = True
        self.look()
        self.assertEqual(len(self.pulled), 1)          # closing again takes hold where the hands are
        self.move(0, (0.05, 0))
        self.look()
        self.assertEqual(self.pulled[-1], "pull 0.05000 0.00000 0.0000")

    def test_outside_a_room_no_controller_is_listened_to(self):
        self.together.mode = "idle"
        closed = []
        self.grips.close = lambda: closed.append(1)
        self.grips.look = lambda now: self.fail("it listened")
        self.grips.both = True
        self.look()
        self.assertEqual((self.puller.in_room, len(closed), self.screens.count("reach")), (False, 1, 0))
        self.assertFalse(json.loads(self.puller.state()[3:])["both"])      # whatever the grips were last said to be

    def test_in_a_room_the_grips_are_looked_at_every_time(self):
        looked = []
        self.grips.look = looked.append
        self.look()
        self.look(STEP / 10)
        self.assertEqual(looked, [self.now - STEP / 10, self.now])

    def test_before_the_floor_is_laid_nothing_is_pulled(self):
        self.together.floor = None
        self.grips.both = True
        self.look()
        self.assertFalse(self.puller.in_room)

    def test_whether_it_is_in_a_room_is_asked_once_a_second(self):
        for _ in range(130):
            self.look()
        self.assertEqual(self.together.asked.count("floor"), 3)
        self.together.mode = "idle"
        self.look(ROOM_EVERY)
        self.assertFalse(self.puller.in_room)

    def test_the_hands_are_asked_for_sixty_times_a_second_at_most(self):
        self.grips.both = True
        for _ in range(100):
            self.look(STEP * 0.3)
        self.assertEqual(self.screens.count("reach"), 25)        # every fourth look: the first a sixtieth later

    def test_a_hand_that_cant_be_had_forgets_the_grips(self):
        self.grips.both = True
        self.look()
        for error in ("error no right controller tracked", "error a game has the controllers"):
            self.grips.both = True
            self.screens.hands = error
            self.look()
            self.assertFalse(self.grips.both, error)
            self.assertFalse(self.puller.grab.held)
        self.screens.hands = hands(*carried(self.at, 0, (0.3, 0)))
        self.look()
        self.assertEqual(self.pulled, [])              # tracked again, but not pressed again

    def test_ft_screens_silent_lets_go_but_the_grips_still_count(self):
        self.grips.both = True
        self.look()
        self.screens.there = False
        self.look()
        self.assertTrue(self.grips.both)
        self.assertFalse(self.puller.grab.held)
        self.assertEqual(json.loads(self.puller.state()[3:]), {"room": True, "both": True, "held": False, "pulls": 0, "refused": 0})
        self.screens.there = True
        self.move(0, (0.2, 0))
        self.look()
        self.assertEqual(self.pulled, [])              # held on from where the hands are now
        self.move(0, (0.01, 0))
        self.look()
        self.assertEqual(len(self.pulled), 1)

    def test_rubbish_about_the_hands_is_no_hands(self):
        self.grips.both = True
        for said in ("ok 1 2 3 4 5", "ok 1 2 3 4 5 6 7", "ok a b c d e f", "ok nan 0 0 0 0 0", "ok 0 0 0 inf 0 0", "fine 1 2 3 4 5 6"):
            self.screens.hands = said
            self.look()
            self.assertFalse(self.puller.grab.held, said)
        self.assertEqual(self.pulled, [])

    def test_a_pull_that_is_refused_is_counted_and_lets_go(self):
        self.grips.both = True
        self.look()
        self.panels = lambda text: "error that is too far"
        self.move(0, (0.1, 0))
        self.look()
        self.assertEqual((self.puller.pulls, self.puller.refused, self.puller.grab.held), (0, 1, False))
        self.panels = lambda text: None                # ft-roompanels isn't there
        self.look()
        self.move(0, (0.1, 0))
        self.look()
        self.assertEqual(self.puller.refused, 2)

    def test_it_says_how_it_is(self):
        self.assertEqual(json.loads(self.puller.state()[3:]), {"room": False, "both": False, "held": False, "pulls": 0, "refused": 0})
        self.grips.both = True
        self.look()
        self.move(0, (0.1, 0))
        self.look()
        self.assertEqual(json.loads(self.puller.state()[3:]), {"room": True, "both": True, "held": True, "pulls": 1, "refused": 0})

    def test_it_waits_a_second_idle_and_a_sixtieth_with_the_room_in_hand(self):
        self.look()
        self.assertAlmostEqual(self.puller.wait(self.now), ROOM_EVERY)
        self.assertAlmostEqual(self.puller.wait(self.now + 0.4), ROOM_EVERY - 0.4)
        self.grips.both = True
        self.look()
        self.assertAlmostEqual(self.puller.wait(self.now), STEP)
        self.assertEqual(self.puller.wait(self.now + 5), 0.0)

    def test_ending_closes_the_grips(self):
        self.grips.both = True
        self.puller.end()
        self.assertFalse(self.grips.both)


if __name__ == "__main__":
    unittest.main()
