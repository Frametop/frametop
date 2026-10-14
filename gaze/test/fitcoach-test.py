#!/usr/bin/env python3
"""The fit coach (gaze/fitcoach.py) on made-up readings: the reference fit, which way each hint
points with both cameras' orientations, the state's hysteresis, lost eyes, and a baseline.

  python3 gaze/test/fitcoach-test.py
"""
import math
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import fitcoach  # noqa: E402

# About the maintainer's headset's (gaze/fit-reference.json): camera 1's image is turned 180
# degrees from camera 0's, so its signs are the other way.
J = {0: [[1.27, -0.42], [-0.48, -1.34]], 1: [[-1.34, -0.53], [-0.48, 1.48]]}
CENTRE = {0: [341.0, 221.4], 1: [334.7, 173.5]}


def ref(follow=None, faces=1):
    return fitcoach.Reference({c: {"centre": CENTRE[c], "jacobian": J[c]} for c in (0, 1)}, ipd=0.065,
                              faces=faces, cameras_follow_lenses=follow)


def pupil(cam, yaw, pitch, centre=None):
    """Where camera cam sees the pupil when the eye appears to look (yaw, pitch) degrees."""
    cx, cy = centre or CENTRE[cam]
    (a, b), (c, d) = J[cam]
    return cx + a * yaw + b * pitch, cy + c * yaw + d * pitch


def summary(right=(0, 0), left=(0, 0), found=(1.0, 1.0), spacing=(14.0, 16.0), extra=(0.0, 0.0)):
    out = {}
    for cam, (yaw, pitch) in ((0, right), (1, left)):
        x, y = pupil(cam, yaw, pitch)
        out[cam] = {"n": 90, "finds": int(90 * found[cam]), "found": found[cam], "x": x, "y": y, "a": 28.0,
                    "spacing": spacing[cam], "extra": extra[cam]}
    return out


def keys(reading):
    return [h["key"] for h in reading["hints"]]


class ReferenceTest(unittest.TestCase):
    def test_fit_recovers_both_orientations(self):
        rng = random.Random(1)
        rows = []
        for made in ("a", "b"):
            for _ in range(27):
                yaw, pitch = rng.uniform(-20, 20), rng.uniform(-15, 15)
                rows.append({"made": made, "truth": [yaw, pitch],
                             "eye": {str(c): {"pupil": [v + rng.gauss(0, 0.5) for v in pupil(c, yaw, pitch)]} for c in (0, 1)}})
        # A third calibration that fits badly is left out.
        for _ in range(27):
            yaw, pitch = rng.uniform(-20, 20), rng.uniform(-15, 15)
            rows.append({"made": "bad", "truth": [yaw, pitch],
                         "eye": {str(c): {"pupil": [v + rng.gauss(0, 15) for v in pupil(c, yaw, pitch)]} for c in (0, 1)}})
        r = fitcoach.Reference.from_dots(rows)
        for c in (0, 1):
            self.assertEqual(r.eyes[c]["fits"], 2)
            for i in range(2):
                self.assertAlmostEqual(r.eyes[c]["centre"][i], CENTRE[c][i], delta=0.5)
                for j in range(2):
                    self.assertAlmostEqual(r.eyes[c]["jacobian"][i][j], J[c][i][j], delta=0.05)
        # degrees() undoes the Jacobian.
        x, y = pupil(1, 7.0, -3.0)
        yaw, pitch = r.degrees(1, x - r.eyes[1]["centre"][0], y - r.eyes[1]["centre"][1])
        self.assertAlmostEqual(yaw, 7.0, delta=0.2)
        self.assertAlmostEqual(pitch, -3.0, delta=0.2)

    def test_json_round_trip(self):
        r = ref(follow=True, faces=4)
        back = fitcoach.Reference.from_json(r.to_json())
        self.assertEqual(back.eyes[1]["jacobian"], J[1])
        self.assertTrue(back.cameras_follow_lenses)
        self.assertFalse(back.provisional)
        self.assertTrue(ref().provisional)

    def test_the_shipped_reference_loads(self):
        r = fitcoach.Reference.load()
        self.assertIsNotNone(r, fitcoach.REFERENCE)
        self.assertEqual(sorted(r.eyes), [0, 1])
        self.assertTrue(r.provisional)


