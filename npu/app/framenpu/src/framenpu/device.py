"""Synchronous custom jobs and HMX operations. Importing opens no device."""

import hashlib
from importlib.resources import files
import math
import struct
import threading
import time
from .errors import JobFailed, RecoveryRequired
from .job import Job, JobResult
from .power import HmxPower
from .profile import PROFILE, JOB_MAGIC, JOB_ABI, HMX, DIRECT
from .transport import Transport
from .dispatch import DirectDispatcher


def _matrix(value, *, fp16=True):
    rows = [list(row) for row in value]
    if not rows or not rows[0] or any(len(row) != len(rows[0]) for row in rows):
        raise ValueError("matrix must be nonempty and rectangular")
    if len(rows) > 256 or len(rows[0]) > 256:
        raise ValueError("version 0.1 supports each matrix dimension up to 256")
    if any(
        not math.isfinite(float(x)) or abs(float(x)) > (65504 if fp16 else 3.402823466e38)
        for row in rows
        for x in row
    ):
        raise ValueError("matrix elements must be finite and representable in FP16")
    return rows


def _fp32(values):
    result = [float(v) for v in values]
    if any(not math.isfinite(v) or abs(v) > 3.402823466e38 for v in result):
        raise ValueError("values must be finite FP32 numbers")
    return struct.pack("<%df" % len(result), *result)


