#!/usr/bin/env python3
"""Tests for the hand bounties (DESIGN.md "Bounties", "Scenes"): the shipped board and round
scripts, the scene steps' acts (buttons and moves), and a dry-run round: its act and scene
events in prompts.jsonl, session.json's bounty and sets a second, and the done mark.

  python3 hands/rec/tests/test_bounties.py
"""
import json
import os
import random
import shutil
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import session  # noqa: E402
from recorder import bounties  # noqa: E402

WIN = {"id": "a", "at": [0, -0.15, -1.0], "size": [0.6, 0.4], "look": "grid:3x3"}

SCRIPT = {
    "version": 1, "name": "test", "record_hz": 30, "intro_s": 1, "between_s": 1,
    "welcome": {"title": "Test", "seconds": 1, "text": "A test."},
    "done": {"title": "Done", "seconds": 0, "text": "Done."},
    "stopped": {"title": "Stopped", "seconds": 0, "text": "Stopped."},
    "sections": [{"id": "taps", "title": "Taps", "kind": "scene", "intro": "Tap.", "prompts": [
        {"id": "one", "hands": "right", "text": "Tap the lit button.", "windows": [WIN],
         "acts": {"gen": "buttons", "every_s": 1.0, "lit_s": 0.6, "count": 3}},
        {"id": "two", "hands": "both", "text": "Hold the bar, tap the buttons.",
         "windows": [dict(WIN, id="b", look="plain"), dict(WIN, at=[0.5, 0, -1])],
         "steady": [{"win": "b", "lit": "bar", "gesture": "pinch", "label": "L"}],
         "acts": [{"s": 0.8, "win": "a", "lit": "button:4", "gesture": "tap"},
                  {"s": 0.5, "win": "a", "gesture": "rest"},
                  {"s": 0.8, "win": "a", "lit": "tl+br", "gesture": "pinch", "ghost": [0.1, 0, 0, 0, 0, 0, 1.2]}]}]}]}


def step(p, seed="x"):
    """One scene step through _scene_step, as build_plan does."""
    p = json.loads(json.dumps(p))
    session._scene_step({"id": "t"}, p, random.Random(seed))
    return p


