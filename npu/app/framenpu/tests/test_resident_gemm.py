"""Host lifecycle simulation only; native GEMM/cache correctness needs hardware."""

from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest.mock import Mock
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import Device, Job, JobFailed, RecoveryRequired
from framenpu.profile import PROFILE
import framenpu.resident_gemm as rg
import hardware_resident
from test_library import FakePower, FakeTransport


class Emulated(FakeTransport):
    def __init__(self):
        super().__init__()
        self.memory[rg.ARENA_START : rg.ARENA_START + rg.SPAN] = b"\xa5" * rg.SPAN
        self.submissions = []
        self.bad_marker = False
        self.bad_output = False

    def publish(self, *, direct=False):
        super().publish()

    def wait_complete(self, sequence, timeout):
        if self.fail:
            raise RecoveryRequired("simulated timeout")
        d = list(struct.unpack_from("<32I", self.memory, PROFILE.arena))
        op = d[2]
        self.submissions.append(op)
        state = list(struct.unpack_from("<18I", self.memory, rg.STATE))
        seq = d[8] | d[9] << 32
        rejected = op not in (0, 1) and (
            state[3:8] != d[3:8]
            or state[10:18] != d[10:18]
            or seq != (state[8] | state[9] << 32) + 1
            or state[0] != 1
            and op != 4
        )
        error = 1 if rejected else 2 if op == 3 else 0
        if not rejected:
            if op == 1:
                size = d[19]
                raw = bytes(self.memory[rg.WEIGHTS : rg.WEIGHTS + size])
                self.memory[rg.WEIGHTS : rg.WEIGHTS + size] = rg.packed_weights(raw)
                state = d[:18]
                state[0] = 1
            elif op in (2, 3, 4):
                state[8:10] = d[8:10]
                if op == 4:
                    state[0] = 0
                elif op == 3:
                    state[0] = 2
                    self.memory[rg.OUTPUT : rg.OUTPUT + 128] = struct.pack(
                        "<32I", *([0xDECAFBAD] * 32)
                    )
                else:
                    self.memory[rg.OUTPUT : rg.OUTPUT + 4096] = bytes([int(self.bad_output)]) * 4096
            struct.pack_into("<18I", self.memory, rg.STATE, *state)
        meta = d + [0] * 32
        meta[30] = int(op == 3 and not rejected)
        meta[31] = 0x87A92500
        meta[32] = 110
        if not rejected:
            meta[42 if op != 3 else 44] = 150
        struct.pack_into("<64I", self.memory, PROFILE.arena + 128, *meta)
        for worker in range(3):
            row = [0] * 16
            row[:3] = [sequence, worker, error if worker == 0 else 0]
            row[7] = rg.SSR[worker]
            if worker == 0:
                row[8] = 0x87000000
                row[10], row[12] = 100, 200
                row[14:16] = [3, int(not self.bad_marker) * 3]
                if op == 3 and not rejected:
                    row[3] = 0x85860225
                    row[4] = 0x87A92500
            struct.pack_into("<16I", self.memory, PROFILE.status + worker * 64, *row)


def session(t):
    def factory(**kwargs):
        device = Device(_transport=t, _power_factory=lambda: FakePower(t.events), **kwargs)
        device._direct = Mock(installed=True)
        return device

    return rg.ResidentGemm(
        Job(bytes(2048), "proof", True, True),
        [[0.25] * 32 for _ in range(32)],
        _device_factory=factory,
        _lease_factory=Mock,
    )


