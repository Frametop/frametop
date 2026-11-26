"""Independent word oracle, buffer budgets and coverage for gather qualification."""

from pathlib import Path
import struct
import subprocess
import sys
import unittest

from hardware_gather_optimization import SHAPES, cases_for, expected_words, payload


class GatherQualificationTests(unittest.TestCase):
    def test_stride_two_leading_padding_preserves_raw_bits_and_zero_fill(self):
        source = (0x80000000, 0x7FC00001, 1, 0x7F800000, 0xFFA12345)
        case = (1, 2, 0, 2, 0, 0, 0, 0, 0, 3)
        expected = [0, 0x80000000, 1] + [0] * 29
        self.assertEqual(expected_words((1, 1, 5), source, case), expected)

    def test_last_channel_row_never_uses_the_following_descriptor_bytes(self):
        source = tuple(range(1, 13))
        case = (1, 1, 0, 0, 1, 1, 0, 0, 0, 32)
        self.assertEqual(expected_words((2, 2, 3), source, case), [10, 11, 12] + [0] * 29)
        outside = (1, 1, 0, 0, 1, 2, 0, 0, 0, 32)
        self.assertEqual(expected_words((2, 2, 3), source, outside), [0] * 32)

    def test_maximum_batch_fits_borrowed_arena_and_work_budget(self):
        shape = (2, 112, 112)
        case = (1, 1, 0, 0, 1, 111, 96, 0, 0, 16)
        raw, header = payload(shape, (0,) * (2 * 112 * 112), [case] * 32)
        self.assertEqual(header[:5], [2, 112, 112, 32, 1])
        self.assertLessEqual(((len(raw) + 127) & ~127) + 32 * 128 + 128, 131072)
        self.assertEqual(struct.unpack_from("<16I", raw, len(raw) - 64)[:10], case)
        with self.assertRaises(ValueError):
            payload(shape, (), [case] * 33)
        with self.assertRaises(ValueError):
            payload(shape, (), [case] * 3, 16)

    def test_coverage_contains_small_tensors_first_last_channels_and_full_masks(self):
        self.assertIn((1, 1, 1), SHAPES)
        self.assertIn((2, 7, 7), SHAPES)
        self.assertIn((2, 14, 14), SHAPES)
        cases = cases_for((2, 7, 7))
        self.assertEqual(len(cases), len(set(cases)))
        self.assertTrue(any(c[4] == 0 and c[5] == 0 and c[6] == 0 and c[3] == 2 for c in cases))
        self.assertTrue(any(c[4] == 1 and c[5] == 6 for c in cases))
        self.assertEqual({c[7] for c in cases}, set(range(5)))
        self.assertEqual({c[8] for c in cases}, set(range(5)))
        self.assertTrue(any(c[9] == 32 for c in cases))

    def test_runner_refuses_before_artifacts_without_explicit_opt_in(self):
        path = Path(__file__).with_name("hardware_gather_optimization.py")
        result = subprocess.run(
            [sys.executable, str(path), "--probes", "unused", "--output", "unused"],
            cwd="/tmp",
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device and Frame root are required", result.stderr)


if __name__ == "__main__":
    unittest.main()
