#!/usr/bin/env python3
"""compare.py on a made-up run with known errors: SteamVR 1 degree high, our live tracker 0.5
degrees left, and our tracker's finds made from a calibration, with the headset slipping 8 px
(2 degrees) a third of the way in, so the replays' jump rules differ in a known way. Needs
numpy and OpenCV (the tracker's Python):

  scripts/in-box /opt/frametop/venv/bin/python gaze/bench/tests/test_compare.py
"""
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
try:
    import numpy as np
    import cv2  # noqa: F401  (eyes_pupil's)
    import compare
    import eyes_model
except ImportError as e:
    compare, NO_DEPS = None, str(e)

PPD = 20.0               # screen pixels per degree, as fake_ft_gaze.py
K = 4.0                  # pupil pixels per degree
T0, WALL = 1000.0, 1.79e9
SLIP_AT, SLIP = 14.0, (8.0, 0.0)   # s after T0, px: the headset slips this much (both eyes)
# (s after T0 of the click, yaw, pitch, nudged)
TRIALS = [(3, 0, 0, False), (5, 10, 0, False), (7, -15, 5, True), (9, 5, -10, False), (11, 25, 0, False),
          (13, -5, 8, False),
          (16, 8, 3, False),                                  # 2 s after the slip: the jump isn't in yet
          (24, -10, -5, False), (27, 12, 6, False), (30, 0, -12, False), (33, -18, 0, False)]


def hit(hy, hp):
    return {"s": 2, "x": 500 - PPD * hy, "y": 400 - PPD * hp, "j": [-PPD, 0.0, 0.0, -PPD], "dpp": 1 / PPD}


def pupil(e, hy, hp, t):
    """Where eye e's pupil is for a gaze, t s after T0."""
    sx = SLIP[0] if t >= SLIP_AT else 0.0
    return (256 + 10 * e + K * hy + sx, 200 - K * hp)


def mid(e, t):
    sx = SLIP[0] if t >= SLIP_AT else 0.0
    return (256 + 10 * e + sx, 230.0)


def looking(t):
    """The target being looked at: the next one to be clicked."""
    for tc, hy, hp, _ in TRIALS:
        if t <= T0 + tc:
            return hy, hp
    return TRIALS[-1][1:3]


def make_run(d):
    (d / "start").mkdir(parents=True)
    dots = [{"truth": (hy, hp), "eye": [{"pupil": np.array(pupil(e, hy, hp, 0)), "mid": np.array(mid(e, 0))}
                                        for e in (0, 1)]}
            for hy in (-20, -10, 0, 10, 20) for hp in (-15, -7, 0, 7, 15)]
    cal = eyes_model.Calibration.fit(dots, {"made": "M", "feature": "pupil", "dots": len(dots)})
    cal.save(d / "start" / "eyes-calibration.json")
    with open(d / "start" / "eyes-calibration-dots.jsonl", "w") as f:
        for k in dots:
            f.write(json.dumps({"made": "M", "truth": k["truth"]}) + "\n")
    blank = {"base": [0, 0], "ref": None, "meas": []}
    (d / "start" / "eyes-state.json").write_text(json.dumps({"calibration": "M", "shift": {"0": blank, "1": blank}}))
    eyes = {"right": {"reseat": False, "jump": False, "jump_px": [0, 0]},
            "left": {"reseat": False, "jump": False, "jump_px": [0, 0]}}
    (d / "run.json").write_text(json.dumps({
        "scenario": {"id": "synthetic"}, "screen": {"w": 1000, "h": 800, "dpr": 1.0}, "look": [0.35, 0.05],
        "start": {"time": WALL, "t_raw": T0, "eyes": {"feature": "pupil", "eyes": eyes}}, "last_lesson": WALL - 500}))
    with open(d / "gaze.jsonl", "w") as g, open(d / "own.jsonl", "w") as o:
        n = 0
        while n / 90 < TRIALS[-1][0] + 1:
            t = T0 + n / 90
            hy, hp = looking(t)
            src = {"mmap1": {"hy": hy, "hp": hp + 1, "hit": hit(hy, hp + 1)},
                   "left": {"hy": hy + 1, "hp": hp + 1, "hit": hit(hy + 1, hp + 1)},
                   "right": {"hy": hy - 1, "hp": hp + 1, "hit": hit(hy - 1, hp + 1)},
                   "own": {"hy": hy + 0.5, "hp": hp, "hit": hit(hy + 0.5, hp)}}
            g.write(json.dumps({"t": t, "head": {"yaw": 0, "pitch": 0, "hit": hit(0, 0)}, "src": src}) + "\n")
            for e in (0, 1):
                px, py = pupil(e, hy, hp, t - T0)
                mx, my = mid(e, t - T0)
                o.write(json.dumps({"t": t + 0.001 * e, "cam": e, "x": px, "y": py, "a": 20, "b": 18, "angle": 0,
                                    "glints": [[mx, my - 10], [mx, my + 10]]}) + "\n")
            n += 1
    with open(d / "trials.jsonl", "w") as f, open(d / "lessons.jsonl", "w") as ls:
        for i, (tc, hy, hp, nudged) in enumerate(TRIALS):
            x, y = 500 - PPD * hy, 400 - PPD * hp
            f.write(json.dumps({"kind": "trial", "i": i, "u": x / 1000, "v": y / 800, "x": x, "y": y,
                                "click": {"x": x + 3, "y": y, "t": T0 + tc, "button": "left"},
                                "truth": {"x": x, "y": y, "from": "target"}}) + "\n")
            if nudged:
                ls.write(json.dumps({"time": WALL + tc + 0.02, "kind": "own", "raw": [hy + 0.5, hp], "true": [hy, hp],
                                     "look_t": T0 + tc - 0.05}) + "\n")


class CompareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if compare is None:
            raise unittest.SkipTest(f"needs numpy and OpenCV: {NO_DEPS}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name) / "run"
        make_run(cls.dir)
        cls.bench_run = compare.Run(cls.dir)
        cls.recs = compare.score(cls.bench_run)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def errs(self, name):
        return [r["err"][name] for r in self.recs]

    def test_trials_and_truth(self):
        self.assertEqual(len(self.recs), len(TRIALS))
        self.assertEqual(self.bench_run.screen, 2)
        for r, (_, hy, hp, nudged) in zip(self.recs, TRIALS):
            self.assertAlmostEqual(r["truth"][0], hy, places=6)
            self.assertAlmostEqual(r["truth"][1], hp, places=6)
            self.assertEqual(r["nudged"], nudged)
        # 25 degrees left is the screen's left edge here (x 0)
        self.assertEqual([r["where"] for r in self.recs], ["edge" if t[1] == 25 else "inside" for t in TRIALS])
        self.assertAlmostEqual(self.recs[0]["since"], 3 + 500, delta=0.01)

    def test_live(self):
        for r, (_, hy, hp, _) in zip(self.recs, TRIALS):
            self.assertAlmostEqual(r["err"]["steamvr"], compare.angle((hy, hp + 1), (hy, hp)), places=4)
            self.assertAlmostEqual(r["err"]["steamvr-eyes"], r["err"]["steamvr"], places=6)
            self.assertAlmostEqual(r["err"]["own-live"], compare.angle((hy + 0.5, hp), (hy, hp)), places=4)
            fused = (hy + 0.35, hp + 0.3)
            self.assertAlmostEqual(r["err"]["fuse"], compare.angle(fused, (hy, hp)), places=4)
        # steamvr+clicks learns the 1 degree from the first click on
        self.assertTrue(all(e < 1e-6 for e in self.errs("steamvr+clicks")[1:]))

    def test_pointer(self):
        for r, (_, hy, hp, nudged) in zip(self.recs, TRIALS):
            want = compare.angle((hy + 0.5, hp), (hy, hp)) if nudged else 3 / PPD
            self.assertAlmostEqual(r["err"]["pointer"], want, delta=0.02)
            if nudged:
                self.assertAlmostEqual(r["check"], 3 / PPD, delta=0.02)  # the click vs the lesson's truth

    def test_replays(self):
        before = [k for k, t in enumerate(TRIALS) if t[0] < SLIP_AT]
        late = [k for k, t in enumerate(TRIALS) if t[0] > SLIP_AT + 8]
        slip_deg = SLIP[0] / K
        for name in ("own-replay", "no-jump", "both-jump", "edge-guard", "every-click", "bridge"):
            e = self.errs(name)
            self.assertTrue(all(e[k] < 0.05 for k in before), (name, e))
        # The slip: the glint jump puts it right some seconds later; without it, it stays 2 degrees
        # off until a click (there are none after it here); every click puts it right at once.
        for name in ("own-replay", "both-jump", "edge-guard", "bridge"):
            e = self.errs(name)
            self.assertTrue(all(e[k] < 0.1 for k in late), (name, e))
        nj = self.errs("no-jump")
        self.assertTrue(all(abs(nj[k] - slip_deg) < 0.15 for k in late), nj)
        ec = self.errs("every-click")
        self.assertTrue(all(ec[k] < 0.1 for k in late), ec)
        k16 = next(k for k, t in enumerate(TRIALS) if t[0] == 16)   # 2 s after: nothing has caught it yet
        self.assertAlmostEqual(self.errs("own-replay")[k16], slip_deg, delta=0.15)
        # The live gaze here is made up, 0.5 degrees from the replay's
        for k in before:
            self.assertAlmostEqual(self.recs[k]["replay_vs_live"], compare.angle((TRIALS[k][1] + 0.5, TRIALS[k][2]),
                                                                                 TRIALS[k][1:3]), delta=0.05)

    def test_edge_guard(self):
        cal = eyes_model.Calibration.load(self.dir / "start" / "eyes-calibration.json")
        box = compare.calibration_box(self.bench_run, cal)
        self.assertEqual(box, (-20, 20, -15, 15))
        clicks = [(T0 + 5, T0 + 4.9, (10.0, 0.0)), (T0 + 11, T0 + 10.9, (25.0, 0.0))]
        rp = compare.Replay(self.bench_run, self.bench_run.frames(), cal, {}, None, clicks, guard=True, box=box)
        rp()
        self.assertEqual((rp.taught, rp.skipped), (1, 1))

    def test_screen_scale(self):
        """A window 800 x 640 on the 1000 x 800 screen with Qt's pixel ratio 2 (on the Frame: 2752 wide
        on 3440 pixels): the window's pixels are scaled by the sizes in run.json, not by the ratio."""
        self.assertIn("no screen sizes", " ".join(self.bench_run.notes))   # this run has none: the ratio (1)
        d = Path(self.tmp.name) / "scaled"
        d.mkdir()
        for f in self.dir.iterdir():
            if f.is_file():
                (d / f.name).write_bytes(f.read_bytes())
        (d / "start").symlink_to(self.dir / "start")
        info = json.loads((d / "run.json").read_text())
        info.update(screen={"w": 800, "h": 640, "dpr": 2.0}, screens={"2": [1000, 800]})
        (d / "run.json").write_text(json.dumps(info))
        rows = []
        for line in (d / "trials.jsonl").read_text().splitlines():
            r = json.loads(line)
            for k in ("click", "truth"):
                r[k]["x"], r[k]["y"] = 0.8 * r[k]["x"], 0.8 * r[k]["y"]
            r["x"], r["y"] = r["truth"]["x"], r["truth"]["y"]
            rows.append(json.dumps(r))
        (d / "trials.jsonl").write_text("\n".join(rows) + "\n")
        run = compare.Run(d)
        self.assertEqual((run.sx, run.sy), (1.25, 1.25))
        self.assertEqual(run.notes, [])
        for r, (_, hy, hp, _) in zip(compare.score(run), TRIALS):
            self.assertAlmostEqual(r["truth"][0], hy, places=6)
            self.assertAlmostEqual(r["truth"][1], hp, places=6)

    def test_panel_run(self):
        """A run on the headset's panel: the truth is the target's direction (no screen), there's
        no pointer, and the nudges' lessons (none would come from the bench) aren't matched."""
        d = Path(self.tmp.name) / "panel"
        d.mkdir()
        for f in self.dir.iterdir():
            if f.is_file():
                (d / f.name).write_bytes(f.read_bytes())
        (d / "start").symlink_to(self.dir / "start")
        info = json.loads((d / "run.json").read_text())
        del info["screen"]
        info.update(display="panel", look=[0.4, 0.1])
        (d / "run.json").write_text(json.dumps(info))
        with open(d / "trials.jsonl", "w") as f:
            for i, (tc, hy, hp, _) in enumerate(TRIALS):
                f.write(json.dumps({"kind": "trial", "i": i, "u": 0.0 if hy == 25 else 0.5, "v": 0.5, "yaw": hy, "pitch": hp,
                                    "click": {"t": T0 + tc, "by": "button"},
                                    "truth": {"yaw": hy, "pitch": hp, "from": "target"}}) + "\n")
        run = compare.Run(d)
        self.assertTrue(run.panel)
        self.assertEqual(run.notes, [])
        recs = compare.score(run)
        self.assertEqual(len(recs), len(TRIALS))
        for r, (_, hy, hp, _) in zip(recs, TRIALS):
            self.assertEqual(r["truth"], (hy, hp))
            self.assertFalse(r["nudged"])
            self.assertNotIn("pointer", r["err"])
            self.assertAlmostEqual(r["err"]["steamvr"], compare.angle((hy, hp + 1), (hy, hp)), places=4)
            self.assertAlmostEqual(r["err"]["own-live"], compare.angle((hy + 0.5, hp), (hy, hp)), places=4)
        self.assertEqual([r["where"] for r in recs], ["edge" if t[1] == 25 else "inside" for t in TRIALS])

    def test_main(self):
        out = Path(self.tmp.name) / "recs.json"
        sys.argv = ["compare.py", str(self.dir.parent), "--json", str(out)]
        compare.main()
        self.assertEqual(len(json.loads(out.read_text())), len(TRIALS))


if __name__ == "__main__":
    unittest.main()
