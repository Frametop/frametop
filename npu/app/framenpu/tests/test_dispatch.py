"""Host lifecycle simulation; these tests do not execute Hexagon instructions."""

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
from dataclasses import replace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import (
    Device,
    DeviceBusy,
    Job,
    JobFailed,
    JobResult,
    RecoveryRequired,
    UnsupportedFirmware,
)
from framenpu.dispatch import DirectDispatcher, _acquire_lease, _firmware_original
from framenpu.profile import PROFILE
from test_library import FakeTransport


ORIGINAL = b"\xa5" * PROFILE.dispatch_size


class DispatchTransport(FakeTransport):
    def __init__(self, *, fail_at=0, patch_error_at=0, partial=False):
        super().__init__()
        self.memory[PROFILE.dispatch_offset : PROFILE.dispatch_offset + len(ORIGINAL)] = ORIGINAL
        self.fail_at = fail_at
        self.patch_error_at = patch_error_at
        self.partial = partial
        self.submissions = []
        self.submitted_flags = []

    def publish(self, *, direct=False):
        super().publish()
        self.put(PROFILE.callback, PROFILE.dsp_base + PROFILE.code + int(direct))
        self.submissions.append(direct)
        self.submitted_flags.append(self.get(PROFILE.header + 12))

    def wait_complete(self, seq, timeout):
        number = len(self.submissions)
        if number == self.fail_at:
            raise RecoveryRequired("simulated incomplete dispatch")
        size = self.get(PROFILE.header + 24)
        if not self.submissions[-1] and size == 192:
            off = PROFILE.arena
            expected = bytes(self.memory[off : off + 96])
            replacement = bytes(self.memory[off + 96 : off + 192])
            if DirectDispatcher.window(self) != expected:
                raise AssertionError("patcher expected bytes do not match")
            if number == self.patch_error_at:
                if self.partial:
                    self.memory[PROFILE.dispatch_offset] ^= 1
                self.error = 1
            else:
                self.memory[PROFILE.dispatch_offset : PROFILE.dispatch_offset + 96] = replacement
        super().wait_complete(seq, timeout)
        self.put(PROFILE.callback, PROFILE.dsp_base + PROFILE.code)


class DirectLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.lease = Mock()
        self.original = patch("framenpu.dispatch._firmware_original", return_value=ORIGINAL)
        self.lock = patch("framenpu.dispatch._acquire_lease", return_value=self.lease)
        self.original.start()
        self.lock.start()
        self.addCleanup(self.original.stop)
        self.addCleanup(self.lock.stop)
        self.job = Job(b"\0" * 4, supports_direct=True)

    def test_install_once_run_repeatedly_and_restore_on_close(self):
        t = DispatchTransport()
        with Device(dispatch="direct", _transport=t) as device:
            device.submit(self.job)
            installed = DirectDispatcher.window(t)
            self.assertNotEqual(installed, ORIGINAL)
            device.submit(self.job)
            self.assertEqual(t.submissions, [False, True, True])
        self.assertEqual(t.submissions, [False, True, True, False])
        self.assertEqual(t.submitted_flags, [0, 2, 2, 0])
        self.assertEqual(DirectDispatcher.window(t), ORIGINAL)
        self.assertEqual(t.status(), {"control": [1, 0, 1], "callback": 0})
        self.lease.close.assert_called_once()

    def test_preflight_rejects_legacy_job_and_oversized_arena_before_install(self):
        t = DispatchTransport()
        with Device(dispatch="direct", _transport=t) as device:
            with self.assertRaisesRegex(ValueError, "rebuild"):
                device.submit(Job(b"\0" * 4))
            with self.assertRaises(ValueError):
                device.submit(self.job, output_size=PROFILE.arena_size + 1)
            self.assertEqual(t.events, [])
            self.assertEqual(DirectDispatcher.window(t), ORIGINAL)

    def test_timed_out_direct_job_is_not_overwritten_by_removal(self):
        t = DispatchTransport(fail_at=2)
        device = Device(dispatch="direct", _transport=t)
        with self.assertRaises(RecoveryRequired):
            device.submit(self.job)
        installed = DirectDispatcher.window(t)
        device.close()
        self.assertTrue(t.poisoned)
        self.assertNotEqual(installed, ORIGINAL)
        self.assertEqual(DirectDispatcher.window(t), installed)
        self.assertEqual(t.submissions, [False, True])
        self.assertEqual(t.get(PROFILE.callback), PROFILE.dsp_base + PROFILE.code + 1)

    def test_modified_firmware_is_never_adopted_or_overwritten(self):
        t = DispatchTransport()
        t.memory[PROFILE.dispatch_offset] ^= 1
        before = DirectDispatcher.window(t)
        with Device(dispatch="direct", _transport=t) as device:
            with self.assertRaises(RecoveryRequired):
                device.submit(self.job)
        self.assertTrue(t.poisoned)
        self.assertEqual(t.submissions, [])
        self.assertEqual(DirectDispatcher.window(t), before)

    def test_clean_patch_refusal_releases_lease(self):
        t = DispatchTransport(patch_error_at=1)
        with Device(dispatch="direct", _transport=t) as device:
            with self.assertRaises(JobFailed):
                device.submit(self.job)
            self.assertFalse(t.poisoned)
            self.assertEqual(DirectDispatcher.window(t), ORIGINAL)
            self.assertIsNone(device._direct)
        self.lease.close.assert_called_once()

    def test_partial_patch_failure_requires_recovery(self):
        t = DispatchTransport(patch_error_at=1, partial=True)
        device = Device(dispatch="direct", _transport=t)
        with self.assertRaises(RecoveryRequired):
            device.submit(self.job)
        self.assertTrue(t.poisoned)
        self.assertNotEqual(DirectDispatcher.window(t), ORIGINAL)
        self.assertNotIn("ack", t.events)
        device.close()
        self.assertEqual(t.submissions, [False])

    def test_corrupted_installed_patch_prevents_tagged_publish(self):
        t = DispatchTransport()
        device = Device(dispatch="direct", _transport=t)
        device.submit(self.job)
        t.memory[PROFILE.dispatch_offset] ^= 1
        with self.assertRaises(RecoveryRequired):
            device.submit(self.job)
        self.assertEqual(t.submissions, [False, True])
        device.close()

    def test_failed_removal_preserves_unknown_code_and_reports_recovery(self):
        t = DispatchTransport(patch_error_at=3, partial=True)
        device = Device(dispatch="direct", _transport=t)
        device.submit(self.job)
        with self.assertRaises(RecoveryRequired):
            device.close()
        self.assertTrue(t.poisoned)
        self.assertNotEqual(DirectDispatcher.window(t), ORIGINAL)
        self.assertEqual(t.submissions, [False, True, False])
        self.assertTrue(device._closed)


