"""Offline numerical and timing-contract checks for the single-owner baseline."""

import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, RecoveryRequired
from framenpu.job import JobResult
import hmx_baseline_run as baseline


def completed(depth=1, transactions=4):
    _, product = baseline.operands("signed_dense")
    expected = baseline.reference(product, depth)
    output = bytearray(256 + transactions * 2048 + 4096)
    words = [0] * 64
    words[:8] = [baseline.MAGIC, 1, depth, transactions, 4096, transactions * 2048, 4096, 0]
    words[8], words[10], words[12] = 2000, 2500, 2600
    previous = 2600
    for rep in range(transactions):
        begin = previous + 100
        end = begin + 200 + 100 * depth
        words[32 + 8 * rep : 40 + 8 * rep] = [begin, 0, end, 0, end + 10, 0, end + 110, 0]
        previous = end + 110
        output[256 + rep * 2048 : 256 + (rep + 1) * 2048] = expected[0]
    words[16], words[18], words[20], words[22] = previous + 100, previous + 500, previous + 1000, 1
    struct.pack_into("<64I", output, 0, *words)
    output[256 + transactions * 2048 :] = expected[1]
    workers = []
    for w in range(3):
        status = [0] * 16
        status[0], status[1] = 17, w
        if w == 0:
            status[10], status[12] = 1000, previous + 2000
        workers.append(tuple(status))
    return JobResult(bytes(output), 5, tuple(workers)), expected


class BaselineReferenceTests(unittest.TestCase):
    def test_identity_reference_is_independent_of_layout(self):
        data, product = baseline.operands("identity")
        a_and_b = struct.unpack("<2048e", data)
        right = a_and_b[1024:]
        self.assertEqual([v / 64 for v in product], list(right))
        packed, converted = baseline.reference(product, 32)
        self.assertEqual(struct.unpack("<1024f", converted), tuple(v * 32 for v in right))
        # First packed row pair alternates the two independent row slices.
        values = struct.unpack("<1024e", packed)
        self.assertEqual(
            values[:64], tuple(right[r * 32 + c] * 32 for c in range(32) for r in (0, 1))
        )

    def test_integer_dense_reference_matches_scalar_dyadic_gemm(self):
        data, product = baseline.operands("signed_dense")
        values = struct.unpack("<2048e", data)
        a, b = values[:1024], values[1024:]
        actual = [
            sum(a[r * 32 + k] * b[k * 32 + c] for k in range(32))
            for r in range(32)
            for c in range(32)
        ]
        self.assertEqual(actual, [v / 64 for v in product])
        self.assertGreater(len(set(actual)), 32)


class BaselineTimingTests(unittest.TestCase):
    def test_separate_phases_and_all_transactions_are_checked(self):
        result, expected = completed(depth=32)
        sample = baseline.decode(result, 32, 4, expected)
        self.assertEqual(sample["checked_fp16_elements"], 4096)
        self.assertEqual(sample["checked_fp32_elements"], 1024)
        self.assertEqual(sample["dsp_phase_ticks"]["hmx"], 4 * 3400)
        self.assertEqual(sample["dsp_phase_ticks"]["raw_output_copy"], 400)
        self.assertGreaterEqual(sample["dsp_phase_ticks"]["unattributed"], 0)
        # An earlier transaction's last element must not escape final-tile checks.
        raw = bytearray(result.output)
        raw[256 + 2046] ^= 1
        with self.assertRaisesRegex(ValueError, "transaction 0"):
            baseline.decode(JobResult(bytes(raw), 5, result.workers), 32, 4, expected)
        raw = bytearray(result.output)
        raw[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "FP32 conversion"):
            baseline.decode(JobResult(bytes(raw), 5, result.workers), 32, 4, expected)

    def test_overlapping_phases_and_wrong_protocol_are_rejected(self):
        result, expected = completed()
        for offset, value in ((0, 0), (4 * 14, 1), (4 * (32 + 4), 1000), (4 * 18, 0)):
            raw = bytearray(result.output)
            struct.pack_into("<I", raw, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                baseline.decode(JobResult(bytes(raw), 5, result.workers), 1, 4, expected)

    def test_quantiles_and_depth_slope_preserve_units(self):
        samples = []
        for depth in (1, 32, 128, 512):
            result, expected = completed(depth)
            sample = baseline.decode(result, depth, 4, expected)
            sample.update(
                case="signed_dense",
                host_phase_ms={"power_on_ms": 1.0},
                submit_wall_ms=10.0,
                session_wall_ms=12.0,
            )
            samples.extend([sample] * 20)
        summary = baseline.summarize(samples)
        slope = summary["resident_slopes"][0]
        self.assertAlmostEqual(slope["ticks_per_depth_load"], 100)
        self.assertAlmostEqual(slope["transaction_fixed_ticks"], 200)
        self.assertAlmostEqual(slope["load_slope_gmac_per_s"], 6.291456)
        self.assertEqual(
            baseline.percentiles(list(range(1, 101))), {"p50": 50, "p95": 95, "p99": 99}
        )

    def test_timeout_stops_without_recovery_or_partial_throughput_claim(self):
        output = io.StringIO()
        with (
            patch.object(baseline.os, "geteuid", return_value=0),
            patch.object(baseline.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
            patch.object(baseline, "PowerSnapshot"),
            patch.object(baseline, "health", return_value={"healthy": True}),
            patch.object(baseline, "failure_snapshot", return_value={"read_only": True}),
            patch.object(baseline, "one", side_effect=RecoveryRequired("test timeout")) as one,
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(baseline.main(["--run-on-device", "--probe", "unused"]), 1)
            self.assertEqual(one.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["samples"], [])
        self.assertIn("inconclusive", report["interpretation"])
        self.assertNotIn("summary", report)


if __name__ == "__main__":
    unittest.main()
