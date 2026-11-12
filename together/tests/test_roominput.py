"""ft-roominput's rules (together/roominput): what ft-together and ft-roompanels say read, and
what ft-screens is told of the others' input, against stand-ins that answer as they do.

    python3 together/tests/test_roominput.py
"""
import json
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from roominput import giver  # noqa: E402


def act(panel, what, a, b, person=1):
    """An act as ft-together sends it to the program that takes the input (local/inputs.h)."""
    return bytes([giver.INPUT, person]) + struct.pack("<BBii", panel, what, a, b)


def control(*numbers):
    return "ok " + json.dumps({"there": True, "left": 9.5, "asks": [], "given": [{"number": n, "person": 1} for n in numbers], "holder": -1, "held": [], "asked": []})


def panels(shares):
    return "ok " + json.dumps({"room": True, "floor": None, "panels": [], "shares": [{"screen": s, "panel": k, "told": told} for s, k, told in shares]})


class Reading(unittest.TestCase):
    def test_what_ft_together_has_given(self):
        self.assertEqual(giver.given(control()), set())
        self.assertEqual(giver.given(control(0, 2)), {0, 2})
        for other in (None, "", "ok", "error you aren't in a room", "ok {", "ok []", 'ok {"asks": []}', 'ok {"given": 3}', 'ok {"given": [3]}',
                      'ok {"given": [{"person": 1}]}', 'ok {"given": [{"number": "x"}]}', 'ok {"given": [{"number": null}]}'):
            self.assertIsNone(giver.given(other), other)

    def test_which_screen_a_panel_is(self):
        self.assertEqual(giver.shared(panels([(2, 0, True), (1, 1, True), (3, 2, False)])), {0: 2, 1: 1})   # one the room doesn't have yet is none
        self.assertEqual(giver.shared(panels([])), {})
        for other in (None, "", "ok", "error share N | unshare N | state | panels", "ok {", "ok []", 'ok {"shares": 1}', 'ok {"shares": [1]}',
                      'ok {"shares": [{"screen": 1}]}', 'ok {"shares": [{"screen": "x", "panel": 0, "told": true}]}', 'ok {"shares": [{"screen": 1, "panel": 0}]}'):
            self.assertEqual(giver.shared(other), {}, other)
        self.assertEqual(giver.shared('ok {"shares": [{"screen": 1, "panel": 0, "told": 1}]}'), {}, "told is true or it isn't")

    def test_an_act(self):
        self.assertEqual(giver.read(act(9, giver.MOVE, 65535, 0)), (9, giver.MOVE, 65535, 0))
        self.assertEqual(giver.read(act(8, giver.SCROLL, -120, 240)), (8, giver.SCROLL, -120, 240))
        for other in (b"", b"\x85", act(9, 1, 1, 1)[:-1], act(9, 1, 1, 1) + b"\0", b"\x84" + act(9, 1, 1, 1)[1:], b"ok", b"error another program takes the input"):
            self.assertIsNone(giver.read(other), other)


class Desk:
    """ft-together, ft-roompanels and ft-screens, as far as the giver asks and tells them."""

    def __init__(self):
        self.control, self.panels = control(), panels([(2, 0, True)])
        self.asked, self.told, self.now = [], [], 100.0
        self.giver = giver.Giver(lambda text: self._ask("together", text, self.control), lambda text: self._ask("panels", text, self.panels), self.told.append, lambda: self.now)

    def _ask(self, who, text, reply):
        self.asked.append((who, text))
        return reply

    def took(self):
        """What ft-screens was told since the last look."""
        told, self.told[:] = list(self.told), []
        return told


