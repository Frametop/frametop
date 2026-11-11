"""ft-roompanels' rules (together/roompanels): the sums between this Frame's floor and the room,
and what the keeper asks of ft-together and ft-screens, against stand-ins for both that answer
as they do.

    python3 together/tests/test_roompanels.py
"""
import json
import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fakescreens import Screens  # noqa: E402
from roompanels import keeper, placing  # noqa: E402


def near(a, b, by=1e-3):
    return len(a) == len(b) and all(abs(x - y) <= by for x, y in zip(a, b))


def turn_about_up(yaw):
    """The axes of something turned by `yaw` about the way up."""
    return tuple(placing.turned(yaw, a) for a in ((1, 0, 0), (0, 1, 0), (0, 0, 1)))


class Placing(unittest.TestCase):
    def test_a_floor_laid_puts_the_head_on_the_spot_facing_its_way(self):
        rng = random.Random(4)
        for _ in range(50):
            spot = (rng.uniform(-9, 9), 0, rng.uniform(-9, 9), rng.uniform(-3, 3))
            head = (rng.uniform(-2, 2), rng.uniform(-2, 2), rng.uniform(-3, 3))
            x, z, yaw = placing.lay(spot, head)
            at = placing.turned(yaw, (head[0], 0, head[1]))
            self.assertTrue(near((at[0] + x, at[2] + z), (spot[0], spot[2]), 1e-9))
            self.assertAlmostEqual(math.cos(head[2] + yaw), math.cos(spot[3]))
            self.assertAlmostEqual(math.sin(head[2] + yaw), math.sin(spot[3]))

    def test_a_turn_and_its_axes_are_one_thing(self):
        rng = random.Random(5)
        turns = [(1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1), (0.5, 0.5, 0.5, 0.5), (0.5, -0.5, 0.5, -0.5)]
        for _ in range(200):
            q = [rng.gauss(0, 1) for _ in range(4)]
            n = math.sqrt(sum(v * v for v in q))
            turns.append(tuple(v / n for v in q))
        for q in turns:
            back = placing.quat(*placing.axes(q))
            same = near(back, q, 1e-6) or near(back, tuple(-v for v in q), 1e-6)
            self.assertTrue(same, (q, back))
            self.assertGreaterEqual(back[0], 0)

    def test_a_turn_of_no_length_is_no_turn(self):
        self.assertEqual(placing.axes((0, 0, 0, 0)), ((1, 0, 0), (0, 1, 0), (0, 0, 1)))

    def test_to_the_room_and_back_is_where_it_stood(self):
        rng = random.Random(6)
        for _ in range(100):
            floor = (rng.uniform(-9, 9), rng.uniform(-9, 9), rng.uniform(-3, 3))
            x, y, z = placing.axes(placing.quat(*turn_about_up(rng.uniform(-3, 3))))
            got = {"at": (rng.uniform(-3, 3), rng.uniform(0, 2), rng.uniform(-3, 3)), "x": x, "y": y, "z": z, "metres": 1.2, "curve": 0.0}
            pose = placing.to_floor(floor, placing.to_room(floor, got))
            want = [x[0], y[0], z[0], got["at"][0], x[1], y[1], z[1], got["at"][1], x[2], y[2], z[2], got["at"][2]]
            self.assertTrue(near(pose, want, 1e-6), (pose, want))

    def test_a_guest_across_the_room_sees_the_back_of_a_panel_that_faces_its_owner(self):
        # The owner's floor is the room's. Their panel stands 2 m ahead of them (-z), facing them.
        panel = placing.to_room((0, 0, 0), {"at": (0, 1.5, -2), "x": (1, 0, 0), "y": (0, 1, 0), "z": (0, 0, 1), "metres": 1, "curve": 0})
        self.assertTrue(near(panel["at"], (0, 1.5, -2)) and near(panel["turn"], (1, 0, 0, 0)))
        # A guest stands 4 m ahead of the owner, looking back at them: their floor is turned half round.
        pose = placing.to_floor(placing.lay((0, 0, -4, math.pi), (0, 0, 0)), panel)
        self.assertTrue(near((pose[3], pose[7], pose[11]), (0, 1.5, -2)))      # 2 m ahead of the guest too
        self.assertTrue(near((pose[2], pose[6], pose[10]), (0, 0, -1)))       # and it faces away from them

    def test_get_is_read_or_refused(self):
        got = placing.parse_get("ok 0.1000 1.5000 -2.0000  1 0 0  0 1 0  0 0 1  1.2000 0.6750 0.000 none")
        self.assertEqual((got["at"], got["z"], got["metres"], got["curve"]), ((0.1, 1.5, -2.0), (0, 0, 1), 1.2, 0.0))
        for bad in ("", "error no screen 4", "ok 1 2 3", "ok " + "nan " * 15 + "none", "ok " + "x " * 15 + "none"):
            self.assertIsNone(placing.parse_get(bad))


