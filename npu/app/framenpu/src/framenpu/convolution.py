"""Strict preflight and bounded patch lowering for the pinned landmark graph.

This validates the explicit operator subset used by the pinned graph. It does
not run a graph or infer support for unrelated ncnn operators/formats.
"""

from dataclasses import dataclass
from array import array
import hashlib
import math
from pathlib import Path
import struct
import time

from .errors import InvalidModel, UnsupportedOperator
from .resident_gemm import ResidentGemm


def _int(params, key, default=None):
    value = params.get(key, default)
    if type(value) is not int:
        raise InvalidModel(f"parameter {key} must be an integer")
    return value


def _params(fields):
    result = {}
    for field in fields:
        key, text = field.split("=", 1)
        key = int(key)
        array = key <= -23300
        key = -key - 23300 if array else key
        if key in result:
            raise InvalidModel("duplicate ncnn parameter")
        values = text.split(",")

        def number(text):
            try:
                return int(text)
            except ValueError:
                value = float(text)
                if not math.isfinite(value):
                    raise InvalidModel("nonfinite ncnn parameter")
                return value

        if array:
            count = int(values.pop(0))
            if not 0 <= count <= 32 or len(values) != count:
                raise InvalidModel("invalid ncnn parameter array")
            result[key] = tuple(number(v) for v in values)
        elif len(values) == 1:
            result[key] = number(values[0])
        else:
            raise InvalidModel("scalar ncnn parameter contains a list")
    return result


def _shape(value):
    if (
        not value
        or len(value) > 3
        or any(type(v) is not int or not 0 < v <= 8192 for v in value)
        or math.prod(value) > 16 * 1024 * 1024
    ):
        raise InvalidModel("inferred tensor exceeds supported shape bounds")
    return tuple(value)


@dataclass(frozen=True)
class ConvLayer:
    name: str
    bottom: str
    top: str
    input_shape: tuple
    output_shape: tuple
    kernel: tuple
    stride: tuple
    dilation: tuple
    padding: tuple
    activation: str
    weight_offset: int
    weight_bytes: int
    weight_dtype: str
    bias_offset: int | None
    segment: tuple

    @property
    def reduction(self):
        return self.input_shape[0] * math.prod(self.kernel)

    def weights(self, raw):
        k, n = self.reduction, self.output_shape[0]
        values = struct.unpack_from(
            f"<{k * n}{'e' if self.weight_dtype == 'f16' else 'f'}", raw, self.weight_offset
        )
        if any(not math.isfinite(v) or abs(v) > 65504 for v in values):
            raise InvalidModel("weights are not finite FP16-representable values")
        # ncnn [output, input, kh, kw] -> GEMM [input*kh*kw, output].
        return [[values[c * k + t] for c in range(n)] for t in range(k)]

    def bias(self, raw):
        n = self.output_shape[0]
        values = (
            (0.0,) * n
            if self.bias_offset is None
            else struct.unpack_from(f"<{n}f", raw, self.bias_offset)
        )
        if any(not math.isfinite(v) for v in values):
            raise InvalidModel("bias contains nonfinite values")
        return values

    def patches(self, chw, start, rows=32):
        """Generate one spatial tile only, in ncnn channel/kernel order."""
        channels, height, width = self.input_shape
        _, oh, ow = self.output_shape
        if (
            len(chw) != channels * height * width
            or type(start) is not int
            or not 0 <= start < oh * ow
            or type(rows) is not int
            or not 1 <= rows <= 32
        ):
            raise InvalidModel("invalid convolution patch input/range")
        rows = min(rows, oh * ow - start)
        kh, kw = self.kernel
        sh, sw = self.stride
        dh, dw = self.dilation
        pt, pb, pl, pr = self.padding
        patches = []
        for pixel in range(start, start + rows):
            oy, ox = divmod(pixel, ow)
            patch = []
            for channel in range(channels):
                for ky in range(kh):
                    iy = oy * sh - pt + ky * dh
                    for kx in range(kw):
                        ix = ox * sw - pl + kx * dw
                        patch.append(
                            chw[(channel * height + iy) * width + ix]
                            if 0 <= iy < height and 0 <= ix < width
                            else 0.0
                        )
            patches.append(patch)
        return patches


@dataclass(frozen=True)
class NcnnGraph:
    layers: tuple
    shapes: dict
    raw: bytes
    param_sha256: str
    weights_sha256: str

    def convolution(self, name):
        for layer in self.layers:
            if isinstance(layer, ConvLayer) and layer.name == name:
                return layer
        raise InvalidModel(f"ordinary convolution not found: {name}")


