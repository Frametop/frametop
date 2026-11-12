"""The windows of its person's PC in the room, the rules (together/roompanels/pcshares.py):
which windows the wearer shares, and what the room is told of them, with the keeper around
them, against stand-ins for ft-together, ft-screens and the PC's side (pclink.py, whose own
rules test_roompclink.py has). This Frame is person 1 of a room person 0 owns, so its PC's
panels 3 to 6 are the room's 11 to 14.

    python3 together/tests/test_roompcs.py
"""
import json
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from roompanels import keeper, pcshares  # noqa: E402
from test_roompanels import KeeperCase, near  # noqa: E402
from test_roompclink import CALC, ELSEWHERE, NOTES  # noqa: E402


class Sends:
    """The PC's side, as far as the shares ask it."""

    def __init__(self):
        self.wanted, self.encoded, self.gone, self.held = None, {}, set(), ""

    def want(self, wanted):
        self.wanted = dict(wanted)

    def sizes(self):
        return dict(self.encoded)

    def take_gone(self):
        gone, self.gone = self.gone, set()
        return gone

    def why(self):
        return self.held


AT_101 = [1, 0, 0, 0.4, 0, 1, 0, 1.3, 0, 0, 1, -1.1]      # where the window's panel stands on this Frame's floor


class SharesCase(KeeperCase):
    def setUp(self):
        super().setUp()
        self.link, self.windows = Sends(), {101: CALC, 102: NOTES}
        self.pcs = pcshares.PcShares(self.together, self.screens, lambda: self.windows, self.link)
        self.keeper = keeper.Keeper(self.together, self.screens, lambda: self.clock, pcs=self.pcs)
        self.together.floor = (0.0, 0.0, 0.0)             # this Frame's floor is the room's
        self.panel(101, CALC, "1280x720")
        self.panel(102, NOTES, "1920x1080")
        self.ticks(1)

    def panel(self, number, window, size):
        self.screens.remotes[number] = [window.client, size]
        self.screens.panels[number] = {"pose": list(AT_101), "width": [0.9]}

    def shared(self):
        """The room's panels of this Frame's: {id: (size, name)}."""
        return {i: (tuple(p["size"]), p["name"]) for i, p in self.together.panels.items() if p["owner"] == 1}


