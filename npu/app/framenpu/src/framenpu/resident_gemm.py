"""Experimental bounded resident 32xKx32 GEMM; requires the qualified proof FJOB.

Data uses fixed, cache-line-disjoint slots in the planner's verified arena.
An abstract lease and an uncached DDR ownership record survive different failure
modes: a vanished socket never authorizes adopting a marked arena. No recovery
or service restart is performed here.
"""

import hashlib
import secrets
import socket
import struct
import threading
import time

from .device import Device, _matrix, _fp32
from .errors import DeviceBusy, JobFailed, RecoveryRequired
from .job import Job
from .resident import ARENA_START, plan_resident

SPAN = 32768
CONTROL = ARENA_START + 4096
STATE = CONTROL + 128
WEIGHTS = ARENA_START + 8192
INPUT = ARENA_START + 16384
OUTPUT = ARENA_START + 24576
GUARD = ARENA_START + 28672
RECORD = b"FRGRES01"
MAGIC = 0x5247454D
SSR = (0x81840200, 0x89840200, 0x91840200)


def proof_layout():
    """Reproduce the fixed proof layout using the offline allocation planner."""
    tensors = [
        {"name": name, "kind": kind, "shape": shape, "dtype": dtype, "layout": layout}
        for name, kind, shape, dtype, layout in (
            ("weights", "weight", [4, 32, 32], "f16", "hmx_packed"),
            ("input", "input", [4, 32, 32], "f16", "tile_major"),
            ("output", "activation", [32, 32], "f32", "matrix"),
        )
    ]
    plan = plan_resident(
        {
            "format": "framenpu-resident-plan",
            "version": 1,
            "name": "resident-gemm-proof",
            "tensors": tensors,
            "steps": [
                {
                    "name": "gemm",
                    "inputs": ["input", "weights"],
                    "outputs": ["output"],
                    "scratch_bytes": 4096,
                }
            ],
            "outputs": ["output"],
        }
    )
    offsets = {entry["name"]: entry["offset"] for entry in plan["allocations"]}
    if (
        offsets
        != {
            "@code": ARENA_START,
            "@control": CONTROL,
            "weights": WEIGHTS,
            "input": INPUT,
            "output": OUTPUT,
            "@scratch:gemm": GUARD,
        }
        or plan["required_bytes"] != SPAN
    ):
        raise ValueError("proof layout no longer matches the native fixed descriptor bounds")
    return plan


def _lease():
    lease = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        lease.bind("\0framenpu-resident-cdsp-abi9")
    except OSError as exc:
        lease.close()
        raise DeviceBusy("another process owns the resident arena") from exc
    return lease


def packed_weights(raw):
    """Independent byte permutation for the native per-vector vshuff."""
    words = struct.unpack(f"<{len(raw) // 2}H", raw)
    shuffled = [
        words[base + col + row * 32]
        for base in range(0, len(words), 64)
        for col in range(32)
        for row in range(2)
    ]
    return struct.pack(f"<{len(shuffled)}H", *shuffled)


