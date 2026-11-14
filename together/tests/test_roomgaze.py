"""ft-roomgaze's rules (together/roomgaze): what ft-together, the pointer helper and the gaze
service say read, and what ft-together is told of where the wearer looks, against stand-ins
that answer as they do.

    python3 together/tests/test_roomgaze.py
"""
import json
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from roomgaze import feeder  # noqa: E402


def status(yaw=10.0, pitch=-5.0, age=0.02, awake=True):
    return json.dumps({"awake": awake, "sample_age_s": age, "last": [yaw, pitch, yaw + 1, pitch + 1], "mode": "on"})


class Reading(unittest.TestCase):
    def test_whether_ft_together_shares_it(self):
        self.assertIs(feeder.shares('ok {"shared":true,"fresh":false,"quiet":false}'), True)
        self.assertIs(feeder.shares('ok {"shared":false,"fresh":false,"quiet":true}'), False)
        for other in (None, "", "ok", "error gaze | gaze on", "ok {", "ok []", 'ok {"fresh":true}', 'ok {"shared":1}', 'ok {"shared":"true"}',
                      '{"shared":true}'):
            self.assertIsNone(feeder.shares(other), other)

    def test_whether_the_wearer_is_there(self):
        def state(mode, away=0):
            return "ok " + json.dumps({"mode": mode, "me": 1, "away": away, "people": []})
        self.assertIs(feeder.there(state("joined")), True)
        self.assertIs(feeder.there(state("hosting")), True)
        self.assertIs(feeder.there(state("joined", 1)), False)
        self.assertIs(feeder.there(state("hosting", 2)), False)
        self.assertIs(feeder.there(state("idle")), False)
        self.assertIs(feeder.there(state("calling")), False)
        for other in (None, "", "error no", "ok", "ok {", "ok []", 'ok {"mode": "joined"}', 'ok {"mode": "joined", "away": "0"}',
                      'ok {"mode": "joined", "away": false}'):
            self.assertIsNone(feeder.there(other), other)

    def test_what_moves_the_pointer(self):
        self.assertIs(feeder.eyes("ok on"), True)
        self.assertIs(feeder.eyes("ok off"), False)
        self.assertIs(feeder.eyes("ok on worn"), True)
        for other in (None, "", "ok", "on", "error what", "ok maybe", "no on"):
            self.assertIsNone(feeder.eyes(other), other)

    def test_where_the_gaze_service_has_them_looking(self):
        self.assertEqual(feeder.sample(status()), (10.0, -5.0))
        self.assertEqual(feeder.sample(status(age=feeder.FRESH_S)), (10.0, -5.0))
        self.assertEqual(feeder.sample(status(-90, 90, age=0)), (-90.0, 90.0))
        self.assertEqual(feeder.sample(json.dumps({"awake": True, "sample_age_s": 0, "last": [3, 4]})), (3.0, 4.0))
        self.assertIsNone(feeder.sample(status(age=feeder.FRESH_S + 0.01)), "an old one")
        self.assertIsNone(feeder.sample(status(age=-0.1)), "one from the future")
        self.assertIsNone(feeder.sample(status(awake=False)), "a gaze service that idles")
        self.assertIsNone(feeder.sample(status(90.5, 0)), "further than an eye turns")
        self.assertIsNone(feeder.sample(status(0, -90.5)))
        for other in (None, "", "ok", "[]", "7", "{", '{"awake": true}', '{"awake": 1, "sample_age_s": 0, "last": [1, 2]}',
                      '{"awake": true, "sample_age_s": null, "last": [1, 2]}', '{"awake": true, "sample_age_s": 0, "last": null}',
                      '{"awake": true, "sample_age_s": 0, "last": [1]}', '{"awake": true, "sample_age_s": 0, "last": ["1", 2]}',
                      '{"awake": true, "sample_age_s": 0, "last": [true, 2]}', '{"awake": true, "sample_age_s": 0, "last": [1, NaN]}',
                      '{"awake": true, "sample_age_s": true, "last": [1, 2]}', '{"awake": true, "sample_age_s": 0, "last": {"0": 1, "1": 2}}'):
            self.assertIsNone(feeder.sample(other), other)


