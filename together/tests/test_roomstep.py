"""Stepping through the room (together/roompanels/moving.py, `step`): so far the way your head
faces or to its right, and a turn where you stand, with the room standing still. Against the
stand-ins of test_roommoving.py.

    python3 together/tests/test_roomstep.py
"""
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from roompanels import moving, placing  # noqa: E402
from test_roommoving import MoverCase  # noqa: E402
from test_roompanels import near  # noqa: E402


class Stepping(MoverCase):
    def ways(self):
        """Where the head is in the room, and the ways it faces and has to its right there."""
        x, z, facing = self.head()
        ahead, right = placing.turned(facing, (0.0, 0.0, -1.0)), placing.turned(facing, (1.0, 0.0, 0.0))
        return (x, z), facing, (ahead[0], ahead[2]), (right[0], right[2])

    def test_forward_is_the_way_the_head_faces(self):
        at, facing, ahead, _ = self.ways()
        self.assertTrue(self.mover.step(0.5, 0.0, 0.0).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), (at[0] + 0.5 * ahead[0], at[1] + 0.5 * ahead[1]), 2e-3), (x, z))
        self.assertFacing(now, facing)
        self.assertRoomStoodStill()

    def test_backward_and_to_the_right(self):
        at, facing, ahead, right = self.ways()
        self.assertTrue(self.mover.step(-0.25, 0.75, 0.0).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), (at[0] - 0.25 * ahead[0] + 0.75 * right[0], at[1] - 0.25 * ahead[1] + 0.75 * right[1]), 2e-3), (x, z))
        self.assertFacing(now, facing)
        self.assertRoomStoodStill()

    def test_a_turn_is_to_the_left_where_you_stand(self):
        at, facing, _, _ = self.ways()
        self.assertTrue(self.mover.step(0.0, 0.0, 30.0).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), at, 2e-3), ((x, z), at))        # about the head, not the floor's middle
        self.assertFacing(now, facing + math.radians(30))
        self.assertRoomStoodStill()

    def test_the_step_comes_before_the_turn(self):
        at, facing, ahead, _ = self.ways()
        self.assertTrue(self.mover.step(1.0, 0.0, -90.0).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), (at[0] + ahead[0], at[1] + ahead[1]), 2e-3), (x, z))
        self.assertFacing(now, facing - math.pi / 2)

    def test_the_room_turns_the_other_way_past_you(self):
        self.together.floor = (0.0, 0.0, 0.0)
        self.screens.head = "ok 0.0000 1.6000 0.0000 0.00"
        self.mover.step(0.0, 0.0, 90.0)                               # you turn left: what was ahead is on your right
        self.assertTrue(near(self.screens.at[1], (2.0, 1.5, 0.0), 1e-3), self.screens.at[1])
        self.mover.step(1.0, 0.0, 0.0)                                # the head still faces -z of your floor: it comes nearer
        self.assertTrue(near(self.screens.at[1], (2.0, 1.5, 1.0), 1e-3), self.screens.at[1])

    def test_four_quarter_turns_and_a_square_walked_end_where_they_began(self):
        at, facing, _, _ = self.ways()
        for _ in range(4):
            self.assertTrue(self.mover.step(1.5, 0.0, 90.0).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), at, 5e-3), ((x, z), at))
        self.assertFacing(now, facing, 5e-3)
        self.assertRoomStoodStill(5e-3)

    def test_the_head_is_asked_for_once(self):
        self.mover.step(0.5, 0.0, 10.0)
        self.assertEqual((self.screens.count("head"), self.screens.count("shift")), (1, 1))

    def test_more_than_a_step_is_refused(self):
        for step in ((5.1, 0, 0), (0, -5.1, 0), (4, 4, 0), (0, 0, 181), (0, 0, -181)):
            self.assertEqual(self.mover.step(*step), "error that is more than a step", step)
        self.assertTrue(self.mover.step(3, 4, 180).startswith("ok "))
        self.assertEqual(len(self.screens.shifted), 1)

    def test_with_no_head_pose_or_no_floor_nobody_steps(self):
        self.screens.head = "error no head pose (headset off?)"
        self.assertTrue(self.mover.step(0.5, 0, 0).startswith("error no head pose"))
        self.screens.head = "ok 0.5000 1.6000 0.2500 90.00"
        self.together.floor = None
        self.assertEqual(self.mover.step(0.5, 0, 0), "error your floor isn't laid in the room yet")
        self.together.mode = "idle"
        self.assertEqual(self.mover.step(0.5, 0, 0), "error you aren't in a room")
        self.assertEqual(self.screens.shifted, [])

    def test_the_question_reaches_it(self):
        at, facing, ahead, _ = self.ways()
        self.assertTrue(moving.answer(self.mover, "step 0.5 0 -45".split()).startswith("ok "))
        x, z, now = self.head()
        self.assertTrue(near((x, z), (at[0] + 0.5 * ahead[0], at[1] + 0.5 * ahead[1]), 2e-3))
        self.assertFacing(now, facing - math.pi / 4)
        for text in ("step", "step 1 2", "step 1 2 3 4", "step a 0 0", "step 0 nan 0", "stepping 1 2 3"):
            self.assertIn(moving.answer(self.mover, text.split()), (moving.USAGE, None), text)
        self.assertIsNone(moving.answer(self.mover, "stepping 1 2 3".split()))
        self.assertEqual(moving.answer(self.mover, "step 1 2".split()), moving.USAGE)
        self.assertEqual(len(self.screens.shifted), 1)


if __name__ == "__main__":
    unittest.main()
