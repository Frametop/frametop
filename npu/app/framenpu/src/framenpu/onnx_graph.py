"""Dependency-free reader for the subset of ONNX used by pinned public models.

This decodes the protobuf wire format directly. It reads models, it does not
execute them: the caller validates every node it intends to support. Only
inline tensors are accepted; external data, sparse tensors, subgraphs and
non-default operator domains are rejected.
"""

from dataclasses import dataclass, field
import hashlib
from pathlib import Path
import struct

from .errors import InvalidModel

# TensorProto.DataType values accepted here.
FLOAT, INT32, INT64, FLOAT16, DOUBLE = 1, 6, 7, 10, 11
_DTYPES = {
    FLOAT: ("<f4", 4),
    INT32: ("<i4", 4),
    INT64: ("<i8", 8),
    FLOAT16: ("<f2", 2),
    DOUBLE: ("<f8", 8),
}
MAX_MODEL_BYTES = 64 << 20


@dataclass(frozen=True)
class Tensor:
    name: str
    dtype: int
    dims: tuple
    data: bytes  # little-endian packed values

    def array(self):
        import numpy as np

        code, _ = _DTYPES[self.dtype]
        return np.frombuffer(self.data, code).reshape(self.dims).copy()

    @property
    def count(self):
        n = 1
        for d in self.dims:
            n *= d
        return n


@dataclass(frozen=True)
class Node:
    op_type: str
    name: str
    inputs: tuple
    outputs: tuple
    attrs: dict


@dataclass(frozen=True)
class ValueInfo:
    name: str
    elem_type: int
    shape: tuple  # ints, or strings for symbolic dimensions


@dataclass
class Graph:
    nodes: list
    initializers: dict
    inputs: list
    outputs: list
    opset: int
    producer: str
    sha256: str
    constants: dict = field(default_factory=dict)

    def tensor(self, name):
        """Initializer or Constant-node value by name."""
        if name in self.initializers:
            return self.initializers[name]
        if name in self.constants:
            return self.constants[name]
        raise InvalidModel(f"{name!r} is not a constant tensor")


# ---------------------------------------------------------------- wire format


def _varint(buf, pos):
    result = shift = 0
    while True:
        if pos >= len(buf):
            raise InvalidModel("truncated varint")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise InvalidModel("varint too long")


def _fields(buf):
    """Yield (field_number, wire_type, value) for one message body."""
    pos, end = 0, len(buf)
    while pos < end:
        key, pos = _varint(buf, pos)
        num, wire = key >> 3, key & 7
        if num == 0:
            raise InvalidModel("field number 0")
        if wire == 0:
            value, pos = _varint(buf, pos)
        elif wire == 1:
            if pos + 8 > end:
                raise InvalidModel("truncated fixed64")
            value, pos = buf[pos : pos + 8], pos + 8
        elif wire == 2:
            n, pos = _varint(buf, pos)
            if pos + n > end:
                raise InvalidModel("truncated length-delimited field")
            value, pos = buf[pos : pos + n], pos + n
        elif wire == 5:
            if pos + 4 > end:
                raise InvalidModel("truncated fixed32")
            value, pos = buf[pos : pos + 4], pos + 4
        else:
            raise InvalidModel(f"unsupported wire type {wire}")
        yield num, wire, value


