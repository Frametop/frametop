"""Panels moved by someone other than their owner (together/roompanels/carrying.py), with the
keeper around the rules as ft-roompanels has it, against the stand-ins for ft-together and
ft-screens. This Frame is person 1 of a room person 0 owns: panel 2 is person 0's and stands
here as the remote screen 116, and this Frame's own screen 1 is the room's panel 8.

    python3 together/tests/test_roomcarrying.py
"""
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from roompanels import keeper, placing  # noqa: E402
from test_roompanels import KeeperCase, near  # noqa: E402

SETTLE, REST, PULLED = 1.0, 2.0, 0.3                 # seconds, seconds, metres: what docs/together.md says
HOME = [-1, 0, 0, 0, 0, 1, 0, 1.5, 0, 0, -1, -2]      # where the room has panel 2, on this Frame's floor
MAY_NOT = "error you may not move that panel"


class CarryCase(KeeperCase):
    def setUp(self):
        super().setUp()
        self.flown = []      # (the panel flown to, whether it stood where the room has it by then)
        self.keeper = keeper.Keeper(self.together, self.screens, lambda: self.clock,
                                    lambda ident: self.flown.append((ident, self.at_home())))
        self.together.floor = (0.0, -4.0, math.pi)       # across the room from its owner, looking back

    def pose(self):
        return self.screens.panels[116]["pose"]

    def at_home(self):
        return near(self.pose(), HOME)

    def toward_the_head(self, metres):
        """The way from the panel to this Frame's head, that long."""
        head = [float(w) for w in self.screens.head.split()[1:4]]
        way = [h - a for h, a in zip(head, HOME[3::4])]
        return tuple(v * metres / math.hypot(*way) for v in way)