class Together:
    """ft-together, as far as the keeper asks it: this Frame is person 1 of a room person 0 owns."""

    def __init__(self):
        self.mode = "joined"
        self.me = 1
        self.spot = [1.0, 0.0, 0.5, 0.0]
        self.floor = None
        self.panels = {}     # id -> the state's panel
        self.asked = []
        self.there = True

    def panel(self, ident, at=(0.0, 1.5, -2.0), turn=(1.0, 0.0, 0.0, 0.0), size=(1280, 720), metres=1.5, curve=0.0, name="Screen 1"):
        self.panels[ident] = {"id": ident, "owner": ident // 8, "number": ident % 8, "name": name, "size": list(size),
                              "at": list(at), "turn": list(turn), "metres": metres, "curve": curve}

    def __call__(self, text):
        self.asked.append(text)
        if not self.there:
            return None
        w = text.split()
        if text == "state":
            people = [{"id": 0, "name": "ada", "spot": [0, 0, 0, 0]}, {"id": self.me, "name": "ben", "spot": self.spot}]
            return "ok " + json.dumps({"mode": self.mode, "me": self.me, "owner": 0, "people": people, "panels": list(self.panels.values())})
        if text == "floor":
            return "ok none" if self.floor is None else "ok {:.4f} {:.4f} {:.5f}".format(*self.floor)
        if w[0] == "floor":
            self.floor = self.floor or tuple(float(v) for v in w[1:])
            return "ok {:.4f} {:.4f} {:.5f}".format(*self.floor)
        if w[0] == "share":
            v = [float(x) for x in w[2:13]]
            self.panel(self.me * 8 + int(w[1]), at=v[2:5], turn=v[5:9], size=(int(v[0]), int(v[1])), metres=v[9], curve=v[10], name=" ".join(w[13:]))
            return "ok"
        if w[0] == "unshare":
            self.panels.pop(self.me * 8 + int(w[1]), None)
            return "ok"
        raise AssertionError(f"ft-together was asked {text!r}")

    def count(self, start):
        return sum(1 for a in self.asked if a.startswith(start))


class KeeperCase(unittest.TestCase):
    def setUp(self):
        self.screens, self.together, self.clock = Screens(), Together(), 100.0
        self.keeper = keeper.Keeper(self.together, self.screens, lambda: self.clock)

    def ticks(self, count=3):
        for _ in range(count):
            self.keeper.tick()
            self.clock += 0.25


class Floor(KeeperCase):
    def test_it_is_laid_from_where_the_head_is(self):
        self.ticks()
        want = placing.lay(self.together.spot, (0.5, 0.25, math.pi / 2))
        self.assertTrue(near(self.together.floor, want), (self.together.floor, want))
        self.assertEqual(self.together.count("floor "), 1)
        self.assertTrue(near(self.keeper.floor, want))

    def test_one_that_stands_is_kept(self):
        self.together.floor = (3.0, 4.0, 1.0)
        self.ticks()
        self.assertEqual(self.together.count("floor "), 0)
        self.assertEqual(self.keeper.floor, (3.0, 4.0, 1.0))

    def test_with_nobody_wearing_the_headset_it_waits_then_lays_the_middle_on_the_spot(self):
        self.screens.head = "error no head pose (headset off?)"
        self.ticks(int(keeper.FLOOR_WAIT / 0.25))
        self.assertIsNone(self.together.floor)
        self.ticks(3)
        self.assertTrue(near(self.together.floor, (1.0, 0.5, 0.0)), self.together.floor)

    def test_no_panel_is_placed_before_there_is_one(self):
        self.screens.head = "error no head pose (headset off?)"
        self.together.panel(2)
        self.ticks(1)
        self.assertEqual(self.keeper.share(1), "ok 0")
        self.ticks(4)
        self.assertEqual((self.screens.remotes, self.screens.shares, self.together.count("share")), ({}, {}, 0))


