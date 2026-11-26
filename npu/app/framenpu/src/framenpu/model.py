"""Portable, validated dense models. Weights are data, never native kernel code."""

from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct
import zipfile
from .device import _matrix, _fp32
from .errors import InvalidModel, UnsupportedOperator
from .profile import PROFILE


@dataclass(frozen=True)
class Dense:
    weights: tuple
    bias: tuple
    activation: str = "none"

    def __init__(self, weights, bias=None, activation="none"):
        w = _matrix(weights)
        n = len(w[0])
        b = [0.0] * n if bias is None else list(bias)
        if len(b) != n or activation not in ("none", "relu"):
            raise InvalidModel("dense bias shape or activation is unsupported")
        _fp32(b)
        # Quantize once to make the serialized and in-memory models agree.
        packed = struct.pack("<%de" % (len(w) * n), *(x for row in w for x in row))
        vals = struct.unpack("<%de" % (len(w) * n), packed)
        object.__setattr__(
            self, "weights", tuple(tuple(vals[i * n : (i + 1) * n]) for i in range(len(w)))
        )
        object.__setattr__(self, "bias", tuple(struct.unpack("<%df" % n, _fp32(b))))
        object.__setattr__(self, "activation", activation)


@dataclass(frozen=True)
class ReLU:
    pass


@dataclass(frozen=True)
class Clip:
    minimum: float = 0.0
    maximum: float = 6.0

    def __post_init__(self):
        if (
            not math.isfinite(self.minimum)
            or not math.isfinite(self.maximum)
            or self.minimum > self.maximum
        ):
            raise InvalidModel("invalid clip bounds")
        _fp32([self.minimum, self.maximum])


