#!/usr/bin/env python3
"""The eye recorder's session runner (gaze/rec/session.py) without the headset: the round
scripts' checks, the plans, the control sockets' answers, and whole rounds in a dry run.

  python3 gaze/rec/tests/test_session.py
"""
import glob
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import session  # noqa: E402


def write_round(tmp, steps, **extra):
    d = dict({"schema": 1, "id": "t", "title": "Test", "steps": steps}, **extra)
    path = os.path.join(tmp, "t.json")
    with open(path, "w") as f:
        json.dump(d, f)
    return path


class RoundTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-round-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def bad(self, steps, want):
        with self.assertRaises(ValueError) as e:
            session.load_round(write_round(self.tmp, steps))
        self.assertIn(want, str(e.exception))

    def test_shipped_rounds_load(self):
        paths = sorted(glob.glob(os.path.join(session.ROUNDS_DIR, "*.json")))
        self.assertEqual(len(paths), 10)
        for p in paths:
            r = session.load_round(p)
            self.assertEqual(r["id"], os.path.basename(p)[:-5])
            self.assertTrue(r["steps"][0]["ready"], p)

    def test_panel_reach(self):
        self.assertFalse(session.off_panel(0, 0))
        self.assertFalse(session.off_panel(30, 20))
        self.assertTrue(session.off_panel(38, 0))
        self.assertTrue(session.off_panel(0, 26))
        self.assertTrue(session.off_panel(30, 23), "corners reach less far up")
        self.bad([{"id": "a", "kind": "dots", "dots": [[40, 0]]}], "off the panel")
        self.bad([{"id": "a", "kind": "path", "path": "circle 0 0 26 4", "seconds": 4}], "off the panel")
        self.bad([{"id": "a", "kind": "jumps", "count": 3, "range": [40, 10], "each": [0.5, 1]}], "off the panel")

    def test_bad_steps(self):
        self.bad([{"id": "a", "kind": "dance"}], "unknown kind")
        self.bad([{"id": "A b", "kind": "dots", "dots": [[0, 0]]}], "bad id")
        self.bad([{"id": "a", "kind": "path", "path": "spiral 0 0 1 2", "seconds": 2}], "bad path")
        self.bad([{"id": "a", "kind": "path", "path": "circle 0 0 10 0", "seconds": 2}], "positive")
        self.bad([{"id": "a", "kind": "prompt"}], "seconds")
        self.bad([{"id": "a", "kind": "world", "point": [0, 0, 1], "seconds": 3}], "ahead")
        self.bad([{"id": "a", "kind": "dots", "dots": [[0, 0]]}, {"id": "a", "kind": "dots", "dots": [[0, 0]]}], "repeat")
        self.bad([{"id": "a", "kind": "dots", "group": "g", "dots": [[0, 0]]}, {"id": "b", "kind": "dots", "dots": [[0, 0]]},
                  {"id": "c", "kind": "dots", "group": "g", "dots": [[0, 0]]}], "comes back")
        self.bad([{"id": "a", "kind": "dots", "group": "dots", "dots": [[0, 0]]}], "opener")

    def test_grid_and_defaults(self):
        r = session.load_round(write_round(self.tmp, [{"id": "a", "kind": "dots", "grid": [3, 2, 10, 5], "ready": False},
                                                      {"id": "b", "kind": "reseat", "text": "off and on"}]))
        a = r["steps"][0]
        self.assertEqual(a["dots"], [[10, 5], [0, 5], [-10, 5], [10, -5], [0, -5], [-10, -5]])
        self.assertTrue(a["ready"], "the first step always waits")
        self.assertEqual((a["bg"], a["distance"], a["group"]), (session.MID_BG, session.PANEL_DISTANCE, "a"))
        self.assertFalse(r["shift_check"], "a round that reseats on purpose has no shift check")

    def test_plan(self):
        rnd = session.load_round(os.path.join(session.ROUNDS_DIR, "jumps.json"))
        a = session.build_plan(rnd, 7, ipd_sweep=True)
        self.assertEqual([s["group"] for s in a[:3]], ["ipd", "dots", "jumps"])
        nine = a[1]["dots"]
        self.assertEqual((nine[0], nine[-1], len(nine)), ([0.0, 0.0], [0.0, 0.0], 10))
        self.assertEqual(sorted(map(tuple, nine[1:-1])), sorted(map(tuple, (d for d in session.grid(3, 3, 20, 20) if d != [0, 0]))))
        jumps = a[2]
        self.assertEqual(len(jumps["dots"]), 40)
        self.assertTrue(all(0.5 <= t <= 1.5 for t in jumps["times"]))
        self.assertTrue(all(not session.off_panel(*d) for d in jumps["dots"]))
        self.assertEqual(a, session.build_plan(rnd, 7, ipd_sweep=True), "the same seed, the same round")
        self.assertNotEqual(a[1]["dots"], session.build_plan(rnd, 8, ipd_sweep=True)[1]["dots"])
        self.assertEqual(session.build_plan(rnd, 7, ipd_sweep=False)[0]["group"], "dots")
        self.assertGreater(session.plan_seconds(a), 60)