class ShippedTest(unittest.TestCase):
    """The board and round scripts that ship with the recorder."""

    def test_board(self):
        board = bounties.load(session.BOARD_PATH)
        self.assertIsNotNone(board, "bounties.json doesn't validate")
        ids = [b["id"] for b in board["bounties"]]
        self.assertEqual(ids, ["close-up", "near-misses", "three-finger", "fists", "sideways-hands", "pokes", "middle-taps", "index-taps",
                               "hand-gestures", "daylight-taps", "pinches", "card-grabs"])
        live = [b["id"] for b in board["bounties"] if b["priority"] != "retired"]
        self.assertEqual(live, ["close-up", "near-misses", "three-finger", "fists", "sideways-hands"])
        for b in board["bounties"]:
            self.assertTrue(session.have_bounty_script(b["script"]), b["script"])

    def test_scripts(self):
        """Each round: hand size first, then a scene section of 100 to 180 s of acts (plus the
        countdowns) at 30 sets a second, maybe a short plain section after it (the palm up),
        every act's gesture and lit part valid (build_plan raises otherwise)."""
        board = bounties.load(session.BOARD_PATH)
        for b in board["bounties"]:
            script = session.load_script(session.bounty_script(b["script"]))
            self.assertEqual(script.get("record_hz"), 30, b["id"])
            plan, skipped = session.build_plan(script, {}, seed=1)
            self.assertEqual(skipped, [])
            self.assertEqual([s["kind"] for s in plan][:2], ["prompts", "scene"], b["id"])
            self.assertTrue(all(s["kind"] == "prompts" for s in plan[2:]), b["id"])
            scene = plan[1]
            seconds = sum(p["seconds"] for p in scene["prompts"])
            self.assertTrue(100 <= seconds <= 180, "%s: %.0f s" % (b["id"], seconds))
            after = sum(p["seconds"] for s in plan[2:] for p in s["prompts"])
            self.assertLessEqual(after, 30, b["id"])
            gestures = set()
            for p in scene["prompts"]:
                self.assertTrue(p["id"].startswith(scene["id"] + "/"), p["id"])
                for a in p["scene"]["acts"]:
                    gestures.add(a["gesture"])
                    if a["gesture"] != "rest":
                        self.assertNotEqual(a["lit"], "none", p["id"])
            want = {"index-taps": "tap", "middle-taps": "middle", "pinches": "pinch", "card-grabs": "card",
                    "pokes": "poke", "hand-gestures": "sweep", "close-up": "poke", "sideways-hands": "pinch",
                    "near-misses": "nogo", "three-finger": "tri", "fists": "fist"}[b["script"]]
            self.assertIn(want, gestures)
            self.assertIn("nogo", gestures, "%s: no near misses" % b["id"])

    def test_pokes_and_gestures(self):
        """The rounds made from the 2026-10-15 testing sessions: pokes on windows within reach (low ones
        too) with holds, slides and hovers; sweeps with look-alikes, pulls, pushes, fists and the palm up
        with each hand."""
        def round_of(name):
            plan = session.build_plan(session.load_script(session.bounty_script(name)), {}, seed=1)[0]
            acts = [a for s in plan if s["kind"] == "scene" for p in s["prompts"] for a in p["scene"]["acts"]]
            wins = [w for s in plan if s["kind"] == "scene" for p in s["prompts"] for w in p["scene"]["windows"]]
            return plan, {a["gesture"] for a in acts}, wins
        plan, gestures, wins = round_of("pokes")
        self.assertLessEqual({"poke", "poke-hold", "poke-slide", "nogo"}, gestures)
        self.assertTrue(all(-w["at"][2] <= 0.5 for w in wins), "pokes: every window within reach")
        self.assertTrue(any(w["at"][1] <= -0.4 for w in wins), "pokes: a low window")
        plan, gestures, wins = round_of("hand-gestures")
        self.assertLessEqual({"sweep", "pull", "push", "fist", "nogo"}, gestures)
        palm = [p for s in plan if s["id"] == "palm-up" for p in s["prompts"]]
        self.assertEqual(sorted(p["hands"] for p in palm), ["left", "right"])

    def test_v20a_rounds(self):
        """The rounds of 2026-10-23: from v20a on the testing sessions, windows close to the face (low and to
        the side too), hands on their side at arm's length with sweeps, and near misses outnumbering taps;
        from the user, three-finger pinches among index and middle taps, and fists closing and opening."""
        def round_of(name):
            plan = session.build_plan(session.load_script(session.bounty_script(name)), {}, seed=1)[0]
            acts = [a for s in plan if s["kind"] == "scene" for p in s["prompts"] for a in p["scene"]["acts"]]
            wins = [w for s in plan if s["kind"] == "scene" for p in s["prompts"] for w in p["scene"]["windows"]]
            return [a["gesture"] for a in acts if a["gesture"] != "rest"], wins
        gestures, wins = round_of("close-up")
        self.assertLessEqual({"poke", "tap", "pinch", "nogo"}, set(gestures))
        self.assertTrue(all(-w["at"][2] <= 0.32 for w in wins), "close-up: every window close")
        self.assertTrue(any(w["at"][1] <= -0.25 for w in wins), "close-up: a low window")
        self.assertTrue(any(w["at"][0] >= 0.2 for w in wins), "close-up: one to the side")
        gestures, wins = round_of("sideways-hands")
        self.assertLessEqual({"pinch", "tap", "sweep", "nogo"}, set(gestures))
        self.assertTrue(any(-w["at"][2] >= 0.7 for w in wins), "sideways-hands: a window at full reach")
        gestures, wins = round_of("near-misses")
        self.assertLessEqual({"tap", "pinch", "middle", "nogo"}, set(gestures))
        self.assertGreater(gestures.count("nogo"), len(gestures) / 2, "near-misses: mostly near misses")
        gestures, wins = round_of("three-finger")
        self.assertLessEqual({"tri", "tri-scroll", "tap", "middle"}, set(gestures))
        self.assertGreater(gestures.count("tri") + gestures.count("tri-scroll"), len(gestures) / 2)
        gestures, wins = round_of("fists")
        self.assertLessEqual({"fist", "pinch", "card", "touch", "nogo"}, set(gestures))
        self.assertGreater(gestures.count("fist"), len(gestures) / 2)

    def test_shipped_rounds_pass_the_fetched_check(self):
        """Each shipped round would pass check_round if it came from the dataset (the check doesn't
        refuse what a real round is)."""
        for n in sorted(os.listdir(session.BOUNTIES_DIR)):
            with open(os.path.join(session.BOUNTIES_DIR, n)) as f:
                session.check_round(n[:-5], json.load(f))

    def test_check_round_refuses(self):
        """A fetched round that could change what is recorded or where it goes, or isn't a round in
        limits, is refused."""
        with open(session.bounty_script("pokes")) as f:
            good = json.load(f)
        session.check_round("pokes", good)

        def bad(change, name="pokes"):
            s = json.loads(json.dumps(good))
            change(s)
            with self.assertRaises(ValueError):
                session.check_round(name, s)
        scene = lambda s: s["sections"][1]["prompts"][0]   # noqa: E731
        bad(lambda s: None, name="other")                                       # not its name
        bad(lambda s: s.update(debug=True))                                     # a testing session
        bad(lambda s: s.update(objects={"keyboard": "a keyboard"}))
        bad(lambda s: s.update(something=1))                                    # an unknown key
        bad(lambda s: s.update(record_hz=60))
        bad(lambda s: s["sections"][0].update(requires=["controllers"]))
        bad(lambda s: s["sections"][0].update(kind="targets"))
        bad(lambda s: scene(s).update(demo="../../x"))
        bad(lambda s: scene(s).update(controller=True))
        bad(lambda s: scene(s).update(text="x" * 700))
        bad(lambda s: scene(s)["windows"][0].update(at=[0, 0, -5]))             # too far
        bad(lambda s: scene(s)["windows"][0].update(size=[0.3, float("inf")]))
        bad(lambda s: scene(s).update(acts={"gen": "buttons", "mix": {"explode": 1}}))   # a gesture it doesn't know
        bad(lambda s: scene(s).update(acts={"gen": "buttons", "seconds": 500, "every_s": 0.5}))   # too long
        bad(lambda s: s.update(sections=[s["sections"][0]]))                    # no scene

    def test_fetched_rounds(self):
        """hub.py board keeps the rounds the board names that the recorder doesn't ship (checked),
        refuses bad ones, drops ones no longer wanted, and a shipped round wins over a fetched one."""
        import hub
        base = tempfile.mkdtemp(prefix="handrec-rounds-")
        self.addCleanup(shutil.rmtree, base)
        with open(session.bounty_script("pokes")) as f:
            pokes = json.load(f)
        served = {"new-round": dict(pokes, name="new-round"), "bad-round": dict(pokes, name="bad-round", debug=True),
                  "pokes": dict(pokes, record_hz=10)}
        asked = []

        def fetch(url, token, max_bytes):
            name = url.rsplit("/", 1)[1][:-5]
            asked.append(name)
            if name not in served:
                raise OSError("404")
            return json.loads(json.dumps(served[name]))
        old = os.path.join(session.fetched_rounds_dir(base), "old-round.json")
        os.makedirs(os.path.dirname(old))
        with open(old, "w") as f:
            json.dump(dict(pokes, name="old-round"), f)
        board = bounties.validate({"schema": 1, "updated": "2026-10-23", "bounties": [
            {"id": "new", "script": "new-round", "priority": "high"}, {"id": "bad", "script": "bad-round"},
            {"id": "gone", "script": "missing-round"}, {"id": "pokes", "script": "pokes"},
            {"id": "old", "script": "old-round", "priority": "retired"}]})
        r = hub.fetch_rounds(board, base, "x/y", None, fetch=fetch)
        self.assertEqual(r["rounds"], ["new-round"])
        self.assertEqual(sorted(r["rounds_failed"]), ["bad-round", "missing-round"])
        self.assertNotIn("pokes", asked)                       # shipped: never fetched
        self.assertFalse(os.path.exists(old))                  # retired: dropped
        self.assertTrue(session.have_bounty_script("new-round", base_dir=base))
        self.assertFalse(session.have_bounty_script("bad-round", base_dir=base))
        path = session.bounty_script("new-round", base_dir=base)
        self.assertTrue(session.bounty_script_fetched(path, base))
        self.assertFalse(session.bounty_script_fetched(session.bounty_script("pokes", base_dir=base), base))
        view = {b["id"]: b["available"] for b in bounties.view(board, {}, {}, lambda n: session.have_bounty_script(n, base_dir=base))}
        self.assertEqual(view["new"], "")
        self.assertEqual(view["bad"], "Needs a newer recorder")
        # A fetch that fails later keeps the copy that passed.
        served.pop("new-round")
        hub.fetch_rounds(board, base, "x/y", None, fetch=fetch)
        self.assertTrue(session.have_bounty_script("new-round", base_dir=base))

    def test_same_plan_for_a_seed(self):
        script = session.load_script(session.bounty_script("index-taps"))
        a = session.build_plan(script, {}, seed=7)[0][1]["prompts"]
        b = session.build_plan(script, {}, seed=7)[0][1]["prompts"]
        c = session.build_plan(script, {}, seed=8)[0][1]["prompts"]
        self.assertEqual([p["scene"]["acts"] for p in a], [p["scene"]["acts"] for p in b])
        self.assertNotEqual([p["scene"]["acts"] for p in a], [p["scene"]["acts"] for p in c])