class Model:
    def __init__(self, input_features, layers, *, name="model"):
        if type(input_features) is not int or not 0 < input_features <= 256:
            raise InvalidModel("input_features must be 1..256")
        layers = tuple(layers)
        if not 0 < len(layers) <= 32:
            raise InvalidModel("model must contain 1..32 layers")
        features = input_features
        for layer in layers:
            if isinstance(layer, Dense):
                if len(layer.weights) != features:
                    raise InvalidModel("dense input dimension differs from the previous layer")
                features = len(layer.weights[0])
            elif not isinstance(layer, (ReLU, Clip)):
                raise UnsupportedOperator("supported layers are Dense, ReLU and Clip")
        self.input_features = input_features
        self.output_features = features
        self.layers = layers
        self.name = str(name)

    def plan(self, batch_size=1):
        """Validate every layer's memory requirement before running any job."""
        if type(batch_size) is not int or not 0 < batch_size <= 256:
            raise InvalidModel("batch_size must be 1..256")
        width = self.input_features
        plan = []
        for index, layer in enumerate(self.layers):
            if isinstance(layer, Dense):
                output_width = len(layer.weights[0])
                tile_rows, tile_k, tile_cols = (
                    min(batch_size, 32),
                    min(width, 32),
                    min(output_width, 32),
                )
                input_bytes = ((tile_rows * tile_k + tile_k * tile_cols) * 2 + 3) & ~3
                input_bytes += tile_cols * 4
                engine = "hmx"
            else:
                output_width = width
                input_bytes = batch_size * width * 4 + 8
                engine = "dsp"
            required = ((input_bytes + 127) & ~127) + (
                min(batch_size, 32) * min(output_width, 32) * 4
                if isinstance(layer, Dense)
                else batch_size * output_width * 4
            )
            if required > PROFILE.arena_size:
                raise InvalidModel(
                    "layer %d needs %d bytes; job arena is %d"
                    % (index, required, PROFILE.arena_size)
                )
            plan.append(
                {
                    "layer": index,
                    "operator": type(layer).__name__,
                    "engine": engine,
                    "input_shape": [batch_size, width],
                    "output_shape": [batch_size, output_width],
                    "arena_bytes": required,
                }
            )
            width = output_width
        return plan

    def run(self, device, inputs):
        inputs = list(inputs)
        single = bool(inputs) and not isinstance(inputs[0], (list, tuple))
        rows = _matrix([inputs] if single else inputs)
        if len(rows[0]) != self.input_features:
            raise InvalidModel("model input feature count differs")
        self.plan(len(rows))
        for layer in self.layers:
            if isinstance(layer, Dense):
                rows = device.matmul(
                    rows, layer.weights, bias=layer.bias, activation=layer.activation
                )
            elif isinstance(layer, ReLU):
                rows = device.elementwise(rows, activation="relu")
            else:
                rows = device.elementwise(
                    rows, activation="clip", minimum=layer.minimum, maximum=layer.maximum
                )
        return rows[0] if single else rows

    def save(self, path):
        tensors = {}
        layers = []

        def tensor(name, shape, dtype, raw):
            tensors[name] = raw
            return {
                "file": name,
                "shape": shape,
                "dtype": dtype,
                "sha256": hashlib.sha256(raw).hexdigest(),
            }

        for index, layer in enumerate(self.layers):
            if isinstance(layer, Dense):
                k, n = len(layer.weights), len(layer.weights[0])
                w = struct.pack("<%de" % (k * n), *(v for row in layer.weights for v in row))
                layers.append(
                    {
                        "op": "Dense",
                        "activation": layer.activation,
                        "weights": tensor("weights/%d.f16" % index, [k, n], "f16", w),
                        "bias": tensor("weights/%d.f32" % index, [n], "f32", _fp32(layer.bias)),
                    }
                )
            elif isinstance(layer, ReLU):
                layers.append({"op": "ReLU"})
            else:
                layers.append({"op": "Clip", "minimum": layer.minimum, "maximum": layer.maximum})
        manifest = {
            "format": "framenpu-model",
            "version": 1,
            "name": self.name,
            "input_features": self.input_features,
            "layers": layers,
        }
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:

            def write_member(name, raw):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.external_attr = 0o100644 << 16
                archive.writestr(info, raw)

            write_member("model.json", json.dumps(manifest, sort_keys=True).encode())
            for name, raw in tensors.items():
                write_member(name, raw)

    @classmethod
    def load(cls, path):
        try:
            if Path(path).stat().st_size > 32 * 1024 * 1024:
                raise InvalidModel("model file exceeds 32 MiB")
            with zipfile.ZipFile(path) as archive:
                infos = archive.infolist()
                if len(infos) > 66 or sum(i.file_size for i in infos) > 16 * 1024 * 1024:
                    raise InvalidModel("model archive exceeds limits")
                names = {i.filename for i in infos}
                if len(names) != len(infos) or "model.json" not in names:
                    raise InvalidModel("duplicate members or missing manifest")
                if archive.getinfo("model.json").file_size > 65536:
                    raise InvalidModel("manifest exceeds 64 KiB")
                manifest = json.loads(archive.read("model.json"))
                if (
                    not isinstance(manifest, dict)
                    or manifest.get("format") != "framenpu-model"
                    or type(manifest.get("version")) is not int
                    or manifest["version"] != 1
                ):
                    raise InvalidModel("unsupported model format/version")
                layers = manifest["layers"]
                if not isinstance(layers, list) or not 0 < len(layers) <= 32:
                    raise InvalidModel("invalid layer table")
                used = {"model.json"}

                def tensor(desc, dtype, rank):
                    if not isinstance(desc, dict):
                        raise InvalidModel("tensor descriptor must be an object")
                    shape = desc["shape"]
                    if (
                        desc["dtype"] != dtype
                        or not isinstance(shape, list)
                        or len(shape) != rank
                        or any(type(n) is not int or not 0 < n <= 256 for n in shape)
                    ):
                        raise InvalidModel("invalid tensor descriptor")
                    name = desc["file"]
                    if name not in names or name in used:
                        raise InvalidModel("missing or reused tensor member")
                    count = math.prod(shape)
                    size = count * (2 if dtype == "f16" else 4)
                    if archive.getinfo(name).file_size != size:
                        raise InvalidModel("tensor byte size differs from its shape")
                    raw = archive.read(name)
                    if hashlib.sha256(raw).hexdigest() != desc["sha256"]:
                        raise InvalidModel("tensor checksum differs")
                    values = struct.unpack("<%d%s" % (count, "e" if dtype == "f16" else "f"), raw)
                    if not all(math.isfinite(v) for v in values):
                        raise InvalidModel("nonfinite tensor values")
                    used.add(name)
                    return shape, values

                parsed = []
                for layer in layers:
                    if not isinstance(layer, dict):
                        raise InvalidModel("layer descriptor must be an object")
                    op = layer["op"]
                    if op == "Dense":
                        (k, n), w = tensor(layer["weights"], "f16", 2)
                        (bn,), b = tensor(layer["bias"], "f32", 1)
                        if bn != n:
                            raise InvalidModel("dense bias dimensions differ")
                        parsed.append(
                            Dense(
                                [w[i * n : (i + 1) * n] for i in range(k)],
                                b,
                                layer.get("activation", "none"),
                            )
                        )
                    elif op == "ReLU":
                        parsed.append(ReLU())
                    elif op == "Clip":
                        parsed.append(Clip(layer["minimum"], layer["maximum"]))
                    else:
                        raise UnsupportedOperator(
                            "operator %r is not implemented by FrameNPU 0.1" % op
                        )
                if used != names:
                    raise InvalidModel("archive contains undeclared members")
                return cls(manifest["input_features"], parsed, name=manifest.get("name", "model"))
        except InvalidModel:
            raise
        except (OSError, ValueError, KeyError, TypeError, struct.error, zipfile.BadZipFile) as exc:
            raise InvalidModel("invalid FrameNPU model: %s" % exc) from exc
