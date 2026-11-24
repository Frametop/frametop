#!/usr/bin/env python3
"""Tests for the three rounds a contributor asked for on 2026-11-24 (DESIGN.md "Bounties"): hands in
front of something moving, thumbs, and hands off-centre. Each has the finger rounds' shape: hand size,
about two minutes of plain steps, then a short scene of taps.

  python3 -m hands.rec.tests.test_requested_rounds
"""
import json
import re
import unittest

from hands.rec import session


class RequestedRoundsTest(unittest.TestCase):
    def round(self, name, scene_ids):
        """A round's plain steps and scene steps by the last part of their ids, after the checks all
        three share: it passes the board's check as a fetched round would (so the dataset can carry it
        to recorders out there), two minutes of plain steps or so, a step for each hand and one for
        both, and a scene of index taps, middle taps and near misses on a window at pointing distance,
        for each hand and then for both with L and R on the buttons."""
        script = session.load_script(session.bounty_script(name))
        self.assertEqual(script.get("record_hz"), 30, name)
        self.assertEqual(session.check_round(name, json.loads(json.dumps(script))), script)
        drawn, marks = {}, set()
        for seed in range(8):   # the mixes are drawn: every gesture of a step over a few sessions, not in each one
            plan, skipped = session.build_plan(script, {}, seed=seed)
            self.assertEqual(skipped, [])
            self.assertEqual([s["kind"] for s in plan], ["prompts", "prompts", "scene"], name)
            for p in plan[2]["prompts"]:
                self.assertTrue(all(-w["at"][2] >= 0.9 for w in p["scene"]["windows"]), p["id"])
                acts = [a for a in p["scene"]["acts"] if a["gesture"] != "rest"]
                drawn.setdefault(p["id"].split("/")[1], set()).update(a["gesture"] for a in acts)
                if p["hands"] == "both":
                    marks.update((a["label"], a["hand"]) for a in acts)
        # ids, hands and words are the same in every session
        steps = {p["id"].split("/")[1]: p for p in plan[1]["prompts"]}
        scene = {p["id"].split("/")[1]: p for p in plan[2]["prompts"]}
        self.assertTrue(100 <= sum(p["seconds"] for p in steps.values()) <= 150, name)
        self.assertLessEqual({"right", "left", "both"}, {p["hands"] for p in steps.values()}, name)
        for p in steps.values():   # a step for one hand that switches hands would warn about the hand put down
            if p["hands"] in ("left", "right"):
                self.assertFalse(any("hand" in w["to"] for w in p["switch"]), p["id"])
        self.assertEqual(list(scene), scene_ids, name)
        self.assertEqual([p["hands"] for p in scene.values()], ["right", "left", "both"], name)
        self.assertEqual(drawn, {k: {"tap", "middle", "nogo"} for k in scene_ids}, name)
        self.assertEqual(marks, {("L", "left"), ("R", "right")}, name)
        for side, other in (("right", "left"), ("left", "right")):
            for p in [p for p in list(steps.values()) + list(scene.values()) if p["hands"] == side]:
                self.assertIn("%s hand" % side, p["text"].lower(), p["id"])
                self.assertNotIn("%s hand" % other, p["text"].lower(), p["id"])
        self.assertLessEqual(session.plan_seconds(script, plan, worst=True), 5 * 60, name)
        return script, steps, scene

    def test_moving_background(self):
        """Hands in front of a fan or a screen that moves: the welcome and the steps' introduction say what
        to find and where to put it, and every step, the taps on the window too, keeps the hand in front of
        it. Still hands, moving hands, fingers, taps and shapes are all asked for."""
        script, steps, scene = self.round("moving-background", ["right", "left", "both-hands"])
        for text in (script["welcome"]["text"], script["sections"][1]["intro"]):
            self.assertTrue(all(w in text for w in ("fan", "TV", "monitor")), text)
        self.assertIn("behind your hands", script["sections"][1]["intro"])
        self.assertIn("behind the window", script["sections"][2]["intro"])
        for p in list(steps.values()) + list(scene.values()):
            self.assertRegex(p["text"], r"moving thing|in front of it", p["id"])
        self.assertEqual(list(steps), ["right-still", "left-still", "across", "fingers", "taps", "shapes", "edge", "both"])
        self.assertTrue(all("still" in steps[k]["text"].lower() for k in ("right-still", "left-still")))
        self.assertEqual(steps["taps"]["pose"], "pinch-tap")

    def test_thumbs(self):
        """The thumb on its own, palm up especially: every plain step is about the thumb, most have the palm
        up (each hand alone first, both hands last), one has the palm toward you and one the hand on its side;
        the taps on the window are made palm up."""
        script, steps, scene = self.round("thumbs", ["right", "left", "both-hands"])
        self.assertTrue(all("thumb" in p["text"].lower() for p in steps.values()))
        up = [k for k, p in steps.items() if re.search(r"palms? up", p["text"], re.I)]
        self.assertEqual(up, ["right-up", "left-up", "circles", "touches", "tuck", "both"])
        self.assertGreater(len(up), len(steps) / 2)
        self.assertIn("palm toward you", steps["palm-in"]["text"])
        self.assertEqual(steps["on-top"]["pose"], "thumbs-up")
        self.assertTrue(all(re.search(r"palms? up", p["text"]) for p in scene.values()))
        self.assertIn("palm up", script["sections"][2]["intro"])

    def test_off_centre(self):
        """Fingers moving with the hand away from the middle of the cameras' view: the head stays still and the
        hand goes right, left, high and low (the panel's diagram shows where), across the body, to the corners
        and out past the edge. In the scene the window is straight ahead and the hand is what is off to the
        side."""
        script, steps, scene = self.round("off-centre", ["right-out", "left-out", "both-low"])
        self.assertIn("keep your head still", script["sections"][1]["intro"])
        where = {k: (p["hands"], p["position"]) for k, p in steps.items() if p["position"]}
        self.assertEqual(where, {"right-out": ("right", "right"), "left-out": ("left", "left"),
                                 "high": ("any", "up"), "low": ("any", "down")})
        for k, words in (("right-out", "to your right"), ("left-out", "to your left"), ("high", "top edge"),
                         ("low", "bottom edge")):
            self.assertIn(words, steps[k]["text"], k)
        for p in steps.values():   # every step names a place that isn't the middle
            self.assertRegex(p["text"], r"edge|corner", p["id"])
        self.assertEqual(list(steps), ["right-out", "left-out", "high", "low", "across", "corners", "leaving", "both"])
        for p in scene.values():
            self.assertTrue(all(abs(w["at"][0]) <= 0.05 for w in p["scene"]["windows"]), p["id"])
        self.assertIn("far out to your right", scene["right-out"]["text"])
        self.assertIn("far out to your left", scene["left-out"]["text"])
        self.assertIn("low", scene["both-low"]["text"])
        self.assertIn("head still", script["sections"][2]["intro"])


if __name__ == "__main__":
    unittest.main()