class ResidentConvolution:
    """Bounded host patch lowering and resident DSP GEMM/bias/clip proof.

    Opens lazily after validating the first input. Each run uses at most one
    32-row patch tile; arbitrary graphs and layers wider/deeper than this proof
    are rejected before opening a device. Weights persist across runs.
    """

    def __init__(self, job, graph, name, **session_options):
        self.layer = graph.convolution(name)
        layer = self.layer
        if (
            layer.reduction > 128
            or layer.output_shape[0] > 32
            or layer.activation not in ("none", "clip")
        ):
            raise UnsupportedOperator(
                "resident convolution proof requires K<=128, N<=32, none/clip"
            )
        k = (layer.reduction + 31) & ~31
        weights = layer.weights(graph.raw)
        padded = [row + [0.0] * (32 - len(row)) for row in weights]
        padded += [[0.0] * 32 for _ in range(k - len(padded))]
        bias = list(layer.bias(graph.raw)) + [0.0] * (32 - layer.output_shape[0])
        self.gemm = ResidentGemm(
            job, padded, _tag=2, _bias=bias, _clip=layer.activation == "clip", **session_options
        )
        self.zero_output = tuple(
            min(6.0, max(0.0, v)) if layer.activation == "clip" else v for v in bias
        )
        self.active = False
        self.closed = False
        self.runs = []

    def __enter__(self):
        if self.closed:
            raise RuntimeError("convolution is closed")
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if not self.closed:
            if self.active:
                self.gemm.close()
            self.closed = True

    def run(self, chw, *, tile_validator=None, stop_after=None):
        if self.closed:
            raise RuntimeError("convolution is closed")
        if len(chw) != math.prod(self.layer.input_shape) or any(
            not math.isfinite(v) or abs(v) > 65504 for v in chw
        ):
            raise InvalidModel("convolution input shape/range differs")
        if tile_validator is not None and not callable(tile_validator):
            raise InvalidModel("tile validator must be callable")
        if stop_after is not None and (type(stop_after) is not int or stop_after <= 0):
            raise InvalidModel("stop_after must be a positive diagnostic tile count")
        if not self.active:
            self.gemm.__enter__()
            self.active = True
        n, h, w = self.layer.output_shape
        output = array("f", [0.0]) * (n * h * w)
        lowering = 0.0
        started = time.monotonic_ns()
        tiles = 0
        for start in range(0, h * w, 32):
            stamp = time.monotonic_ns()
            patches = self.layer.patches(chw, start)
            rows = len(patches)
            patches = [row + [0.0] * (self.gemm.k - len(row)) for row in patches]
            patches += [[0.0] * self.gemm.k for _ in range(32 - rows)]
            lowering += (time.monotonic_ns() - stamp) / 1e6

            def check(raw):
                values = struct.unpack("<1024f", raw)
                if any(not math.isfinite(v) for v in values) or any(
                    values[r * 32 + c] != (self.zero_output[c] if r >= rows else 0.0)
                    for r in range(32)
                    for c in range(32)
                    if r >= rows or c >= n
                ):
                    raise InvalidModel("nonfinite convolution output or nonzero padded tail")
                if tile_validator is not None:
                    tile_validator(start, rows, values)

            values = self.gemm.run(patches, _check=check)
            for r in range(rows):
                for c in range(n):
                    output[c * h * w + start + r] = values[r][c]
            tiles += 1
            if stop_after is not None and tiles >= stop_after:
                break
        self.runs.append(
            {
                "tiles": tiles,
                "complete": tiles * 32 >= h * w,
                "patch_lowering_ms": lowering,
                "run_ms": (time.monotonic_ns() - started) / 1e6,
            }
        )
        return output


