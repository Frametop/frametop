"""Cache alias identity, pre-reclamation cleanup checks and A/B failure boundaries."""

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
from framenpu import Device, Job, JobFailed, RecoveryRequired
from framenpu.job import JobResult
from framenpu.profile import PROFILE
import hmx_cached_run as cached
import hmx_batch2_run as batch
from test_hmx_baseline import completed as baseline_completed
from test_library import FakePower, FakeTransport


def completed(mode=1):
    result, expected = baseline_completed()
    output = bytearray(result.output)
    struct.pack_into("<I", output, 28, cached.BASES[mode] + 3000)
    workers = [list(row) for row in result.workers]
    workers[0][14:16] = [mode, mode]
    return JobResult(bytes(output), result.elapsed_ms, tuple(workers)), expected


class CacheContractTests(unittest.TestCase):
    def test_alias_and_every_output_are_checked(self):
        for mode in (0, 1):
            result, expected = completed(mode)
            sample = cached.decode(result, 1, 4, mode, expected)
            self.assertEqual(sample["cache_cleanup"], [mode, mode])
            self.assertEqual(sample["checked_fp16_elements"], 4096)
            self.assertEqual(sample["checked_fp32_elements"], 1024)
            for offset in (256 + 2046, len(result.output) - 1):
                output = bytearray(result.output)
                output[offset] ^= 1
                with self.subTest(mode=mode, offset=offset), self.assertRaises(ValueError):
                    cached.decode(JobResult(bytes(output), 5, result.workers), 1, 4, mode, expected)

    def test_wrong_alias_legacy_probe_and_partial_cleanup_are_rejected(self):
        result, expected = completed()
        output = bytearray(result.output)
        struct.pack_into("<I", output, 28, cached.BASES[0] + 3000)
        with self.assertRaisesRegex(ValueError, "wrong execution alias"):
            cached.decode(JobResult(bytes(output), 5, result.workers), 1, 4, 1, expected)
        for armed, done in ((0, 0), (1, 0), (0, 1)):
            workers = [list(row) for row in result.workers]
            workers[0][14:16] = [armed, done]
            with self.subTest(markers=(armed, done)), self.assertRaises(RecoveryRequired):
                cached.decode(JobResult(result.output, 5, workers), 1, 4, 1, expected)
        with self.assertRaisesRegex(ValueError, "invalid cache mode"):
            cached.validate_cleanup(result, 2)

    def test_missing_cleanup_retains_code_power_and_ownership(self):
        transport = FakeTransport()
        device = Device(_transport=transport, _power_factory=lambda: FakePower(transport.events))
        with self.assertRaises(RecoveryRequired):
            device._submit(
                Job(b"\x12" * 4, requires_hmx=True),
                _completion_validator=lambda result: cached.validate_cleanup(result, 1),
            )
        self.assertTrue(transport.poisoned)
        self.assertTrue(transport.owned)
        self.assertEqual(transport.memory[PROFILE.code : PROFILE.code + 4], b"\x12" * 4)
        self.assertNotIn("power_off", transport.events)
        self.assertNotIn("ack", transport.events)
        self.assertIn("abandon", transport.events)
        device.close()

    def test_confirmed_fault_cleanup_allows_normal_restoration(self):
        class CompletedFault(FakeTransport):
            def wait_complete(self, seq, timeout):
                super().wait_complete(seq, timeout)
                self.put(PROFILE.status + 56, 1)
                self.put(PROFILE.status + 60, 1)

        transport = CompletedFault(error=2)
        device = Device(_transport=transport, _power_factory=lambda: FakePower(transport.events))
        with self.assertRaises(JobFailed):
            device._submit(
                Job(b"\x12" * 4, requires_hmx=True),
                _completion_validator=lambda result: cached.validate_cleanup(result, 1),
            )
        self.assertFalse(transport.poisoned)
        self.assertFalse(transport.owned)
        self.assertEqual(transport.memory[PROFILE.code : PROFILE.code + 4], b"\0" * 4)
        self.assertIn("power_off", transport.events)
        self.assertIn("ack", transport.events)
        device.close()

    def test_timeout_stops_after_one_submit_without_a_performance_claim(self):
        output = io.StringIO()
        with (
            patch.object(cached.os, "geteuid", return_value=0),
            patch.object(cached.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
            patch.object(cached, "PowerSnapshot"),
            patch.object(cached, "health", return_value={"healthy": True}),
            patch.object(cached, "failure_snapshot", return_value={"read_only": True}),
            patch.object(cached, "one", side_effect=RecoveryRequired("timeout")) as one,
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(cached.main(["--run-on-device", "--probe", "unused"]), 1)
            self.assertEqual(one.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report["samples"], [])
        self.assertNotIn("summary", report)
        self.assertIn("inconclusive", report["interpretation"])

    def test_clock_drift_aborts_both_comparisons_before_another_submit(self):
        for module in (cached, batch):
            output = io.StringIO()
            samples = [{"powered_configuration": {"pll": value}} for value in (1, 2)]
            with (
                self.subTest(module=module.__name__),
                patch.object(module.os, "geteuid", return_value=0),
                patch.object(module.Job, "load", return_value=Job(b"\0" * 4, "probe", True, True)),
                patch.object(module, "PowerSnapshot"),
                patch.object(module, "health", return_value={"healthy": True}),
                patch.object(module, "failure_snapshot", return_value={"read_only": True}),
                patch.object(module, "one", side_effect=samples) as one,
                contextlib.redirect_stdout(output),
            ):
                self.assertEqual(module.main(["--run-on-device", "--probe", "unused"]), 1)
                self.assertEqual(one.call_count, 2)
            report = json.loads(output.getvalue())
            self.assertIn("clock configuration changed", report["error"])
            self.assertNotIn("summary", report)

    def test_guarded_fault_check_requires_cached_elr_and_an_audited_restore(self):
        result, _ = completed()
        workers = [list(row) for row in result.workers]
        workers[0][2], workers[0][4] = 2, cached.BASES[1] + 3000
        fault = JobResult(result.output, 5, workers)
        state = {"healthy": True}

        def audited(power, counters):
            class Stub:
                def __init__(self, **kwargs):
                    pass

                def __enter__(self):
                    return self

                def __exit__(self, *args):
                    pass

                def _submit(self, *args, **kwargs):
                    kwargs["_completion_validator"](fault)
                    counters["restored_hmx_jobs"] = 1
                    raise JobFailed("guarded fault")

            return Stub

        with (
            patch.object(cached, "audited_device", side_effect=audited),
            patch.object(cached, "health", return_value=state),
        ):
            report = cached.verify_fault_cleanup(None, None, None, state)
            self.assertEqual(report["cache_cleanup"], [1, 1])
            self.assertEqual(report["restored_hmx_jobs"], 1)
            workers[0][4] = cached.BASES[0] + 3000
            evidence = {}
            with self.assertRaisesRegex(ValueError, "fault did not originate in cached code"):
                cached.verify_fault_cleanup(None, None, None, state, evidence)
            self.assertEqual(evidence["fault_elr"], workers[0][4])
            self.assertEqual(evidence["worker_status"][0][2], 2)
            self.assertIn("job_error", evidence)


if __name__ == "__main__":
    unittest.main()
