import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import (
    Device,
    Job,
    Model,
    Dense,
    ReLU,
    Clip,
    JobFailed,
    InvalidModel,
    UnsupportedOperator,
    RecoveryRequired,
)
from framenpu.profile import PROFILE
from framenpu.power import HmxPower


class Memory(bytearray):
    closed = False

    def close(self):
        self.closed = True


class FakeTransport:
    def __init__(self, *, fail=False, error=0):
        self.memory = Memory(PROFILE.size)
        self.owned = False
        self.poisoned = False
        self.fail = fail
        self.error = error
        self.events = []
        struct.pack_into("<3I", self.memory, PROFILE.control, 1, 0, 1)

    def get(self, offset):
        return struct.unpack_from("<I", self.memory, offset)[0]

    def put(self, offset, value):
        struct.pack_into("<I", self.memory, offset, value)

    def claim(self, timeout):
        self.events.append("claim")
        self.owned = True
        self.put(PROFILE.control + 8, 4)

    def release_unsubmitted(self):
        if self.owned and not self.poisoned:
            self.put(PROFILE.control + 8, 1)
            self.owned = False

    def publish(self):
        self.events.append("publish")
        self.put(PROFILE.callback, PROFILE.dsp_base + PROFILE.code)
        self.put(PROFILE.control + 4, 1)
        self.put(PROFILE.control, 3)

    def wait_complete(self, seq, timeout):
        if self.fail:
            raise RecoveryRequired("simulated missing completion")
        for i in range(3):
            struct.pack_into("<3I", self.memory, PROFILE.status + i * 64, seq, i, self.error)

    def acknowledge(self, timeout):
        self.events.append("ack")
        self.put(PROFILE.control + 4, 0)
        self.put(PROFILE.control, 1)
        self.put(PROFILE.control + 8, 1)
        self.owned = False

    def close(self):
        self.events.append("close")

    def status(self):
        return {
            "control": list(struct.unpack_from("<3I", self.memory, PROFILE.control)),
            "callback": self.get(PROFILE.callback),
        }


class FakePower:
    def __init__(self, events, *, start_error=None, close_error=None):
        self.events = events
        self.start_error = start_error
        self.close_error = close_error

    def start(self):
        self.events.append("power_on")
        if self.start_error:
            raise self.start_error

    def close(self):
        self.events.append("power_off")
        if self.close_error:
            raise self.close_error

    def abandon(self):
        self.events.append("abandon")

    def hold(self):
        self.events.append("power_hold")

    def verify(self):
        self.events.append("power_verify")
        if getattr(self, "verify_error", None):
            raise self.verify_error