class FirmwareWindowTests(unittest.TestCase):
    def test_worker_returns_before_master_completion_are_not_a_lease_expiry(self):
        from framenpu.transport import Transport

        class Joining:
            profile = PROFILE
            poisoned = False
            state = 2

            def get(self, offset):
                if offset in (PROFILE.status + i * 64 for i in range(3)):
                    return 123
                return self.state if offset == PROFILE.control else 4

            def wait(self, predicate, timeout):
                self.initially_complete = predicate()
                self.state = 3
                return predicate()

        transport = Joining()
        Transport.wait_complete(transport, 123, 0.04)
        self.assertFalse(transport.initially_complete)
        self.assertFalse(transport.poisoned)

    @unittest.skipUnless(sys.platform == "linux", "Linux abstract socket ownership")
    def test_installer_lease_excludes_a_second_owner_and_can_be_released(self):
        name = "\0framenpu-test-" + uuid.uuid4().hex
        lease = _acquire_lease(name)
        try:
            with self.assertRaises(DeviceBusy):
                _acquire_lease(name)
        finally:
            lease.close()
        _acquire_lease(name).close()

    def test_raw_kernel_timer_wraps_at_64_bits_and_old_status_has_no_timer(self):
        row = [0] * 16
        row[10:14] = [0xFFFFFFFE, 0xFFFFFFFF, 3, 0]
        self.assertEqual(JobResult(b"", 0, (row,)).kernel_reference_ticks, 5)
        self.assertIsNone(JobResult(b"", 0, ((0,) * 16,)).kernel_reference_ticks)

    def test_installed_firmware_supplies_original_bytes_and_both_hashes_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture"
            firmware = b"header" + ORIGINAL + b"tail"
            path.write_bytes(firmware)
            profile = replace(
                PROFILE,
                firmware_path=str(path),
                firmware_load_offset=6,
                dispatch_offset=0,
                firmware_sha256=hashlib.sha256(firmware).hexdigest(),
                dispatch_sha256=hashlib.sha256(ORIGINAL).hexdigest(),
            )
            with patch("framenpu.dispatch.PROFILE", profile):
                self.assertEqual(_firmware_original(), ORIGINAL)
                path.write_bytes(firmware[:-1] + b"x")
                with self.assertRaises(UnsupportedFirmware):
                    _firmware_original()
            path.write_bytes(firmware)
            with patch("framenpu.dispatch.PROFILE", replace(profile, dispatch_sha256="0" * 64)):
                with self.assertRaises(UnsupportedFirmware):
                    _firmware_original()

    def test_transport_only_tags_explicit_direct_requests(self):
        from framenpu.transport import Transport

        t = FakeTransport()
        t.profile = PROFILE
        t.sock = Mock()
        Transport.publish(t)
        self.assertEqual(t.get(PROFILE.callback), PROFILE.dsp_base + PROFILE.code)
        Transport.publish(t, direct=True)
        self.assertEqual(t.get(PROFILE.callback), PROFILE.dsp_base + PROFILE.code + 1)
        t.sock.sendall.assert_called_with(b"client_notification\0")


if __name__ == "__main__":
    unittest.main()