class ResidentLifecycleTests(unittest.TestCase):
    def test_hardware_runner_requires_opt_in_before_probe_load(self):
        runner = Path(__file__).with_name("hardware_resident.py")
        result = subprocess.run(
            [sys.executable, str(runner), "--probe", "unused", "--output", "unused"],
            cwd="/tmp",
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device is required", result.stderr)

    def test_weights_uploaded_once_inputs_change_and_all_owned_bytes_restore(self):
        t = Emulated()
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_START + rg.SPAN])
        with session(t) as owner:
            for index in range(3):
                owner.run([[index / 8] * 32 for _ in range(32)], _expected=bytes(4096))
                self.assertEqual(
                    bytes(t.memory[rg.INPUT : rg.INPUT + 2048]),
                    struct.pack("<1024e", *([index / 8] * 1024)),
                )
            self.assertEqual(owner.metrics["weight_uploads"], 1)
            self.assertEqual(owner.metrics["input_uploads"], 3)
            self.assertEqual(owner.metrics["weight_upload_bytes"], 2048)
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_START + rg.SPAN]), original)
        self.assertEqual(t.submissions, [0, 1, 2, 2, 2, 4])
        self.assertEqual(t.events.count("claim"), len(t.submissions))
        self.assertEqual(t.status()["control"], [1, 0, 1])

    def test_stale_sequence_generation_and_content_are_rejected_without_advancement(self):
        t = Emulated()
        with session(t) as owner:
            for override in (
                {"sequence": owner.sequence},
                {"nonce": b"x" * 16},
                {"identity": b"x" * 32},
            ):
                before = owner.sequence
                owner._command(2, descriptor=owner._descriptor(2, **override), reject=True)
                self.assertEqual(owner.sequence, before)
                self.assertFalse(t.poisoned)
            owner.run([[0] * 32 for _ in range(32)], _expected=bytes(4096))

    def test_confirmed_fault_invalidates_mutable_tensors_and_clean_close_allows_new_session(self):
        t = Emulated()
        with session(t) as owner:
            with self.assertRaises(JobFailed):
                owner.run([[0] * 32 for _ in range(32)], _fault=True)
            with self.assertRaisesRegex(RuntimeError, "invalid"):
                owner.run([[0] * 32 for _ in range(32)])
            self.assertFalse(t.poisoned)
        with session(t) as fresh:
            fresh.run([[0] * 32 for _ in range(32)], _expected=bytes(4096))

    def test_timeout_retains_record_code_and_power_and_close_submits_nothing(self):
        t = Emulated()
        owner = session(t).__enter__()
        t.fail = True
        before = t.events.count("power_off")
        with self.assertRaises(RecoveryRequired):
            owner.run([[1] * 32 for _ in range(32)])
        code = bytes(t.memory[PROFILE.code : PROFILE.code + 4096])
        owner.close()
        self.assertEqual(t.events.count("power_off"), before)
        self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)
        self.assertEqual(bytes(t.memory[PROFILE.code : PROFILE.code + 4096]), code)
        self.assertEqual(t.submissions, [0, 1])
        self.assertIn("abandon", t.events)

    def test_lost_socket_does_not_authorize_reusing_persistent_record(self):
        t = Emulated()
        t.memory[rg.CONTROL : rg.CONTROL + 8] = rg.RECORD
        before = bytes(t.memory[rg.ARENA_START : rg.ARENA_START + rg.SPAN])
        with self.assertRaisesRegex(RecoveryRequired, "ownership record"):
            session(t).__enter__()
        self.assertEqual(t.submissions, [])
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_START + rg.SPAN]), before)
        self.assertFalse(t.owned)

    def test_bad_cleanup_or_numerical_result_retains_before_reclamation(self):
        for fault in ("bad_marker", "bad_output"):
            t = Emulated()
            owner = session(t).__enter__()
            setattr(t, fault, True)
            before = t.events.count("power_off")
            with self.subTest(fault=fault), self.assertRaises(RecoveryRequired):
                owner.run([[0] * 32 for _ in range(32)], _expected=bytes(4096))
            owner.close()
            self.assertEqual(t.events.count("power_off"), before)
            self.assertTrue(t.poisoned)
            self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)

    def test_changed_weight_or_guard_is_detected_before_power_down(self):
        for offset in (rg.WEIGHTS, rg.GUARD):
            t = Emulated()
            owner = session(t).__enter__()
            t.memory[offset] ^= 1
            before = t.events.count("power_off")
            with self.subTest(offset=offset), self.assertRaises(RecoveryRequired):
                owner.run([[0] * 32 for _ in range(32)])
            owner.close()
            self.assertEqual(t.events.count("power_off"), before)

    def test_private_hooks_are_validated_before_claim_and_prepare_runs_under_ownership(self):
        t = Emulated()
        device = Device(_transport=t)
        job = Job(bytes(4))
        for options in ({"_prepare": False}, {"_phase_times": []}):
            with self.assertRaises(TypeError):
                device._submit(job, **options)
        self.assertEqual(t.events, [])

        def stop(transport):
            self.assertTrue(transport.owned)
            raise ValueError("before publication")

        with self.assertRaises(ValueError):
            device._submit(job, _prepare=stop)
        self.assertFalse(t.owned)
        self.assertNotIn("publish", t.events)

    def test_invalid_upload_and_shape_are_rejected_without_new_submission(self):
        t = Emulated()
        with session(t) as owner:
            for action in (
                lambda: owner._command(2, upload=bytes(8193)),
                lambda: owner.run([[0] * 31] * 32),
                lambda: owner._command(2, expected=b"x"),
            ):
                with self.assertRaises(ValueError):
                    action()
            self.assertEqual(t.submissions, [0, 1])

    def test_sequence_wrap_reserves_the_final_identity_for_clean_close(self):
        t = Emulated()
        with session(t) as owner:
            owner.sequence = (1 << 64) - 2
            struct.pack_into("<2I", t.memory, rg.STATE + 32, 0xFFFFFFFE, 0xFFFFFFFF)
            with self.assertRaisesRegex(ValueError, "before sequence wrap"):
                owner.run([[0] * 32 for _ in range(32)])
            self.assertEqual(t.submissions, [0, 1])
        self.assertEqual(t.submissions, [0, 1, 4])

    def test_idle_observation_retries_only_unowned_stock_transition_and_has_a_bound(self):
        for state, expired in (([2, 0, 1], False), ([2, 1, 4], False), ([2, 0, 1], True)):
            health = Mock(
                side_effect=[
                    RecoveryRequired("unexpected post-session control: transitional"),
                    {"healthy": True},
                ]
            )
            observer = Mock()
            observer.__enter__ = Mock(return_value=observer)
            observer.__exit__ = Mock(return_value=False)
            observer.status.return_value = {"control": state, "callback": 0, "poisoned": False}
            with (
                patch.object(hardware_resident, "health", health),
                patch.object(hardware_resident, "Device", return_value=observer),
                patch.object(
                    hardware_resident.time, "monotonic", side_effect=[0, 1 if expired else 0.001]
                ),
            ):
                if state == [2, 0, 1] and not expired:
                    self.assertEqual(
                        hardware_resident.settled_health(None, None), {"healthy": True}
                    )
                else:
                    with self.assertRaises(RecoveryRequired):
                        hardware_resident.settled_health(None, None)


if __name__ == "__main__":
    unittest.main()