class LifecycleTests(unittest.TestCase):
    def test_all_uploaded_spans_restore_before_ack(self):
        t = FakeTransport()
        spans = [
            (PROFILE.code, 4096),
            (PROFILE.header, 256),
            (PROFILE.status, 192),
            (PROFILE.backup, 8192),
            (PROFILE.arena, 132),
        ]
        for off, size in spans:
            t.memory[off : off + size] = b"\xa5" * size
        before = [bytes(t.memory[o : o + n]) for o, n in spans]
        with Device(_transport=t, _power_factory=lambda: FakePower(t.events)) as npu:
            result = npu.submit(Job(b"\0" * 4, requires_hmx=True), b"input", output_size=4)
        self.assertEqual(len(result.workers), 3)
        self.assertEqual(t.events, ["claim", "power_on", "publish", "power_off", "ack", "close"])
        self.assertEqual(before, [bytes(t.memory[o : o + n]) for o, n in spans])
        self.assertEqual(t.status(), {"control": [1, 0, 1], "callback": 0})

    def test_timeout_does_not_reclaim_inflight_resources(self):
        t = FakeTransport(fail=True)
        npu = Device(_transport=t, _power_factory=lambda: FakePower(t.events))
        with self.assertRaises(RecoveryRequired):
            npu.submit(Job(b"\x12" * 4, requires_hmx=True), b"weights")
        self.assertTrue(t.poisoned)
        self.assertEqual(t.get(PROFILE.callback), PROFILE.dsp_base + PROFILE.code)
        self.assertEqual(t.get(PROFILE.control + 4), 1)
        self.assertEqual(t.memory[PROFILE.code : PROFILE.code + 4], b"\x12" * 4)
        self.assertNotIn("power_off", t.events)
        self.assertNotIn("ack", t.events)
        self.assertIn("abandon", t.events)
        with self.assertRaises(RecoveryRequired):
            npu.submit(Job(b"\0" * 4))
        npu.close()

    def test_job_error_restores_session_and_allows_reuse(self):
        t = FakeTransport(error=3)
        npu = Device(_transport=t)
        with self.assertRaises(JobFailed):
            npu.submit(Job(b"\0" * 4))
        self.assertFalse(t.poisoned)
        self.assertEqual(t.status(), {"control": [1, 0, 1], "callback": 0})
        t.error = 0
        npu.submit(Job(b"\0" * 4))

    def test_power_refusal_releases_unsubmitted_gate(self):
        t = FakeTransport()
        npu = Device(
            _transport=t,
            _power_factory=lambda: FakePower(
                t.events, start_error=PermissionError("root required")
            ),
        )
        with self.assertRaises(PermissionError):
            npu.submit(Job(b"\0" * 4, requires_hmx=True))
        self.assertNotIn("publish", t.events)
        self.assertEqual(t.get(PROFILE.control + 8), 1)
        self.assertEqual(t.get(PROFILE.callback), 0)

    def test_output_bounds_rejected_before_claim(self):
        t = FakeTransport()
        with self.assertRaises(ValueError):
            Device(_transport=t).submit(Job(b"\0" * 4), b"abcd", output_size=PROFILE.arena_size)
        self.assertEqual(t.events, [])

    def test_completion_timeout_cannot_exceed_stock_window(self):
        with self.assertRaises(ValueError):
            Device(timeout=0.1)

    def test_add_uses_dsp_opcode_and_broadcast_shape(self):
        calls = []

        class Stub(Device):
            def submit(self, job, raw, **kw):
                calls.append((raw, kw))
                from framenpu import JobResult

                return JobResult(struct.pack("<4f", 2, 3, 4, 5), 0, ())

        with patch.object(Device, "builtin", return_value=Job(b"\0" * 4)):
            result = Stub().add([[1, 2], [3, 4]], [[1, 1]])
        self.assertEqual(calls[0][1]["opcode"], 3)
        self.assertEqual(calls[0][1]["params"], (4, 2, 0))
        self.assertEqual(result, [[2, 3], [4, 5]])


