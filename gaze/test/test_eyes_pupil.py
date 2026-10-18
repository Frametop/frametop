#!/usr/bin/env python3
"""Offline pupil detection: shared preprocessing preserves both closing attempts.

Synthetic images exercise normal pupils, glints, the small-kernel retry, lost eyes,
and windowed searches. No camera, calibration, or SteamVR connection is needed.
"""
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tracker"))
import eyes_pupil as pupil


def frame(retry=False):
    img = np.full((400, 512), 100, np.uint8)
    centre = (44, 200) if retry else (256, 200)
    if retry:
        img[:, :11] = 0  # large closing joins the pupil to this edge region
    cv2.ellipse(img, centre, (30, 24), 0, 0, 360, 0, -1)
    return img


def separate_attempts(img, ox, oy):
    """The old preprocessing order: each attempt gets a newly calculated mask."""
    for close in (pupil.CLOSE, pupil.CLOSE_TIGHT):
        dark = (cv2.GaussianBlur(img, (5, 5), 0) < pupil.DARK).astype(np.uint8)
        p = pupil._find(img, dark, ox, oy, close)
        if p is not None:
            return p
    return None


class PupilTest(unittest.TestCase):
    def test_first_attempt(self):
        img = frame()
        with patch.object(pupil.cv2, "GaussianBlur", wraps=cv2.GaussianBlur) as blur, \
                patch.object(pupil, "_find", wraps=pupil._find) as find:
            p = pupil.find_pupil(img)
        self.assertEqual((blur.call_count, find.call_count), (1, 1))
        self.assertAlmostEqual(p["x"], 256, delta=0.1)
        self.assertAlmostEqual(p["y"], 200, delta=0.1)

    def test_retry_preserves_mask_and_kernels(self):
        img = frame(retry=True)
        dark = (cv2.GaussianBlur(img, (5, 5), 0) < pupil.DARK).astype(np.uint8)
        self.assertIsNone(pupil._find(img, dark, 0, 0))
        expected = pupil._find(img, dark, 0, 0, pupil.CLOSE_TIGHT)
        self.assertIsNotNone(expected)
        kernels = {k: v.copy() for k, v in pupil._KERNELS.items()}
        with patch.object(pupil.cv2, "GaussianBlur", wraps=cv2.GaussianBlur) as blur, \
                patch.object(pupil, "_find", wraps=pupil._find) as find:
            self.assertEqual(pupil._find_either(img, 0, 0), expected)
        self.assertEqual((blur.call_count, find.call_count), (1, 2))
        first, second = (call.args[1] for call in find.call_args_list)
        self.assertIs(first, second)
        np.testing.assert_array_equal(first, dark)
        for k, v in kernels.items():
            np.testing.assert_array_equal(pupil._KERNELS[k], v)

    def test_exact_results_for_full_and_cropped_images(self):
        rng = np.random.default_rng(512400)
        images = [np.zeros((400, 512), np.uint8), np.full((400, 512), 255, np.uint8),
                  frame(), frame(retry=True)]
        glints = frame()
        for y in (194, 207):
            cv2.circle(glints, (256, y), 2, 255, -1)
        images.append(glints)
        for _ in range(12):
            img = rng.integers(35, 180, (400, 512), np.uint8)
            cv2.ellipse(img, (int(rng.integers(30, 480)), int(rng.integers(25, 375))),
                        (int(rng.integers(10, 45)), int(rng.integers(8, 25))),
                        int(rng.integers(0, 180)), 0, 360, 0, -1)
            images.append(img)
        for i, img in enumerate(images):
            for x, y in ((0, 0), (11, 15), (200, 120)):
                roi = img if (x, y) == (0, 0) else img[y:y + 180, x:x + 200]
                before = roi.copy()
                with self.subTest(image=i, offset=(x, y)):
                    self.assertEqual(pupil._find_either(roi, x, y), separate_attempts(roi, x, y))
                    np.testing.assert_array_equal(roi, before)

    def test_window_hint_and_full_frame_fallback(self):
        img = frame()
        expected = pupil.find_pupil(img)
        self.assertEqual(pupil.find_pupil(img, expected), expected)
        self.assertEqual(pupil.find_pupil(img, {"x": 50, "y": 50, "a": 5}), expected)
        self.assertEqual(pupil.find_pupil(np.zeros_like(img), expected), None)

    def test_a_much_smaller_spot_isnt_the_pupil(self):
        """The right camera's dark spot of 2026-10-17: darker and smaller than the pupil, so it
        wins on score unless the pupil's recent size rules it out."""
        img = np.full((400, 512), 100, np.uint8)
        cv2.ellipse(img, (256, 200), (30, 24), 0, 0, 360, 20, -1)
        cv2.ellipse(img, (215, 250), (10, 7), 0, 0, 360, 0, -1)
        self.assertAlmostEqual(pupil.find_pupil(img)["x"], 215, delta=0.5)
        p = pupil.find_pupil(img, smallest=15)
        self.assertAlmostEqual(p["x"], 256, delta=0.5)
        self.assertAlmostEqual(pupil.find_pupil(img, p, 15)["x"], 256, delta=0.5)
        self.assertIsNone(pupil.find_pupil(img, p, 40))

    def test_pupil_size(self):
        size = pupil.PupilSize()
        for i in range(pupil.SIZE_FINDS - 1):
            size.add(i / 90, {"a": 20.0})
        self.assertEqual(size.smallest(9 / 90), 0.0)  # too few finds yet
        size.add(9 / 90, {"a": 24.0})
        size.add(10 / 90, None)  # lost frames don't count
        self.assertAlmostEqual(size.smallest(10 / 90), pupil.SMALLEST * 20.0)
        self.assertEqual(size.smallest(10 / 90 + pupil.SIZE_WINDOW + 0.2), 0.0)  # all of them too old


