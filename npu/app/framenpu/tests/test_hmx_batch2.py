"""Offline references, mode identity and throughput units for batching two blocks."""

import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, RecoveryRequired
from framenpu.job import JobResult
import hmx_batch2_run as batch
from test_hmx_baseline import completed as baseline_completed


def completed(groups=1, transactions=4, mode=0):
    result, _ = baseline_completed(groups, transactions)
    _, product = batch.operands("signed_dense")
    expected = batch.reference(product, groups)
    output = bytearray(result.output)
    struct.pack_into("<I", output, 28, mode + 1)
    for rep in range(transactions):
        output[256 + rep * 2048 : 256 + (rep + 1) * 2048] = expected[0]
    output[256 + transactions * 2048 :] = expected[1]
    return JobResult(bytes(output), result.elapsed_ms, result.workers), expected


class BatchReferenceTests(unittest.TestCase):
    def test_two_distinct_blocks_contribute_to_the_exact_reference(self):
        for case in batch.CASES:
            data, product = batch.operands(case)
            values = struct.unpack("<2048e", data)
            first, second = values[:1024], values[1024:]
            self.assertNotEqual(first, second)
            square0 = [
                sum(first[r * 32 + k] * first[k * 32 + c] for k in range(32))
                for r in range(32)
                for c in range(32)
            ]
            square1 = [
                sum(second[r * 32 + k] * second[k * 32 + c] for k in range(32))
                for r in range(32)
                for c in range(32)
            ]
            self.assertEqual([v / 64 for v in product], [a + b for a, b in zip(square0, square1)])
            self.assertNotEqual([v / 64 for v in product], square0)
            self.assertNotEqual([v / 64 for v in product], square1)

    def test_full_output_checks_and_mac_count_cover_both_blocks(self):
        for mode in (0, 1):
            result, expected = completed(groups=16, mode=mode)
            sample = batch.decode(result, 16, 4, mode, expected)
            self.assertEqual(sample["depth"], 32)
            self.assertEqual(sample["groups"], 16)
            self.assertEqual(sample["macs"], 32768 * 32 * 4)
            self.assertEqual(sample["checked_fp16_elements"], 4096)
            self.assertEqual(sample["checked_fp32_elements"], 1024)
            raw = bytearray(result.output)
            raw[256 + 2046] ^= 1
            with self.assertRaisesRegex(ValueError, "transaction 0"):
                batch.decode(JobResult(bytes(raw), 5, result.workers), 16, 4, mode, expected)

    def test_wrong_mode_or_legacy_probe_metadata_is_rejected(self):
        result, expected = completed()
        with self.assertRaisesRegex(ValueError, "mode tag"):
            batch.decode(result, 1, 4, 1, expected)
        raw = bytearray(result.output)
        struct.pack_into("<I", raw, 28, 0)
        with self.assertRaisesRegex(ValueError, "mode tag"):
            batch.decode(JobResult(bytes(raw), 5, result.workers), 1, 4, 0, expected)
        with self.assertRaisesRegex(ValueError, "parameters"):
            batch.decode(result, 257, 4, 0, expected)

    def test_summary_uses_two_depth_blocks_per_group(self):
        samples = []
        for groups in (1, 16, 64, 256):
            for mode in (0, 1):
                result, expected = completed(groups, mode=mode)
                sample = batch.decode(result, groups, 4, mode, expected)
                sample["dsp_phase_ticks"]["hmx"] = 4 * (200 + groups * (200 if mode == 0 else 100))
                sample.update(
                    matrix_case="signed_dense",
                    case=f"signed_dense/{batch.MODES[mode]}",
                    host_phase_ms={"power_on_ms": 1.0},
                    submit_wall_ms=10.0,
                    session_wall_ms=12.0,
                )
                samples.append(sample)
        summary = batch.summarize(samples)
        slopes = {row["case"]: row for row in summary["resident_slopes"]}
        self.assertAlmostEqual(slopes["signed_dense/control"]["ticks_per_depth_load"], 100)
        self.assertAlmostEqual(slopes["signed_dense/batch2"]["ticks_per_depth_load"], 50)
        self.assertAlmostEqual(slopes["signed_dense/control"]["load_slope_gmac_per_s"], 6.291456)
        self.assertAlmostEqual(slopes["signed_dense/batch2"]["load_slope_gmac_per_s"], 12.582912)
        deepest = next(row for row in summary["comparisons"] if row["groups"] == 256)
        self.assertEqual(deepest["logical_shape"], [32, 16384, 32])
        self.assertAlmostEqual(deepest["speedup_at_p50"], 51400 / 25800)

    def test_runner_opt_in_and_timeout_boundary(self):
        completed_process = subprocess.run(
            [sys.executable, str(ROOT / "tests/hmx_batch2_run.py"), "--probe", "unused"],
            cwd="/tmp",
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed_process.returncode, 2)
        self.assertIn("--run-on-device is required", completed_process.stderr)
        output = io.StringIO()
        with (
            patch.object(batch.os, "geteuid", return_value=0),
            patch.object(batch.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
            patch.object(batch, "PowerSnapshot"),
            patch.object(batch, "health", return_value={"healthy": True}),
            patch.object(batch, "failure_snapshot", return_value={"read_only": True}),
            patch.object(batch, "one", side_effect=RecoveryRequired("test timeout")) as one,
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(
                batch.main(["--run-on-device", "--probe", "unused", "--modes", "0", "1"]), 1
            )
            self.assertEqual(one.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["samples"], [])
        self.assertNotIn("summary", report)
        self.assertIn("inconclusive", report["interpretation"])


if __name__ == "__main__":
    unittest.main()