class Others(KeeperCase):
    def setUp(self):
        super().setUp()
        self.together.floor = (0.0, -4.0, math.pi)   # across the room from its owner, looking back

    def test_a_panel_of_someone_elses_stands_where_the_room_has_it(self):
        self.together.panel(2, metres=1.5, curve=2.0, name="Screen 2")
        self.ticks()
        self.assertEqual(self.screens.remotes, {116: ["peer-2", "1280x720"]})
        panel = self.screens.panels[116]
        self.assertEqual((panel["label"], panel["width"], panel["curve"]), ("Screen 2", [1.5], [2.0]))
        self.assertTrue(near(panel["pose"], [-1, 0, 0, 0, 0, 1, 0, 1.5, 0, 0, -1, -2]), panel["pose"])

    def test_it_is_placed_once_and_then_left_to_its_viewer(self):
        self.together.panel(2)
        self.ticks(8)
        self.assertEqual((self.screens.count("remote 116 peer"), self.screens.count("pose"), self.screens.count("width")), (1, 1, 1))

    def test_the_room_moving_it_moves_it(self):
        self.together.panel(2)
        self.ticks()
        self.together.panel(2, at=(1.0, 1.5, -2.0))
        self.ticks()
        self.assertEqual(self.screens.count("remote 116 peer"), 1)
        self.assertTrue(near(self.screens.panels[116]["pose"][3::4], (-1.0, 1.5, -2.0)), self.screens.panels[116]["pose"])

    def test_one_that_leaves_the_room_goes(self):
        self.together.panel(2)
        self.together.panel(3)
        self.ticks()
        self.assertEqual(sorted(self.screens.remotes), [115, 116])
        del self.together.panels[2]
        self.ticks()
        self.assertEqual(self.screens.remotes, {115: ["peer-3", "1280x720"]})

    def test_another_video_size_is_another_screen(self):
        self.together.panel(2)
        self.ticks()
        self.together.panel(2, size=(1920, 1080))
        self.ticks()
        self.assertEqual(self.screens.remotes, {116: ["peer-2", "1920x1080"]})
        self.assertEqual(self.screens.count("remote 116 stop"), 1)

    def test_remote_displays_keep_their_numbers(self):
        self.screens.remotes = {101: ["mac", "1920x1080"], 116: ["oma7", "1920x1080"]}
        self.together.panel(2)
        self.ticks()
        self.assertEqual(self.screens.remotes[115], ["peer-2", "1280x720"])
        self.assertEqual((self.screens.remotes[101][0], self.screens.remotes[116][0]), ("mac", "oma7"))

    def test_with_every_number_taken_it_waits(self):
        self.screens.remotes = {n: ["host", "1920x1080"] for n in range(101, 117)}
        self.together.panel(2)
        self.ticks()
        self.assertEqual(self.screens.count("remote 1"), 0)
        del self.screens.remotes[109]
        self.ticks()
        self.assertEqual(self.screens.remotes[109], ["peer-2", "1280x720"])

    def test_my_own_panels_are_not_shown_to_me(self):
        self.together.panel(8)
        self.keeper.wishes[1] = 0
        self.ticks(1)
        self.assertEqual(self.screens.count("remote"), 1)   # only the question "remotes"

    def test_a_name_is_only_plain_characters_in_a_command(self):
        self.together.panel(2, name="x\nremote 101 stop\x00‮<b> \t café")
        self.ticks()
        self.assertEqual(self.screens.panels[116]["label"], "x remote 101 stop b caf")
        self.assertFalse(any("\n" in a or "\x00" in a for a in self.screens.asked))
        self.assertEqual(keeper.plain("\n\t", "Panel 2"), "Panel 2")
        self.assertEqual(len(keeper.plain("a" * 500, "")), 48)

    def test_what_an_earlier_one_left_is_made_again(self):
        self.screens.remotes = {116: ["peer-2", "1280x720"], 114: ["peer-5", "1280x720"]}
        self.together.panel(2)
        self.ticks()
        self.assertEqual(self.screens.remotes, {116: ["peer-2", "1280x720"]})
        self.assertEqual((self.screens.count("remote 116 stop"), self.screens.count("remote 114 stop")), (1, 1))
        self.assertIn("pose", self.screens.panels[116])

    def test_a_screen_that_went_by_itself_is_made_again(self):
        self.together.panel(2)
        self.ticks()
        self.screens.remotes.clear(), self.screens.panels.clear()   # ft-screens started again
        self.ticks()
        self.assertEqual(self.screens.remotes, {116: ["peer-2", "1280x720"]})
        self.assertIn("pose", self.screens.panels[116])

    def test_a_pose_that_was_not_taken_is_said_again(self):
        self.together.panel(2)
        self.screens.there = False
        self.ticks()
        self.screens.there = True
        self.ticks()
        self.assertIn("pose", self.screens.panels[116])