def warm(guard, t=0.0, a=(20.0, 20.0)):
    """Both eyes' usual sizes: GUARD_FINDS finds each, 90 Hz, from t. Returns the next t."""
    for i in range(pupil.GUARD_FINDS):
        for e in (0, 1):
            assert guard.keep(e, t + i / 90, a[e])
    return t + pupil.GUARD_FINDS / 90


class SizeGuardTest(unittest.TestCase):
    def test_a_misfind_in_one_eye(self):
        g = pupil.SizeGuard("both")
        t = warm(g)
        self.assertTrue(g.keep(1, t, 19.0))
        self.assertFalse(g.keep(0, t + 0.001, 13.0))   # 0.65 of usual, the other eye at 0.95
        self.assertTrue(g.keep(0, t + 1 / 90, 19.5))
        self.assertEqual(g.refused, {0: 1, 1: 0})

    def test_light_shrinks_both(self):
        """Both pupils down to under half their usual size: kept with "both", refused with "long"."""
        for rule, kept in (("both", True), ("long", False)):
            g = pupil.SizeGuard(rule)
            t = warm(g)
            ok = []
            for i in range(20):   # narrowing over 20 frames, both eyes alike
                a = 20.0 * (1 - 0.03 * i)
                ok += [g.keep(0, t, a), g.keep(1, t + 0.001, a)]
                t += 1 / 90
            with self.subTest(rule=rule):
                self.assertEqual(all(ok), kept)

    def test_other_eye_not_seen(self):
        g = pupil.SizeGuard("both")
        t = warm(g) + 1.0   # the other eye's last find is old
        self.assertTrue(g.keep(0, t, 13.0))    # 0.65: over GUARD_ALONE
        self.assertFalse(g.keep(0, t + 0.01, 11.0))

    def test_refusals_going_on_start_over(self):
        g = pupil.SizeGuard("long")
        t = warm(g)
        ok = [g.keep(0, t + i / 90, 10.0) for i in range(int(pupil.GUARD_RESET * 90) + 5)]
        self.assertFalse(ok[0])
        self.assertTrue(ok[-1])        # taken again after GUARD_RESET s
        self.assertTrue(g.keep(0, t + 2.0, 10.0))

    def test_off_and_unknown(self):
        self.assertTrue(pupil.SizeGuard("off").keep(0, 0.0, 1.0))
        self.assertEqual(pupil.SizeGuard("nonsense").rule, "both")


if __name__ == "__main__":
    unittest.main()
