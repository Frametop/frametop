"""Failure retention and bounded restoration; native arithmetic needs hardware."""

from pathlib import Path
import struct
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import Device, Job, RecoveryRequired
from framenpu.profile import PROFILE
import framenpu.resident_landmark as rg
from test_library import FakePower, FakeTransport


class AdminTransport(FakeTransport):
    def __init__(self):
        super().__init__()
        self.memory[rg.ARENA_START : rg.ARENA_END] = b"\xa5" * rg.SPAN
        self.requests = []
        self.close_count = 0
        self.fail_close = 0
        self.bad_cache_markers = False
        self.bad_pc = False

    def publish(self, *, direct=False):
        super().publish()

    def wait_complete(self, seq, timeout):
        words = list(struct.unpack_from("<32I", self.memory, PROFILE.arena))
        self.requests.append(words)
        op, off, size = words[2], words[21], words[22]
        if op == 6:
            self.close_count += 1
            if self.close_count == self.fail_close:
                raise RecoveryRequired("injected missing completion during arena restore")
        state = list(struct.unpack_from("<32I", self.memory, rg.STATE))
        error = 0
        if op == 0:
            state[:3] = [1, 1, rg.MAGIC]
            state[4:18] = words[4:18]
            state[25] = rg.PROGRAM
        elif state[4:8] != words[4:8] or words[8] != state[8] + 1:
            error = 1
        else:
            state[8:10] = words[8:10]
            if op == 1:
                state[25] = off + size
            elif op == 2:
                state[26] += size
                if off + size == rg.ACTIVATIONS:
                    state[0], state[3], state[19] = 2, 63, 63
            elif op == 3:
                state[0], state[19] = 3, 0
                state[28] += 1
            elif op == 6:
                state[0] = 6 if size else 5
                if size:
                    state[25] = off
            else:
                raise AssertionError("admin simulation received arithmetic")
        struct.pack_into("<32I", self.memory, rg.STATE, *state)
        meta = words + state
        cached = self.get(PROFILE.header + 40)
        meta[31] = 0x87A92010 if bool(cached) != self.bad_pc else 0xF0A92010
        struct.pack_into("<64I", self.memory, PROFILE.arena + 128, *meta)
        for worker in range(3):
            row = [0] * 16
            row[:3] = [seq, worker, error if worker == 0 else 0]
            row[7], row[8] = rg.SSR[worker], 0x87000000
            row[10], row[12] = 100, 200
            if worker == 0 and cached:
                row[14:16] = [1, int(not self.bad_cache_markers)]
            struct.pack_into("<16I", self.memory, PROFILE.status + 64 * worker, *row)


def owner_for(transport, *, cached=False, tagged=True):
    def factory(**kwargs):
        device = Device(
            _transport=transport, _power_factory=lambda: FakePower(transport.events), **kwargs
        )
        device._direct = Mock(installed=True)
        return device

    plan = {
        "records": [],
        "program": bytes(rg.PROGRAM_BYTES),
        "packed_weights": bytes(rg.WEIGHT_BYTES),
        "roots": {},
        "allocations": {},
    }
    with (
        patch.object(rg, "resident_landmark_plan", return_value=plan),
        patch.object(rg, "specifications", return_value=[]),
    ):
        return rg.ResidentLandmark(
            SimpleNamespace(),
            {
                name: Job(
                    bytes(2048), f"resident_graph_{name}_cache_v1" if tagged else name, True, True
                )
                for name in rg.JOB_KINDS.values()
            },
            cached_code=cached,
            _device_factory=factory,
            _lease_factory=Mock,
        )


class ResidentGraphLifecycleTests(unittest.TestCase):
    def test_cached_mode_requires_entry_contract_before_device_access(self):
        t = AdminTransport()
        with self.assertRaisesRegex(ValueError, "cache-v1"):
            owner_for(t, cached=True, tagged=False)
        self.assertFalse(t.events)

    def test_cached_mode_binds_identity_and_restores_all_arena_bytes(self):
        t = AdminTransport()
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        control = owner_for(t)
        with owner_for(t, cached=True) as owner:
            self.assertNotEqual(owner.identity, control.identity)
            self.assertTrue(owner.metrics["cached_code"])
            self.assertTrue(all(c["cache_cleanup"] == [1, 1] for c in owner.metrics["commands"]))
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)

    def test_missing_cache_cleanup_or_wrong_alias_retains_before_reclamation(self):
        for field in ("bad_cache_markers", "bad_pc"):
            t = AdminTransport()
            owner = owner_for(t, cached=True).__enter__()
            setattr(t, field, True)
            before = t.events.count("power_off")
            with self.subTest(field=field), self.assertRaises(RecoveryRequired):
                owner._command(3)
            owner.close()
            self.assertEqual(t.events.count("power_off"), before)
            self.assertTrue(t.poisoned)
            self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)

    def test_hardware_tools_refuse_without_opt_in_before_loading_artifacts(self):
        for name, options in (
            (
                "hardware_resident_landmark.py",
                ["--jobs", "unused", "--model", "unused", "--corpus", "unused"],
            ),
            ("hardware_resident_conversion.py", ["--probe", "unused"]),
            ("hardware_resident_gather.py", ["--probe", "unused"]),
        ):
            with self.subTest(tool=name):
                result = subprocess.run(
                    [
                        sys.executable,
                        str(Path(__file__).with_name(name)),
                        *options,
                        "--output",
                        "unused",
                    ],
                    cwd="/tmp",
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("--run-on-device and Frame root are required", result.stderr)

    def test_bounded_restore_retires_refusal_last_and_preserves_all_bytes(self):
        t = AdminTransport()
        before = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with owner_for(t) as owner:
            self.assertEqual(owner.metrics["weight_loads"], 1)
            t.memory[rg.ACTIVATIONS : rg.SCRATCH] = b"\x5a" * (rg.SCRATCH - rg.ACTIVATIONS)
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), before)
        closes = [d for d in t.requests if d[2] == 6]
        self.assertTrue(all(0 < d[22] <= 32768 for d in closes[:-1]))
        self.assertEqual(closes[-1][21:23], [0, 0])
        self.assertEqual(sum(d[22] for d in closes), rg.ARENA_END - rg.PROGRAM)
        self.assertEqual(t.events.count("claim"), len(t.requests))
        self.assertFalse(t.poisoned)

    def test_interrupted_close_keeps_record_code_and_power_and_never_retries(self):
        t = AdminTransport()
        owner = owner_for(t).__enter__()
        t.fail_close = 3
        before = t.events.count("power_off")
        with self.assertRaises(RecoveryRequired):
            owner.close()
        self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)
        self.assertTrue(t.poisoned)
        self.assertEqual(t.events.count("power_off"), before + 2)
        self.assertIn("abandon", t.events)
        self.assertEqual(t.close_count, 3)
        owner.close()
        self.assertEqual(t.close_count, 3)

    def test_program_corruption_retains_before_reclamation(self):
        t = AdminTransport()
        owner = owner_for(t).__enter__()
        t.memory[rg.PROGRAM] ^= 1
        count = len(t.requests)
        with self.assertRaises(RecoveryRequired):
            owner._command(3)
        owner.close()
        self.assertEqual(len(t.requests), count)
        self.assertTrue(t.poisoned)
        self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)

    def test_refusal_record_survives_disappeared_lease_without_new_publication(self):
        t = AdminTransport()
        t.memory[rg.CONTROL : rg.CONTROL + 8] = rg.RECORD
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with self.assertRaisesRegex(RecoveryRequired, "refusal record"):
            owner_for(t).__enter__()
        self.assertFalse(t.requests)
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)