class Mine(KeeperCase):
    def setUp(self):
        super().setUp()
        self.together.floor = (0.0, -4.0, math.pi)
        self.ticks(1)

    def test_a_shared_screen_is_in_the_room_where_it_stands_here(self):
        self.assertEqual(self.keeper.share(1), "ok 0")
        self.ticks()
        panel = self.together.panels[8]
        self.assertEqual((panel["size"], panel["name"], panel["metres"], panel["curve"]), ([1920, 800], "Screen 1", 1.2, 0.0))
        self.assertTrue(near(panel["at"], (0, 1.5, -2)) and near(panel["turn"], (0, 0, 1, 0)), panel)   # (0, 1.5, 2) turned half round, then 4 m back
        self.assertEqual(self.screens.shares, {1: (0, "1920x800")})
        self.assertEqual((self.together.count("share"), self.screens.count("share 1 start")), (1, 1))

    def test_moving_the_screen_moves_it_in_the_room(self):
        self.keeper.share(1)
        self.ticks()
        self.screens.at[1] = (1.0, 1.5, -2.0)
        self.ticks()
        self.assertTrue(near(self.together.panels[8]["at"], (-1, 1.5, -2)), self.together.panels[8])
        self.assertEqual((self.together.count("share"), self.screens.count("share 1 start")), (2, 1))

    def test_three_at_most_and_only_the_desktops_own(self):
        self.screens.kwin.update({3: (1280, 800), 4: (1280, 800)})
        self.screens.at.update({3: (0, 1, -1), 4: (0, 1, -1)})
        self.assertEqual([self.keeper.share(n) for n in (2, 1, 2, 3)], ["ok 0", "ok 1", "ok 0", "ok 2"])
        self.assertTrue(self.keeper.share(4).startswith("error 3 screens"))
        self.assertTrue(self.keeper.share(101).startswith("error screen 101 isn't"))
        self.assertEqual(self.keeper.unshare(1), "ok")
        self.assertEqual(self.keeper.share(4), "ok 1")
        self.assertTrue(self.keeper.unshare(1).startswith("error"))

    def test_taking_it_back_takes_it_out_of_the_room(self):
        self.keeper.share(2)
        self.ticks()
        self.assertEqual(self.keeper.unshare(2), "ok")
        self.assertEqual((self.together.panels, self.screens.shares), ({}, {}))
        self.ticks()
        self.assertEqual((self.together.panels, self.screens.shares), ({}, {}))

    def test_a_screen_that_went_is_not_shared(self):
        self.keeper.share(2)
        self.ticks()
        del self.screens.kwin[2]
        self.ticks()
        self.assertEqual((self.together.panels, self.screens.shares, self.keeper.wishes), ({}, {}, {}))

    def test_the_video_is_never_larger_than_it_may_be_and_of_the_screens_shape(self):
        cases = {(3440, 1440): (1920, 800), (1920, 1080): (1920, 1080), (1280, 800): (1280, 800), (2560, 1600): (1920, 1200),
                 (1080, 1920): (640, 1136), (1366, 768): (1408, 792), (854, 480): (896, 504), (1900, 1200): (1792, 1128),
                 (1600, 900): (1664, 936), (1280, 720): (1280, 720), (640, 360): (640, 360), (20000, 100): (1920, 128), (100, 20000): (128, 1200)}
        for screen, video in cases.items():
            self.assertEqual(keeper.stream_size(*screen), video)

    def test_the_video_is_always_of_a_size_the_encoder_takes(self):
        # (media/irisenc.h: any other width across resets the Frame as the encoder starts)
        for width in list(range(1, 400)) + list(range(400, 8200, 7)):
            for height in (1, 99, 128, 360, 480, 600, 767, 768, 800, 900, 1050, 1080, 1200, 1440, 1600, 2160, 4320, 8191):
                across, down = keeper.stream_size(width, height)
                self.assertTrue(128 <= across <= 1920 and across % 128 == 0, (width, height, across, down))
                self.assertTrue(128 <= down <= 1200 and down % 8 == 0, (width, height, across, down))

    def test_what_an_earlier_one_shared_is_taken_back(self):
        self.together.panel(9)
        self.screens.shares[2] = (1, "1920x1080")
        self.ticks(1)
        self.assertEqual((self.together.panels, self.screens.shares), ({}, {}))

    def test_a_share_ft_together_refused_is_not_sent_pictures(self):
        self.together.there = False
        self.keeper.share(1)
        self.ticks(2)
        self.assertEqual(self.screens.shares, {})

    def test_state_says_it(self):
        self.together.panel(2)
        self.keeper.share(2)
        self.ticks()
        state = json.loads(self.keeper.state())
        self.assertEqual((state["room"], state["shares"], state["panels"]),
                         (True, [{"screen": 2, "panel": 0, "told": True}], [{"id": 2, "screen": 116, "placed": True}]))


