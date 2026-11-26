"""Timing resolution and descriptor preservation for the opt-in profiler."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import RecoveryRequired
from framenpu.job import JobResult
from hardware_depthwise_profile import decode_profile_result


def result(*, ticks=0, segments=4032, magic=0x44575031):
    words = list(range(64))
    words[26:30] = [ticks, 0, segments, magic]
    worker = [0] * 16
    worker[10], worker[12] = 100, 200
    return JobResult(struct.pack("<64I", *words), 1.0, (tuple(worker),))


class DepthwiseTimingTests(unittest.TestCase):
    def test_subtick_nonempty_window_is_valid(self):
        original = result()
        clean, sample = decode_profile_result(original)
        self.assertEqual(sample["ticks"], 0)
        self.assertEqual(sample["segments"], 4032)
        self.assertEqual(clean.output[:104], original.output[:104])
        self.assertEqual(clean.output[104:120], bytes(16))
        self.assertEqual(clean.output[120:], original.output[120:])
        self.assertNotEqual(original.output[104:120], bytes(16))

    def test_bad_magic_impossible_count_and_overlong_interval_require_retention(self):
        for value in (result(magic=0), result(ticks=1, segments=0), result(ticks=101)):
            with self.assertRaises(RecoveryRequired):
                decode_profile_result(value)

    def test_positive_and_empty_windows_remain_valid(self):
        for value in (result(ticks=17), result(segments=0)):
            clean, sample = decode_profile_result(value)
            self.assertEqual(sample["ticks"], struct.unpack_from("<I", value.output, 104)[0])
            self.assertEqual(clean.workers, value.workers)


if __name__ == "__main__":
    unittest.main()