def _signed(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def _ints(wire, value):
    if wire == 0:
        return [_signed(value)]
    out, pos = [], 0
    while pos < len(value):
        v, pos = _varint(value, pos)
        out.append(_signed(v))
    return out


def _floats(wire, value):
    if wire == 5:
        return [struct.unpack("<f", value)[0]]
    if wire != 2 or len(value) % 4:
        raise InvalidModel("bad packed float field")
    return list(struct.unpack(f"<{len(value) // 4}f", value))


def _text(value):
    try:
        return bytes(value).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise InvalidModel("invalid UTF-8 string") from exc


# ---------------------------------------------------------------- messages


def _tensor(buf):
    dims, dtype, name, raw = [], None, "", None
    typed = {INT32: [], INT64: [], FLOAT: [], DOUBLE: []}
    for num, wire, value in _fields(buf):
        if num == 1:
            dims += _ints(wire, value)
        elif num == 2:
            dtype = value
        elif num == 4:
            typed[FLOAT] += _floats(wire, value)
        elif num == 5:
            typed[INT32] += _ints(wire, value)
        elif num == 7:
            typed[INT64] += _ints(wire, value)
        elif num == 8:
            name = _text(value)
        elif num == 9:
            raw = bytes(value)
        elif num == 10:
            typed[DOUBLE] += list(struct.unpack(f"<{len(value) // 8}d", value))
        elif num in (13, 14):
            raise InvalidModel(f"tensor {name!r}: external data is not supported")
        elif num in (3, 6, 12):  # segment, string_data, doc_string
            if num == 6:
                raise InvalidModel(f"tensor {name!r}: string tensors are not supported")
    if dtype not in _DTYPES:
        raise InvalidModel(f"tensor {name!r}: unsupported data type {dtype}")
    if any(d < 0 for d in dims):
        raise InvalidModel(f"tensor {name!r}: negative dimension")
    code, size = _DTYPES[dtype]
    count = 1
    for d in dims:
        count *= d
    if raw is None:
        if dtype == FLOAT16:
            # FP16 typed storage uses int32_data holding the bit patterns.
            raw = struct.pack(f"<{len(typed[INT32])}H", *(v & 0xFFFF for v in typed[INT32]))
        else:
            fmt = {FLOAT: "f", INT32: "i", INT64: "q", DOUBLE: "d"}[dtype]
            vals = typed[dtype]
            raw = struct.pack(f"<{len(vals)}{fmt}", *vals)
    if len(raw) != count * size:
        raise InvalidModel(f"tensor {name!r}: {len(raw)} bytes for {count} values")
    return Tensor(name, dtype, tuple(dims), raw)


def _attribute(buf):
    name, kind, vals = "", None, {}
    for num, wire, value in _fields(buf):
        if num == 1:
            name = _text(value)
        elif num == 20:
            kind = value
        elif num == 2:
            vals["f"] = _floats(wire, value)[0]
        elif num == 3:
            vals["i"] = _ints(wire, value)[0]
        elif num == 4:
            vals["s"] = bytes(value)
        elif num == 5:
            vals["t"] = _tensor(value)
        elif num == 7:
            vals.setdefault("floats", []).extend(_floats(wire, value))
        elif num == 8:
            vals.setdefault("ints", []).extend(_ints(wire, value))
        elif num == 9:
            vals.setdefault("strings", []).append(bytes(value))
        elif num in (6, 10, 11, 21, 22, 23):
            raise InvalidModel(f"attribute {name!r}: graph/sparse/type attributes unsupported")
    # AttributeType: FLOAT=1 INT=2 STRING=3 TENSOR=4 FLOATS=6 INTS=7 STRINGS=8
    keys = {1: "f", 2: "i", 3: "s", 4: "t", 6: "floats", 7: "ints", 8: "strings"}
    if kind not in keys:
        raise InvalidModel(f"attribute {name!r}: unsupported type {kind}")
    value = vals.get(keys[kind], [] if kind in (6, 7, 8) else None)
    if value is None:
        raise InvalidModel(f"attribute {name!r}: missing value")
    if kind in (6, 7, 8):
        value = tuple(value)
    return name, value


def _node(buf):
    inputs, outputs, name, op, domain, attrs = [], [], "", "", "", {}
    for num, _, value in _fields(buf):
        if num == 1:
            inputs.append(_text(value))
        elif num == 2:
            outputs.append(_text(value))
        elif num == 3:
            name = _text(value)
        elif num == 4:
            op = _text(value)
        elif num == 5:
            key, val = _attribute(value)
            if key in attrs:
                raise InvalidModel(f"node {name!r}: duplicate attribute {key!r}")
            attrs[key] = val
        elif num == 7:
            domain = _text(value)
    if domain not in ("", "ai.onnx"):
        raise InvalidModel(f"node {name!r}: unsupported domain {domain!r}")
    if not op:
        raise InvalidModel("node without op_type")
    return Node(op, name, tuple(inputs), tuple(outputs), attrs)


def _value_info(buf):
    name, elem, shape = "", None, []
    for num, _, value in _fields(buf):
        if num == 1:
            name = _text(value)
        elif num == 2:
            for tnum, _, tval in _fields(value):
                if tnum != 1:
                    raise InvalidModel(f"value {name!r}: only tensor types are supported")
                for fnum, fwire, fval in _fields(tval):
                    if fnum == 1:
                        elem = fval
                    elif fnum == 2:
                        for dnum, _, dval in _fields(fval):
                            if dnum != 1:
                                continue
                            dim = None
                            for xnum, _, xval in _fields(dval):
                                if xnum == 1:
                                    dim = _signed(xval)
                                elif xnum == 2:
                                    dim = _text(xval)
                            shape.append(dim)
    return ValueInfo(name, elem, tuple(shape))


def parse(data):
    """Parse serialized ModelProto bytes into a Graph."""
    data = memoryview(bytes(data))
    if len(data) > MAX_MODEL_BYTES:
        raise InvalidModel("model too large")
    graph_buf, opset, producer = None, None, ""
    for num, _, value in _fields(data):
        if num == 7:
            if graph_buf is not None:
                raise InvalidModel("multiple graphs")
            graph_buf = value
        elif num == 8:
            domain, version = "", None
            for onum, _, oval in _fields(value):
                if onum == 1:
                    domain = _text(oval)
                elif onum == 2:
                    version = oval
            if domain in ("", "ai.onnx"):
                opset = version
            else:
                raise InvalidModel(f"unsupported opset domain {domain!r}")
        elif num == 2:
            producer = _text(value)
    if graph_buf is None or opset is None:
        raise InvalidModel("model has no graph or default opset")
    nodes, inits, inputs, outputs = [], {}, [], []
    for num, _, value in _fields(graph_buf):
        if num == 1:
            nodes.append(_node(value))
        elif num == 5:
            t = _tensor(value)
            if t.name in inits:
                raise InvalidModel(f"duplicate initializer {t.name!r}")
            inits[t.name] = t
        elif num == 11:
            inputs.append(_value_info(value))
        elif num == 12:
            outputs.append(_value_info(value))
        elif num == 15:
            raise InvalidModel("sparse initializers are not supported")
    inputs = [v for v in inputs if v.name not in inits]
    constants = {}
    for n in nodes:
        if n.op_type == "Constant":
            if set(n.attrs) != {"value"} or len(n.outputs) != 1:
                raise InvalidModel(f"constant {n.name!r}: only tensor values are supported")
            constants[n.outputs[0]] = n.attrs["value"]
    return Graph(
        nodes,
        inits,
        inputs,
        outputs,
        opset,
        producer,
        hashlib.sha256(data).hexdigest(),
        constants,
    )


def load(path):
    path = Path(path)
    if path.stat().st_size > MAX_MODEL_BYTES:
        raise InvalidModel("model too large")
    return parse(path.read_bytes())