class Theirs(CarryCase):
    """Someone else's panel, here."""

    def setUp(self):
        super().setUp()
        self.together.panel(2)
        self.ticks()
        self.assertTrue(self.at_home())

    def test_left_alone_nothing_is_asked_of_anyone_but_where_it_stands(self):
        self.ticks(8)
        self.assertEqual((self.screens.count("pose 116"), self.screens.count("width 116"), self.together.count("move ")), (1, 1, 0))
        self.assertEqual(self.flown, [])

    def test_carried_by_someone_who_may_the_room_has_it_where_they_put_it(self):
        self.screens.carry(116, by=(0.5, 0.1, 0.2), yaw=0.3)
        carried = list(self.pose())
        self.ticks(1)
        self.assertEqual(self.together.count("move 2 "), 1)
        want = placing.to_floor(self.together.floor, self.together.panels[2])
        self.assertTrue(near(want, carried, 1e-3), (want, carried))       # the room, seen from here, has it in their hand
        self.assertTrue(near(self.together.panels[2]["at"], (-0.5, 1.6, -2.2)), self.together.panels[2])
        self.ticks(8)
        self.assertEqual((self.together.count("move "), self.screens.count("pose 116")), (1, 1))      # and it was never taken out of it
        self.assertTrue(near(self.pose(), carried))

    def test_after_a_carry_the_rooms_own_move_of_it_is_followed_again(self):
        self.screens.carry(116, by=(0.5, 0.0, 0.0))
        self.ticks(2)
        self.together.panel(2, at=(0.0, 2.0, -2.0))         # its owner moved it
        self.ticks(2)
        self.assertTrue(near(self.pose()[3::4], (0.0, 2.0, -2.0)), self.pose())
        self.assertEqual(self.together.count("move 2 "), 1)

    def test_carried_there_and_straight_back_it_is_back_for_the_room_too(self):
        self.screens.carry(116, by=(0.5, 0.0, 0.0))
        self.ticks(1)
        self.screens.carry(116, by=(-0.5, 0.0, 0.0))        # between two looks
        self.ticks(2)
        self.assertTrue(self.at_home() and near(self.together.panels[2]["at"], (0.0, 1.5, -2.0)), self.together.panels[2])
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (2, 1))

    def test_a_drag_is_followed_look_by_look(self):
        for _ in range(6):
            self.screens.carry(116, by=(0.05, 0.0, 0.0))
            self.ticks(1)
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (6, 1))
        self.assertTrue(near(self.together.panels[2]["at"], (-0.3, 1.5, -2.0)), self.together.panels[2])

    def test_a_drag_the_room_is_slow_to_follow_is_still_asked_for_at_every_look(self):
        self.together.takes = False
        for _ in range(6):
            self.screens.carry(116, by=(0.05, 0.0, 0.0))
            self.ticks(1)
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (6, 1))

    def test_a_room_that_does_not_come_round_gets_it_back(self):
        self.together.takes = False       # its owner's Frame says ok and then doesn't take it there
        self.screens.carry(116, by=(0.5, 0.0, 0.0))
        self.ticks(1 + int(SETTLE / 0.25))
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (1, 1))      # waited for, and not asked again
        self.assertFalse(self.at_home())
        self.ticks(1)
        self.assertTrue(self.at_home())
        self.ticks(4)
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (1, 2))

    def test_the_room_coming_round_late_is_still_followed(self):
        self.together.takes = False
        self.screens.carry(116, by=(0.5, 0.0, 0.0))
        self.ticks(3)
        self.together.panel(2, at=(-0.5, 1.5, -2.0))
        self.ticks(8)
        self.assertEqual(self.screens.count("pose 116"), 1)
        self.assertTrue(near(self.pose()[3::4], (0.5, 1.5, -2.0)))

    def test_while_the_room_is_waited_for_its_own_move_of_the_panel_waits_too(self):
        self.together.takes = False
        self.screens.carry(116, by=(0.5, 0.0, 0.0))
        self.ticks(2)
        self.together.panel(2, at=(0.0, 2.0, -2.0))         # its owner moved it meanwhile
        self.ticks(2)
        self.assertEqual(self.screens.count("pose 116"), 1)
        self.ticks(3)
        self.assertTrue(near(self.pose()[3::4], (0.0, 2.0, -2.0)), self.pose())

    def test_refused_it_goes_back_once_it_rests(self):
        self.together.may = MAY_NOT
        self.screens.carry(116, by=(-0.4, 0.0, -0.1), yaw=0.2)
        self.ticks(1)
        self.assertFalse(self.at_home())                    # one look to see that it rests
        self.ticks(1)
        self.assertTrue(self.at_home())
        self.assertEqual((self.together.count("move 2 "), self.flown), (1, []))
        self.ticks(6)
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (1, 2))

    def test_any_refusal_is_one(self):
        for why in ("error no such panel", "error you aren't in a room", "error that isn't a place in the room", "rubbish", ""):
            self.together.may = why
            self.screens.carry(116, by=(-0.4, 0.0, 0.0))
            self.ticks(2)
            self.assertTrue(self.at_home(), why)

    def test_in_their_hand_it_is_left_there_for_a_while_and_then_put_back(self):
        self.together.may = MAY_NOT
        looks = 1 + int(REST / 0.25)               # the one that asks, and those within REST of it
        for _ in range(looks):
            self.screens.carry(116, by=(-0.01, 0.0, 0.0))
            self.ticks(1)
        self.assertEqual((self.together.count("move 2 "), self.screens.count("pose 116")), (1, 1))      # asked once: no means no
        self.screens.carry(116, by=(-0.01, 0.0, 0.0))
        self.ticks(1)
        self.assertTrue(self.at_home())

    def test_pulled_nearer_they_are_flown_to_it_and_it_goes_back(self):
        self.together.may = MAY_NOT
        self.screens.carry(116, by=self.toward_the_head(1.0))
        self.ticks(2)
        self.assertEqual(self.flown, [(2, True)])           # back before they set off
        self.ticks(8)
        self.assertEqual((self.flown, self.together.count("move 2 ")), ([(2, True)], 1))

    def test_a_pull_is_more_than_a_nudge(self):
        self.together.may = MAY_NOT
        self.screens.carry(116, by=self.toward_the_head(PULLED - 0.02))
        self.ticks(2)
        self.assertEqual(self.flown, [])
        self.screens.carry(116, by=self.toward_the_head(PULLED + 0.02))
        self.ticks(2)
        self.assertEqual(self.flown, [(2, True)])

    def test_pushed_away_or_aside_nobody_flies(self):
        self.together.may = MAY_NOT
        for by in (self.toward_the_head(-1.0), (-1.5, 0.0, 0.0), (0.0, 0.8, 0.0)):
            self.screens.carry(116, by=by)
            self.ticks(2)
            self.assertTrue(self.at_home())
        self.assertEqual(self.flown, [])

    def test_a_pull_in_the_hand_flies_them_when_it_is_put_back(self):
        self.together.may = MAY_NOT
        for _ in range(2 + int(REST / 0.25)):
            self.assertEqual(self.flown, [])
            self.screens.carry(116, by=self.toward_the_head(0.1))
            self.ticks(1)
        self.assertEqual(self.flown, [(2, True)])

    def test_with_no_head_to_measure_from_nobody_flies(self):
        self.together.may = MAY_NOT
        for head in ("error no head pose", "ok 1 2", "ok a b c d", "ok nan 1.6 0.25 90"):
            self.screens.head = head
            self.screens.carry(116, by=(0.2, 0.0, 1.0))
            self.ticks(2)
            self.assertTrue(self.at_home(), head)
        self.assertEqual(self.flown, [])

    def test_too_fast_or_no_answer_is_asked_again_at_the_next_look(self):
        self.together.takes = False
        for wait in ("error too fast", None):
            self.together.may = wait
            self.screens.carry(116, by=(0.2, 0.0, 0.0))
            before = self.together.count("move 2 ")
            self.ticks(3)
            self.assertEqual((self.together.count("move 2 ") - before, self.screens.count("pose 116")), (3, 1), wait)
        self.together.may, self.together.takes = "ok", True
        self.ticks(1)
        self.assertTrue(near(self.together.panels[2]["at"], (-0.4, 1.5, -2.0)), self.together.panels[2])

    def test_a_grant_that_ends_in_mid_carry_is_a_refusal_from_then_on(self):
        self.together.takes = False
        self.screens.carry(116, by=self.toward_the_head(0.5))
        self.ticks(1)                                       # taken there, as far as this Frame was told
        self.together.may = MAY_NOT
        self.screens.carry(116, by=self.toward_the_head(0.5))
        self.ticks(1)
        self.screens.carry(116, by=self.toward_the_head(-0.5))     # back where it was last let
        self.ticks(2)
        self.assertEqual(self.flown, [(2, True)])           # not waited for as if its owner might still take it

    def test_once_it_is_back_a_new_pull_is_asked_about_again(self):
        self.together.may = MAY_NOT
        self.screens.carry(116, by=(-0.4, 0.0, 0.0))
        self.ticks(3)
        self.together.may = "ok"                            # they were let meanwhile
        self.screens.carry(116, by=(-0.4, 0.0, 0.0))
        self.ticks(2)
        self.assertEqual(self.together.count("move 2 "), 2)
        self.assertTrue(near(self.together.panels[2]["at"], (0.4, 1.5, -2.0)), self.together.panels[2])

    def test_what_was_known_of_a_panel_that_left_is_forgotten(self):
        self.together.may = MAY_NOT
        self.screens.carry(116, by=(-0.4, 0.0, 0.0))
        self.ticks(1)
        del self.together.panels[2]
        self.ticks()
        self.assertEqual((self.keeper.carrying.asked, self.keeper.carrying.refused, self.keeper.carrying.sized), ({}, {}, {}))

    def test_leaving_the_room_forgets_them_all(self):
        self.screens.carry(116, by=(-0.4, 0.0, 0.0))
        self.together.takes = False
        self.ticks(1)
        self.together.mode = "idle"
        self.ticks()
        self.assertEqual((self.keeper.carrying.asked, self.keeper.carrying.refused, self.keeper.carrying.sized), ({}, {}, {}))

    def test_made_larger_or_bent_by_its_viewer_it_is_made_as_the_room_has_it(self):
        self.screens("width 116 2.5")
        self.screens("curve 116 3")
        self.ticks(1)
        panel = self.screens.panels[116]
        self.assertEqual((panel["width"], panel["curve"]), ([1.5], [0.0]))
        self.assertEqual((self.screens.count("pose 116"), self.together.count("move ")), (1, 0))       # it stood where it should
        self.ticks(8)
        self.assertEqual(self.screens.count("width 116"), 3)        # the keeper's two and the test's own

    def test_the_rooms_new_size_is_its_size_here(self):
        self.together.panel(2, metres=2.0, curve=1.0)
        self.ticks()
        panel = self.screens.panels[116]
        self.assertEqual((panel["width"], panel["curve"], self.screens.count("pose 116")), ([2.0], [1.0], 1))

    def test_a_bend_ft_screens_makes_its_own_way_is_not_said_over_and_over(self):
        self.together.panel(2, curve=0.2)                   # tighter than ft-screens bends
        self.ticks(8)
        self.assertEqual((self.screens.panels[116]["curve"], self.screens.count("curve 116")), ([0.5], 2))

    def test_a_size_that_was_not_taken_is_said_again(self):
        self.together.panel(2, metres=2.0)
        self.screens.there = False
        self.ticks(2)
        self.screens.there = True
        self.ticks(2)
        self.assertEqual(self.screens.panels[116]["width"], [2.0])

    def test_when_where_it_stands_cant_be_read_it_is_left_alone(self):
        get = self.screens.__class__.__call__
        self.screens.carry(116, by=(0.4, 0.0, 0.0))
        self.keeper.screens = self.keeper.carrying.screens = lambda text: "error busy" if text.startswith("get 116") else get(self.screens, text)
        self.ticks(4)
        self.assertEqual((self.screens.count("pose 116"), self.together.count("move ")), (1, 0))
        self.together.panel(2, at=(0.0, 2.0, -2.0))         # but what the room changes is still put
        self.ticks(1)
        self.assertTrue(near(self.pose()[3::4], (0.0, 2.0, -2.0)), self.pose())