class World:
    """ft-together, the pointer helper and the gaze service, as far as the feeder asks them."""

    def __init__(self):
        self.now = self.due = self.until = 100.0   # the time, when the next pass is due, and how far the runs went
        self.shared, self.mode, self.away = False, "joined", 0
        self.mode_said = "ok off"                # the helper's answer
        self.status = status()                   # the gaze service's
        self.down, self.refuse = False, False    # ft-together isn't there; it takes no direction
        self.deaf = False                        # ...and no word on the pointer
        self.asked = []                          # (whom, what), in order
        self.feeder = feeder.Feeder(self.together, self.helper, self.gazed, lambda: self.now)

    def together(self, text):
        self.asked.append(("together", text))
        if self.down:
            return None
        if text == "gaze":
            return "ok " + json.dumps({"shared": self.shared, "fresh": False, "quiet": False})
        if text == "state":
            return "ok " + json.dumps({"mode": self.mode, "me": 1, "away": self.away, "people": []})
        if text.startswith("gaze say "):
            return "error you don't share where you look" if self.refuse or not self.shared else "ok"
        return "ok" if text in ("gaze pointer eyes", "gaze pointer hand") and not self.deaf else "error what"

    def helper(self, text):
        self.asked.append(("helper", text))
        return self.mode_said

    def gazed(self, text):
        self.asked.append(("gazed", text))
        return self.status if text == "status" else "ok"

    def tick(self):
        """The pass that is due. How long it said to wait."""
        self.now = self.due
        wait = self.feeder.tick()
        self.due = self.now + wait
        return wait

    def run(self, seconds):
        """The passes of that much longer, each when the one before said it is due. What was
        asked."""
        self.asked.clear()
        self.until += seconds
        while self.due <= self.until + 1e-6:
            self.tick()
        return list(self.asked)

    def said(self):
        """The directions told since the last run began."""
        return [tuple(float(v) for v in text.split()[2:]) for whom, text in self.asked if text.startswith("gaze say ")]

    def of(self, whom):
        return [text for who, text in self.asked if who == whom]