class SocketTest(unittest.TestCase):
    """The panel and ft-eyes answer every command in order: an answer waited for is the one
    after the answers of commands sent before it."""

    def setUp(self):
        self.name = "eyerec_socket_test_%d_%s" % (os.getpid(), self.id().rsplit(".", 1)[-1])
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.server.bind("\0" + self.name)
        self.server.settimeout(0.2)
        self.delay = 0.02
        self.drop = set()
        self.running = True
        threading.Thread(target=self.serve, daemon=True).start()

    def tearDown(self):
        self.running = False
        self.server.close()

    def serve(self):
        while self.running:
            try:
                data, addr = self.server.recvfrom(4096)
            except OSError:
                continue
            text = data.decode()
            time.sleep(self.delay)
            if text in self.drop:
                continue
            try:
                self.server.sendto(("ok " + text).encode(), addr)
            except OSError:
                pass

    def test_answers_in_order(self):
        s = session.Socket(self.name)
        for n in range(5):
            s.cmd("dot %d" % n)
        self.assertEqual(s.cmd("ipd", reply=True), "ok ipd")
        self.assertEqual(s.pending, 0)
        s.cmd("bg 1")
        self.assertEqual(s.cmd("log stop", reply=True), "ok log stop")
        s.close()

    def test_lost_answer(self):
        s = session.Socket(self.name)
        self.drop.add("fit")
        self.assertIsNone(s.cmd("fit", reply=True, timeout=0.2))
        self.assertEqual(s.cmd("ipd", reply=True), "ok ipd", "counting starts afresh")
        s.close()

    def test_errors_logged(self):
        logs = []
        s = session.Socket(self.name, log=logs.append)
        s.cmd("x")
        time.sleep(0.1)
        s._got("error bad")
        self.assertEqual(logs, ["%s: error bad" % self.name])
        s.close()


