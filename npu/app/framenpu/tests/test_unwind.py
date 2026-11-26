"""Regression checks for public-entry fault qualification and its failure boundary."""

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
from framenpu import Device, Job, RecoveryRequired
from framenpu.job import JobResult
from framenpu.profile import PROFILE
import hardware_unwind as unwind
from test_library import FakePower, FakeTransport


def completed(depth=3, trap=1, hmx=False):
    probe = Job(bytes(2048), "unwind", hmx, True)
    words = [0] * 32
    frames = depth + 1
    words[:8] = [
        unwind.MAGIC,
        1,
        depth,
        trap,
        int(hmx),
        frames,
        0 if trap else frames,
        unwind.PENDING if trap else unwind.reference(depth),
    ]
    for index, level in enumerate(range(depth, -1, -1)):
        words[8 + index] = 0x8702C000 - index * 64
        words[16 + index] = words[8 + index] + 40
        words[24 + index] = level
    workers = []
    for worker in range(3):
        row = [0] * 16
        row[0], row[1], row[7] = 11, worker, unwind.BASELINE_SSR[worker]
        if worker == 0:
            row[8], row[10], row[12] = 0x87000000, 1000, 2000
            if trap:
                row[2] = 2
                row[3] = (row[7] & ~255) | 37 | (1 << 17) | (int(hmx) << 26)
                row[4] = PROFILE.dsp_base + PROFILE.code + 1024
        workers.append(row)
    return probe, JobResult(struct.pack("<32I", *words), 5.0, workers)


class UnwindReportTests(unittest.TestCase):
    def test_nested_controls_and_faults_verify_return_counts_and_checksum(self):
        for depth in (0, 1, 3, 6):
            for trap in (0, 1):
                for hmx in (False, True):
                    probe, result = completed(depth, trap, hmx)
                    report = unwind.decode(result, probe, depth, trap)
                    self.assertEqual(report["frames_entered"], depth + 1)
                    self.assertEqual(report["frames_returned"], 0 if trap else depth + 1)
                    self.assertGreaterEqual(report["stack_span_bytes"], 40)

    def test_resumed_c_frames_and_modified_metadata_are_rejected(self):
        probe, result = completed()
        for word, value in ((6, 1), (7, 0), (24, 0), (15, 1)):
            raw = bytearray(result.output)
            struct.pack_into("<I", raw, word * 4, value)
            with self.subTest(word=word), self.assertRaises(ValueError):
                unwind.decode(JobResult(bytes(raw), 5, result.workers), probe, 3, 1)

    def test_bad_error_sequence_trap_pc_or_machine_state_are_rejected(self):
        for worker, word, value in (
            (0, 2, 0xF0A93400),
            (1, 0, 12),
            (2, 1, 1),
            (0, 4, 0),
            (0, 3, 0),
            (0, 3, 0x85860225),
            (0, 7, 0),
            (0, 8, 0),
        ):
            probe, result = completed()
            result.workers[worker][word] = value
            with self.subTest(worker=worker, word=word), self.assertRaises(ValueError):
                unwind.decode(result, probe, 3, 1)

    def test_live_frames_must_be_distinct_aligned_and_in_mapped_stack_memory(self):
        probe, result = completed()
        for word, value in ((17, 0x8702C000), (8, 0x8702C004), (8, 0xF002C000)):
            raw = bytearray(result.output)
            struct.pack_into("<I", raw, word * 4, value)
            with self.subTest(word=word), self.assertRaises(ValueError):
                unwind.decode(JobResult(bytes(raw), 5, result.workers), probe, 3, 1)

    def test_noncanonical_completion_retains_code_and_power_before_cleanup(self):
        probe, broken = completed(hmx=True)
        broken.workers[0][2] = 0xF0A93400

        class BrokenUnwind(FakeTransport):
            def wait_complete(self, sequence, timeout):
                for worker, row in enumerate(broken.workers):
                    row[0] = sequence
                    struct.pack_into("<16I", self.memory, PROFILE.status + worker * 64, *row)
                self.memory[PROFILE.arena : PROFILE.arena + 128] = broken.output

        transport = BrokenUnwind()
        device = Device(_transport=transport, _power_factory=lambda: FakePower(transport.events))
        with self.assertRaises(RecoveryRequired):
            device._submit(
                probe,
                output_size=128,
                _completion_validator=lambda result: unwind.validate_completion(
                    result, probe, 3, 1
                ),
            )
        self.assertTrue(transport.poisoned)
        self.assertTrue(transport.owned)
        self.assertEqual(transport.memory[PROFILE.code : PROFILE.code + 2048], probe.code)
        self.assertNotIn("power_off", transport.events)
        self.assertNotIn("ack", transport.events)
        self.assertIn("abandon", transport.events)
        device.close()

    def test_timeout_stops_after_first_session_without_recovery(self):
        probe, _ = completed()
        output = io.StringIO()
        with (
            patch.object(unwind.os, "geteuid", return_value=0),
            patch.object(unwind.Job, "load", return_value=probe),
            patch.object(unwind, "PowerSnapshot"),
            patch.object(unwind, "health", return_value={"healthy": True}),
            patch.object(unwind, "failure_snapshot", return_value={"read_only": True}),
            patch.object(unwind, "session", side_effect=RecoveryRequired("timeout")) as session,
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(
                unwind.main(["--run-on-device", "--probe", "unused", "--phase", "suite"]), 1
            )
            self.assertEqual(session.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["sessions"], [])
        self.assertNotIn("all_checks_passed", report)
        self.assertIn("no recovery attempted", report["interpretation"])

    def test_hardware_runner_requires_opt_in_before_opening_a_probe(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests/hardware_unwind.py"), "--probe", "unused"],
            cwd="/tmp",
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device is required", result.stderr)


if __name__ == "__main__":
    unittest.main()
