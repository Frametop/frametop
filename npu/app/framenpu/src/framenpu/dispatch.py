"""Opt-in, reversible CPU-request dispatcher for the exact ABI 9 firmware."""

import hashlib
from importlib.resources import files
import socket

from .errors import DeviceBusy, RecoveryRequired, UnsupportedFirmware
from .job import Job
from .profile import PROFILE
from .provenance import verify_kernel


def _firmware_original():
    p = PROFILE
    with open(p.firmware_path, "rb") as stream:
        firmware = stream.read()
    start = p.firmware_load_offset + p.dispatch_offset
    original = firmware[start : start + p.dispatch_size]
    if (
        hashlib.sha256(firmware).hexdigest() != p.firmware_sha256
        or hashlib.sha256(original).hexdigest() != p.dispatch_sha256
    ):
        raise UnsupportedFirmware("direct dispatcher requires the exact ABI 9 firmware window")
    return original


def _acquire_lease(name="\0framenpu-direct-cdsp-abi9"):
    # An abstract socket serializes cooperating installers across processes and
    # UIDs without a world-writable lock file or a stale filesystem pathname.
    lease = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        lease.bind(name)
    except OSError as exc:
        lease.close()
        raise DeviceBusy("another process owns the direct dispatcher") from exc
    return lease


class DirectDispatcher:
    def __init__(self):
        verify_kernel()
        resource = files("framenpu").joinpath("data")
        self.original = _firmware_original()
        self.code = resource.joinpath("dispatch.bin").read_bytes()
        self.patcher = Job(resource.joinpath("dispatch_patch.bin").read_bytes(), "dispatch-patch")
        if len(self.code) != PROFILE.dispatch_size:
            raise RuntimeError("invalid bundled direct dispatcher size")
        self.lease = None
        self.installed = False

    @staticmethod
    def window(transport):
        p = PROFILE
        return bytes(transport.memory[p.dispatch_offset : p.dispatch_offset + p.dispatch_size])

    def install(self, device):
        self.lease = _acquire_lease()
        t = device._transport
        if self.window(t) != self.original:
            self.abandon()
            # Never adopt a patch left by a crashed process or overwrite a
            # different modification. Ownership cannot be inferred from bytes.
            t.poisoned = True
            raise RecoveryRequired("dispatcher window is modified; establish DSP recovery first")
        try:
            self._apply(device, self.original, self.code)
            self.installed = True
        except BaseException:
            if self.window(t) != self.original:
                t.poisoned = True
            self.abandon()
            raise

    def _apply(self, device, expected, replacement):
        t = device._transport

        def validate_before_ack(result):
            window = self.window(t)
            if window not in (expected, replacement) or (
                window == replacement and any(row[2] for row in result.workers)
            ):
                raise RecoveryRequired(
                    "dispatcher write fault; do not acknowledge or reclaim the job"
                )

        try:
            device._submit(
                self.patcher,
                expected + replacement,
                _completion_validator=validate_before_ack,
            )
            if self.window(t) != replacement:
                raise RecoveryRequired("DSP dispatcher patch readback failed")
        except BaseException as exc:
            if t.poisoned or self.window(t) != expected:
                t.poisoned = True
                raise RecoveryRequired(
                    "dispatcher update did not finish cleanly; retain DSP state for deliberate recovery"
                ) from exc
            raise

    def verify(self, transport):
        if not self.installed or self.window(transport) != self.code:
            transport.poisoned = True
            raise RecoveryRequired("direct dispatcher changed while this session owned it")

    def remove(self, device):
        try:
            self.verify(device._transport)
            self._apply(device, self.code, self.original)
            self.installed = False
        finally:
            self.abandon()

    def abandon(self):
        # Releasing this host lease never restores code or resets active work.
        # A later owner must still verify the original firmware window.
        if self.lease is not None:
            self.lease.close()
            self.lease = None
