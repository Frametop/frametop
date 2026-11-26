"""Offline validation of experimental timing reports and failure boundaries."""

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
import hmx_multi_run as multi
import hmx_pipe_run as pipe
import hmx_stream_run as stream


def result(iters=200, active=2):
    output = bytearray(multi.OUTPUT_BYTES)
    workers = []
    for w in range(3):
        row = [0] * 16
        row[0], row[1] = 17, w
        row[10] = 10000 + 10 * w
        # Deliberately unequal fixed overhead for the VTCM backup paths.
        row[12] = row[10] + (500 + 80000 * w + 100 * iters if w < active else 100)
        workers.append(tuple(row))
        if w < active:
            struct.pack_into("<4I", output, w * 16, multi.MAGIC, iters, 0x4000, w)
    return JobResult(bytes(output), 5.0, tuple(workers))


class MultiReportTests(unittest.TestCase):
    def test_slope_cancels_different_worker_setup_costs(self):
        points = [multi.decode(result(n), n, 2) for n in (200, 1000)]
        report = multi.summarize(points, 2)
        self.assertEqual(report["worker_ticks_per_iter"], [100, 100])
        self.assertEqual(report["envelope_ticks_per_iter"], 100)
        self.assertAlmostEqual(report["aggregate_issue_gmac_per_s"], 12.582912)
        self.assertFalse(points[0]["workers"][2]["ran"])

    def test_partial_completion_and_invalid_timestamps_are_not_throughput(self):
        original = result()
        for worker, word, value in (
            (0, 0, 0),
            (1, 0, 18),
            (1, 1, 0),
            (2, 2, 2),
            (0, 10, 0),
            (0, 12, 9999),
            (0, 12, 10000 + multi.MAX_TICKS),
        ):
            rows = [list(row) for row in original.workers]
            rows[worker][word] = value
            invalid = JobResult(original.output, original.elapsed_ms, rows)
            with self.subTest(worker=worker, word=word), self.assertRaises(ValueError):
                multi.decode(invalid, 200, 2)

    def test_wrong_result_markers_and_idle_worker_writes_are_rejected(self):
        original = result()
        for offset, value in ((0, 0), (4, 201), (16 + 12, 0), (32, multi.MAGIC)):
            output = bytearray(original.output)
            struct.pack_into("<I", output, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                multi.decode(JobResult(bytes(output), 1.0, original.workers), 200, 2)
        with self.assertRaises(ValueError):
            multi.decode(JobResult(original.output[:64], 1.0, original.workers), 200, 2)

    def test_nonpositive_slope_is_inconclusive(self):
        low = multi.decode(result(), 200, 2)
        high = dict(low, iters=1000)
        with self.assertRaisesRegex(ValueError, "inconclusive"):
            multi.summarize([low, high], 2)

    def test_timeout_stops_without_another_submission(self):
        output = io.StringIO()
        with (
            patch.object(multi.os, "geteuid", return_value=0),
            patch.object(multi.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
            patch.object(multi, "PowerSnapshot"),
            patch.object(multi, "health", return_value={"healthy": True}),
            patch.object(multi, "failure_snapshot", return_value={"read_only": True}),
            patch.object(multi, "Device") as device,
            contextlib.redirect_stdout(output),
        ):
            submit = device.return_value.__enter__.return_value.submit
            submit.side_effect = RecoveryRequired("test timeout")
            rc = multi.main(["--run-on-device", "--probe", "unused"])
            self.assertEqual(rc, 1)
            self.assertEqual(submit.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["points"], [])
        self.assertIn("no recovery attempted", report["interpretation"])
        self.assertNotIn("measurement", report)


class ProbeBoundaryTests(unittest.TestCase):
    def test_sweep_timeout_is_propagated_without_recovery(self):
        for module in (pipe, stream):
            with self.subTest(module=module.__name__), patch.object(module, "one") as one:
                one.side_effect = RecoveryRequired("test timeout")
                with self.assertRaises(RecoveryRequired):
                    module.sweep(None, 1, 32)
                self.assertEqual(one.call_count, 1)

    def test_all_runners_require_opt_in_from_an_unrelated_directory(self):
        for name in ("bench", "pipe", "stream", "multi", "accum", "baseline", "cached", "batch2"):
            completed = subprocess.run(
                [sys.executable, str(ROOT / "tests" / f"hmx_{name}_run.py"), "--probe", "unused"],
                cwd="/tmp",
                capture_output=True,
                text=True,
                check=False,
            )
            with self.subTest(name=name):
                self.assertEqual(completed.returncode, 2)
                self.assertIn("--run-on-device is required", completed.stderr)


if __name__ == "__main__":
    unittest.main()
