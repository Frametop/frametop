"""Experimental complete v20a landmark graph, streamed through bounded DSP jobs.

Intermediates are FP32 host tensors. Split/Reshape/Squeeze share storage and
last-use accounting retires every alias together. This is not a resident graph
runtime: the existing borrowed arena and VTCM are restored on every command.
"""

from array import array
from dataclasses import asdict
import hashlib
import math
import struct
import threading
import time

from .convolution import ConvLayer
from .device import Device
from .errors import InvalidModel, RecoveryRequired, UnsupportedOperator
from .job import Job

PARAM_SHA = "54e08f284ff6f121ff00f4a9c65afc38c0dbb22a32f4af44fc4e0fde6c4a1bbd"
WEIGHTS_SHA = "b7c3d1be232725a5a143e2648858ec5aec9596c7b83c6a3c508479b04057e00c"
OUTPUTS = ("out0", "out1", "out2", "out3")
ALIASES = ("Split", "Reshape", "Squeeze")
SSR = (0x81840200, 0x89840200, 0x91840200)


def specifications(graph):
    result = []
    for layer in graph.layers:
        if isinstance(layer, ConvLayer):
            s = asdict(layer)
            s.update(operator="Convolution", input=[layer.bottom], output=[layer.top])
        else:
            s = dict(layer)
            s["output_shape"] = s["shape"]
            if s["input"]:
                s["input_shape"] = graph.shapes[s["input"][0]]
        result.append(s)
    return tuple(result)


def tensor_lifetimes(graph):
    """Host allocation sizes/roots and inclusive lifetimes, including aliases."""
    roots, entries = {}, {}
    specs = specifications(graph)
    for step, s in enumerate(specs):
        for key in s["input"]:
            entries[roots[key]]["last"] = step
        for key in s["output"]:
            if s["operator"] in ALIASES:
                roots[key] = roots[s["input"][0]]
            else:
                roots[key] = key
                entries[key] = {
                    "first": step,
                    "last": step,
                    "bytes": math.prod(graph.shapes[key]) * 4,
                }
    for key in OUTPUTS:
        if key in roots:
            entries[roots[key]]["last"] = len(specs)
    peak = max(
        sum(e["bytes"] for e in entries.values() if e["first"] <= i <= e["last"])
        for i in range(len(specs) + 1)
    )
    return {
        "storage": "host_fp32",
        "roots": roots,
        "allocations": entries,
        "peak_bytes": peak,
        "alias_count": len(roots) - len(entries),
    }


def weights_and_bias(graph, s):
    n = s["output_shape"][0]
    count = s["weight_bytes"] // (2 if s["weight_dtype"] == "f16" else 4)
    values = struct.unpack_from(
        f"<{count}{'e' if s['weight_dtype'] == 'f16' else 'f'}", graph.raw, s["weight_offset"]
    )
    bias = (
        (0.0,) * n
        if s["bias_offset"] is None
        else struct.unpack_from(f"<{n}f", graph.raw, s["bias_offset"])
    )
    return values, bias


def _float_bytes(values):
    return struct.pack(f"<{len(values)}f", *values)


def _half_bytes(values):
    try:
        return struct.pack(f"<{len(values)}e", *values)
    except (OverflowError, struct.error) as exc:
        raise InvalidModel("graph operand exceeds finite FP16 range") from exc