def load_landmark_graph(param_path, weight_path, *, input_shape=(3, 224, 224)):
    """Check the whole supported DAG, tagged weights, biases and exact EOF."""
    try:
        with Path(param_path).open("rb") as stream:
            text = stream.read(1024 * 1024 + 1)
        with Path(weight_path).open("rb") as stream:
            raw = stream.read(16 * 1024 * 1024 + 1)
        if len(text) > 1024 * 1024 or len(raw) > 16 * 1024 * 1024:
            raise InvalidModel("ncnn input exceeds bounds")
        lines = [line.strip() for line in text.decode().splitlines() if line.strip()]
        if len(lines) < 3 or lines[0] != "7767517":
            raise InvalidModel("invalid ncnn param magic")
        declared, blobs = map(int, lines[1].split())
        if not 1 <= declared <= 1024 or declared != len(lines) - 2 or not 1 <= blobs <= 4096:
            raise InvalidModel("invalid ncnn layer/blob count")
        shapes = {}
        names = set()
        layers = []
        position = 0

        def read_weights(count, bias_count):
            nonlocal position
            if not 0 < count <= 8 * 1024 * 1024:
                raise InvalidModel("invalid weight count")
            begin = position
            tag = struct.unpack_from("<I", raw, position)[0]
            position += 4
            if tag == 0x01306B47:
                dtype, element = "f16", 2
            elif tag in (0, 0x0002C056):
                dtype, element = "f32", 4
            else:
                raise UnsupportedOperator(f"unsupported ncnn weight tag {tag:#x}")
            offset = position
            size = count * element
            position += (size + 3) & ~3
            bias = position if bias_count else None
            position += bias_count * 4
            if position > len(raw):
                raise InvalidModel("truncated ncnn weights/biases")
            if dtype == "f32" and any(
                not math.isfinite(v[0])
                for v in struct.iter_unpack("<f", raw[offset : offset + size])
            ):
                raise InvalidModel("nonfinite weights")
            if dtype == "f16" and any(
                v[0] & 0x7C00 == 0x7C00
                for v in struct.iter_unpack("<H", raw[offset : offset + size])
            ):
                raise InvalidModel("nonfinite weights")
            if bias_count and any(
                not math.isfinite(v[0]) for v in struct.iter_unpack("<f", raw[bias:position])
            ):
                raise InvalidModel("nonfinite biases")
            return offset, size, dtype, bias, (begin, position)

        for line in lines[2:]:
            parts = line.split()
            op, name = parts[:2]
            bottoms, tops = map(int, parts[2:4])
            if (
                name in names
                or not 0 <= bottoms <= 8
                or not 1 <= tops <= 8
                or len(parts) < 4 + bottoms + tops
            ):
                raise InvalidModel("invalid/duplicate layer declaration")
            names.add(name)
            inputs = parts[4 : 4 + bottoms]
            outputs = parts[4 + bottoms : 4 + bottoms + tops]
            if (
                len(set(outputs)) != len(outputs)
                or any(key in shapes for key in outputs)
                or any(key not in shapes for key in inputs)
            ):
                raise InvalidModel("invalid DAG connectivity or blob reuse")
            p = _params(parts[4 + bottoms + tops :])
            activation = _int(p, 9, 0)
            if activation not in (0, 3, 4) or activation == 3 and p.get(10) != (0.0, 6.0):
                raise UnsupportedOperator(
                    "only none, clip[0,6] and sigmoid activations are supported"
                )
            item = None
            metadata = {}
            if op == "Input":
                if bottoms or tops != 1 or shapes or p:
                    raise UnsupportedOperator("require one externally shaped Input")
                shape = _shape(input_shape)
            elif op in ("Convolution", "ConvolutionDepthWise"):
                if (
                    bottoms != 1
                    or tops != 1
                    or len(shapes[inputs[0]]) != 3
                    or p.keys() - {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19}
                ):
                    raise UnsupportedOperator("unsupported convolution schema")
                channels, h, w = shapes[inputs[0]]
                n = _int(p, 0)
                kw = _int(p, 1)
                kh = _int(p, 11, kw)
                sw = _int(p, 3, 1)
                sh = _int(p, 13, sw)
                dw = _int(p, 2, 1)
                dh = _int(p, 12, dw)
                pl = _int(p, 4, 0)
                pr = _int(p, 15, pl)
                pt = _int(p, 14, pl)
                pb = _int(p, 16, pt)
                group = _int(p, 7, 1)
                bias = _int(p, 5, 0)
                count = _int(p, 6)
                if (
                    min(n, kw, kh, sw, sh, dw, dh, group) <= 0
                    or max(kw, kh) > 7
                    or min(pl, pr, pt, pb) < 0
                    or max(pl, pr, pt, pb) > 7
                    or bias not in (0, 1)
                    or p.get(18, 0) != 0
                    or _int(p, 8, 0)
                    or _int(p, 19, 0)
                ):
                    raise UnsupportedOperator("unsupported convolution attributes")
                if (
                    op == "Convolution"
                    and group != 1
                    or op == "ConvolutionDepthWise"
                    and group != channels
                    or channels % group
                    or n % group
                ):
                    raise UnsupportedOperator("unsupported convolution groups")
                if count != n * (channels // group) * kh * kw:
                    raise InvalidModel("convolution weight count does not match inferred channels")
                shape = _shape(
                    (
                        n,
                        (h + pt + pb - dh * (kh - 1) - 1) // sh + 1,
                        (w + pl + pr - dw * (kw - 1) - 1) // sw + 1,
                    )
                )
                offset, size, dtype, bias_offset, segment = read_weights(count, n if bias else 0)
                metadata = {
                    "weight_offset": offset,
                    "weight_bytes": size,
                    "weight_dtype": dtype,
                    "bias_offset": bias_offset,
                    "segment": segment,
                    "kernel": (kh, kw),
                    "stride": (sh, sw),
                    "dilation": (dh, dw),
                    "padding": (pt, pb, pl, pr),
                    "activation": {0: "none", 3: "clip", 4: "sigmoid"}[activation],
                    "groups": group,
                }
                if op == "Convolution":
                    item = ConvLayer(
                        name,
                        inputs[0],
                        outputs[0],
                        shapes[inputs[0]],
                        shape,
                        (kh, kw),
                        (sh, sw),
                        (dh, dw),
                        (pt, pb, pl, pr),
                        {0: "none", 3: "clip", 4: "sigmoid"}[activation],
                        offset,
                        size,
                        dtype,
                        bias_offset,
                        segment,
                    )
            elif op == "Split":
                if bottoms != 1 or p:
                    raise UnsupportedOperator("unsupported Split")
                shape = shapes[inputs[0]]
            elif op == "BinaryOp":
                if (
                    bottoms != 2
                    or tops != 1
                    or p != {0: 0}
                    or shapes[inputs[0]] != shapes[inputs[1]]
                ):
                    raise UnsupportedOperator("only equal-shape residual addition supported")
                shape = shapes[inputs[0]]
            elif op == "Pooling":
                if (
                    bottoms != 1
                    or tops != 1
                    or len(shapes[inputs[0]]) != 3
                    or p.keys() - {0, 1, 2, 3, 4, 5, 11, 12, 13, 14, 15}
                    or _int(p, 0) not in (0, 1)
                ):
                    raise UnsupportedOperator("unsupported Pooling")
                c, h, w = shapes[inputs[0]]
                if _int(p, 4, 0) == 1:
                    shape = (c, 1, 1)
                else:
                    kw = _int(p, 1)
                    kh = _int(p, 11, kw)
                    sw = _int(p, 2, 1)
                    sh = _int(p, 12, sw)
                    if (
                        _int(p, 5, 0) != 1
                        or any(_int(p, i, 0) for i in (3, 13, 14, 15))
                        or min(kw, kh, sw, sh) <= 0
                    ):
                        raise UnsupportedOperator("only explicit valid unpadded Pooling supported")
                    shape = _shape((c, (h - kh) // sh + 1, (w - kw) // sw + 1))
            elif op == "Reshape":
                if bottoms != 1 or tops != 1 or p != {0: 1, 1: 1, 2: -1}:
                    raise UnsupportedOperator("only landmark-head Reshape supported")
                shape = _shape((math.prod(shapes[inputs[0]]), 1, 1))
            elif op == "Squeeze":
                if bottoms != 1 or tops != 1 or p != {3: (1, 2)} or shapes[inputs[0]][1:] != (1, 1):
                    raise UnsupportedOperator("only landmark-head Squeeze supported")
                shape = (shapes[inputs[0]][0],)
            elif op == "InnerProduct":
                if bottoms != 1 or tops != 1 or p.keys() - {0, 1, 2, 9, 10}:
                    raise UnsupportedOperator("unsupported InnerProduct")
                n = _int(p, 0)
                bias = _int(p, 1, 0)
                count = _int(p, 2)
                if n <= 0 or bias not in (0, 1) or count != n * math.prod(shapes[inputs[0]]):
                    raise InvalidModel("InnerProduct weight shape mismatch")
                offset, size, dtype, bias_offset, segment = read_weights(count, n if bias else 0)
                metadata = {
                    "weight_offset": offset,
                    "weight_bytes": size,
                    "weight_dtype": dtype,
                    "bias_offset": bias_offset,
                    "segment": segment,
                    "activation": {0: "none", 3: "clip", 4: "sigmoid"}[activation],
                }
                shape = _shape((n,))
            else:
                raise UnsupportedOperator(f"unsupported ncnn operator: {op}")
            shapes.update({output: shape for output in outputs})
            layers.append(
                item
                if item is not None
                else {
                    "name": name,
                    "operator": op,
                    "input": inputs,
                    "output": outputs,
                    "shape": shape,
                    "params": p,
                    **metadata,
                }
            )
        if len(shapes) != blobs or position != len(raw):
            raise InvalidModel("blob count or binary length does not match complete graph")
        return NcnnGraph(
            tuple(layers),
            shapes,
            raw,
            hashlib.sha256(text).hexdigest(),
            hashlib.sha256(raw).hexdigest(),
        )
    except (ValueError, IndexError, KeyError, UnicodeError, struct.error, OverflowError) as exc:
        if isinstance(exc, InvalidModel):
            raise
        raise InvalidModel("invalid ncnn graph or binary") from exc