class AssessTest(unittest.TestCase):
    def test_centred_is_ok(self):
        r = fitcoach.assess(summary(), ref())
        self.assertEqual(r["state"], "ok")
        self.assertEqual(r["hints"], [])
        self.assertTrue(r["provisional"])

    def test_eyes_high_means_push_up(self):
        r = fitcoach.assess(summary(right=(0, 12), left=(0, 12)), ref())
        self.assertEqual(r["state"], "adjust")
        self.assertEqual(keys(r), ["up"])
        r = fitcoach.assess(summary(right=(0, -12), left=(0, -12)), ref())
        self.assertEqual(keys(r), ["down"])

    def test_eyes_left_means_slide_left(self):
        r = fitcoach.assess(summary(right=(11, 0), left=(11, 0)), ref())
        self.assertEqual(keys(r), ["left"])
        self.assertAlmostEqual(r["terms"]["left"], 11, delta=0.1)
        r = fitcoach.assess(summary(right=(-11, 0), left=(-11, 0)), ref())
        self.assertEqual(keys(r), ["right"])

    def test_ipd_only_advised_once_the_cameras_are_known_to_follow_the_lenses(self):
        apart = summary(right=(-10, 0), left=(10, 0))   # each eye outward of its lens
        r = fitcoach.assess(apart, ref(follow=None))
        self.assertAlmostEqual(r["terms"]["ipd"], 10, delta=0.1)
        self.assertNotIn("ipd-wider", keys(r))
        self.assertFalse(r["ipd_advice"])
        self.assertEqual(r["state"], "ok", "the common terms are centred")
        r = fitcoach.assess(apart, ref(follow=True))
        self.assertEqual(keys(r), ["ipd-wider"])
        self.assertEqual(r["state"], "adjust")
        r = fitcoach.assess(summary(right=(10, 0), left=(-10, 0)), ref(follow=True))
        self.assertEqual(keys(r), ["ipd-narrower"])

    def test_tilt(self):
        r = fitcoach.assess(summary(right=(0, -5), left=(0, 5)), ref())
        self.assertEqual(keys(r), ["tilt-left-up"])
        r = fitcoach.assess(summary(right=(0, 5), left=(0, -5)), ref())
        self.assertEqual(keys(r), ["tilt-right-up"])

    def test_hysteresis_between_ok_and_adjust(self):
        mid = summary(right=(0, 6.5), left=(0, 6.5))
        self.assertEqual(fitcoach.assess(mid, ref(), previous={"state": "ok"})["state"], "ok")
        self.assertEqual(fitcoach.assess(mid, ref(), previous={"state": "adjust"})["state"], "adjust")
        self.assertEqual(fitcoach.assess(mid, ref())["state"], "adjust")
        self.assertEqual(fitcoach.assess(mid, ref(), previous={"state": "lost"})["state"], "adjust")

    def test_a_lost_eye_comes_first(self):
        r = fitcoach.assess(summary(right=(0, 12), left=(0, 12), found=(1.0, 0.3)), ref())
        self.assertEqual(r["state"], "lost")
        self.assertEqual(keys(r)[0], "lost")
        self.assertIn("left eye", r["hints"][0]["text"])

    def test_waiting_without_frames(self):
        r = fitcoach.assess({0: {"n": 0}, 1: {"n": 0}}, ref())
        self.assertEqual(r["state"], "waiting")
        self.assertEqual(fitcoach.assess({}, ref())["state"], "waiting")

    def test_reflections(self):
        r = fitcoach.assess(summary(extra=(0.0, 0.8)), ref())
        self.assertEqual(keys(r), ["reflections"])
        self.assertIn("left", r["hints"][0]["text"])


class BaselineTest(unittest.TestCase):
    def test_against_the_baseline(self):
        good = summary(right=(4, -3), left=(4, -3))     # this face's good fit isn't the reference's
        base = fitcoach.make_baseline(good, ipd=0.065)
        r = fitcoach.assess(good, ref(), ipd=0.065, baseline=base)
        self.assertEqual((r["state"], r["against"]), ("ok", "baseline"))
        self.assertFalse(fitcoach.moved(r))
        # Back on, 7 degrees lower on the face: moved, and which way to put it back.
        slipped = summary(right=(4, 4), left=(4, 4))
        r = fitcoach.assess(slipped, ref(), ipd=0.065, baseline=base)
        self.assertTrue(fitcoach.moved(r))
        self.assertAlmostEqual(r["terms"]["up"], 7, delta=0.1)

    def test_eye_relief_and_ipd_change(self):
        base = fitcoach.make_baseline(summary(), ipd=0.065)
        near = summary(spacing=(14.0 * 1.12, 16.0 * 1.12))
        r = fitcoach.assess(near, ref(), ipd=0.0665, baseline=base)
        self.assertEqual(keys(r), ["ipd-changed", "nearer"])
        self.assertIn("65.0 mm, now 66.5 mm", r["hints"][0]["text"])
        self.assertTrue(fitcoach.moved(r))
        far = summary(spacing=(14.0 * 0.9, 16.0 * 0.9))
        self.assertEqual(keys(fitcoach.assess(far, ref(), ipd=0.065, baseline=base)), ["further"])


class ObservationsTest(unittest.TestCase):
    def test_window_and_medians(self):
        obs = fitcoach.Observations(window=1.0)
        # A pair 14 px apart and four stray ones: MANY_GLINTS.
        glints = [(300.0, 200.0), (301.0, 214.0), (250.0, 120.0), (260.0, 120.0), (270.0, 120.0), (280.0, 120.0)]
        for i in range(60):
            t = i / 30
            obs.add(0, t, {"x": 300 + (i % 3), "y": 210.0, "a": 25.0, "glints": glints})
            obs.add(1, t, None if i % 2 else {"x": 320.0, "y": 170.0, "a": 30.0, "glints": []})
        s = obs.summary()
        self.assertEqual(s[0]["n"], 31)                     # the last second only
        self.assertEqual(s[0]["x"], 301)
        self.assertAlmostEqual(s[0]["spacing"], math.hypot(1, 14), places=5)
        self.assertEqual(s[0]["extra"], 1.0)
        self.assertAlmostEqual(s[1]["found"], 15 / 31, places=3)
        self.assertIsNone(s[1]["spacing"])

    def test_glint_spacing_matches_eyes_pupil(self):
        try:
            sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tracker"))
            import eyes_pupil
        except ImportError:
            self.skipTest("no numpy or OpenCV here")
        rng = random.Random(3)
        for _ in range(200):
            p = {"x": 300.0, "y": 200.0, "glints": [(rng.uniform(270, 330), rng.uniform(170, 230)) for _ in range(rng.randint(0, 5))]}
            pair = eyes_pupil.glint_pair(p)
            want = None if pair is None else math.hypot(pair[1][0] - pair[0][0], pair[1][1] - pair[0][1])
            got = fitcoach.glint_spacing(p)
            if want is None:
                self.assertIsNone(got)
            else:
                self.assertAlmostEqual(got, want, places=6)


if __name__ == "__main__":
    unittest.main()