class SceneTest(unittest.TestCase):
    def test_buttons(self):
        p = step({"text": "x", "windows": [WIN], "acts": {"gen": "buttons", "every_s": 1.5, "count": 12,
                                                           "mix": {"tap": 1, "nogo": 1}, "hands": ["left", "right"]}})
        acts = p["scene"]["acts"]
        self.assertEqual(len(acts), 24)   # each lit act, then a rest
        lit, rests = acts[0::2], acts[1::2]
        self.assertTrue(all(a["gesture"] == "rest" and a["lit"] == "none" for a in rests))
        for a, b in zip(lit, lit[1:]):
            self.assertNotEqual(a["lit"], b["lit"])   # never the same button twice in a row
        for a in lit:
            self.assertRegex(a["lit"], r"^button:[0-8]$")
            self.assertEqual(a["label"][0], "L" if a["hand"] == "left" else "R")
            self.assertEqual(a["colour"], {"tap": "cyan", "nogo": "orange"}[a["gesture"]])
        self.assertAlmostEqual(p["seconds"], 12 * (session.CHOICE_MIN_S + 1.5 * 0.3))   # L/R: 2 s lit, the rest kept
        self.assertEqual(p["pose"], lit[0]["gesture"])

    def test_buttons_need_a_grid(self):
        with self.assertRaises(ValueError):
            step({"text": "x", "windows": [dict(WIN, look="plain")], "acts": {"gen": "buttons", "count": 2}})

    def test_moves(self):
        p = step({"text": "x", "windows": [WIN], "acts": {
            "gen": "moves", "gesture": "card", "grab": "right", "every_s": 3, "gap_s": 1, "shuffle": False,
            "moves": [[0.1, 0, 0], {"grab": "left+right", "ghost": [0, 0, 0, 30, 0, 0, 1.4], "hand": "both"},
                      {"grab": "tr", "gesture": "nogo"}]}})
        acts = p["scene"]["acts"]
        self.assertEqual([a["gesture"] for a in acts], ["card", "rest", "card", "rest", "nogo", "rest"])
        self.assertEqual(acts[0]["ghost"], [0.1, 0, 0, 0, 0, 0])
        self.assertEqual(acts[2]["ghost"], [0, 0, 0, 30, 0, 0, 1.4])
        self.assertEqual(acts[2]["lit"], "left+right")
        self.assertIsNone(acts[4]["ghost"])
        self.assertEqual(acts[1]["say"], "Let go")
        self.assertAlmostEqual(p["seconds"], 12.0)

    def test_hand_choice(self):
        """Acts that switch hands get CHOICE_MIN_S each: the generator stretches them, written ones fail."""
        p = step({"text": "x", "windows": [WIN], "acts": {"gen": "buttons", "every_s": 1.25, "count": 6,
                                                           "hands": ["left", "right"]}})
        lit = [a for a in p["scene"]["acts"] if a["gesture"] != "rest"]
        self.assertTrue(all(a["s"] >= session.CHOICE_MIN_S for a in lit))
        one = step({"text": "x", "windows": [WIN], "acts": {"gen": "buttons", "every_s": 1.25, "count": 6, "hands": ["right"]}})
        self.assertLess(one["scene"]["acts"][0]["s"], session.CHOICE_MIN_S)   # one hand: no choice, no stretch
        with self.assertRaises(ValueError):
            step({"text": "x", "windows": [WIN], "acts": [{"s": 1, "hand": "left"}, {"s": 1, "hand": "right"}]})
        step({"text": "x", "windows": [WIN], "acts": [{"s": 2, "hand": "left"}, {"s": 0.5, "gesture": "rest"},
                                                       {"s": 2, "hand": "right"}]})

    def test_bad_steps(self):
        bad = [{"windows": [], "acts": [{"s": 1}]},                                       # no window
               {"windows": [WIN] * 2, "acts": [{"s": 1}]},                                # same id twice
               {"windows": [dict(WIN, look="grid:9x9")], "acts": [{"s": 1}]},             # too many buttons
               {"windows": [WIN], "acts": [{"s": 1, "gesture": "wave"}]},                 # unknown gesture
               {"windows": [WIN], "acts": [{"s": 1, "lit": "middle"}]},                   # unknown part
               {"windows": [WIN], "acts": [{"s": 1, "win": "z"}]},                        # unknown window
               {"windows": [WIN], "acts": [{"s": 1, "ghost": [1, 2, 3, 4, 5, 6, 7, 8]}]},
               {"windows": [WIN], "acts": []},
               {"windows": [WIN], "acts": [{"s": 1}], "steady": [{"win": "a", "lit": "bar"}]}]   # steady on an act's window
        for p in bad:
            with self.assertRaises(ValueError, msg=json.dumps(p)):
                step(dict(p, text="x"))


