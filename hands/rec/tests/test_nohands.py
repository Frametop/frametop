#!/usr/bin/env python3
"""Tests for the session's camera check: the preflight that keeps a session from starting
with the upper cameras off, and the early no-hands stop in the hand-size section's first step
(dry runs with a made-up hands file; no processes, no real cameras).

  python3 -m hands.rec.tests.test_nohands
"""
import json
import os
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
from hands.rec import session  # noqa: E402
from hands import camcheck  # noqa: E402  (session.py put hands/ on the path)
from hands.rec.tests.test_session import SessionBase  # noqa: E402

SCRIPT = {
    "version": 1, "intro_s": 1, "between_s": 1,
    "welcome": {"title": "Test", "seconds": 1, "text": "A test."},
    "done": {"title": "Done", "seconds": 0, "text": "Done."},
    "stopped": {"title": "Stopped", "seconds": 0, "text": "Stopped."},
    "sections": [
        {"id": "hand-size", "title": "Hand size", "intro": "Hand size.", "go": "Hold",
         "defaults": {"hands": "both"},
         "prompts": [{"text": "Both hands flat.", "seconds": 6, "pose": "flat"},
                     {"text": "Backs.", "seconds": 2, "pose": "flat-back"}]}]}

HAND = {"palm": [0.0, 0.0, -0.4], "palm_m": 0.4, "tip": [0.0, 0.05, -0.45]}
DEGRADED = {"status": "degraded", "reason": camcheck.VCINT_REASON, "summary": camcheck.DEGRADED_VCINT,
            "evidence": ["17:02:47 Failed to load VCINT FPGA image when passthrough cameras are connected"]}
OK = {"status": "ok", "reason": "4 tracking cameras running", "summary": "ok", "evidence": []}


class FakeHands:
    """Stands in for the hands file: what read() gives can change mid-session."""

    def __init__(self, value):
        self.value = value
        self.lock = threading.Lock()

    def set(self, value):
        with self.lock:
            self.value = value

    def read(self):
        with self.lock:
            return None if self.value is None else dict(self.value)