class ResidentGemm:
    """One exclusive session with weights loaded once, bounded work and teardown.

    K must be 32, 64 or 128; M=N=32. Power and the existing 8 KiB VTCM scratch
    remain per-request. A confirmed trap invalidates this session; close it and
    open a fresh session. Timeout or invalid completion retains resources.
    """

    def __init__(
        self,
        job,
        weights,
        *,
        _device_factory=Device,
        _lease_factory=_lease,
        _tag=0,
        _bias=None,
        _clip=False,
    ):
        if not isinstance(job, Job) or not job.requires_hmx or not job.supports_direct:
            raise ValueError("resident proof requires its HMX/direct FJOB")
        weights = _matrix(weights)
        self.k = len(weights)
        if self.k not in (32, 64, 128) or len(weights[0]) != 32:
            raise ValueError("resident proof weights must be 32/64/128 by 32")
        self.layout = proof_layout()
        if type(_tag) is not int or _tag not in (0, 1, 2):
            raise ValueError("unknown resident proof image tag")
        self.tag = _tag
        if type(_clip) is not bool or (_tag == 2) != (_bias is not None) or _clip and _tag != 2:
            raise ValueError("bias/clip require the convolution proof image")
        self.bias_data = b"" if _bias is None else _fp32(_bias)
        if _tag == 2 and len(self.bias_data) != 128:
            raise ValueError("convolution proof requires 32 padded FP32 biases")
        self.clip = _clip
        self.job = job
        self.raw_weights = struct.pack(f"<{self.k * 32}e", *(v for row in weights for v in row))
        self.identity = hashlib.sha256(
            job.code
            + struct.pack("<I", self.k)
            + self.raw_weights
            + self.bias_data
            + (struct.pack("<I", int(self.clip)) if self.tag == 2 else b"")
        ).digest()
        self.nonce = secrets.token_bytes(16)
        if not any(self.nonce):
            raise ValueError("zero session nonce")
        self._device_factory, self._lease_factory = _device_factory, _lease_factory
        self.device = self.lease = None
        self.saved = None
        self.sequence = 0
        self.invalid = self.closed = self.owned = False
        self.weight_digest = None
        self._lock = threading.RLock()
        self.metrics = {
            "commands": [],
            "weight_uploads": 0,
            "weight_upload_bytes": 0,
            "input_uploads": 0,
            "input_upload_bytes": 0,
        }

    def __enter__(self):
        with self._lock:
            return self._open()

    def _open(self):
        if self.device is not None or self.closed:
            raise RuntimeError("resident session cannot be reopened")
        started = time.monotonic_ns()
        self.lease = self._lease_factory()
        try:
            self.device = self._device_factory(dispatch="direct")
            self.device.__enter__()
            t = self.device._transport
            if bytes(t.memory[CONTROL : CONTROL + 8]) == RECORD:
                raise RecoveryRequired(
                    "resident ownership record remains; establish idle/reset and cache recovery before reuse"
                )
            self._command(0)
            self._command(1, upload=self.raw_weights)
            self.metrics["open_ms"] = (time.monotonic_ns() - started) / 1e6
            return self
        except BaseException:
            self.close()
            raise

    def _descriptor(self, op, *, sequence=None, nonce=None, identity=None):
        seq = self.sequence + 1 if sequence is None else sequence
        if not 0 < seq < 1 << 64:
            raise ValueError("resident sequence exhausted; close before wrap")
        if op != 4 and seq == (1 << 64) - 1:
            raise ValueError("close the resident session before sequence wrap")
        words = [MAGIC, 1, op, self.k // 32]
        words += list(struct.unpack("<4I", self.nonce if nonce is None else nonce))
        words += [seq & 0xFFFFFFFF, seq >> 32]
        words += list(struct.unpack("<8I", self.identity if identity is None else identity))
        words += [WEIGHTS, self.k * 64, INPUT, self.k * 64, OUTPUT, 4096]
        words += [0] * 8
        words[24] = self.tag
        words[25] = int(self.clip)
        return words

    def _check_owner(self, transport):
        if bytes(transport.memory[CONTROL : CONTROL + 56]) != RECORD + self.nonce + self.identity:
            raise RecoveryRequired("resident arena ownership changed")

    def _command(
        self, op, *, upload=None, expected=None, descriptor=None, reject=False, check=None
    ):
        with self._lock:
            return self._execute_command(
                op,
                upload=upload,
                expected=expected,
                descriptor=descriptor,
                reject=reject,
                check=check,
            )

    def _execute_command(
        self, op, *, upload=None, expected=None, descriptor=None, reject=False, check=None
    ):
        if check is not None and not callable(check):
            raise ValueError("resident completion check must be callable")
        if (
            op not in range(5)
            or upload is not None
            and (op not in (1, 2, 3) or len(upload) != self.k * 64)
        ):
            raise ValueError("invalid resident command/upload bounds")
        if expected is not None and len(expected) != 4096:
            raise ValueError("expected resident output must contain 1024 FP32 values")
        if self.device is None or self.closed or not self.owned and op != 0:
            raise RuntimeError("resident session is not open")
        if self.device._transport.poisoned:
            raise RecoveryRequired("resident session requires deliberate recovery")
        if self.invalid and op != 4:
            raise RuntimeError("confirmed fault invalidated resident tensors; close the session")
        words = self._descriptor(op) if descriptor is None else descriptor
        phase, captured = {}, []
        total = time.monotonic_ns()

        def prepare(t):
            if not self.owned:
                if bytes(t.memory[CONTROL : CONTROL + 8]) == RECORD:
                    raise RecoveryRequired("resident ownership appeared during initialization")
                self.saved = bytes(t.memory[ARENA_START : ARENA_START + SPAN])
                # Initialize under a real request's gate, without a separate
                # claim-only window that can trigger stock processing.
                t.memory[CONTROL : CONTROL + 8] = RECORD
                self.owned = True
                t.memory[CONTROL + 8 : CONTROL + 128] = (self.nonce + self.identity).ljust(
                    120, b"\0"
                )
                t.memory[STATE : STATE + 128] = bytes(128)
                t.memory[ARENA_START : ARENA_START + 4096] = self.job.code.ljust(4096, b"\0")
            self._check_owner(t)
            if (
                self.bias_data
                and self.weight_digest is not None
                and bytes(t.memory[CONTROL + 256 : CONTROL + 384]) != self.bias_data
            ):
                raise RecoveryRequired("resident bias changed before submission")
            if bytes(t.memory[ARENA_START : ARENA_START + 4096]) != self.job.code.ljust(
                4096, b"\0"
            ):
                raise RecoveryRequired("resident stored code changed")
            if upload is not None:
                start = time.monotonic_ns()
                offset = WEIGHTS if op == 1 else INPUT
                t.memory[offset : offset + len(upload)] = upload
                if op == 1 and self.bias_data:
                    t.memory[CONTROL + 256 : CONTROL + 384] = self.bias_data
                phase["resident_upload_ms"] = (time.monotonic_ns() - start) / 1e6
                key = "weight" if op == 1 else "input"
                self.metrics[f"{key}_uploads"] += 1
                self.metrics[f"{key}_upload_bytes"] += len(upload)

        def validate(result):
            try:
                t = self.device._transport
                self._check_owner(t)
                if len(result.output) != 256 or len(result.workers) != 3:
                    raise ValueError("incomplete resident completion")
                for worker, row in enumerate(result.workers):
                    error = (1 if reject else 2 if op == 3 else 0) if worker == 0 else 0
                    if (
                        len(row) != 16
                        or not row[0]
                        or row[0] != result.workers[0][0]
                        or row[1] != worker
                        or row[2] != error
                        or row[7] != SSR[worker]
                    ):
                        raise ValueError("resident worker identity/error/SSR mismatch")
                row = result.workers[0]
                if row[8] != 0x87000000 or tuple(row[14:16]) != (3, 3):
                    raise ValueError("resident cache cleanup/EVB not confirmed")
                if op == 3 and not reject:
                    if (
                        row[3] & 255 != 37
                        or row[3] & ((1 << 17) | (1 << 26)) != ((1 << 17) | (1 << 26))
                        or not 0x87A92000 <= row[4] < 0x87A92000 + len(self.job.code)
                    ):
                        raise ValueError("resident guarded trap differs")
                elif any(row[3:6]):
                    raise ValueError("unexpected resident fault")
                meta = list(struct.unpack("<64I", result.output))
                wanted = words[:31]
                wanted[30] = int(op == 3 and not reject)
                if meta[:31] != wanted or not 0x87A92000 <= meta[31] < 0x87A92000 + len(
                    self.job.code
                ):
                    raise ValueError("resident command identity or cached PC differs")
                if bytes(t.memory[GUARD : ARENA_START + SPAN]) != self.saved[GUARD - ARENA_START :]:
                    raise ValueError("resident boundary guard changed")
                weight = bytes(t.memory[WEIGHTS : WEIGHTS + 8192])
                if op == 1 and not reject:
                    wanted_weight = (
                        packed_weights(self.raw_weights)
                        + self.saved[
                            WEIGHTS - ARENA_START + len(self.raw_weights) : INPUT - ARENA_START
                        ]
                    )
                    if weight != wanted_weight:
                        raise ValueError("resident weight prepacking differs")
                    self.weight_digest = hashlib.sha256(weight).hexdigest()
                elif (
                    self.weight_digest is not None
                    and hashlib.sha256(weight).hexdigest() != self.weight_digest
                ):
                    raise ValueError("resident weights changed after loading")
                output = bytes(t.memory[OUTPUT : OUTPUT + 4096])
                if (
                    op != 0
                    and self.bias_data
                    and bytes(t.memory[CONTROL + 256 : CONTROL + 384]) != self.bias_data
                ):
                    raise ValueError("resident bias changed after loading")
                if expected is not None and output != expected:
                    raise ValueError("resident numerical output differs")
                if check is not None:
                    check(output)
                if (
                    op == 3
                    and not reject
                    and output[:128] != struct.pack("<32I", *([0xDECAFBAD] * 32))
                ):
                    raise ValueError("faulted cached write was not reconciled")
                state = struct.unpack_from("<18I", t.memory, STATE)
                if op != 0:
                    expected_sequence = self.sequence if reject else words[8] | words[9] << 32
                    expected_state = 2 if op == 3 and not reject else 0 if op == 4 else 1
                    if state[0] != expected_state or state[8] | state[9] << 32 != expected_sequence:
                        raise ValueError("resident native state or sequence differs")
                    if (
                        state[3] != self.k // 32
                        or state[4:8] != struct.unpack("<4I", self.nonce)
                        or state[10:18] != struct.unpack("<8I", self.identity)
                    ):
                        raise ValueError("resident native generation/content identity differs")
                begin = row[10] | row[11] << 32
                end = row[12] | row[13] << 32

                def tick(i):
                    return meta[i] | meta[i + 1] << 32

                if not begin <= tick(32) <= end or end - begin >= 768000:
                    raise ValueError("resident timing exceeds the bounded wrapper interval")
                finish = tick(42) or tick(44)
                if not reject and not tick(32) <= finish <= end:
                    raise ValueError("resident cleanup timestamp differs")
                if (
                    any(meta[46:])
                    or any(meta[34:42])
                    and sum(tick(i) for i in (34, 36, 38, 40)) > end - begin
                ):
                    raise ValueError("resident phase timestamps differ")
                captured.append(
                    {
                        "output": output,
                        "dsp": {
                            "wrapper_ticks": end - begin,
                            "publication_ticks": tick(32) - begin,
                            "operand_copy_ticks": tick(34),
                            "packing_ticks": tick(36),
                            "hmx_ticks": tick(38),
                            "output_ticks": tick(40),
                            "cleanup_ticks": end - finish if finish else None,
                        },
                        "fault_registers": list(row[3:6]),
                        "publish_to_completion_ms": result.elapsed_ms,
                    }
                )
                if getattr(self, "_tearing_down", False):
                    # Restore under the completed request's gate, after every
                    # worker and cache cleanup has returned. Retain the host-only
                    # refusal prefix until dispatcher removal also succeeds.
                    t.memory[ARENA_START:CONTROL] = self.saved[:4096]
                    t.memory[CONTROL + 128 : ARENA_START + SPAN] = self.saved[4096 + 128 :]
                    if (
                        bytes(t.memory[ARENA_START:CONTROL]) != self.saved[:4096]
                        or bytes(t.memory[CONTROL + 128 : ARENA_START + SPAN])
                        != self.saved[4096 + 128 :]
                    ):
                        raise ValueError("resident teardown body readback differs")
            except BaseException as exc:
                raise RecoveryRequired(
                    "resident completion invalid; retain code, arena and power"
                ) from exc

        try:
            self.device._submit(
                self.job,
                struct.pack("<32I", *words),
                output_size=256,
                opcode=11,
                use_direct=True,
                _prepare=prepare,
                _phase_times=phase,
                _completion_validator=validate,
            )
            if op == 3 or reject:
                raise RecoveryRequired("expected resident rejection/fault did not fail")
        except JobFailed:
            if not (op == 3 or reject) or len(captured) != 1:
                raise
        except RecoveryRequired:
            self.device._transport.poisoned = True
            raise
        if not reject:
            self.sequence = words[8] | words[9] << 32
            if op == 3:
                self.invalid = True
        record = captured[0]
        output = record.pop("output")
        record.update(
            op=op,
            rejected=reject,
            sequence=self.sequence,
            host_phase_ms=phase,
            command_ms=(time.monotonic_ns() - total) / 1e6,
        )
        self.metrics["commands"].append(record)
        return output

    def run(self, inputs, *, _expected=None, _fault=False, _check=None):
        with self._lock:
            return self._run(inputs, _expected=_expected, _fault=_fault, _check=_check)

    def _run(self, inputs, *, _expected=None, _fault=False, _check=None):
        if self.invalid or self.closed:
            raise RuntimeError("resident session is invalid or closed")
        start = time.monotonic_ns()
        inputs = _matrix(inputs)
        if len(inputs) != 32 or len(inputs[0]) != self.k:
            raise ValueError("resident inputs must be 32 by K")
        raw = struct.pack(
            f"<{32 * self.k}e",
            *(
                inputs[r][tile + c]
                for tile in range(0, self.k, 32)
                for r in range(32)
                for c in range(32)
            ),
        )
        encoded = (time.monotonic_ns() - start) / 1e6
        output = self._command(3 if _fault else 2, upload=raw, expected=_expected, check=_check)
        self.metrics["commands"][-1]["host_input_encoding_ms"] = encoded
        if _fault:
            raise JobFailed(
                "confirmed resident trap; mutable tensors invalidated, close this session"
            )
        values = struct.unpack("<1024f", output)
        return [list(values[r * 32 : (r + 1) * 32]) for r in range(32)]

    def __exit__(self, *exc):
        self.close()

    def close(self):
        with self._lock:
            self._close()

    def _close(self):
        if self.closed:
            return
        started = time.monotonic_ns()
        try:
            if self.device is not None and self.owned:
                t = self.device._transport
                if not t.poisoned:
                    # A failed open may not have loaded weights yet. SYNC is
                    # sufficient to reconcile caches in that case.
                    self._tearing_down = True
                    self._command(4 if self.weight_digest is not None else 0)
                    if self.device._direct is not None:
                        self.device._direct.remove(self.device)
                        self.device._direct = None
                    self._check_owner(t)
                    # Prefix is host-only and uncached. Retire it under the
                    # arena lease after validated return and patch removal.
                    t.memory[CONTROL : CONTROL + 128] = self.saved[4096 : 4096 + 128]
                    if bytes(t.memory[ARENA_START : ARENA_START + SPAN]) != self.saved:
                        raise RecoveryRequired("resident teardown readback differs")
        except BaseException:
            if self.device is not None and self.owned:
                self.device._transport.poisoned = True
                # This host-only record is outside every DSP data/code span.
                # Reestablish refusal even if teardown failed during retirement.
                self.device._transport.memory[CONTROL : CONTROL + 24] = RECORD + self.nonce
            raise
        finally:
            try:
                if self.device is not None:
                    self.device.close()
            finally:
                if self.lease is not None:
                    self.lease.close()
                self.closed = True
                self.metrics["close_ms"] = (time.monotonic_ns() - started) / 1e6