class RoundTest(unittest.TestCase):
    """A dry-run bounty round of SCRIPT."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handrec-bounty-test-")
        self.base = os.path.join(self.tmp, "base")
        os.makedirs(self.base)
        self.script = os.path.join(self.tmp, "test.json")
        with open(self.script, "w") as f:
            json.dump(SCRIPT, f)
        with open(os.path.join(self.base, "profile.json"), "w") as f:
            json.dump({"schema": 1, "contributor": "c", "bounties": {"test": {"rounds": 1, "last": "x"}}}, f)
        self.panel = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_round(self):
        s = session.Session(self.base, {}, {}, "room", self.script, dry_run=True, speed=10, next_after=0.02,
                            poses_dir=os.path.join(self.tmp, "no-poses"), button=False,
                            bounty={"id": "test", "title": "Test taps", "updated": "2026-10-14"})
        s.print = self.panel.append
        s.start()
        end = time.monotonic() + 30
        while s._thread.is_alive() and time.monotonic() < end:
            s.join(0.1)
        self.assertEqual(s.state, "done")
        return s

    def test_round(self):
        s = self.run_round()
        with open(os.path.join(s.session_dir, "session.json")) as f:
            meta = json.load(f)
        self.assertEqual(meta["bounty"], {"id": "test", "title": "Test taps", "board_updated": "2026-10-14"})
        self.assertEqual(meta["record_hz"], 30)
        take = os.path.join(s.session_dir, "takes", os.listdir(os.path.join(s.session_dir, "takes"))[0])
        with open(os.path.join(take, "prompts.jsonl")) as f:
            events = [json.loads(line) for line in f]
        acts = [e for e in events if e.get("event") == "prompt" and "act" in e]
        # step one: 3 taps, each with a rest; step two: 3 acts
        self.assertEqual([a["act"]["gesture"] for a in acts], ["tap", "rest"] * 3 + ["tap", "rest", "pinch"])
        self.assertEqual([a["id"].split("/")[-1] for a in acts[6:]], ["1-tap", "2-rest", "3-pinch"])
        self.assertEqual(acts[-1]["act"]["ghost"], [0.1, 0, 0, 0, 0, 0, 1.2])
        self.assertIn("ghost_room", acts[-1]["act"])
        scenes = [e for e in events if e.get("event") == "scene"]
        self.assertEqual([len(e["windows"]) for e in scenes], [1, 0, 2, 0])
        steady = [e for e in events if e.get("event") == "steady"]
        self.assertEqual([(e["win"], e["lit"], e["gesture"]) for e in steady], [("b", "bar", "pinch")])
        # the panel: windows up and down, the lights, the band layout and back
        cmds = [c for c in self.panel if c.startswith("panel: win") or c.startswith("panel: layout")]
        self.assertIn("panel: win b lit bar cyan L", cmds)
        self.assertIn("panel: win a lit tl+br cyan", cmds)
        self.assertIn("panel: win a ghost 0.1 0 0 0 0 0 1.2", cmds)
        self.assertEqual(cmds[-1], "panel: layout full")

    def test_done_mark(self):
        """Only a real round counts: a dry run leaves the profile alone, _mark_done adds one."""
        s = self.run_round()
        path = os.path.join(self.base, "profile.json")
        with open(path) as f:
            self.assertEqual(json.load(f)["bounties"]["test"]["rounds"], 1)
        s._mark_done()
        with open(path) as f:
            mark = json.load(f)["bounties"]["test"]
        self.assertEqual(mark, {"rounds": 2, "last": os.path.basename(s.session_dir)})


SWITCH_SCRIPT = dict(SCRIPT, sections=[
    {"id": "size", "title": "Size", "intro": "", "prompts": [
        {"text": "Palms toward you", "seconds": 2, "hands": "both",
         "switch": [{"at": 0.5, "to": "backs toward you", "text": "Backs toward you"}]}]},
    {"id": "taps", "title": "Taps", "kind": "scene", "intro": "Tap.", "prompts": [
        {"id": "one", "hands": "right", "text": "Tap.", "windows": [WIN], "switch": "elbow bent",
         "acts": {"gen": "buttons", "every_s": 1.0, "lit_s": 0.6, "count": 2}}]}])


class SwitchTest(unittest.TestCase):
    """Two poses in a step (DESIGN.md): the bar's tick, "Switch to ...", a "switch" event."""

    def test_list(self):
        sec = {"id": "s"}
        self.assertEqual(session._switch_list(sec, None), [])
        self.assertEqual(session._switch_list(sec, "backs"), [{"at": 0.5, "to": "backs", "text": ""}])
        two = session._switch_list(sec, [{"at": 0.3, "to": "a"}, {"at": 0.6, "to": "b", "text": "B"}])
        self.assertEqual([(w["at"], w["to"], w["text"]) for w in two], [(0.3, "a", ""), (0.6, "b", "B")])
        for bad in ([{"at": 0.6, "to": "a"}, {"at": 0.3, "to": "b"}], [{"at": 1, "to": "a"}], [{"at": 0.5}],
                    [{"at": 0.5, "to": " "}], [{"at": 0.5, "to": "a", "text": 3}], 5):
            with self.assertRaises(ValueError, msg=repr(bad)):
                session._switch_list(sec, bad)

    def test_round(self):
        tmp = tempfile.mkdtemp(prefix="handrec-switch-test-")
        self.addCleanup(shutil.rmtree, tmp, True)
        base, script = os.path.join(tmp, "base"), os.path.join(tmp, "test.json")
        os.makedirs(base)
        with open(script, "w") as f:
            json.dump(SWITCH_SCRIPT, f)
        panel = []
        s = session.Session(base, {}, {}, "room", script, dry_run=True, speed=10, next_after=0.02,
                            poses_dir=os.path.join(tmp, "no-poses"), button=False)
        s.print = panel.append
        s.start()
        end = time.monotonic() + 30
        while s._thread.is_alive() and time.monotonic() < end:
            s.join(0.1)
        self.assertEqual(s.state, "done")
        events = []
        takes = os.path.join(s.session_dir, "takes")
        for t in sorted(os.listdir(takes)):
            with open(os.path.join(takes, t, "prompts.jsonl")) as f:
                events += [json.loads(line) for line in f]
        switches = [e for e in events if e.get("event") == "switch"]
        self.assertEqual([(e["id"], e["to"]) for e in switches], [("size/both", "backs toward you"), ("taps/one", "elbow bent")])
        size = [e for e in events if e.get("event") == "prompt" and e["id"] == "size/both"]
        self.assertEqual(size[0]["switch"][0]["to"], "backs toward you")
        # the switch comes between the step's prompt event and its end
        k = events.index(switches[0])
        self.assertLess(events.index(size[0]), k)
        self.assertEqual(events[k + 1]["event"], "wait")
        cmds = [c for c in panel if c.startswith("panel: split") or c.startswith("panel: text Backs")]
        self.assertIn("panel: split 0.500 backs toward you", cmds)
        self.assertIn("panel: split 0.500 elbow bent", cmds)
        self.assertIn("panel: text Backs toward you", cmds)
        self.assertEqual(cmds[-1], "panel: split off")


if __name__ == "__main__":
    unittest.main()