class DryRunTest(unittest.TestCase):
    """Whole rounds with no processes: the files, the fitment, the controls."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-dry-")
        self.out = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_round(self, script, steer=None, profile=None, **kw):
        path = os.path.join(self.tmp, "profile.json")
        with open(path, "w") as f:
            json.dump(profile or {}, f)
        states = []
        s = session.Session(self.tmp, script, profile=profile, dry_run=True, speed=kw.pop("speed", 40),
                            next_after=kw.pop("next_after", 0.25), profile_path=path, button=False,
                            on_status=lambda st: states.append(dict(st)), **kw)
        s.print = self.out.append
        s.start()
        if steer:
            steer(s, states)
        s.join(60)
        self.assertFalse(s._thread.is_alive())
        with open(path) as f:
            return s, states, json.load(f)

    def files(self, s):
        with open(os.path.join(s.session_dir, "session.json")) as f:
            sj = json.load(f)
        with open(os.path.join(s.session_dir, "fit.json")) as f:
            fit = json.load(f)
        return sj, fit

    def prompts(self, s, take):
        with open(os.path.join(s.session_dir, "takes", take, "prompts.jsonl")) as f:
            return [json.loads(line) for line in f]

    def test_round_and_profile(self):
        s, states, profile = self.run_round("depth")
        self.assertEqual(s.state, "done")
        sj, fit = self.files(s)
        self.assertEqual(sj["takes"], ["01-ipd", "02-dots", "03-near", "04-mid", "05-far", "06-near-far"])
        self.assertEqual((sj["status"], sj["fit"], sj["ipd_sweep"], sj["ipd_start"]), ("done", "good", True, 0.063))
        self.assertEqual(fit["fitment"]["result"], "good")
        self.assertIsNotNone(fit["baseline"])
        self.assertEqual(profile["bounties"]["depth"]["rounds"], 1)
        self.assertEqual(profile["eyes_ipd"], 0.063)
        for take in sj["takes"]:
            with open(os.path.join(s.session_dir, "takes", take, "take.json")) as f:
                self.assertEqual(json.load(f)["status"], "complete", take)
        events = [e["event"] for e in self.prompts(s, "03-near")]
        self.assertEqual(events[:4], ["take", "ready", "rec", "step"])
        self.assertEqual(events[-3:], ["wait", "rec", "end"])
        self.assertIn("ft_eyerecpanel: distance 0.500", self.out)
        self.assertIn("ft_eyerecpanel: distance 5.000", self.out)
        # The IPD is the same next time: no sweep.
        s2, _, profile = self.run_round("depth", profile=profile)
        self.assertEqual(self.files(s2)[0]["takes"][0], "01-dots")
        self.assertEqual(profile["bounties"]["depth"]["rounds"], 2)

    def test_fitment_warns_then_skips(self):
        # Next before the fit has been good for FIT_GOOD_S: a warning, then the round goes on.
        s, states, _ = self.run_round("reading", next_after=0.01, speed=20)
        sj, fit = self.files(s)
        self.assertEqual((sj["fit"], fit["fitment"]["result"]), ("skipped", "skipped"))
        self.assertTrue(any(st["note"] == session.FIT_SKIP_NOTE for st in states))
        self.assertIsNone(fit["baseline"])

    def test_skip_redo_stop(self):
        def steer(s, states):
            def wait_for(cond):
                end = time.monotonic() + 30
                while time.monotonic() < end and not cond():
                    time.sleep(0.01)
            # In the jumps take: redo the step while it runs, then skip the rest of the take.
            wait_for(lambda: s._status["step"] == "jumps" and s._status["state"] == "running")
            s.redo()
            wait_for(lambda: s._status["step"] == "jumps" and s._status["state"] == "ready")
            wait_for(lambda: s._status["step"] == "jumps" and s._status["state"] == "running")
            s.skip()
            wait_for(lambda: s._status["step"] == "circle-slow" and s._status["state"] == "running")
            s.stop(wait=0)
        s, states, profile = self.run_round("jumps", steer=steer, speed=10, profile={"eyes_ipd": 0.063})
        self.assertEqual(s.state, "stopped")
        sj, _ = self.files(s)
        self.assertEqual(sj["takes"], ["01-dots", "02-jumps", "03-pursuit"])
        with open(os.path.join(s.session_dir, "takes", "02-jumps", "take.json")) as f:
            self.assertEqual(json.load(f)["status"], "skipped")
        with open(os.path.join(s.session_dir, "takes", "03-pursuit", "take.json")) as f:
            self.assertEqual(json.load(f)["status"], "stopped")
        redo = [e for e in self.prompts(s, "02-jumps") if e["event"] == "redo"]
        self.assertEqual(len(redo), 1)
        self.assertLess(redo[0]["from"], redo[0]["to"])
        self.assertNotIn("bounties", profile, "a stopped round isn't counted")

    def test_pause_stops_the_recording(self):
        def steer(s, states):
            end = time.monotonic() + 30
            while time.monotonic() < end and not (s._status["step"] == "dots" and s._status["state"] == "running"):
                time.sleep(0.01)
            s.pause()
            time.sleep(0.3)
            self.assertFalse(s._recording)
            s.resume()
        s, _, _ = self.run_round("face", steer=steer, speed=10, profile={"eyes_ipd": 0.063})
        self.assertEqual(s.state, "done")
        events = [e["event"] for e in self.prompts(s, "01-dots")]
        i = events.index("pause")
        self.assertEqual(events[i - 1:i + 3], ["rec", "pause", "resume", "rec"])

    def test_shift_check(self):
        moved = {"on": False}

        def steer(s, states):
            # Once the fitment is done, the eyes appear 12 degrees up: the headset slipped down.
            end = time.monotonic() + 30
            while time.monotonic() < end and s._status["state"] in ("starting", "fitment"):
                time.sleep(0.01)
            ref = s.ref
            summary = {str(cam): {"n": 90, "finds": 90, "found": 1.0,
                                  "x": ref.eyes[cam]["centre"][0] + ref.eyes[cam]["jacobian"][0][1] * 12,
                                  "y": ref.eyes[cam]["centre"][1] + ref.eyes[cam]["jacobian"][1][1] * 12,
                                  "a": 28.0, "spacing": 15.0, "extra": 0.0} for cam in (0, 1)}
            s._eyes.fake = lambda text: json.dumps(summary) if text == "fit" else "ok"
            moved["on"] = True
        s, states, _ = self.run_round("glasses", steer=steer, profile={"eyes_ipd": 0.063})
        self.assertTrue(moved["on"])
        _, fit = self.files(s)
        self.assertGreaterEqual(len(fit["slips"]), 2, "the nine dots' middle, twice, and the grid's")
        self.assertIn("up", fit["slips"][0]["hints"])
        self.assertTrue(any(st["note"].startswith("The headset moved") for st in states))
        self.assertTrue(any(e["event"] == "slip" for e in self.prompts(s, "01-dots")))


if __name__ == "__main__":
    unittest.main()
