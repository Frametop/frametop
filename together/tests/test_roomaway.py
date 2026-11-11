"""ft-roomaway's rules (together/roomaway): what the pointer helper and the relay say read, and
what the room is told, against stand-ins that answer as ft-together, the helper and the relay do.

    python3 together/tests/test_roomaway.py
"""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from roomaway import teller  # noqa: E402


class Reading(unittest.TestCase):
    def test_the_helpers_answer(self):
        self.assertIs(teller.worn("ok on worn"), True)
        self.assertIs(teller.worn("ok off worn on-at 12"), True)
        self.assertIs(teller.worn("ok on away"), False)
        self.assertIs(teller.worn("ok off away"), False)
        for other in (None, "", "ok on", "ok", "error what", "worn", "ok on asleep", "ok worn"):
            self.assertIsNone(teller.worn(other), other)

    def test_the_relays_answer(self):
        self.assertIs(teller.paused('{"t": "pause", "paused": true, "reason": "game"}'), True)
        self.assertIs(teller.paused('{"t":"pause","paused":false}'), False)
        for other in (None, "", "ok", "{", "[]", '{"t": "pause"}', '{"t": "pause", "paused": 1}', '{"t": "error", "paused": true}', "7"):
            self.assertIsNone(teller.paused(other), other)

    def test_how_someone_is_away(self):
        self.assertEqual(teller.away(True, False), 0)
        self.assertEqual(teller.away(None, None), 0)     # nobody can tell: they are there
        self.assertEqual(teller.away(True, True), teller.GAME)
        self.assertEqual(teller.away(None, True), teller.GAME)
        self.assertEqual(teller.away(False, False), teller.OFF)
        self.assertEqual(teller.away(False, True), teller.OFF)   # off their head in a game: off
        self.assertEqual(teller.away(False, None), teller.OFF)

    def test_how_ft_together_has_me(self):
        def state(mode, away=2):
            return "ok " + json.dumps({"mode": mode, "me": 1, "away": away, "people": [{"id": 0, "away": 0}, {"id": 1, "away": 0}]})
        self.assertEqual(teller.has(state("joined")), (2, True))
        self.assertEqual(teller.has(state("hosting", 0)), (0, True))
        self.assertEqual(teller.has(state("idle", 1)), (1, False))
        self.assertEqual(teller.has(state("calling", 0)), (0, False))
        for other in (None, "", "error no", "ok", "ok {", "ok []", 'ok {"mode": "joined"}', 'ok {"mode": "joined", "away": "x"}', 'ok {"mode": "joined", "away": true}',
                      'ok {"mode": "joined", "away": 1.5}'):
            self.assertIsNone(teller.has(other), other)


class World:
    """ft-together, the helper and the relay, as far as the teller asks them."""

    def __init__(self):
        self.mode, self.away, self.now = "joined", 0, 100.0
        self.headset, self.pause = "ok on worn", '{"t": "pause", "paused": false}'
        self.asked, self.refuse, self.down = [], False, False

    def together(self, text):
        self.asked.append(text)
        if self.down:
            return None
        if text == "state":
            return "ok " + json.dumps({"mode": self.mode, "me": 1, "away": self.away, "people": [{"id": 0, "away": 0}, {"id": 1, "away": 0}]})
        if self.refuse:
            return "error not now"
        if text == "control here":
            return "ok"
        self.away = {"away back": 0, "away game": 1, "away off": 2}[text]
        return "ok"

    def helper(self, text):
        self.asked.append("helper: " + text)
        return self.headset

    def relay(self, text):
        self.asked.append("relay: " + text)
        return self.pause

    def teller(self):
        return teller.Teller(self.together, self.helper, self.relay, lambda: self.now)

    def said(self):
        told, self.asked = [a for a in self.asked if a.startswith("away")], []
        return told