class Panels(KeeperCase):
    """What ft-roompoints asks: the screen here that shows each of the room's panels."""

    def setUp(self):
        super().setUp()
        self.together.floor = (0.0, -4.0, math.pi)
        self.ticks(1)

    def test_with_nothing_shared_there_are_none(self):
        self.assertEqual(self.keeper.panels(), "ok 0")

    def test_someone_elses_is_the_remote_screen_it_stands_on_with_its_videos_shape(self):
        self.together.panel(2, size=(1408, 792))
        self.together.panel(19, size=(640, 1136))
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 2 2:116:1408x792 19:115:640x1136")

    def test_one_that_is_not_placed_yet_is_not_told(self):
        self.together.panel(2)
        taken = Screens.__call__
        self.screens.__class__ = type("NoPose", (Screens,), {"__call__": lambda s, text: "error busy" if text.startswith("pose") else taken(s, text)})
        self.ticks()
        self.assertEqual((sorted(self.screens.remotes), self.keeper.panels()), ([116], "ok 0"))
        self.screens.__class__ = Screens
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 1 2:116:1280x720")

    def test_a_screen_of_mine_is_the_screen_itself_as_large_as_the_desktop_has_it(self):
        self.assertEqual((self.keeper.share(2), self.keeper.share(1)), ("ok 0", "ok 1"))
        self.assertEqual(self.keeper.panels(), "ok 0")   # the room doesn't have them yet
        self.ticks()
        self.assertEqual(self.together.panels[9]["size"], [1920, 800])   # the video's size, not the screen's
        self.assertEqual(self.keeper.panels(), "ok 2 8:2:1920x1080 9:1:3440x1440")

    def test_one_ft_together_did_not_take_is_not_told(self):
        self.together.there = False
        self.keeper.share(1)
        self.ticks(2)
        self.assertEqual(self.keeper.panels(), "ok 0")

    def test_what_an_earlier_one_shared_under_the_same_number_is_not_mine_until_the_room_has_it_from_me(self):
        self.together.panel(8, size=(1280, 720))
        del self.screens.at[1]   # where the screen stands isn't known, so the room isn't told of it
        self.keeper.share(1)
        self.ticks()
        self.assertEqual((self.keeper.told, self.keeper.panels()), ({}, "ok 0"))

    def test_mine_and_theirs_together(self):
        self.together.panel(2)
        self.keeper.share(1)
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 2 8:1:3440x1440 2:116:1280x720")

    def test_taken_back_it_is_gone_at_once(self):
        self.keeper.share(1)
        self.keeper.share(2)
        self.ticks()
        self.keeper.unshare(1)
        self.assertEqual(self.keeper.panels(), "ok 1 9:2:1920x1080")
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 1 9:2:1920x1080")

    def test_a_panel_that_left_the_room_is_gone(self):
        self.together.panel(2)
        self.ticks()
        del self.together.panels[2]
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 0")

    def test_out_of_the_room_there_are_none(self):
        self.together.panel(2)
        self.keeper.share(1)
        self.ticks()
        self.together.mode, self.together.panels = "idle", {}
        self.ticks()
        self.assertEqual(self.keeper.panels(), "ok 0")