class Shares(SharesCase):
    def test_a_pc_window_is_shared_as_one_of_the_pcs_panels(self):
        self.assertEqual((self.keeper.share(101), self.keeper.share(102), self.keeper.share(101)), ("ok 3", "ok 4", "ok 3"))
        self.assertEqual(self.keeper.share(1), "ok 0")            # a screen of the desktop's is one of the Frame's
        self.ticks()
        self.assertEqual(self.link.wanted, {3: CALC, 4: NOTES})

    def test_only_what_is_a_window_of_the_pcs_and_four_of_them(self):
        self.assertEqual(self.keeper.share(103), "error screen 103 isn't a window of your PC's")
        self.windows = None
        self.assertEqual(self.keeper.share(101), "error the saved layout doesn't read")
        self.windows = {n: CALC._replace(window=f"0x{n}") for n in range(101, 106)}
        self.assertEqual([self.keeper.share(n) for n in range(101, 106)], ["ok 3", "ok 4", "ok 5", "ok 6", "error 4 windows are shared already"])
        self.assertEqual(self.keeper.unshare(102), "ok")
        self.assertEqual(self.keeper.share(105), "ok 4")          # the panel that came free

    def test_one_pc_at_a_time(self):
        self.windows[103] = ELSEWHERE
        self.assertEqual(self.keeper.share(101), "ok 3")
        self.assertEqual(self.keeper.share(103), "error windows of another PC are shared already: one PC at a time")
        self.assertEqual(self.keeper.unshare(101), "ok")
        self.assertEqual(self.keeper.share(103), "ok 3")

    def test_out_of_a_room_nothing_is_shared(self):
        self.together.mode = "none"
        self.ticks(1)
        self.assertEqual(self.keeper.share(101), "error not in a room")

    def test_the_room_is_told_once_the_pc_says_how_large_it_encodes_it(self):
        self.keeper.share(101)
        self.ticks()
        self.assertEqual((self.shared(), self.together.count("share")), ({}, 0))
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.assertEqual(self.shared(), {11: ((1280, 720), "Calculator")})
        panel = self.together.panels[11]
        self.assertTrue(near(panel["at"], (0.4, 1.3, -1.1)) and near(panel["turn"], (1, 0, 0, 0)), panel)
        self.assertAlmostEqual(panel["metres"], 0.9)
        self.ticks(8)
        self.assertEqual(self.together.count("share"), 1)         # told once while nothing changes

    def test_where_it_stands_is_this_frames_floor_put_into_the_room(self):
        self.together.floor = (0.0, -4.0, math.pi)                # across the room from its owner, looking back
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.assertTrue(near(self.together.panels[11]["at"], (-0.4, 1.3, -2.9)), self.together.panels[11])

    def test_no_floor_no_telling(self):
        self.together.floor, self.together.fixed = None, True
        self.screens.head = "error no head pose (headset off?)"
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks(2)
        self.assertEqual(self.together.count("share"), 0)

    def test_what_ft_together_refuses_the_room_doesnt_have(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.pcs.together = lambda text: "error the room is full" if text.startswith("share") else self.together(text)
        self.ticks()
        self.assertEqual((json.loads(self.keeper.state())["shares"][0]["told"], self.keeper.panels()), (False, "ok 0"))

    def test_an_earlier_programs_panel_under_a_new_shares_number_isnt_ours_until_the_room_is_told(self):
        self.together.panel(11, name="An old window")
        self.together.others = [(3, 0, (0.0, 1.5, -2.0), (1.0, 0.0, 0.0, 0.0))]
        self.keeper.share(101)
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 0")
        self.assertTrue(near(self.screens.panels[101]["pose"][3::4], (0.4, 1.3, -1.1)), self.screens.panels[101])

    def test_moved_or_encoded_at_another_size_the_room_is_told_again(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.screens.panels[101]["pose"][3] = 1.0
        self.ticks()
        self.assertTrue(near(self.together.panels[11]["at"], (1.0, 1.3, -1.1)))
        self.link.encoded = {3: (1024, 768)}
        self.ticks()
        self.assertEqual((self.shared(), self.together.count("share")), ({11: ((1024, 768), "Calculator")}, 3))

    def test_a_title_is_cut_down_to_plain_characters_and_one_without_gets_a_name(self):
        self.windows = {101: CALC._replace(label="Résumé\n— draft"), 102: NOTES._replace(label="")}
        self.keeper.share(101), self.keeper.share(102)
        self.link.encoded = {3: (1280, 720), 4: (1920, 1080)}
        self.ticks()
        self.assertEqual(self.shared(), {11: ((1280, 720), "R sum draft"), 12: ((1920, 1080), "Window 2")})

    def test_a_window_whose_panel_here_went_is_taken_back(self):
        self.keeper.share(101), self.keeper.share(102)
        self.link.encoded = {3: (1280, 720), 4: (1920, 1080)}
        self.ticks()
        del self.screens.remotes[101]                             # its panel was closed
        self.ticks()
        self.assertEqual((self.shared(), self.link.wanted), ({12: ((1920, 1080), "Notes")}, {4: NOTES}))
        self.screens.remotes[102][0] = "frametop-pc-main"         # the screen is another stream's by now
        self.ticks()
        self.assertEqual((self.shared(), self.link.wanted), ({}, {}))

    def test_a_window_that_left_the_layout_is_taken_back(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        del self.windows[101]
        self.ticks()
        self.assertEqual((self.shared(), self.link.wanted, self.keeper.unshare(101)), ({}, {}, "error screen 101 isn't shared"))

    def test_a_window_that_went_from_the_pc_is_taken_back(self):
        self.keeper.share(101), self.keeper.share(102)
        self.link.encoded = {3: (1280, 720), 4: (1920, 1080)}
        self.ticks()
        self.link.gone, self.link.encoded = {4}, {3: (1280, 720)}
        self.ticks()
        self.assertEqual((self.shared(), self.link.wanted), ({11: ((1280, 720), "Calculator")}, {3: CALC}))

    def test_while_the_layout_or_ft_screens_doesnt_read_nothing_changes(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        asked = self.together.count("share") + self.together.count("unshare")
        self.windows = None
        self.ticks()
        self.windows = {101: CALC, 102: NOTES}
        for said in ("ok 1 101:frametop-pc-win1:live", "ok 1 x:frametop-pc-win1:live:1280x720", "ok 1 101:frametop-pc-win1:live:1280", "ok 1 101:other:live:1280x720:more", "error busy"):
            real = self.screens.__class__.__call__
            self.pcs.screens = lambda text, said=said: said if text == "remotes" else real(self.screens, text)
            self.ticks()
        self.assertEqual((self.shared(), self.together.count("share") + self.together.count("unshare")), ({11: ((1280, 720), "Calculator")}, asked))

    def test_taken_back_it_leaves_the_room_and_the_pc(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.assertEqual(self.keeper.unshare(101), "ok")
        self.assertEqual((self.shared(), self.keeper.panels()), ({}, "ok 0"))      # at once
        self.assertEqual(self.keeper.share(102), "ok 3")                           # the panel is free for another window,
        self.assertEqual(json.loads(self.keeper.state())["shares"], [{"screen": 102, "panel": 3, "told": False}])   # which the room hasn't yet
        self.keeper.unshare(102)
        self.ticks()
        self.assertEqual((self.shared(), self.link.wanted), ({}, {}))

    def test_an_earlier_programs_pc_panels_are_taken_back_and_the_frames_own_left(self):
        self.together.panel(12, name="An old window")             # person 1's panel 4: nobody here shares it
        self.together.panel(3, name="Ada's window")               # person 0's PC panels aren't ours to take
        self.together.panel(5, name="Ada's other window")
        self.assertEqual(self.keeper.share(1), "ok 0")
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.assertEqual(sorted(self.together.panels), [3, 5, 8, 11])
        self.ticks(4)
        self.assertEqual(sorted(self.together.panels), [3, 5, 8, 11])   # (and neither takes the other's back)
        self.assertEqual([a for a in self.together.asked if a.startswith("unshare")], ["unshare 4"])
        self.together.panel(19, name="Cy's window")               # person 2's panel 3 is not our panel 3
        self.ticks()
        self.assertEqual(self.pcs.mine, {11: (101, (1280, 720))})
        self.assertEqual((self.keeper.unshare(1), self.keeper.unshare(101)), ("ok", "ok"))

    def test_leaving_the_room_takes_everything_back_and_stops_the_pc(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.keeper.end()
        self.assertEqual((self.shared(), self.link.wanted, self.pcs.wishes), ({}, {}, {}))

    def test_where_the_others_put_it_is_where_its_panel_goes(self):
        self.keeper.share(101)
        self.link.encoded = {3: (1280, 720)}
        self.ticks()
        self.together.others = [(3, 0, (0.0, 1.5, -2.0), (1.0, 0.0, 0.0, 0.0))]
        self.ticks(1)
        self.assertTrue(near(self.screens.panels[101]["pose"][3::4], (0.0, 1.5, -2.0)), self.screens.panels[101])
        self.ticks()
        self.assertTrue(near(self.together.panels[11]["at"], (0.0, 1.5, -2.0)))     # and the room is told it stands there

    def test_its_state_and_its_panels_are_in_the_keepers(self):
        self.keeper.share(101)
        state = json.loads(self.keeper.state())
        self.assertEqual(state["shares"], [{"screen": 101, "panel": 3, "told": False}])
        self.assertEqual(state["windows"], [{"screen": 101, "name": "Calculator"}, {"screen": 102, "name": "Notes"}])
        self.link.encoded, self.link.held = {3: (1024, 576)}, "the PC doesn't answer"
        self.ticks()
        state = json.loads(self.keeper.state())
        self.assertEqual((state["shares"], state["pc"]), ([{"screen": 101, "panel": 3, "told": True}], "the PC doesn't answer"))
        self.assertEqual(self.keeper.panels(), "ok 1 11:101:1280x720")      # (as large as its panel here is, not as its video)
        self.windows = None
        self.assertEqual(json.loads(self.keeper.state())["windows"], [])
        self.windows = {101: CALC, 102: NOTES}
        self.assertEqual(self.keeper.share(1), "ok 0")
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 2 8:1:3440x1440 11:101:1280x720")


class WithoutAPc(KeeperCase):
    def test_a_keeper_with_no_pc_side_shares_no_remote_screen(self):
        self.ticks(1)
        self.assertEqual(self.keeper.share(101), "error screen 101 isn't one of this desktop's screens")
        self.assertEqual(self.keeper.unshare(101), "error screen 101 isn't shared")
        state = json.loads(self.keeper.state())
        self.assertEqual((state["windows"], state["pc"]), ([], ""))


if __name__ == "__main__":
    unittest.main()