class StreamingLandmark:
    """Pinned graph only; one direct Device/HMX owner and <=128 KiB per job.

    Optional validator_factory(spec, input_arrays) returns a checker accepting
    (flat_indices, values, final, phase). It runs before DSP reclamation on each
    arithmetic chunk; aliases are checked synchronously without a submission.
    Any checker failure retains uncertain resources via RecoveryRequired.
    """

    def __init__(self, graph, jobs, *, _device_factory=Device):
        if graph.param_sha256 != PARAM_SHA or graph.weights_sha256 != WEIGHTS_SHA:
            raise UnsupportedOperator("complete graph execution requires the pinned v20a model")
        if set(jobs) != {"hmx", "hvx", "sigmoid"} or any(
            not isinstance(j, Job) or not j.requires_hmx or not j.supports_direct
            for j in jobs.values()
        ):
            raise InvalidModel("three current HMX/direct landmark FJOBs are required")
        self.graph, self.jobs = graph, dict(jobs)
        self._lock = threading.RLock()
        self.specs, self.layout = specifications(graph), tensor_lifetimes(graph)
        self._device_factory = _device_factory
        self.device = None
        self.closed = self.invalid = False
        self.metrics = []
        self.packed = {}
        prepack_started = time.monotonic_ns()
        for s in self.specs:
            if s["operator"] in ("Convolution", "InnerProduct"):
                w, bias = weights_and_bias(graph, s)
                n = s["output_shape"][0]
                k = len(w) // n
                if k > 704 or s["activation"] not in ("none", "clip", "sigmoid"):
                    raise UnsupportedOperator("layer exceeds bounded full-depth GEMM")
                depth = (k + 31) // 32
                blocks = []
                for nc in range(0, n, 32):
                    b = [
                        w[c * k + t] if c < n and t < k else 0.0
                        for kt in range(0, depth * 32, 32)
                        for t in range(kt, kt + 32)
                        for c in range(nc, nc + 32)
                    ]
                    blocks.append(
                        _half_bytes(b)
                        + _float_bytes(
                            list(bias[nc : nc + 32]) + [0.0] * (32 - len(bias[nc : nc + 32]))
                        )
                    )
                self.packed[s["name"]] = (k, depth, blocks)
        self.prepack_ms = (time.monotonic_ns() - prepack_started) / 1e6

    def __enter__(self):
        if self.closed:
            raise RuntimeError("landmark executor is closed")
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        with self._lock:
            if not self.closed:
                stamp = time.monotonic_ns()
                if self.device is not None:
                    self.device.close()
                if self.metrics:
                    self.metrics[-1]["close_ms"] = (time.monotonic_ns() - stamp) / 1e6
                self.closed = True

    def _submit(self, kind, data, opcode, params, vectors, check):
        if self.device is None:
            self.device = self._device_factory(dispatch="direct")
            self.device.__enter__()
        phases = {}
        started = time.monotonic_ns()

        def validate(result):
            try:
                rows = result.workers
                if len(rows) != 3 or len(result.output) != (vectors + 1) * 128:
                    raise ValueError("incomplete landmark completion")
                for i, row in enumerate(rows):
                    if (
                        len(row) != 16
                        or not row[0]
                        or row[0] != rows[0][0]
                        or row[1] != i
                        or row[2]
                        or any(row[3:6])
                        or row[7] != SSR[i]
                    ):
                        raise ValueError("landmark worker identity/fault/SSR differs")
                if rows[0][8] != 0x87000000 or any(rows[0][14:]):
                    raise ValueError("landmark EVB or cache markers differ")
                ticks = result.kernel_reference_ticks
                if ticks is None or ticks >= 768000:
                    raise ValueError("landmark wrapper exceeds existing 40 ms limit")
                if any(result.output[-128:]):
                    raise ValueError("nonfinite raw arithmetic before activation")
                values = struct.unpack(f"<{vectors * 32}f", result.output[:-128])
                if any(not math.isfinite(v) for v in values):
                    raise ValueError("nonfinite landmark output")
                check(values)
            except BaseException as exc:
                raise RecoveryRequired(
                    "landmark validation failed; retain payload and power"
                ) from exc

        result = self.device._submit(
            self.jobs[kind],
            data,
            opcode=opcode,
            params=params,
            output_size=(vectors + 1) * 128,
            use_direct=True,
            _completion_validator=validate,
            _phase_times=phases,
        )
        self._commands.append(
            {
                "opcode": opcode,
                "input_bytes": len(data),
                "output_bytes": len(result.output),
                "host_phase_ms": phases,
                "command_ms": (time.monotonic_ns() - started) / 1e6,
                "wrapper_ms": result.kernel_reference_ticks / 19200,
                "publication_ms": result.elapsed_ms,
            }
        )

    def run(self, chw, *, validator_factory=None, stop_after=None):
        with self._lock:
            return self._run(chw, validator_factory=validator_factory, stop_after=stop_after)

    def _run(self, chw, *, validator_factory=None, stop_after=None):
        if self.closed or self.invalid:
            raise RuntimeError("landmark executor is closed or invalidated")
        if len(chw) != math.prod(self.graph.shapes["in0"]) or any(
            not math.isfinite(v) or abs(v) > 65504 for v in chw
        ):
            raise InvalidModel("invalid graph input shape/range")
        if validator_factory is not None and not callable(validator_factory):
            raise InvalidModel("validator factory must be callable")
        if stop_after is not None and stop_after not in {s["name"] for s in self.specs}:
            raise InvalidModel("unknown diagnostic stop layer")
        tensors = {"in0": array("f", chw)}
        roots, lifetimes = self.layout["roots"], self.layout["allocations"]
        records = []
        started = time.monotonic_ns()
        run_record = {"layers": records, "complete": False, "weight_prepack_ms": self.prepack_ms}
        self.metrics.append(run_record)
        try:
            for step, s in enumerate(self.specs):
                self.current_layer = s["name"]
                self._commands = []
                stamp = time.monotonic_ns()
                inputs = [tensors[k] for k in s["input"]]
                checker = validator_factory(s, inputs) if validator_factory else None
                reference_ms = (time.monotonic_ns() - stamp) / 1e6
                self._packing_ms = 0.0
                if s["operator"] == "Input":
                    out = tensors["in0"]
                    if checker:
                        checker(range(len(out)), out, True, "output")
                elif s["operator"] in ALIASES:
                    out = inputs[0]
                    if checker:
                        checker(range(len(out)), out, True, "output")
                elif s["operator"] in ("Convolution", "InnerProduct"):
                    out = self._gemm(s, inputs[0], checker)
                else:
                    out = self._hvx(s, inputs, checker)
                for key in s["output"]:
                    tensors[key] = out
                records.append(
                    {
                        "name": s["name"],
                        "operator": s["operator"],
                        "values": len(out),
                        "sha256": hashlib.sha256(out.tobytes()).hexdigest(),
                        "layer_ms": (time.monotonic_ns() - stamp) / 1e6,
                        "reference_ms": reference_ms,
                        "packing_ms": self._packing_ms,
                        "commands": self._commands,
                    }
                )
                if stop_after == s["name"]:
                    break
                for key in tuple(tensors):
                    if lifetimes[roots[key]]["last"] <= step:
                        del tensors[key]
            result = {key: tensors[key] for key in OUTPUTS if key in tensors}
            run_record.update(
                {
                    "complete": len(records) == len(self.specs),
                    "run_ms": (time.monotonic_ns() - started) / 1e6,
                }
            )
            return result
        except BaseException:
            self.invalid = True
            run_record.update(
                failed_layer=self.current_layer,
                run_ms=(time.monotonic_ns() - started) / 1e6,
                completed_commands_in_failed_layer=self._commands,
            )
            raise

    def _gemm(self, s, source, checker):
        n = s["output_shape"][0]
        dense = s["operator"] == "InnerProduct"
        spatial = 1 if dense else math.prod(s["output_shape"][1:])
        out = array("f", [0.0]) * (n * spatial)
        k, depth, blocks = self.packed[s["name"]]
        if dense:

            def patches(start):
                return [source]
        elif s["kernel"] == (1, 1):

            def patches(start):
                return [
                    [source[c * spatial + r] for c in range(k)]
                    for r in range(start, min(start + 32, spatial))
                ]
        else:
            layer = self.graph.convolution(s["name"])

            def patches(start):
                return layer.patches(source, start)

        for start in range(0, spatial, 32):
            pack_started = time.monotonic_ns()
            rows = patches(start)
            m = len(rows)
            a = _half_bytes(
                [
                    rows[r][t] if r < m and t < k else 0.0
                    for kt in range(0, depth * 32, 32)
                    for r in range(32)
                    for t in range(kt, kt + 32)
                ]
            )
            self._packing_ms += (time.monotonic_ns() - pack_started) / 1e6
            for block, nc in zip(blocks, range(0, n, 32)):
                cols = min(32, n - nc)
                sigmoid = s["activation"] == "sigmoid"
                final = start + m == spatial and nc + cols == n and not sigmoid
                bias = struct.unpack("<32f", block[-128:])
                tail = tuple(
                    min(6.0, max(0.0, b)) if s["activation"] == "clip" else b for b in bias
                )

                def check(values):
                    for r in range(32):
                        for c in range(32):
                            v = values[r * 32 + c]
                            if r >= m or c >= cols:
                                if v != (tail[c] if r >= m else 0.0):
                                    raise ValueError(
                                        f"GEMM padded row/channel differs at ({r},{c}): {v} vs {tail[c] if r >= m else 0.0}"
                                    )
                            else:
                                out[(nc + c) * spatial + start + r] = v
                    if checker:
                        checker(
                            [(nc + c) * spatial + start + r for r in range(m) for c in range(cols)],
                            [values[r * 32 + c] for r in range(m) for c in range(cols)],
                            final,
                            "logits" if sigmoid else "output",
                        )

                self._submit(
                    "hmx", a + block, 20, (depth, int(s["activation"] == "clip")), 32, check
                )
        if s["activation"] == "sigmoid":
            if n != 1 or spatial != 1:
                raise UnsupportedOperator("sigmoid proof only supports single-value heads")

            def check_sigmoid(values):
                if any(v != 0.5 for v in values[1:]):
                    raise ValueError("sigmoid padded tail differs")
                out[0] = values[0]
                if checker:
                    checker([0], out, True, "output")

            self._submit(
                "sigmoid", _float_bytes(list(out) + [0.0] * 31), 25, (1,), 1, check_sigmoid
            )
        return out

    def _hvx(self, s, inputs, checker):
        count = math.prod(s["output_shape"])
        out = array("f", [0.0]) * count
        op = s["operator"]
        if op == "ConvolutionDepthWise":
            if s["input_shape"][0] != s["output_shape"][0] or s["activation"] not in (
                "none",
                "clip",
            ):
                raise UnsupportedOperator("depthwise multiplier/activation differs")
            w, bias = weights_and_bias(self.graph, s)
            kh, kw = s["kernel"]
            taps, chunk, opcode = kh * kw, 512, 21
        elif op == "BinaryOp":
            taps, chunk, opcode = 0, 8192, 22
        elif op == "Pooling":
            p = s["params"]
            if p.get(4, 0):
                taps, chunk, opcode = math.prod(s["input_shape"][1:]), 512, 24
                if taps > 49:
                    raise UnsupportedOperator("global pooling exceeds bounded 49 values")
            else:
                kw, kh = p[1], p.get(11, p[1])
                if p[0] != 0:
                    raise UnsupportedOperator("only max valid pooling supported by this graph")
                taps, chunk, opcode = kh * kw, 2048, 23
        else:
            raise UnsupportedOperator(f"missing graph kernel for {op}")
        for start in range(0, count, chunk):
            pack_started = time.monotonic_ns()
            active = min(chunk, count - start)
            size = (active + 31) & ~31
            if opcode == 22:
                values = [list(a[start : start + active]) + [0.0] * (size - active) for a in inputs]
            elif opcode == 24:
                values = [
                    [
                        inputs[0][c * taps + t] if c < count else 0.0
                        for c in range(start, start + size)
                    ]
                    for t in range(taps)
                ]
                values.append([1.0 / taps] * size)
            else:
                c0, ih, iw = s["input_shape"]
                _, oh, ow = s["output_shape"]
                if opcode == 21:
                    sh, sw = s["stride"]
                    dh, dw = s["dilation"]
                    pt, pb, pl, pr = s["padding"]
                else:
                    sw = s["params"].get(2, 1)
                    sh = s["params"].get(12, sw)
                    dh = dw = 1
                    pt = pl = 0
                values = []
                weight = []
                for ky in range(kh):
                    for kx in range(kw):
                        row = []
                        wr = []
                        for i in range(start, start + size):
                            channel, pixel = divmod(i, oh * ow)
                            oy, ox = divmod(pixel, ow)
                            iy, ix = oy * sh - pt + ky * dh, ox * sw - pl + kx * dw
                            row.append(
                                inputs[0][(channel * ih + iy) * iw + ix]
                                if i < count and 0 <= iy < ih and 0 <= ix < iw
                                else 0.0
                            )
                            if opcode == 21:
                                wr.append(w[channel * taps + ky * kw + kx] if i < count else 0.0)
                        values.append(row)
                        if opcode == 21:
                            weight.append(wr)
                if opcode == 21:
                    values += weight
                    values.append(
                        [
                            bias[i // (oh * ow)] if i < count else 0.0
                            for i in range(start, start + size)
                        ]
                    )
            data = b"".join(_float_bytes(row) for row in values)
            self._packing_ms += (time.monotonic_ns() - pack_started) / 1e6

            def check(result):
                if any(v != 0.0 for v in result[active:]):
                    raise ValueError("HVX padded tail differs")
                out[start : start + active] = array("f", result[:active])
                if checker:
                    checker(
                        range(start, start + active),
                        result[:active],
                        start + active == count,
                        "output",
                    )

            self._submit(
                "hvx",
                data,
                opcode,
                (size // 32, taps, int(s.get("activation") == "clip")),
                size // 32,
                check,
            )
        return out