class NoHandsTest(SessionBase):
    def setUp(self):
        super().setUp()
        with open(self.script, "w") as f:
            json.dump(SCRIPT, f)

    def events(self, s):
        with open(os.path.join(s.session_dir, "takes", "01-hand-size", "prompts.jsonl")) as f:
            # feedback lines (twice a second, as the tracker publishes) aren't what's tested here
            return [e for e in map(json.loads, f) if e["event"] != "feedback"]

    def run_to_first_hold(self, hands, camera=DEGRADED, **kw):
        s = self.session(hands_reader=hands, speed=3, **kw)
        s.camera_check_fn = lambda: camera
        s.start()
        self.waiting(s, "starting")
        s.next_step()
        self.waiting(s, "intro")
        s.next_step()
        self.waiting(s, "ready", "Both hands flat.")
        s.next_step()
        return s

    def session_json(self, s):
        with open(os.path.join(s.session_dir, "session.json")) as f:
            return json.load(f)

    def log(self, s):
        with open(os.path.join(s.session_dir, "session.log")) as f:
            return f.read()

    def test_no_hands_then_redo(self):
        hands = FakeHands({"left": None, "right": None})   # the tracker publishes, sees nothing
        s = self.run_to_first_hold(hands)
        st = self.wait_for(s, lambda st: st["state"] == "nohands" and st["waiting"], "the no-hands stop")
        self.assertTrue(st["nohands"])
        self.assertTrue(st["can_redo"])
        self.assertEqual(st["camera"]["status"], "degraded")
        self.assertIn(camcheck.USER_TEXT, st["prompt"])
        self.assertIn("panel: title " + session.NO_HANDS_TITLE, self.panel)
        hands.set({"left": HAND, "right": HAND})
        s.next_step()   # try again: straight to the countdown, no second ready wait
        self.wait_for(s, lambda st: st["state"] == "countdown", "the countdown again")
        self.waiting(s, "ready", "Backs.")
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        names = [e["event"] for e in ev]
        self.assertEqual(names, ["take", "ready", "prompt", "wait", "nohands", "redo",
                                 "ready", "prompt", "wait", "ready", "prompt", "wait", "end"])
        nohands, redo = ev[4], ev[5]
        self.assertGreaterEqual(nohands["reads"], session.HANDS_CHECK_MIN_READS)
        self.assertEqual(redo["from"], ev[1]["t"])   # the failed try gets no labels
        self.assertTrue(ev[2]["t"] < redo["to"])
        # the retry wasn't stopped: a hand was seen
        self.assertRegex(self.log(s), r"hands check hand-size/flat/both: a hand in [1-9][0-9]* of")
        self.assertNotIn("stop_reason", self.session_json(s))

    def test_no_hands_then_stop(self):
        s = self.run_to_first_hold(FakeHands({"left": None, "right": None}))
        self.wait_for(s, lambda st: st["state"] == "nohands" and st["waiting"], "the no-hands stop")
        s.stop(wait=10)
        self.assertEqual(s.state, "stopped")
        sj = self.session_json(s)
        self.assertIn("no hands were seen", sj["stop_reason"])
        self.assertIn(camcheck.DEGRADED_VCINT, sj["stop_reason"])
        self.assertIn(camcheck.USER_TEXT, s._status["prompt"])
        self.assertIn("camera check: " + camcheck.DEGRADED_VCINT, self.log(s))

    def test_redo_key_and_skip(self):
        hands = FakeHands({"left": None, "right": None})
        s = self.run_to_first_hold(hands, camera=OK)
        st = self.wait_for(s, lambda st: st["state"] == "nohands" and st["waiting"], "the no-hands stop")
        self.assertIn("The camera check found nothing wrong", st["prompt"])
        s.redo()   # R is the same as Next there
        self.wait_for(s, lambda st: st["state"] == "countdown", "the countdown again")
        self.wait_for(s, lambda st: st["state"] == "nohands" and st["waiting"], "no hands again")
        s.skip()   # S skips the section; the no-hands note doesn't stick
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertNotIn("stop_reason", self.session_json(s))
        self.assertEqual([e["event"] for e in self.events(s)].count("nohands"), 2)

    def test_hand_seen_sometimes(self):
        hands = FakeHands({"left": None, "right": None})
        s = self.run_to_first_hold(hands)
        self.wait_for(s, lambda st: st["state"] == "running", "the hold")
        hands.set({"left": HAND, "right": None})   # one hand, for part of the hold: fine
        self.waiting(s, "ready", "Backs.")
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertNotIn("nohands", [e["event"] for e in self.events(s)])

    def test_no_tracker_cant_tell(self):
        s = self.run_to_first_hold(FakeHands(None))   # nothing published: can't tell, go on
        self.waiting(s, "ready", "Backs.")
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertNotIn("nohands", [e["event"] for e in self.events(s)])
        self.assertIn("can't tell", self.log(s))

    def test_only_the_first_step(self):
        hands = FakeHands({"left": HAND, "right": HAND})
        s = self.run_to_first_hold(hands)
        self.wait_for(s, lambda st: st["state"] == "running", "the hold")
        self.waiting(s, "ready", "Backs.")
        hands.set({"left": None, "right": None})   # gone in step 2: notes, but no stop
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertNotIn("nohands", [e["event"] for e in self.events(s)])

    def test_auto_mode(self):
        hands = FakeHands({"left": None, "right": None})
        s = self.session(hands_reader=hands, speed=3, auto=True)
        s.camera_check_fn = lambda: DEGRADED
        s.start()
        self.wait_for(s, lambda st: st["state"] == "nohands" and st["waiting"], "the no-hands stop")
        hands.set({"left": HAND, "right": HAND})
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        names = [e["event"] for e in self.events(s)]
        self.assertEqual(names, ["take", "prompt", "prompt", "pause", "nohands", "redo", "resume", "prompt",
                                 "prompt", "end"])


class PreflightTest(SessionBase):
    def test_degraded_stops_before_anything(self):
        s = self.session()
        s.dry_run = False            # as a real session; the check is the only step run
        s.camera_check_fn = lambda: DEGRADED
        self.assertTrue(s._preflight())
        self.assertEqual(s.state, "error")
        self.assertEqual(s._status["error"], camcheck.USER_TEXT)
        self.assertEqual(s.session_dir, "")   # no session folder made
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "base", "sessions")))

    def test_other_degraded_text(self):
        r = dict(DEGRADED, reason="only 3 of 4 tracking cameras running")
        self.assertIn("only 3 of 4", session.camera_text(r))
        self.assertEqual(session.camera_text(OK), "")
        self.assertEqual(session.camera_text({"status": "unknown"}), "")
        self.assertEqual(session.camera_text(None), "")

    def test_ok_unknown_ignored(self):
        for result in (OK, {"status": "unknown", "summary": "unknown: x", "evidence": []}):
            s = self.session()
            s.dry_run = False
            s.camera_check_fn = lambda: result
            self.assertFalse(s._preflight())
        s = self.session(check_cameras=False)
        s.dry_run = False
        s.camera_check_fn = lambda: DEGRADED
        self.assertFalse(s._preflight())   # --ignore-cameras
        s = self.session(ring="/tmp/some-ring")
        s.dry_run = False
        s.camera_check_fn = lambda: DEGRADED
        self.assertFalse(s._preflight())   # ft-ringplay's frames: no headset cameras involved

    def test_camera_check_never_raises(self):
        orig = camcheck.check
        try:
            camcheck.check = lambda: 1 / 0
            r = session.camera_check()
        finally:
            camcheck.check = orig
        self.assertEqual(r["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
