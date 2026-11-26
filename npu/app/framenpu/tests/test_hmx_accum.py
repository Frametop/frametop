"""Offline checks for accumulator isolation protocol and full-tile comparison."""

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
import hmx_accum_run as accum


def completed(iters=1, mask=3, mode=0):
    output = bytearray(accum.OUTPUT_BYTES)
    selected = [w for w in range(3) if mask & 1 << w]
    rows = []
    for w in range(3):
        status = [0] * 16
        status[0], status[1], status[10], status[12] = 17, w, 1000, 100000
        rows.append(tuple(status))
        if w not in selected:
            continue
        index = selected.index(w)
        generation = 2 if mode == 2 else 2 + len(selected) * (2 if mode == 1 else 1)
        struct.pack_into("<I", output, w * 4, generation)
        start = 3000 + (10 if mode == 2 else 2000) * index
        store = 10000 + 1000 * index if mode == 1 else start + 220
        meta = (
            accum.MAGIC,
            w,
            mask,
            mode,
            iters,
            0,
            2000 + w,
            0,
            start,
            0,
            start + 200,
            0,
            store,
            0,
            store + 200,
            0,
        )
        struct.pack_into("<16I", output, 64 + 64 * w, *meta)
        output[256 + w * 2048 : 256 + (w + 1) * 2048] = accum.expected_tile(iters, w)
    return JobResult(bytes(output), 5, tuple(rows))


class AccumulatorProtocolTests(unittest.TestCase):
    def test_distinct_exact_references_and_every_element_checked(self):
        for w, bits in enumerate((0x5000, 0x5400, 0x5800)):
            self.assertEqual(accum.expected_tile(1, w), struct.pack("<H", bits) * 1024)
        run, tiles = accum.decode(completed(mask=7), 1, 7, 0, {})
        self.assertTrue(run["matches_independent_results"])
        self.assertEqual(sum(w["checked_elements"] for w in run["workers"]), 3072)
        # Corrupt the last element of worker 2, beyond any sentinel-only check.
        original = completed(mask=7)
        raw = bytearray(original.output)
        struct.pack_into("<H", raw, 256 + 3 * 2048 - 2, 0x7E00)
        run, _ = accum.decode(JobResult(bytes(raw), 5, original.workers), 1, 7, 0, tiles)
        self.assertFalse(run["matches_independent_results"])
        bad = run["workers"][2]
        self.assertEqual(bad["mismatched_elements"], 1)
        self.assertEqual(bad["mismatches_vs_isolated_control"], 1)
        self.assertEqual(bad["first_mismatches"][0]["index"], 1023)

    def test_numeric_interference_is_a_finding_and_keeps_other_workers(self):
        original = completed(mode=1)
        raw = bytearray(original.output)
        raw[256 : 256 + 2048] = accum.expected_tile(1, 1)
        run, _ = accum.decode(JobResult(bytes(raw), 5, original.workers), 1, 3, 1, {})
        self.assertEqual(run["workers"][0]["mismatched_elements"], 1024)
        self.assertEqual(run["workers"][1]["mismatched_elements"], 0)

    def test_staged_and_concurrent_timing_are_distinguished(self):
        for mode in (0, 1, 2):
            run, _ = accum.decode(completed(mode=mode), 1, 3, mode, {})
            overlap = run["all_worker_window_overlap_ticks"]
            self.assertTrue(overlap > 0 if mode == 2 else overlap is None)
        original = completed(mode=1)
        raw = bytearray(original.output)
        # Force worker 0 to store before worker 1 has finished its load.
        struct.pack_into("<I", raw, 64 + 12 * 4, 3300)
        struct.pack_into("<I", raw, 64 + 14 * 4, 3400)
        with self.assertRaisesRegex(ValueError, "deferred store"):
            accum.decode(JobResult(bytes(raw), 5, original.workers), 1, 3, 1, {})

    def test_missing_generations_abort_and_inactive_writes_are_invalid(self):
        original = completed()
        for offset, value in ((0, 0), (16, 1), (64 + 5 * 4, 1), (256 + 4096, 1)):
            raw = bytearray(original.output)
            struct.pack_into("<I", raw, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                accum.decode(JobResult(bytes(raw), 5, original.workers), 1, 3, 0, {})

    def test_worker_started_before_another_ready_is_invalid(self):
        original = completed(mode=2)
        raw = bytearray(original.output)
        struct.pack_into("<I", raw, 128 + 6 * 4, 3005)
        with self.assertRaisesRegex(ValueError, "before every selected worker"):
            accum.decode(JobResult(bytes(raw), 5, original.workers), 1, 3, 2, {})

    def test_controls_precede_experiments_and_cover_all_worker_pairs(self):
        cases = list(accum.cases("suite", [1, 32], 3))
        self.assertEqual(len(cases), 62)
        self.assertTrue(all(case[0] == "isolated" for case in cases[:6]))
        self.assertTrue(all(case[0] == "serialized" for case in cases[6:14]))
        self.assertEqual({case[2] for case in cases if case[0] == "staged"}, {3, 5, 6, 7})

    def test_timeout_stops_after_one_submission_and_reports_inconclusive(self):
        output = io.StringIO()
        with (
            patch.object(accum.os, "geteuid", return_value=0),
            patch.object(accum.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
            patch.object(accum, "PowerSnapshot"),
            patch.object(accum, "health", return_value={"healthy": True}),
            patch.object(accum, "failure_snapshot", return_value={"read_only": True}),
            patch.object(accum, "Device") as device,
            contextlib.redirect_stdout(output),
        ):
            submit = device.return_value.__enter__.return_value.submit
            submit.side_effect = RecoveryRequired("test timeout")
            self.assertEqual(
                accum.main(["--run-on-device", "--probe", "unused", "--phase", "suite"]), 1
            )
            self.assertEqual(submit.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertIn("inconclusive", report["interpretation"])
        self.assertNotIn("isolation_passed", report)


if __name__ == "__main__":
    unittest.main()
