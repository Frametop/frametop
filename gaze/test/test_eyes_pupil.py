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


if __name__ == "__main__":
    unittest.main()