class Telling(unittest.TestCase):
    def test_without_ft_together_nobody_is_asked(self):
        w = World()
        w.down = True
        t = w.teller()
        t.tick()
        self.assertEqual((t.in_room, t.told, w.asked), (False, None, ["state"]))

    def test_outside_a_room_it_is_told_too_so_the_next_room_has_it_from_the_start(self):
        w = World()
        w.mode, w.headset = "idle", "ok on away"
        t = w.teller()
        t.tick()
        self.assertEqual((t.in_room, t.told, w.said()), (False, "off", ["away off"]))
        w.mode = "hosting"
        t.tick()
        self.assertEqual((t.in_room, w.said()), (True, []))

    def test_worn_and_not_paused_only_that_they_are_there_is_told(self):
        w = World()
        t = w.teller()
        t.tick()
        self.assertEqual(w.asked, ["state", "helper: gaze ? headset", "relay: pause ?", "control here"])
        self.assertEqual((t.in_room, t.told), (True, None))

    def test_that_they_are_there_is_said_each_look_and_only_while_the_helper_says_so(self):
        def here(w, t):
            w.asked = []
            t.tick()
            return w.asked.count("control here")

        w = World()
        t = w.teller()
        self.assertEqual((here(w, t), here(w, t)), (1, 1))
        w.pause = '{"t": "pause", "paused": true}'
        self.assertEqual(here(w, t), 0)     # in a game
        w.pause, w.headset = '{"t": "pause", "paused": false}', "ok on away"
        self.assertEqual(here(w, t), 0)     # off their head
        w.headset = "ok on worn"
        self.assertEqual(here(w, t), 1)
        w.mode = "idle"
        self.assertEqual(here(w, t), 0)     # in no room there is nothing to stand
        w.mode, w.headset = "hosting", None
        w.now += teller.FRESH + 1
        self.assertEqual(here(w, t), 0)     # nobody knows: taken to be there for the room, not for this

    def test_the_headset_off_a_game_and_back(self):
        w = World()
        t = w.teller()
        w.headset = "ok on away"
        t.tick()
        self.assertEqual((w.said(), t.told), (["away off"], "off"))
        t.tick()
        self.assertEqual(w.said(), [])    # told once
        w.headset, w.pause = "ok on worn", '{"t": "pause", "paused": true}'
        t.tick()
        self.assertEqual((w.said(), t.told), (["away game"], "game"))
        w.headset = "ok off away"
        t.tick()
        self.assertEqual(w.said(), ["away off"])    # off their head in a game: off
        w.headset, w.pause = "ok off worn", '{"t": "pause", "paused": false}'
        t.tick()
        self.assertEqual((w.said(), t.told), (["away back"], "back"))

    def test_what_the_room_has_is_put_right(self):
        w = World()
        t = w.teller()
        w.away = 2    # another program said so, or an earlier run did
        t.tick()
        self.assertEqual(w.said(), ["away back"])
        w.refuse, w.headset = True, "ok on away"
        t.tick()
        t.tick()
        self.assertEqual((w.said(), t.told), (["away off", "away off"], "back"))   # asked again until it is taken
        w.refuse = False
        t.tick()
        self.assertEqual((w.said(), t.told), (["away off"], "off"))

    def test_an_answer_stands_for_a_few_seconds_then_they_are_taken_to_be_there(self):
        w = World()
        t = w.teller()
        w.headset, w.pause = "ok on away", '{"t": "pause", "paused": true}'
        t.tick()
        self.assertEqual(w.said(), ["away off"])
        w.headset = None    # the helper ended
        w.now += teller.FRESH - 0.5
        t.tick()
        self.assertEqual(w.said(), [])
        w.now += 1.0
        t.tick()
        self.assertEqual(w.said(), ["away game"])    # the relay still answers
        w.pause = "rubbish"
        w.now += teller.FRESH + 0.1
        t.tick()
        self.assertEqual(w.said(), ["away back"])

    def test_an_ft_together_that_started_again_is_told_again(self):
        w = World()
        t = w.teller()
        w.headset = "ok on away"
        t.tick()
        w.down = True
        t.tick()
        self.assertEqual((t.in_room, t.told), (False, None))
        w.said()
        w.down, w.mode, w.away = False, "idle", 0
        t.tick()
        self.assertEqual((w.said(), t.told), (["away off"], "off"))


if __name__ == "__main__":
    unittest.main()