class Device:
    def __init__(
        self,
        *,
        timeout=0.04,
        claim_timeout=0.5,
        dispatch="benchmark",
        _transport=None,
        _power_factory=HmxPower,
    ):
        if dispatch not in ("benchmark", "direct"):
            raise ValueError("dispatch must be 'benchmark' or 'direct'")
        if not math.isfinite(timeout) or not 0 < timeout <= 0.04:
            raise ValueError(
                "timeout must be positive and at most 0.04 seconds (stock completion window)"
            )
        if not math.isfinite(claim_timeout) or not 0 < claim_timeout <= 30:
            raise ValueError(
                "claim_timeout must be positive and at most 0.04 seconds (stock completion window)"
            )
        self.timeout = timeout
        self.claim_timeout = claim_timeout
        self.dispatch = dispatch
        self._direct = None
        self._transport = _transport
        self._power_factory = _power_factory
        self._lock = threading.RLock()
        self._sequence = time.monotonic_ns() & 0xFFFFFFFF or 1
        self._closed = False

    def __enter__(self):
        with self._lock:
            self._connect()
        return self

    def __exit__(self, *exc):
        self.close()

    def _connect(self):
        if self._closed:
            raise RuntimeError("device is closed")
        if self._transport is None:
            self._transport = Transport()
        if self._transport.poisoned:
            raise RecoveryRequired("device session is poisoned")
        return self._transport

    def close(self):
        with self._lock:
            if self._closed:
                return
            try:
                if self._direct is not None:
                    if self._direct.installed and not self._transport.poisoned:
                        self._direct.remove(self)
                    else:
                        self._direct.abandon()
            finally:
                if self._transport:
                    self._transport.close()
                self._closed = True

    def status(self):
        with self._lock:
            return self._connect().status()

    @staticmethod
    def builtin(*, hmx):
        resource = files("framenpu").joinpath("data")
        import json

        manifest = json.loads(resource.joinpath("runtime.json").read_text())
        code = resource.joinpath("runtime.bin").read_bytes()
        if (
            manifest.get("abi") != JOB_ABI
            or manifest.get("target") != "hexagonv75"
            or manifest.get("bytes") != len(code)
            or hashlib.sha256(code).hexdigest() != manifest.get("sha256")
        ):
            raise RuntimeError("packaged DSP kernel checksum mismatch; rebuild kernels")
        return Job(code, "framenpu-runtime", hmx, manifest.get("supports_direct", False))

    def submit(self, job, input_data=b"", *, output_size=0, opcode=0, params=()):
        """Run one trusted FJOB/C kernel using ABI-relative arena offsets."""
        return self._submit(
            job,
            input_data,
            output_size=output_size,
            opcode=opcode,
            params=params,
            use_direct=self.dispatch == "direct",
        )

    def _submit(
        self,
        job,
        input_data=b"",
        *,
        output_size=0,
        opcode=0,
        params=(),
        use_direct=False,
        _completion_validator=None,
        _prepare=None,
        _phase_times=None,
        _held_power=None,
        _expect_s=0.0,
        _while_waiting=None,
    ):
        if not isinstance(job, Job):
            raise TypeError("job must be a Job")
        if _prepare is not None and not callable(_prepare):
            raise TypeError("private prepare hook must be callable")
        if _while_waiting is not None and not callable(_while_waiting):
            raise TypeError("private wait hook must be callable")
        if _phase_times is not None and type(_phase_times) is not dict:
            raise TypeError("private phase timing output must be a dict")
        if use_direct and not job.supports_direct:
            raise ValueError("job lacks the direct master entry; rebuild it with the current SDK")
        input_data = bytes(input_data)
        if type(output_size) is not int or output_size < 0:
            raise ValueError("output_size must be a nonnegative integer")
        if type(opcode) is not int or not 0 <= opcode <= 0xFFFFFFFF:
            raise ValueError("invalid opcode")
        params = tuple(params)
        if len(params) > 6 or any(type(v) is not int or not 0 <= v <= 0xFFFFFFFF for v in params):
            raise ValueError("params must be at most six unsigned 32-bit words")
        p = PROFILE
        output_offset = (len(input_data) + 127) & ~127
        used = output_offset + output_size
        if used > p.arena_size:
            raise ValueError("input and output exceed the 128 KiB job arena")
        with self._lock:
            t = self._connect()
            if use_direct and self._direct is None:
                candidate = DirectDispatcher()
                self._direct = candidate
                try:
                    candidate.install(self)
                except BaseException:
                    if not t.poisoned:
                        self._direct = None
                    raise
            phase = time.monotonic_ns()

            def mark(name):
                nonlocal phase
                now = time.monotonic_ns()
                if _phase_times is not None:
                    _phase_times[name] = (now - phase) / 1e6
                phase = now

            t.claim(self.claim_timeout)
            mark("claim_ms")
            power = None
            backup = []
            waiting_error = None
            published = False
            returned = False
            try:
                if use_direct:
                    self._direct.verify(t)
                spans = [(p.code, p.code_size), (p.header, 256), (p.status, 192), (p.arena, used)]
                if job.requires_hmx:
                    spans.append((p.backup, 8192))
                backup = [(off, bytes(t.memory[off : off + length])) for off, length in spans]
                mark("backup_ms")
                if _prepare is not None:
                    _prepare(t)
                mark("prepare_ms")
                self._sequence = (self._sequence + 1) & 0xFFFFFFFF or 1
                seq = self._sequence
                words = [
                    seq,
                    JOB_MAGIC,
                    JOB_ABI,
                    (HMX if job.requires_hmx else 0) | (DIRECT if use_direct else 0),
                    used,
                    0,
                    len(input_data),
                    output_offset,
                    output_size,
                    opcode,
                    *params,
                    *([0] * (6 - len(params))),
                ]
                t.memory[p.code : p.code + p.code_size] = job.code.ljust(p.code_size, b"\0")
                t.memory[p.header : p.header + 256] = bytes(256)
                struct.pack_into("<16I", t.memory, p.header, *words)
                t.memory[p.status : p.status + 192] = bytes(192)
                t.memory[p.arena : p.arena + used] = input_data.ljust(used, b"\0")
                mark("job_upload_ms")
                if job.requires_hmx:
                    if _held_power is not None:
                        # Session-held power: read-only proof, under the gate,
                        # that the configuration is still the one we set.
                        _held_power.verify()
                    else:
                        power = self._power_factory()
                        power.start()
                mark("power_start_ms")
                started = time.monotonic()
                # Mark before publishing: an interrupted send may already have woken the DSP.
                published = True
                if use_direct:
                    t.publish(direct=True)
                else:
                    t.publish()
                if _while_waiting is not None:
                    # Host-only work of the caller (no device access) in place of part of the
                    # expected-time sleep; the caller sizes it to fit. Its failure is raised
                    # only after this job has completed and been acknowledged.
                    try:
                        _while_waiting()
                    except BaseException as exc:
                        waiting_error = exc
                if 0 < _expect_s < self.timeout:
                    # The caller's expected DSP time: sleep through it instead of polling.
                    left = _expect_s - (time.monotonic() - started)
                    if left > 0:
                        time.sleep(left)
                t.wait_complete(seq, self.timeout)
                mark("publish_wait_ms")
                returned = True
                statuses = tuple(
                    struct.unpack_from("<16I", t.memory, p.status + 64 * i) for i in range(3)
                )
                output = bytes(t.memory[p.arena + output_offset : p.arena + used])
                result = JobResult(output, (time.monotonic() - started) * 1000, statuses)
                if _completion_validator is not None:
                    _completion_validator(result)
                mark("read_validate_ms")
                t.put(p.callback, 0)
                if power is not None:
                    power.close()
                    power = None
                mark("power_close_ms")
                for off, saved in backup:
                    t.memory[off : off + len(saved)] = saved
                backup = []
                mark("restore_ms")
                t.acknowledge(self.timeout)
                mark("ack_ms")
                failed = [(row[1], row[2], row[3:6]) for row in statuses if row[2]]
                if failed:
                    raise JobFailed(
                        "DSP kernel failed (worker, error, fault registers): %r" % failed
                    )
                if waiting_error is None:
                    return result
            except BaseException as exc:
                if (published and not returned) or isinstance(exc, RecoveryRequired):
                    t.poisoned = True
                    if power is not None:
                        power.abandon()
                    if _held_power is not None:
                        _held_power.abandon()
                    raise RecoveryRequired(
                        "DSP state requires recovery; payload and clocks were retained, but the stock operation gate may expire"
                    ) from exc
                if power is not None:
                    try:
                        power.close()
                    except BaseException as cleanup:
                        t.poisoned = True
                        raise RecoveryRequired(
                            "HMX cleanup failed; operation gate retained"
                        ) from cleanup
                for off, saved in backup:
                    t.memory[off : off + len(saved)] = saved
                t.release_unsubmitted()
                raise
            # The job completed and was acknowledged; only the caller's wait hook failed.
            raise waiting_error

    def _hold_power(self):
        """Power HMX on under the operation gate and keep it on after release.

        The returned owner is passed to each later _submit as _held_power and
        must be given back to _release_power. Nothing is published to the DSP.
        Until release, any other HMX power user on this device finds HMX on.
        """
        with self._lock:
            t = self._connect()
            t.claim(self.claim_timeout)
            try:
                power = self._power_factory()
                power.start()
                try:
                    power.hold()
                except BaseException:
                    power.abandon()
                    raise RecoveryRequired("held HMX power could not be recorded")
            except RecoveryRequired:
                t.poisoned = True
                raise
            except BaseException:
                t.release_unsubmitted()
                raise
            t.release_unsubmitted()
            return power

    def _release_power(self, power):
        """Under the gate: prove the held configuration, then power HMX off."""
        with self._lock:
            t = self._connect()
            t.claim(self.claim_timeout)
            try:
                power.verify()
                power.close()
            except BaseException as exc:
                t.poisoned = True
                power.abandon()
                raise RecoveryRequired(
                    "held HMX power could not be released; clocks were retained"
                ) from exc
            t.release_unsubmitted()

    def _matmul_tile(
        self, a, b, *, bias=None, activation="none", tiles_per_job=1, accumulator=None
    ):
        """FP16 tile products accumulated in FP32 on DSP; returns row-major lists."""
        a, b = _matrix(a), _matrix(b)
        m, k, n = len(a), len(a[0]), len(b[0])
        if k != len(b):
            raise ValueError("matmul inner dimensions differ")
        if m > 32 or n > 32 or k > 32 * tiles_per_job:
            raise ValueError("GEMM chunk exceeds its tile budget")
        if activation not in ("none", "relu"):
            raise ValueError("supported fused activations: none, relu")
        bias = [0.0] * n if bias is None else list(bias)
        if len(bias) != n:
            raise ValueError("bias length must equal output columns")
        data = struct.pack("<%de" % (m * k), *(v for row in a for v in row))
        data += struct.pack("<%de" % (k * n), *(v for row in b for v in row))
        data = data.ljust((len(data) + 3) & ~3, b"\0") + _fp32(bias)
        if accumulator is not None:
            accumulator = _matrix(accumulator, fp16=False)
            if tiles_per_job != 2 or len(accumulator) != m or len(accumulator[0]) != n:
                raise ValueError("accumulator must match a multi-tile output")
            data += _fp32(v for row in accumulator for v in row)
        params = (m, k, n, int(activation == "relu"))
        if tiles_per_job == 2:
            params += (int(accumulator is not None),)
        output = self.submit(
            self.builtin(hmx=True),
            data,
            output_size=m * n * 4,
            opcode=6 if tiles_per_job == 2 else 1,
            params=params,
        ).output
        values = struct.unpack("<%df" % (m * n), output)
        return [list(values[i * n : (i + 1) * n]) for i in range(m)]

    def elementwise(self, rows, *, activation="relu", minimum=0.0, maximum=6.0):
        rows = _matrix(rows, fp16=False)
        if activation not in ("relu", "clip"):
            raise ValueError("supported elementwise operations: relu, clip")
        if not math.isfinite(minimum) or not math.isfinite(maximum) or minimum > maximum:
            raise ValueError("invalid clipping bounds")
        flat = [v for row in rows for v in row]
        data = _fp32(flat) + _fp32([minimum, maximum])
        out = self.submit(
            self.builtin(hmx=False),
            data,
            output_size=len(flat) * 4,
            opcode=2,
            params=(len(flat), int(activation == "clip")),
        ).output
        values = struct.unpack("<%df" % len(flat), out)
        n = len(rows[0])
        return [list(values[i * n : (i + 1) * n]) for i in range(len(rows))]

    def add(self, a, b, *, activation="none"):
        """DSP FP32 addition; one-row b broadcasts across all rows of a."""
        a, b = _matrix(a, fp16=False), _matrix(b, fp16=False)
        if len(a[0]) != len(b[0]) or len(b) not in (1, len(a)):
            raise ValueError("add requires equal shapes or a single broadcast row")
        if activation not in ("none", "relu"):
            raise ValueError("supported fused activations: none, relu")
        av, bv = [v for row in a for v in row], [v for row in b for v in row]
        raw = self.submit(
            self.builtin(hmx=False),
            _fp32(av + bv),
            output_size=len(av) * 4,
            opcode=3,
            params=(len(av), len(bv), int(activation == "relu")),
        ).output
        vals = struct.unpack("<%df" % len(av), raw)
        n = len(a[0])
        return [list(vals[i * n : (i + 1) * n]) for i in range(len(a))]

    def matmul(self, a, b, *, bias=None, activation="none", tiles_per_job=1):
        """Multiply using bounded HMX jobs; direct mode can combine two depth tiles.

        tiles_per_job=2 keeps FP32 accumulation on the DSP within each chunk
        and carries that accumulator between chunks without separate add jobs.
        """
        if type(tiles_per_job) is not int or tiles_per_job not in (1, 2):
            raise ValueError("tiles_per_job must be 1 or 2")
        if tiles_per_job == 2 and self.dispatch != "direct":
            raise ValueError("two-tile jobs require dispatch='direct' for the execution budget")
        a, b = _matrix(a), _matrix(b)
        m, k, n = len(a), len(a[0]), len(b[0])
        if k != len(b):
            raise ValueError("matmul inner dimensions differ")
        if activation not in ("none", "relu"):
            raise ValueError("supported fused activations: none, relu")
        bias = [0.0] * n if bias is None else list(bias)
        if len(bias) != n:
            raise ValueError("bias length must equal output columns")
        _fp32(bias)
        out = [[0.0] * n for _ in range(m)]
        for mr in range(0, m, 32):
            for nc in range(0, n, 32):
                acc = None
                depth = 32 * tiles_per_job
                for kb in range(0, k, depth):
                    aa = [row[kb : kb + depth] for row in a[mr : mr + 32]]
                    bb = [row[nc : nc + 32] for row in b[kb : kb + depth]]
                    if tiles_per_job == 2:
                        final = kb + depth >= k
                        acc = self._matmul_tile(
                            aa,
                            bb,
                            bias=bias[nc : nc + 32] if final else None,
                            activation=activation if final else "none",
                            tiles_per_job=2,
                            accumulator=acc,
                        )
                        continue
                    partial = self._matmul_tile(
                        aa,
                        bb,
                        bias=bias[nc : nc + 32] if k <= 32 else None,
                        activation=activation if k <= 32 else "none",
                    )
                    acc = partial if acc is None else self.add(acc, partial)
                if k > 32 and tiles_per_job == 1:
                    acc = self.add(acc, [bias[nc : nc + 32]], activation=activation)
                for r, row in enumerate(acc):
                    out[mr + r][nc : nc + len(row)] = row
        return out
