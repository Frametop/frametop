"""Independent overwrite/lifetime proofs for the pinned resident schedule."""

import unittest
import sys
import struct
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.resident_landmark import (
    _storage_schedule,
    ACTIVATIONS,
    GUARD,
    _transition,
    resident_weights,
)


class ResidentLandmarkLayoutTests(unittest.TestCase):
    def test_lossless_pair_packing_and_odd_row_padding(self):
        original = struct.pack("<6H", 0x3C00, 0xBC00, 0x3555, 0x1234, 0x7BFF, 0x8000)
        bias = struct.pack("<3f", 1.3051319122314453, -0.25, 3.0)
        g = SimpleNamespace(raw=original + bias)
        specs = [
            {
                "name": "dense",
                "operator": "InnerProduct",
                "weight_offset": 0,
                "weight_bytes": 12,
                "weight_dtype": "f16",
                "bias_offset": 12,
                "output_shape": (3,),
            }
        ]
        with (
            patch("framenpu.resident_landmark.specifications", return_value=specs),
            patch("framenpu.resident_landmark.WEIGHT_BYTES", 28),
        ):
            packed, offsets = resident_weights(g)
        self.assertEqual(
            struct.unpack("<8H", packed[:16]),
            (0x3C00, 0x3555, 0xBC00, 0x1234, 0x7BFF, 0, 0x8000, 0),
        )
        self.assertEqual(packed[16:], bias)
        self.assertEqual(offsets, {"dense": (0, 16)})

    def test_host_fsm_keeps_packed_input_until_all_output_channels_finish(self):
        record = [0] * 32
        record[0] = 1
        record[7:10] = [64, 1, 64]
        record[24] = 4
        record[26] = 16
        state = [0] * 32
        state[0] = 3
        state[3] = 63

        def step(kind, count):
            nonlocal state
            words = [0] * 32
            words[2] = 5
            words[3] = kind
            words[8] = state[8] + 1
            words[19] = 256
            state = _transition(state, words, count, record)

        step(9, 16)
        self.assertEqual((state[24], state[18]), (1, 16))
        step(1, 1)
        self.assertEqual((state[24], state[21]), (1, 32))
        step(1, 1)
        self.assertEqual((state[24], state[20]), (3, 2))
        step(8, 64)
        self.assertEqual((state[24], state[22], state[20]), (0, 32, 2))
        step(9, 16)
        step(1, 2)
        step(8, 64)
        self.assertEqual((state[19], state[24], state[20]), (1, 0, 0))

    def test_host_fsm_rejects_cross_phase_counts_and_models_guarded_fault(self):
        record = [0] * 32
        record[0] = 1
        record[7] = 32
        record[24] = 1
        record[26] = 16
        state = [0] * 32
        state[0] = 3
        state[24] = 1
        state[18] = 16
        words = [0] * 32
        words[2] = 5
        words[3] = 1
        words[8] = 1
        words[19] = 32
        with self.assertRaises(ValueError):
            _transition(state, words, 2, record)
        words[30] = 1
        result = _transition(state, words, 1, record)
        self.assertEqual(result[0], 4)
        self.assertEqual(result[20], 1)
        self.assertEqual(result[21], 0)
        self.assertEqual(result[24], 1)

    def test_downsample_chunks_never_overwrite_future_reads(self):
        future = set()
        units = [(y, x, min(32, 56 - x)) for y in range(56) for x in range(0, 56, 32)]
        for y, x, count in reversed(units):
            writes = set(range(y * 56 + x, y * 56 + x + count))
            self.assertFalse(writes & future, (y, x))
            reads = {
                (y * 2 + ky) * 112 + ox * 2 + kx
                for ox in range(x, x + count)
                for ky in range(3)
                for kx in range(3)
                if y * 2 + ky < 112 and ox * 2 + kx < 112
            }
            future.update(reads)
        self.assertEqual(max(future), 112 * 112 - 1)

    def test_forward_channel_compaction_never_overwrites_future_sources(self):
        future = set()
        for channel in reversed(range(64)):
            for vector in reversed(range(98)):
                source = channel * 392 + vector
                target = channel * 98 + vector
                self.assertNotIn(target, future, (channel, vector))
                future.add(source)
        self.assertEqual(64 * 98 * 128, 802816)

    def test_fp32_schedule_retains_skip_and_releases_compacted_tail(self):
        shapes = {
            "in0": (3, 224, 224),
            "2": (24, 112, 112),
            "3": (24, 112, 112),
            "4": (16, 112, 112),
            "5": (16, 112, 112),
            "6": (16, 112, 112),
            "7": (64, 112, 112),
            "8": (64, 56, 56),
            "9": (16, 56, 56),
            "10": (16, 56, 56),
            "out0": (16, 56, 56),
        }
        specs = []
        for op, name, ins, outs in (
            ("Input", "input", [], ["in0"]),
            ("Convolution", "conv0", ["in0"], ["2"]),
            ("ConvolutionDepthWise", "dw0", ["2"], ["3"]),
            ("Convolution", "conv10", ["3"], ["4"]),
            ("Split", "split", ["4"], ["5", "6"]),
            ("Convolution", "expand", ["6"], ["7"]),
            ("ConvolutionDepthWise", "convdwclip_1", ["7"], ["8"]),
            ("Convolution", "project", ["8"], ["9"]),
            ("Pooling", "pool", ["5"], ["10"]),
            ("BinaryOp", "add", ["9", "10"], ["out0"]),
        ):
            specs.append({"name": name, "operator": op, "input": ins, "output": outs})
        specs[6].update(
            input_shape=shapes["7"],
            output_shape=shapes["8"],
            kernel=(3, 3),
            stride=(2, 2),
            padding=(0, 1, 0, 1),
        )
        roots, placed, high = _storage_schedule(specs, shapes)
        self.assertEqual(roots["5"], "4")
        self.assertEqual(placed["7"]["offset"], placed["8"]["offset"])
        self.assertEqual(placed["8"]["producer_span"], 3211264)
        self.assertEqual(placed["8"]["bytes"], 802816)
        self.assertEqual(placed["4"]["last"], 8)
        self.assertEqual(high - ACTIVATIONS, 4014080)
        self.assertLess(high, GUARD)
        self.assertGreaterEqual(placed["9"]["offset"], placed["8"]["offset"] + 802816)
        specs[6]["stride"] = (1, 1)
        with self.assertRaises(ValueError):
            _storage_schedule(specs, shapes)


if __name__ == "__main__":
    unittest.main()