class Leaving(KeeperCase):
    def setUp(self):
        super().setUp()
        self.together.floor = (0.0, 0.0, 0.0)

    def test_outside_a_room_nothing_is_shared(self):
        self.together.mode = "idle"
        self.ticks()
        self.assertEqual(self.keeper.share(1), "error not in a room")

    def test_leaving_takes_everything_away(self):
        self.together.panel(2)
        self.ticks(1)
        self.keeper.share(1)
        self.ticks()
        self.screens.remotes[101] = ["mac", "1920x1080"]
        self.together.mode, self.together.panels = "idle", {}
        self.ticks()
        self.assertEqual((self.screens.remotes, self.screens.shares, self.keeper.wishes), ({101: ["mac", "1920x1080"]}, {}, {}))
        asked = len(self.screens.asked)
        self.ticks()
        self.assertEqual(len(self.screens.asked), asked)   # and then leaves ft-screens alone

    def test_a_start_outside_a_room_clears_what_an_earlier_one_left(self):
        self.together.mode = "idle"
        self.screens.remotes = {116: ["peer-2", "1280x720"]}
        self.screens.shares = {1: (0, "1920x800")}
        self.ticks()
        self.assertEqual((self.screens.remotes, self.screens.shares), ({}, {}))

    def test_ending_takes_everything_away_too(self):
        self.together.panel(2)
        self.ticks(1)
        self.keeper.share(1)
        self.ticks()
        self.keeper.end()
        self.assertEqual((self.screens.remotes, self.screens.shares), ({}, {}))

    def test_that_waits_for_ft_screens(self):
        self.together.mode = "idle"
        self.screens.remotes = {116: ["peer-2", "1280x720"]}
        self.screens.there = False
        self.ticks()
        self.screens.there = True
        self.ticks(1)
        self.assertEqual(self.screens.remotes, {})

    def test_a_missed_answer_is_not_the_end_of_the_room(self):
        self.together.panel(2)
        self.ticks()
        self.together.there = False
        self.ticks(keeper.MISSES - 1)
        self.assertEqual(sorted(self.screens.remotes), [116])
        self.ticks(1)
        self.assertEqual(self.screens.remotes, {})

    def test_an_answer_that_is_not_the_state_counts_as_missed(self):
        self.together.panel(2)
        self.ticks()
        self.together.__class__ = type("Odd", (Together,), {"__call__": lambda s, text: "ok [1, 2]"})
        self.ticks(keeper.MISSES)
        self.assertEqual(self.screens.remotes, {})


if __name__ == "__main__":
    unittest.main()