class Feeding(unittest.TestCase):
    def test_without_ft_together_nobody_else_is_asked(self):
        w = World()
        w.down = True
        self.assertEqual(w.tick(), feeder.IDLE_S)
        self.assertEqual(w.asked, [("together", "gaze")])
        self.assertEqual((w.feeder.running, w.feeder.pointer, w.feeder.shared, w.feeder.feeding), (False, None, False, False))
        self.assertEqual(w.run(6), [("together", "gaze")] * 3)

    def test_it_tells_what_moves_the_pointer_each_second(self):
        w = World()
        self.assertEqual(w.tick(), feeder.LOOK_S)
        self.assertEqual(w.asked, [("together", "gaze"), ("helper", "gaze ?"), ("together", "gaze pointer hand")])
        self.assertEqual((w.feeder.running, w.feeder.pointer, w.feeder.shared, w.feeder.feeding), (True, "hand", False, False))
        w.mode_said = "ok on"
        self.assertEqual(w.run(3), [("together", "gaze"), ("helper", "gaze ?"), ("together", "gaze pointer eyes")] * 3)
        self.assertEqual(w.feeder.pointer, "eyes")

    def test_a_helper_that_says_nothing_is_no_word_on_the_pointer(self):
        w = World()
        w.tick()
        for said in (None, "error what", "ok"):
            w.mode_said = said
            self.assertEqual(w.run(2), [("together", "gaze"), ("helper", "gaze ?")] * 2, said)
            self.assertIsNone(w.feeder.pointer)
        w.mode_said = "ok off"
        w.run(1)
        self.assertEqual(w.feeder.pointer, "hand")

    def test_a_word_on_the_pointer_that_isnt_taken_isnt_one(self):
        w = World()
        w.deaf = True
        self.assertEqual(w.run(1), [("together", "gaze"), ("helper", "gaze ?"), ("together", "gaze pointer hand")] * 2)
        self.assertIsNone(w.feeder.pointer)
        w.deaf = False
        w.run(1)
        self.assertEqual(w.feeder.pointer, "hand")

    def test_the_gaze_service_is_left_alone_while_nothing_is_shared(self):
        w = World()
        w.run(5)
        self.assertEqual(w.of("gazed"), [])
        self.assertNotIn("state", w.of("together"))

    def test_shared_and_there_the_direction_is_told_five_times_a_second(self):
        w = World()
        w.shared = True
        self.assertEqual(w.tick(), feeder.SAMPLE_S)
        self.assertEqual(w.asked, [("together", "gaze"), ("helper", "gaze ?"), ("together", "gaze pointer hand"), ("together", "state"),
                                   ("gazed", "wake 3"), ("gazed", "status"), ("together", "gaze say 10.0 -5.0")])
        self.assertEqual((w.feeder.shared, w.feeder.feeding, w.feeder.told), (True, True, True))
        w.run(2)
        self.assertEqual(w.of("gazed").count("status"), 10)
        self.assertEqual(w.of("gazed").count("wake 3"), 2)
        self.assertEqual(w.of("together").count("gaze"), 2)
        self.assertEqual(w.said(), [(10.0, -5.0)] * 10)

    def test_it_is_smoothed(self):
        w = World()
        w.shared = True
        w.run(1)
        w.status = status(20.0, 5.0)
        w.run(0.2)
        part = 1 - math.exp(-feeder.SAMPLE_S / feeder.SMOOTH_S)
        self.assertEqual(w.said(), [(round(10 + 10 * part, 1), round(-5 + 10 * part, 1))])
        self.assertTrue(0.3 < part < 0.6)
        w.run(3)
        self.assertEqual(w.said()[-1], (20.0, 5.0))
        self.assertEqual(w.said(), sorted(w.said()))

    def test_an_old_direction_is_told_to_nobody(self):
        w = World()
        w.shared = True
        w.run(1)
        for none in (status(age=2.0), status(awake=False), None, "error busy"):
            w.status = none
            w.run(1)
            self.assertEqual(w.said(), [], none)
            self.assertEqual((w.feeder.feeding, w.feeder.told), (True, False))
            self.assertEqual(w.of("gazed").count("status"), 5)
        w.status = status(-30.0, 12.0)
        w.run(0.2)
        self.assertEqual(w.said(), [(-30.0, 12.0)], "after seconds without one, the next is where they look now")
        self.assertTrue(w.feeder.told)

    def test_a_short_gap_keeps_the_smoothing(self):
        w = World()
        w.shared = True
        w.run(1)
        w.status = status(age=2.0)
        w.run(0.4)
        w.status = status(20.0, -5.0)
        w.run(0.2)
        (yaw, pitch), = w.said()
        self.assertTrue(10.0 < yaw < 20.0 and pitch == -5.0, (yaw, pitch))

    def test_away_or_in_no_room_nothing_is_read(self):
        for change in ({"away": 1}, {"away": 2}, {"mode": "idle"}):
            w = World()
            w.shared = True
            w.run(1)
            vars(w).update(change)
            w.run(1)   # (the look that finds out)
            w.run(4)
            self.assertEqual(w.of("gazed"), [], change)
            self.assertEqual((w.feeder.shared, w.feeder.feeding, w.feeder.told), (True, False, False))
            w.mode, w.away, w.status = "joined", 0, status(40.0, 0.0)
            w.run(1.2)
            self.assertEqual(w.said()[0], (40.0, 0.0), "back: from where they look now")
            self.assertIn("wake 3", w.of("gazed"))

    def test_a_state_that_cant_be_read_is_not_being_there(self):
        w = World()
        w.shared = True
        w.together = lambda text, real=w.together: None if text == "state" else real(text)
        w.feeder = feeder.Feeder(w.together, w.helper, w.gazed, lambda: w.now)
        w.run(3)
        self.assertEqual(w.of("gazed"), [])
        self.assertFalse(w.feeder.feeding)

    def test_no_longer_shared_it_stops(self):
        w = World()
        w.shared = True
        w.run(1)
        w.shared = False
        w.run(1)
        w.run(4)
        self.assertEqual(w.of("gazed"), [])
        self.assertEqual(w.said(), [])
        self.assertEqual((w.feeder.shared, w.feeder.feeding), (False, False))

    def test_a_direction_ft_together_refuses_isnt_told(self):
        w = World()
        w.shared = True
        w.refuse = True
        w.run(1)
        self.assertEqual(len(w.said()), 6)
        self.assertFalse(w.feeder.told)

    def test_ft_together_gone_while_feeding(self):
        w = World()
        w.shared = True
        w.run(1)
        w.down = True
        w.run(1)
        self.assertEqual((w.feeder.running, w.feeder.pointer, w.feeder.shared, w.feeder.feeding, w.feeder.told), (False, None, False, False, False))
        self.assertEqual(w.run(4), [("together", "gaze")] * 2)


if __name__ == "__main__":
    unittest.main()