class Mine(CarryCase):
    """This Frame's own panel, moved by the others."""

    def setUp(self):
        super().setUp()
        self.ticks(1)
        self.assertEqual(self.keeper.share(1), "ok 0")
        self.ticks()
        self.assertTrue(near(self.together.panels[8]["at"], (0, 1.5, -2)))

    def test_where_someone_put_it_is_where_my_screen_goes_and_the_room_has_it(self):
        turn = placing.quat(*[placing.turned(0.5, a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1))])
        self.together.others = [(0, 0, (1.0, 1.2, -3.0), turn)]
        self.ticks(1)
        self.assertTrue(near(self.screens.at[1], (-1.0, 1.2, -1.0)), self.screens.at)      # (1, 1.2, -3) of the room, on a floor turned half round 4 m back
        panel = self.together.panels[8]
        same_turn = near(panel["turn"], turn) or near(panel["turn"], [-v for v in turn])
        self.assertTrue(near(panel["at"], (1.0, 1.2, -3.0)) and same_turn, panel)           # told to the room in the same look
        self.ticks(4)
        self.assertEqual((self.screens.count("pose 1 "), self.together.count("moves")), (1, 4 + 5))      # asked at every look in the room

    def test_a_panel_i_dont_share_goes_nowhere(self):
        self.together.others = [(1, 0, (1.0, 1.2, -3.0), (1, 0, 0, 0)), (7, 0, (1.0, 1.2, -3.0), (1, 0, 0, 0))]
        self.ticks()
        self.assertEqual(self.screens.count("pose "), 0)

    def test_the_second_of_mine_is_its_own_screen(self):
        self.assertEqual(self.keeper.share(2), "ok 1")
        self.ticks()
        self.together.others = [(1, 0, (1.0, 1.2, -3.0), (1, 0, 0, 0))]
        self.ticks(1)
        self.assertTrue(near(self.screens.at[2], (-1.0, 1.2, -1.0)) and near(self.screens.at[1], (0, 1.5, -2)), self.screens.at)

    def test_rubbish_about_them_moves_nothing(self):
        for said in ("ok 1 0:0:1:2", "ok 1 0:0:a:b:c:d:e:f:g", "ok 1 0:0:nan:1:1:1:0:0:0", "ok 1 0.5:0:1:1:1:1:0:0:0", "ok 1 0:0:1:1:1:1:0:0:0:0",
                     "error you aren't in a room", "ok", "", "no 1 0:0:1:1:1:1:0:0:0", None):
            self.together.others = said
            self.ticks(1)
        self.assertEqual(self.screens.count("pose "), 0)

    def test_outside_a_room_nobody_is_asked(self):
        self.together.mode = "idle"
        self.ticks()
        before = self.together.count("moves")
        self.ticks()
        self.assertEqual(self.together.count("moves"), before)


if __name__ == "__main__":
    unittest.main()
