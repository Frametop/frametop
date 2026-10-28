#!/usr/bin/env python3
"""Tests for hands/tools/desk_train.py: the prompts' spans, the labels it reads off them (from the
desk-work round's own plan, as hands/rec/session.py logs its prompts), the per-session hold-out and
the model it writes, read back by pointer/helper/deskclass.h. Made-up features, no recordings.

  python3 hands/tests/test_desk_train.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
HANDS = os.path.dirname(HERE)
ROOT = os.path.dirname(HANDS)
sys.path.insert(0, os.path.join(HANDS, "tools"))
sys.path.insert(0, os.path.join(HANDS, "rec"))
import desk_train as dt  # noqa: E402
import session  # noqa: E402

FEATURES = ["palm_x", "palm_y", "palm_z", "wander_x", "wander_z", "speed"]
S = 1e9


def write_prompts(path, events):
    with open(path, "w") as f:
        for e in events:
            f.write(json.dumps(e) + "\n")


def rows(t0, t1, palm_y=-0.35, wander=(0.0, 0.0), extra=()):
    """Both hands every 50 ms from t0 to t1 (s): (head, rows) as load_csv gives them."""
    head = ["t", "side", "id"] + FEATURES + list(extra)
    out = []
    for t in np.arange(t0, t1, 0.05):
        for s in (0, 1):
            out.append([t * S, s, s + 1, 0.1, palm_y, -0.3, wander[s], wander[s], 0.0] + [0.5] * len(extra))
    return head, np.array(out)


def prompt(t, pid, **kw):
    e = {"t": t * S, "event": "prompt", "id": pid, "hands": "both", "pose": "", "position": "centre"}
    e.update(kw)
    return e


class TestIntervals(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "p.jsonl")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_spans(self):
        """A prompt runs to the next prompt, wait or end; a paused stretch and a redone try count for
        nothing; bad lines are skipped."""
        write_prompts(self.path, [
            prompt(1, "a"), prompt(3, "b"), {"t": 5 * S, "event": "pause"}, {"t": 6 * S, "event": "resume"},
            prompt(7, "c"), {"t": 9 * S, "event": "redo", "from": 7 * S, "to": 9 * S}, prompt(9, "c"),
            {"t": 11 * S, "event": "end"}])
        with open(self.path, "a") as f:
            f.write("not json\n")
        spans = [(a / S, b / S, e["id"]) for a, b, e in dt.intervals(self.path)]
        self.assertEqual(spans, [(1, 3, "a"), (3, 5, "b"), (9, 11, "c")])

    def test_prompt_while_paused(self):
        write_prompts(self.path, [{"t": 1 * S, "event": "pause"}, prompt(2, "a"), {"t": 3 * S, "event": "resume"},
                                  prompt(4, "b"), {"t": 6 * S, "event": "end"}])
        self.assertEqual([e["id"] for _, _, e in dt.intervals(self.path)], ["b"])


class TestLabels(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "p.jsonl")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def labels(self, events, head_rows):
        write_prompts(self.path, events)
        head, r = head_rows
        return head, r, dt.label(head, r, self.path)

    def at(self, head, r, labels, t, side):
        i = int(np.argmin(np.abs(r[:, 0] - t * S) + (r[:, 1] != side) * 1e18))
        return labels[i]

    def test_round_vocabulary(self):
        """Every prompt the desk-work round logs (its own plan: the desk steps, the switch cues and the
        scene's acts) gets the label it is meant to: the round and the trainer agree on the ids."""
        script = session.load_script(session.bounty_script("desk-work"))
        plan, _ = session.build_plan(script, {}, seed=1)
        events, t, want = [], 1.0, []
        for sec in plan:
            for p in sec["prompts"]:
                if sec["kind"] == "scene":
                    for j, a in enumerate(p["scene"]["acts"]):
                        events.append(prompt(t, "%s/%d-%s" % (p["id"], j + 1, a["gesture"]), hands=p["hands"],
                                             pose=a["gesture"], position=p["position"], cue=True, step=p["id"],
                                             act={"gesture": a["gesture"], "win": a["win"]}))
                        want.append((t, p["id"], a["gesture"]))
                        t += 4
                elif p.get("cues"):
                    for c in p["cues"]:
                        events.append(prompt(t, "%s/%s" % (p["id"], c), hands=p["hands"], pose=c,
                                             position=p["position"], cue=True, step=p["id"]))
                        want.append((t, p["id"], c))
                        t += 4
                else:
                    events.append(prompt(t, p["id"], hands=p["hands"], pose=p["pose"], position=p["position"]))
                    want.append((t, p["id"], p["pose"]))
                    t += 4
        events.append({"t": t * S, "event": "end"})
        head, r, labels = self.labels(events, rows(0, t, wander=(0.01, 0.05)))
        for t0, pid, pose in want:
            got = [self.at(head, r, labels, t0 + 3, s) for s in (0, 1)]
            if pid == "desk/type" or (pid == "desk/switch" and pose == "typing"):
                self.assertEqual(got, [(dt.KEYS,)] * 2, pid)
            elif pid == "desk/mouse" or (pid == "desk/switch" and pose == "mouse"):
                self.assertEqual(got, [None, (dt.MOUSE,)], pid)  # the right hand wanders more
            elif pid in ("desk/rest", "desk-scene/look-up", "desk-scene/pinch-look"):
                self.assertEqual(got, [dt.DESK] * 2, pid)
            elif pid.startswith("desk-scene/reach-"):
                h = 1 if pid.endswith("right") else 0
                self.assertEqual(got[1 - h], dt.DESK, pid)
                self.assertEqual(got[h], dt.DESK if pose == "rest" else (dt.FREE,), (pid, pose))
            elif pid.startswith("hand-size"):
                self.assertEqual(got, [(dt.FREE,)] * 2, pid)
            else:
                self.fail("a prompt the trainer doesn't know: %s" % pid)

    def test_cue_skip_and_raised_hands(self):
        """The first CUE_SKIP s of a prompt get no label, nor does a hand at the desk held up high."""
        events = [prompt(1, "desk/type", pose="typing", position="down"), {"t": 5 * S, "event": "end"}]
        head, r, labels = self.labels(events, rows(0, 6))
        self.assertIsNone(self.at(head, r, labels, 1.5, 0))
        self.assertEqual(self.at(head, r, labels, 2.5, 0), (dt.KEYS,))
        self.assertIsNone(self.at(head, r, labels, 5.5, 0), "after the end")
        head, r, labels = self.labels(events, rows(0, 6, palm_y=-0.1))
        self.assertTrue(all(l is None for l in labels), "raised: no desk label")

    def test_other_rounds(self):
        """Elsewhere: the named hands free, a low step and an intro nothing, a rest act nothing."""
        events = [prompt(1, "gestures/pinch", hands="right", pose="pinch"),
                  prompt(4, "gestures/low", hands="both", pose="flat", position="down"),
                  prompt(7, "hand-size/intro", hands="both"),
                  prompt(10, "pokes/a/1-rest", hands="right", act={"gesture": "rest"}, step="pokes/a"),
                  {"t": 13 * S, "event": "end"}]
        head, r, labels = self.labels(events, rows(0, 14, palm_y=0.0))
        self.assertEqual([self.at(head, r, labels, 3, s) for s in (0, 1)], [None, (dt.FREE,)])
        for t in (6, 9, 12):
            self.assertEqual([self.at(head, r, labels, t, s) for s in (0, 1)], [None, None], t)


class TestTraining(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def take(self, name, palm_y=-0.35, extra=()):
        """A take: 6 s typing at the desk, then 6 s with the hands up and free."""
        base = os.path.join(self.dir, name)
        write_prompts(base + ".prompts.jsonl", [
            prompt(1, "desk/type", pose="typing", position="down"),
            prompt(7, "gestures/flat", pose="flat"), {"t": 13 * S, "event": "end"}])
        head, r1 = rows(0, 7, palm_y=palm_y, extra=extra)
        _, r2 = rows(7, 14, palm_y=0.05, extra=extra)
        r = np.vstack([r1, r2])
        with open(base + ".csv", "w") as f:
            f.write(",".join(head) + "\n")
            for row in r:
                f.write(",".join("%.6g" % v for v in row) + "\n")
        return base

    def test_sessions(self):
        self.assertEqual(dt.session("/x/20261005-160713-03-gestures"), "20261005-160713")
        self.assertEqual(dt.session("/x/my-take"), "my-take")

    def test_dataset(self):
        """Features by name (a model's p_* left out), each row in its take's session."""
        a = self.take("20261001-100000-01-a", extra=("p_free", "p_keys", "p_mouse"))
        b = self.take("20261001-100000-02-b", extra=("p_free", "p_keys", "p_mouse"))
        c = self.take("20261002-100000-01-a", extra=("p_free", "p_keys", "p_mouse"))
        names, x, mask, groups, kinds = dt.dataset([a, b, c])
        self.assertEqual(names, FEATURES)
        self.assertEqual(x.shape[1], len(FEATURES))
        self.assertEqual(sorted(set(groups)), ["20261001-100000", "20261002-100000"])
        self.assertEqual(set(kinds), {(dt.KEYS,), (dt.FREE,)})
        self.assertTrue(((mask.sum(axis=1)) == 1).all())

    def test_nothing_labelled(self):
        base = os.path.join(self.dir, "empty")
        write_prompts(base + ".prompts.jsonl", [])
        head, r = rows(0, 2)
        with open(base + ".csv", "w") as f:
            f.write(",".join(head) + "\n")
            for row in r:
                f.write(",".join("%.6g" % v for v in row) + "\n")
        with self.assertRaises(SystemExit):
            dt.dataset([base])

    def test_model_round_trip(self):
        """A model trained here and written out reads back in deskclass.h with the same numbers."""
        names, x, mask, groups, kinds = dt.dataset([self.take("20261001-100000-01-a"), self.take("20261002-100000-01-a")])
        net, mean, std = dt.fit(x, mask, kinds, [4], 300, 1)
        res = dt.score(net, mean, std, x, kinds)
        self.assertGreater(res[(dt.KEYS,)][1], 0.9)
        self.assertGreater(res[(dt.FREE,)][1], 0.9)
        path = os.path.join(self.dir, "model.txt")
        dt.write_model(path, names, net, mean, std)
        with open(path) as f:
            text = f.read()
        self.assertTrue(text.startswith("deskclass 1\nfeatures %d\nclasses free keys mouse\n" % len(names)))
        gxx = shutil.which("g++")
        if not gxx:
            self.skipTest("no g++")
        # deskclass.h wants its 30 features: the real model's shape, these 6 padded out with zeros
        full = os.path.join(self.dir, "full.txt")
        n = 30
        w0 = np.zeros((net.W[0].shape[0], n))
        w0[:, :len(names)] = net.W[0]
        net.W[0] = w0
        m = np.zeros(n)
        m[:len(names)] = mean
        s = np.ones(n)
        s[:len(names)] = std
        dt.write_model(full, ["f%d" % i for i in range(n)], net, m, s)
        src = os.path.join(self.dir, "probe.cpp")
        with open(src, "w") as f:
            f.write('#include "pointer/helper/deskclass.h"\n#include <cstdio>\n'
                    "int main(int, char **argv) { desk::Model m; if (!m.Load(argv[1])) { std::puts(m.error.c_str()); return 1; }\n"
                    "  double f[desk::Features::kCount] = {}, p[3]; double v;\n"
                    "  for (int i = 0; i < desk::Features::kCount && std::scanf(\"%lf\", &v) == 1; ++i) f[i] = v;\n"
                    '  m.Probs(f, p); std::printf("%.6f %.6f %.6f\\n", p[0], p[1], p[2]); }\n')
        exe = os.path.join(self.dir, "probe")
        subprocess.run([gxx, "-std=c++17", "-I", ROOT, src, "-o", exe], check=True)
        for row in (x[0], x[-1]):
            feats = np.zeros(n)
            feats[:len(names)] = row
            out = subprocess.run([exe, full], input=" ".join("%.9g" % v for v in feats), capture_output=True,
                                 text=True, check=True).stdout.split()
            want = net.probs(((feats - m) / s)[None, :])[0]
            np.testing.assert_allclose([float(v) for v in out], want, atol=1e-3)


if __name__ == "__main__":
    unittest.main()