class Giving(unittest.TestCase):
    def test_nothing_is_said_while_nobody_has_a_panel(self):
        d = Desk()
        d.giver.look()
        self.assertEqual((d.took(), d.giver.live, d.asked), ([], False, [("together", "control")]))   # nor is ft-roompanels asked
        d.control = None   # ft-together isn't there
        d.giver.look()
        self.assertEqual((d.took(), d.giver.live), ([], False))

    def test_while_someone_has_one_ft_screens_hears_it_each_look(self):
        d = Desk()
        d.control = control(0)
        d.giver.look()
        d.giver.look()
        self.assertEqual((d.took(), d.giver.live), (["peer on", "peer on"], True))
        self.assertEqual(d.asked.count(("panels", "state")), 2)

    def test_an_act_goes_to_the_screen_its_panel_is(self):
        d = Desk()
        d.control, d.panels = control(0, 1), panels([(2, 0, True), (1, 1, True)])
        d.giver.look()
        d.took()
        for one in (act(8, giver.MOVE, 30000, 65535), act(8, giver.BUTTON, 1, 1), act(8, giver.BUTTON, 1, 0), act(9, giver.MOVE, 0, 0),
                    act(9, giver.KEY, 30, 1), act(9, giver.KEY, 30, 0), act(8, giver.SCROLL, -240, 120)):
            d.giver.act(one)
        self.assertEqual(d.took(), ["peer 2 move 30000 65535", "peer 2 button 1 1", "peer 2 button 1 0", "peer 1 move 0 0", "peer 1 key 30 1", "peer 1 key 30 0",
                                    "peer 2 scroll -240 120"])
        self.assertEqual((d.giver.passed, d.giver.dropped), (7, 0))

    def test_a_window_of_the_pcs_is_the_remote_screen_it_stands_on(self):
        # ft-roompanels shares a window of the wearer's PC as the remote screen its stream is on
        # (roompanels/pcshares.py): the act goes there, and ft-screens gives it to that stream
        d = Desk()
        d.control, d.panels = control(3, 0), panels([(1, 0, True), (101, 3, True), (102, 4, False)])
        d.giver.look()
        d.took()
        for what, a, b in ((giver.MOVE, 100, 200), (giver.BUTTON, 1, 1), (giver.KEY, 30, 1)):
            d.giver.act(act(8 + 3, what, a, b))
        d.giver.act(act(8 + 4, giver.MOVE, 1, 1))     # not told to the room yet: no screen
        self.assertEqual(d.took(), ["peer 101 move 100 200", "peer 101 button 1 1", "peer 101 key 30 1"])
        self.assertEqual((d.giver.passed, d.giver.dropped), (3, 1))

    def test_whose_place_the_panel_is_in_doesnt_matter(self):
        """A panel's id is its owner's place * 8 + its number, and ft-together passes on only acts for this Frame's."""
        d = Desk()
        d.control = control(0)
        d.giver.look()
        d.took()
        d.giver.act(act(0, giver.MOVE, 1, 2))
        d.giver.act(act(24, giver.MOVE, 3, 4))
        self.assertEqual(d.took(), ["peer 2 move 1 2", "peer 2 move 3 4"])

    def test_a_let_go_lets_everything_go(self):
        d = Desk()
        d.giver.act(act(0, giver.LETGO, 0, 0, person=255))
        self.assertEqual(d.took(), ["peer letgo"])

    def test_what_isnt_an_act_or_is_for_no_screen_is_dropped(self):
        d = Desk()
        d.control = control(0)
        d.giver.look()
        d.took()
        for one in (b"", b"ok", act(8, 5, 1, 1), act(8, 255, 1, 1), act(8, giver.MOVE, 1, 1)[:-2]):
            d.giver.act(one)
        self.assertEqual(d.took(), [])
        d.giver.act(act(10, giver.MOVE, 1, 1))   # panel 2 of this Frame's: ft-roompanels has no such share
        self.assertEqual((d.took(), d.giver.dropped), ([], 3))

    def test_a_panel_given_since_the_last_look_is_asked_about_at_once_but_not_too_often(self):
        d = Desk()
        d.giver.act(act(8, giver.MOVE, 1, 1))   # before any look
        self.assertEqual((d.took(), d.asked), (["peer 2 move 1 1"], [("panels", "state")]))
        d.giver.act(act(9, giver.MOVE, 1, 1))   # not known, and asked about a moment ago
        d.now += 0.2
        d.giver.act(act(9, giver.MOVE, 2, 2))
        self.assertEqual((d.took(), len(d.asked), d.giver.dropped), ([], 1, 2))
        d.panels = panels([(2, 0, True), (3, 1, True)])
        d.now += 0.1
        d.giver.act(act(9, giver.MOVE, 3, 3))
        self.assertEqual((d.took(), len(d.asked)), (["peer 3 move 3 3"], 2))

    def test_a_screen_that_is_shared_no_more_takes_nothing_after_the_next_look(self):
        d = Desk()
        d.control = control(0)
        d.giver.look()
        d.panels = panels([])
        d.giver.look()
        d.took()
        d.giver.act(act(8, giver.BUTTON, 1, 1))
        self.assertEqual(d.took(), [])

    def test_when_nobody_has_a_panel_any_more_everything_is_let_go_once(self):
        d = Desk()
        d.control = control(0)
        d.giver.look()
        d.control = control()
        d.giver.look()
        d.giver.look()
        self.assertEqual((d.took(), d.giver.live), (["peer on", "peer letgo"], False))

    def test_when_ft_together_stops_answering_everything_is_let_go_once(self):
        d = Desk()
        d.control = control(1)
        d.giver.look()
        for gone in (None, "error what", None):
            d.control = gone
            d.giver.look()
        self.assertEqual((d.took(), d.giver.live), (["peer on", "peer letgo"], False))

    def test_the_end_lets_go(self):
        d = Desk()
        d.giver.end()
        self.assertEqual(d.took(), ["peer letgo"])


if __name__ == "__main__":
    unittest.main()