class ModelTests(unittest.TestCase):
    def test_model_round_trip_and_plan(self):
        model = Model(
            2, [Dense([[1, -1], [-1, 1]], activation="relu"), ReLU(), Clip(0, 2), Dense([[1], [1]])]
        )
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model.fnpu"
            model.save(path)
            loaded = Model.load(path)
        self.assertEqual(model.layers, loaded.layers)
        self.assertEqual(loaded.output_features, 1)
        self.assertEqual([p["engine"] for p in loaded.plan(4)], ["hmx", "dsp", "dsp", "hmx"])

    def test_checksum_tamper_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.fnpu"
            Model(1, [Dense([[1]])]).save(p)
            with zipfile.ZipFile(p) as z:
                items = {n: z.read(n) for n in z.namelist()}
            items["weights/0.f16"] = struct.pack("<e", 2)
            with zipfile.ZipFile(p, "w") as z:
                for n, b in items.items():
                    z.writestr(n, b)
            with self.assertRaises(InvalidModel):
                Model.load(p)

    def test_unsupported_operator_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.fnpu"
            with zipfile.ZipFile(p, "w") as z:
                z.writestr(
                    "model.json",
                    json.dumps(
                        {
                            "format": "framenpu-model",
                            "version": 1,
                            "input_features": 1,
                            "layers": [{"op": "Convolution"}],
                        }
                    ),
                )
            with self.assertRaises(UnsupportedOperator):
                Model.load(p)

    def test_tensor_size_checked_before_decompression(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.fnpu"
            Model(1, [Dense([[1]])]).save(p)
            with zipfile.ZipFile(p) as z:
                items = {n: z.read(n) for n in z.namelist()}
            items["weights/0.f16"] += b"\0"
            with zipfile.ZipFile(p, "w") as z:
                for n, b in items.items():
                    z.writestr(n, b)
            with self.assertRaises(InvalidModel):
                Model.load(p)

    def test_dimension_mismatch_and_nonfinite_rejected(self):
        with self.assertRaises(InvalidModel):
            Model(2, [Dense([[1]])])
        with self.assertRaises(ValueError):
            Dense([[float("nan")]])

    def test_streamed_dense_plan_bounds_per_job(self):
        model = Model(256, [Dense([[0] * 256 for _ in range(256)])])
        self.assertLess(model.plan(256)[0]["arena_bytes"], PROFILE.arena_size)


class JobTests(unittest.TestCase):
    def test_job_container_round_trip_and_corruption(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.fjob"
            job = Job(b"\0" * 4, "test", True)
            job.save(p)
            self.assertEqual(Job.load(p), job)
            raw = p.read_bytes()
            p.write_bytes(raw[:-1] + b"\x01")
            with self.assertRaises(ValueError):
                Job.load(p)

    def test_kernel_code_limit(self):
        with self.assertRaises(ValueError):
            Job(b"\0" * 4100)
        with self.assertRaises(ValueError):
            Job(b"\0")


class PowerTests(unittest.TestCase):
    @staticmethod
    def make_power(*, broken_identity=False, fail_trigger=False):
        class Simulated(HmxPower):
            def __init__(self):
                self.views = {}
                self.fail_trigger = fail_trigger
                super().__init__(mmio_factory=self.make_view)

            def _program(self):
                return tuple(range(76))

            def make_view(self, address):
                mm = Memory(4096)
                self.views[address] = mm
                if address == 0x32341000:
                    struct.pack_into("<I", mm, 0, 0x100)
                    struct.pack_into("<I", mm, 0x18, 1)
                elif address == 0x32348000:
                    for off in [0x180, 0x1A0, 0x200]:
                        struct.pack_into("<I", mm, off, 0x80000000)
                else:
                    struct.pack_into("<I", mm, 0, 0 if broken_identity else 0x30200)
                    struct.pack_into("<I", mm, 8, 0x1600214)
                return mm

            def _w(self, mm, off, value):
                if mm is self.views.get(0x32341000) and off == 0:
                    if value & 4 and self._r(mm, 4) == 1:
                        value |= 0x80000000
                    else:
                        value &= ~0x80000000
                if mm is self.views.get(0x32348000) and off == 0x180:
                    value &= ~1
                if mm is self.views.get(0x323C0000):
                    if off == 0x410 and value & 0x80000000:
                        value = 0
                    if off == 0x460 and value == 1 and not self.fail_trigger:
                        struct.pack_into("<I", mm, 0x410, 1)
                        enabled = self._r(mm, 0x464) == 2
                        cc = self.views[0x32348000]
                        struct.pack_into("<I", cc, 0x180, 0 if enabled else 0x80000000)
                        struct.pack_into("<I", cc, 0x1A0, 1 if enabled else 0x80000001)
                        struct.pack_into("<I", cc, 0x200, 0 if enabled else 0x80000000)
                return super()._w(mm, off, value)

            def _poll(self, mm, off, mask, value, timeout=0.05):
                return self._r(mm, off) & mask == value

        return Simulated()

    def test_power_on_off_restores_every_configuration_word(self):
        power = self.make_power()
        power.start()
        self.assertEqual(power._r(power.c, 0x1A0), 1)
        power.close()
        for address, mm in power.views.items():
            self.assertTrue(mm.closed)
        pll = power.views[0x32341000]
        self.assertEqual(struct.unpack_from("<I", pll, 0)[0], 0x100)
        self.assertEqual(struct.unpack_from("<I", pll, 0x10)[0], 0)
        rsc = power.views[0x323C0000]
        self.assertEqual(bytes(rsc[0x550:0x680]), bytes(304))

    def test_unknown_identity_never_writes_program(self):
        from framenpu import UnsupportedFirmware

        power = self.make_power(broken_identity=True)
        with self.assertRaises(UnsupportedFirmware):
            power.start()
        self.assertEqual(bytes(power.views[0x323C0000][0x550:0x680]), bytes(304))

    def test_incomplete_sequence_keeps_program_and_power_state(self):
        power = self.make_power(fail_trigger=True)
        with self.assertRaises(RecoveryRequired):
            power.start()
        self.assertTrue(power.abandoned)
        rsc = power.views[0x323C0000]
        self.assertNotEqual(bytes(rsc[0x550:0x680]), bytes(304))

    def test_late_completion_does_not_claim_stock_lease_survived(self):
        from framenpu.transport import Transport

        class Expired:
            profile = PROFILE
            poisoned = False

            def get(self, off):
                if off in [PROFILE.status + i * 64 for i in range(3)]:
                    return 123
                return 1

            def wait(self, fn, timeout):
                return fn()

        t = Expired()
        with self.assertRaises(RecoveryRequired):
            Transport.wait_complete(t, 123, 0.04)
        self.assertTrue(t.poisoned)


if __name__ == "__main__":
    unittest.main()
