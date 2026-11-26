"""Monado Mercury keypoint model: validated lowering, host reference and FP16 contract.

The pinned model is ``grayscale_keypoint_jan18.onnx`` from
gitlab.freedesktop.org/monado/utilities/hand-tracking-models at 37a8a81b. That
repository has no license file, so the weights are never bundled or committed:
callers fetch them and this module checks the SHA256. Mercury's tracker code is
BSL-1.0 (Monado); the decoder below follows its ``hg_model.cpp`` conventions.

The graph is lowered to a short list of device-shaped operations:

* ``conv``: any convolution with optional fused ReLU6 and fused residual add.
  Classified as ``pointwise`` (1x1), ``depthwise`` or ``dense`` (k>1, group 1).
* ``deconv2x2``: the 2x2 stride-2 ConvTranspose, lowered as a pointwise layer
  to ``4 * cout`` channels followed by a pixel shuffle.
* ``dense``: Gemm with folded BatchNormalization and optional ReLU6.
* ``avgpool``, ``concat``, ``scale`` (by the ``useLastKeypoints`` scalar),
  ``reshape``.

Lowering refuses any node pattern it does not recognize, so a different model
cannot silently run through these semantics.
"""

from dataclasses import dataclass, field
import math

from . import onnx_graph
from .errors import InvalidModel, UnsupportedOperator

MODELS_REVISION = "37a8a81b"
MODELS_URL = (
    "https://gitlab.freedesktop.org/monado/utilities/hand-tracking-models/-/raw/"
    + MODELS_REVISION
    + "/{name}"
)
KEYPOINT_NAME = "grayscale_keypoint_jan18.onnx"
KEYPOINT_SHA256 = "40c0daa598cedb993b54fff17685231b7465d6db342656c401b01c2029efd1d5"
DETECTOR_NAME = "grayscale_detection_160x160.onnx"
DETECTOR_SHA256 = "1f1a039a266e13dc186bb884430ebd9c8216bdda680ab08a533d4c671f27ed36"

INPUT_SIZE = 128
GRID = 22
JOINTS = 21
INPUTS = {"inputImg": (1, 1, 128, 128), "lastKeypoints": (1, 42), "useLastKeypoints": (1,)}
OUTPUTS = {
    "heatmap_xy": (1, 21, 22, 22),
    "heatmap_depth": (1, 21, 22),
    "scalar_extras": (1, 8),
    "curls": (1, 10),
}
# Two-view 3D stereo student (distill make_stereo.py + make_stereo3d.py): Mercury's trunk on both
# cameras' crops as channels, both views' hints, the two crops' pair geometry, view-1 copies of the
# heatmap heads and a joints3d head on the trunk features. Unpinned (a retrained model) only.
S3D_INPUTS = {"inputImg": (1, 2, 128, 128), "lastKeypoints": (1, 84), "useLastKeypoints": (1,), "geom": (1, 16)}
S3D_OUTPUTS = OUTPUTS | {
    "heatmap_xy_1": (1, 21, 22, 22),
    "heatmap_depth_1": (1, 21, 22),
    "joints3d": (1, 63),
}
# Mercury's hand detector (grayscale_detection_160x160.onnx): the keypoint trunk's operators on a
# 160x160 image, then three Gemm layers to 8 values that four Slices (one Sigmoid) split per side.
DET_SIZE = 160
DET_INPUTS = {"inputImg": (1, 1, DET_SIZE, DET_SIZE)}
DET_OUTPUTS = {"hand_exists": (1, 2), "cx": (1, 2), "cy": (1, 2), "size": (1, 2)}
SIGNATURES = ((INPUTS, OUTPUTS), (S3D_INPUTS, S3D_OUTPUTS), (DET_INPUTS, DET_OUTPUTS))
BN_EPSILON_MAX = 1e-3


@dataclass
class Op:
    name: str
    kind: str
    inputs: tuple
    output: str
    params: dict = field(default_factory=dict)
    shape: tuple = ()  # output shape without batch

    @property
    def conv_class(self):
        if self.kind != "conv":
            return None
        w = self.params["weight"]
        if self.params["group"] > 1:
            return "depthwise"
        return "pointwise" if w.shape[2:] == (1, 1) else "dense"


@dataclass
class KeypointModel:
    ops: list
    sha256: str
    pinned: bool
    params: int
    inputs: dict = field(default_factory=lambda: dict(INPUTS))
    outputs: dict = field(default_factory=lambda: dict(OUTPUTS))

    @property
    def detector(self):
        """the hand detector graph (DET_INPUTS/DET_OUTPUTS), not a keypoint network"""
        return "hand_exists" in self.outputs

    @property
    def views(self):
        """crops per inference: 1 (Mercury) or 2 (the stereo student's two cameras)"""
        return self.inputs["inputImg"][1]

    def op(self, name):
        for op in self.ops:
            if op.name == name:
                return op
        raise KeyError(name)


# ---------------------------------------------------------------- lowering


def _attrs(node, allowed, required=()):
    extra = set(node.attrs) - set(allowed)
    if extra:
        raise UnsupportedOperator(f"{node.name}: unsupported attributes {sorted(extra)}")
    missing = set(required) - set(node.attrs)
    if missing:
        raise UnsupportedOperator(f"{node.name}: missing attributes {sorted(missing)}")
    return node.attrs


def _clip6(graph, node):
    if len(node.inputs) != 3 or node.attrs:
        raise UnsupportedOperator(f"{node.name}: Clip must have constant min/max inputs")
    lo = float(graph.tensor(node.inputs[1]).array())
    hi = float(graph.tensor(node.inputs[2]).array())
    if (lo, hi) != (0.0, 6.0):
        raise UnsupportedOperator(f"{node.name}: only ReLU6 clips are supported, got {lo},{hi}")


def lower(graph, *, require_pin=True, detector=False):
    """Validate a parsed ONNX graph against the Mercury keypoint structure, or with
    ``detector`` against the Mercury hand detector's."""
    import numpy as np

    pin, what = (DETECTOR_SHA256, "detector") if detector else (KEYPOINT_SHA256, "keypoint")
    if require_pin and graph.sha256 != pin:
        raise InvalidModel(f"Mercury {what} model hash differs from the pinned model")
    if graph.opset != 13:
        raise InvalidModel(f"expected opset 13, got {graph.opset}")
    sig = ({v.name: v.shape for v in graph.inputs}, {v.name: v.shape for v in graph.outputs})
    allowed = SIGNATURES[2:] if detector else SIGNATURES[:2]
    if sig not in allowed:
        known = any(sig[0] == i for i, _ in allowed)
        raise InvalidModel("Mercury %s %s differ" % (what, "outputs" if known else "inputs"))
    INS, OUTS = sig

    consumers = {}
    for node in graph.nodes:
        for name in node.inputs:
            consumers.setdefault(name, []).append(node)
    for out in OUTS:
        consumers.setdefault(out, []).append(None)

    def sole(name):
        users = consumers.get(name, [])
        return users[0] if len(users) == 1 else None

    def f32(name):
        t = graph.tensor(name)
        if t.dtype != onnx_graph.FLOAT:
            raise InvalidModel(f"{name}: expected FP32 weights")
        return t.array().astype(np.float32)

    shapes = {k: v[1:] for k, v in INS.items()}
    alias = {}  # tensor name -> lowered tensor name
    ops, consumed, scalars = [], set(), {"useLastKeypoints"}

    def src(name):
        return alias.get(name, name)

    def emit(op):
        ops.append(op)
        shapes[op.output] = op.shape

    for node in graph.nodes:
        if node.name in consumed or node.op_type == "Constant":
            continue
        t = node.op_type
        if t == "Conv":
            a = _attrs(node, {"dilations", "group", "kernel_shape", "pads", "strides"})
            if tuple(a.get("dilations", (1, 1))) != (1, 1):
                raise UnsupportedOperator(f"{node.name}: dilation")
            w = f32(node.inputs[1])
            b = f32(node.inputs[2]) if len(node.inputs) > 2 else np.zeros(w.shape[0], np.float32)
            group = a.get("group", 1)
            cin, h, wd = shapes[src(node.inputs[0])]
            if tuple(a["kernel_shape"]) != w.shape[2:] or w.shape[1] * group != cin:
                raise InvalidModel(f"{node.name}: weight shape mismatch")
            if group not in (1, cin) or (group > 1 and w.shape[0] != cin):
                raise UnsupportedOperator(f"{node.name}: grouped convolution")
            pads = tuple(a.get("pads", (0, 0, 0, 0)))
            stride = tuple(a.get("strides", (1, 1)))
            kh, kw = w.shape[2:]
            oh = (h + pads[0] + pads[2] - kh) // stride[0] + 1
            ow = (wd + pads[1] + pads[3] - kw) // stride[1] + 1
            out, act, res = node.outputs[0], None, None
            nxt = sole(out)
            if nxt is not None and nxt.op_type == "Clip":
                _clip6(graph, nxt)
                consumed.add(nxt.name)
                out, act = nxt.outputs[0], "relu6"
                nxt = sole(out)
            if act is None and nxt is not None and nxt.op_type == "Add":
                other = [x for x in nxt.inputs if x != out]
                if len(other) != 1 or shapes.get(src(other[0])) != (w.shape[0], oh, ow):
                    raise UnsupportedOperator(f"{nxt.name}: unsupported residual add")
                consumed.add(nxt.name)
                res, out = src(other[0]), nxt.outputs[0]
            params = dict(weight=w, bias=b, group=group, pads=pads, stride=stride, act=act)
            ins = (src(node.inputs[0]),) + ((res,) if res else ())
            emit(Op(node.name, "conv", ins, out, params, (w.shape[0], oh, ow)))
        elif t == "ConvTranspose":
            a = _attrs(node, {"dilations", "group", "kernel_shape", "pads", "strides"})
            w = f32(node.inputs[1])  # (cin, cout, 2, 2)
            b = f32(node.inputs[2])
            if (
                tuple(a.get("kernel_shape")) != (2, 2)
                or tuple(a.get("strides")) != (2, 2)
                or tuple(a.get("pads", (0, 0, 0, 0))) != (0, 0, 0, 0)
                or a.get("group", 1) != 1
                or tuple(a.get("dilations", (1, 1))) != (1, 1)
                or "output_padding" in a
            ):
                raise UnsupportedOperator(f"{node.name}: only 2x2 stride-2 ConvTranspose")
            cin, h, wd = shapes[src(node.inputs[0])]
            if w.shape[0] != cin:
                raise InvalidModel(f"{node.name}: weight shape mismatch")
            params = dict(weight=w, bias=b)
            emit(
                Op(
                    node.name,
                    "deconv2x2",
                    (src(node.inputs[0]),),
                    node.outputs[0],
                    params,
                    (w.shape[1], 2 * h, 2 * wd),
                )
            )
        elif t == "Gemm":
            a = _attrs(node, {"alpha", "beta", "transA", "transB"})
            if (
                a.get("alpha", 1.0),
                a.get("beta", 1.0),
                a.get("transA", 0),
                a.get("transB", 0),
            ) != (
                1.0,
                1.0,
                0,
                1,
            ):
                raise UnsupportedOperator(f"{node.name}: only y = x W^T + b")
            w, b = f32(node.inputs[1]), f32(node.inputs[2])
            (n_in,) = shapes[src(node.inputs[0])]
            if w.shape[1] != n_in or b.shape != (w.shape[0],):
                raise InvalidModel(f"{node.name}: weight shape mismatch")
            out, act = node.outputs[0], None
            nxt = sole(out)
            if nxt is not None and nxt.op_type == "BatchNormalization":
                ba = _attrs(nxt, {"epsilon", "momentum", "training_mode"})
                if ba.get("training_mode", 0) or len(nxt.outputs) != 1:
                    raise UnsupportedOperator(f"{nxt.name}: training-mode BatchNormalization")
                eps = ba.get("epsilon", 1e-5)
                if not 0 < eps <= BN_EPSILON_MAX:
                    raise InvalidModel(f"{nxt.name}: epsilon {eps}")
                gamma, beta, mean, var = (f32(x).astype(np.float64) for x in nxt.inputs[1:5])
                s = gamma / np.sqrt(var + eps)
                w = (w.astype(np.float64) * s[:, None]).astype(np.float32)
                b = ((b.astype(np.float64) - mean) * s + beta).astype(np.float32)
                consumed.add(nxt.name)
                out = nxt.outputs[0]
                nxt = sole(out)
            if nxt is not None and nxt.op_type == "Clip":
                _clip6(graph, nxt)
                consumed.add(nxt.name)
                out, act = nxt.outputs[0], "relu6"
            params = dict(weight=w, bias=b, act=act)
            emit(Op(node.name, "dense", (src(node.inputs[0]),), out, params, (w.shape[0],)))
        elif t == "Pad":
            _attrs(node, {"mode"})
            if node.attrs.get("mode", b"constant") != b"constant" or len(node.inputs) > 3:
                raise UnsupportedOperator(f"{node.name}: Pad mode")
            if any(int(v) for v in graph.tensor(node.inputs[1]).array().ravel()):
                raise UnsupportedOperator(f"{node.name}: only zero-width Pad is supported")
            alias[node.outputs[0]] = src(node.inputs[0])
        elif t == "AveragePool":
            a = _attrs(
                node,
                {"ceil_mode", "kernel_shape", "pads", "strides", "count_include_pad"},
                ("kernel_shape",),
            )
            k = tuple(a["kernel_shape"])
            s = tuple(a.get("strides", k))
            if a.get("ceil_mode", 0) or any(a.get("pads", (0, 0, 0, 0))) or s != k:
                raise UnsupportedOperator(f"{node.name}: only floor-mode, unpadded, k==stride")
            c, h, wd = shapes[src(node.inputs[0])]
            emit(
                Op(
                    node.name,
                    "avgpool",
                    (src(node.inputs[0]),),
                    node.outputs[0],
                    {"kernel": k},
                    (c, h // k[0], wd // k[1]),
                )
            )
        elif t == "Flatten":
            if _attrs(node, {"axis"}).get("axis", 1) != 1:
                raise UnsupportedOperator(f"{node.name}: Flatten axis")
            shp = shapes[src(node.inputs[0])]
            emit(
                Op(
                    node.name,
                    "reshape",
                    (src(node.inputs[0]),),
                    node.outputs[0],
                    {},
                    (math.prod(shp),),
                )
            )
        elif t == "Reshape":
            _attrs(node, {"allowzero"})
            target = [int(v) for v in graph.tensor(node.inputs[1]).array()]
            shp = shapes[src(node.inputs[0])]
            if target[0] != -1 or math.prod(target[1:]) != math.prod(shp):
                raise UnsupportedOperator(f"{node.name}: Reshape must keep batch as -1")
            emit(
                Op(
                    node.name,
                    "reshape",
                    (src(node.inputs[0]),),
                    node.outputs[0],
                    {},
                    tuple(target[1:]),
                )
            )
        elif t == "Unsqueeze":
            if src(node.inputs[0]) not in scalars:
                raise UnsupportedOperator(f"{node.name}: Unsqueeze only on the hint flag")
            alias[node.outputs[0]] = src(node.inputs[0])
        elif t == "Mul":
            a, b = (src(x) for x in node.inputs)
            if a in scalars:
                a, b = b, a
            if b not in scalars or a in scalars:
                raise UnsupportedOperator(f"{node.name}: Mul only by the hint flag")
            emit(Op(node.name, "scale", (a, b), node.outputs[0], {}, shapes[a]))
        elif t == "Concat":
            if _attrs(node, {"axis"})["axis"] != 1:
                raise UnsupportedOperator(f"{node.name}: Concat axis")
            ins = tuple(src(x) for x in node.inputs)
            parts = [shapes[x] for x in ins]
            if any(p[1:] != parts[0][1:] for p in parts):
                raise InvalidModel(f"{node.name}: Concat spatial mismatch")
            shape = (sum(p[0] for p in parts),) + parts[0][1:]
            emit(Op(node.name, "concat", ins, node.outputs[0], {}, shape))
        elif t == "Slice" and detector:
            # the detector's split of its last Gemm: constant [start, end) on axis 1, step 1
            _attrs(node, set())
            if len(node.inputs) != 5:
                raise UnsupportedOperator(f"{node.name}: Slice needs constant starts/ends/axes/steps")
            start, end, axis, step = (
                [int(v) for v in graph.tensor(x).array().ravel()] for x in node.inputs[1:]
            )
            shp = shapes[src(node.inputs[0])]
            if axis != [1] or step != [1] or len(shp) != 1 or not 0 <= start[0] < end[0] <= shp[0]:
                raise UnsupportedOperator(f"{node.name}: only a step-1 Slice of a vector")
            emit(Op(node.name, "slice", (src(node.inputs[0]),), node.outputs[0],
                    {"start": start[0], "end": end[0]}, (end[0] - start[0],)))
        elif t == "Sigmoid" and detector:
            _attrs(node, set())
            shp = shapes[src(node.inputs[0])]
            emit(Op(node.name, "sigmoid", (src(node.inputs[0]),), node.outputs[0], {}, shp))
        else:
            raise UnsupportedOperator(f"{node.name}: {t} is not part of the Mercury keypoint graph")

    produced = {op.output for op in ops}
    for out, shape in OUTS.items():
        if out not in produced or shapes[out] != shape[1:]:
            raise InvalidModel(f"output {out} not produced with shape {shape}")
    params = sum(t.count for t in graph.initializers.values())
    return KeypointModel(ops, graph.sha256, graph.sha256 == pin, params, dict(INS), dict(OUTS))


def load_keypoint(path, *, require_pin=True):
    return lower(onnx_graph.load(path), require_pin=require_pin)


def load_detector(path, *, require_pin=True):
    return lower(onnx_graph.load(path), require_pin=require_pin, detector=True)


def load_model(path, *, require_pin=True):
    """A keypoint network or the hand detector, told apart by the graph's outputs."""
    graph = onnx_graph.load(path)
    detector = {v.name for v in graph.outputs} == set(DET_OUTPUTS)
    return lower(graph, require_pin=require_pin, detector=detector)


# ---------------------------------------------------------------- numerics


def _f16(x):
    import numpy as np

    # Round once, from the exact (FP64) value: no FP32 double rounding.
    return np.asarray(x, np.float64).astype(np.float16).astype(np.float32)


def _im2col(x, kh, kw, pads, stride):
    import numpy as np

    c, h, w = x.shape
    p = np.zeros((c, h + pads[0] + pads[2], w + pads[1] + pads[3]), x.dtype)
    p[:, pads[0] : pads[0] + h, pads[1] : pads[1] + w] = x
    oh = (p.shape[1] - kh) // stride[0] + 1
    ow = (p.shape[2] - kw) // stride[1] + 1
    cols = np.empty((c, kh, kw, oh, ow), x.dtype)
    for dy in range(kh):
        for dx in range(kw):
            cols[:, dy, dx] = p[
                :, dy : dy + stride[0] * oh : stride[0], dx : dx + stride[1] * ow : stride[1]
            ]
    return cols.reshape(c * kh * kw, oh * ow), oh, ow


# Which numeric contract each op runs under in "fp16" mode.
#   hmx:  FP16 inputs and weights, FP16-rounded accumulator, FP16 bias/clip/residual.
#   hvx:  FP16 inputs/weights, FP32 tap accumulation, FP16 bias/clip (teacher depthwise).
#   fp32: FP32 arithmetic on FP16 inputs (pooling, heads, hint branch), FP16 store
#         only where the result re-enters an FP16 activation grid.
def unit(op):
    if op.kind == "conv":
        if op.conv_class == "depthwise":
            return "hvx"
        _, h, w = op.shape
        return "fp32" if (h, w) == (1, 1) else "hmx"
    if op.kind == "deconv2x2":
        return "hmx"
    if op.kind in ("dense", "avgpool"):
        return "fp32"
    return "layout"


def _conv(op, x, res, numerics):
    import numpy as np

    p = op.params
    w, b = p["weight"], p["bias"]
    cout = w.shape[0]
    u = unit(op) if numerics == "fp16" else "exact"
    if op.conv_class == "depthwise":
        c, h, wd = x.shape
        kh, kw = w.shape[2:]
        pads, stride = p["pads"], p["stride"]
        pad = np.zeros((c, h + pads[0] + pads[2], wd + pads[1] + pads[3]), np.float32)
        pad[:, pads[0] : pads[0] + h, pads[1] : pads[1] + wd] = x
        oh, ow = op.shape[1:]
        wt = _f16(w) if u == "hvx" else w
        acc = np.zeros((c, oh, ow), np.float64 if u == "exact" else np.float32)
        for dy in range(kh):
            for dx in range(kw):
                tap = pad[
                    :, dy : dy + stride[0] * oh : stride[0], dx : dx + stride[1] * ow : stride[1]
                ]
                acc = acc + wt[:, 0, dy, dx, None, None] * tap
        if u == "hvx":
            v = _f16(_f16(acc) + _f16(b)[:, None, None])
        else:
            v = (acc + b[:, None, None]).astype(np.float32)
    else:
        kh, kw = w.shape[2:]
        cols, oh, ow = _im2col(x, kh, kw, p["pads"], p["stride"])
        wm = w.reshape(cout, -1)
        if u == "hmx":
            acc = _f16(_f16(wm).astype(np.float64) @ cols.astype(np.float64))
            v = _f16(acc + _f16(b)[:, None])
        elif u == "fp32":
            v = (wm.astype(np.float32) @ cols + b[:, None]).astype(np.float32)
        else:
            v = (wm.astype(np.float64) @ cols.astype(np.float64) + b[:, None]).astype(np.float32)
        v = v.reshape(cout, oh, ow)
    if p["act"] == "relu6":
        v = np.clip(v, 0, 6)
    if res is not None:
        v = _f16(v + res) if u in ("hmx", "hvx") else (v + res).astype(np.float32)
    return v


def _deconv(op, x, numerics):
    import numpy as np

    w, b = op.params["weight"], op.params["bias"]  # (cin, cout, 2, 2)
    cin, h, wd = x.shape
    cout = w.shape[1]
    # Pointwise to (cout, dy, dx) channels, then pixel shuffle.
    wm = w.transpose(1, 2, 3, 0).reshape(cout * 4, cin)
    bm = np.repeat(b, 4)
    xs = x.reshape(cin, -1)
    if numerics == "fp16":
        v = _f16(_f16(_f16(wm).astype(np.float64) @ xs.astype(np.float64)) + _f16(bm)[:, None])
    else:
        v = (wm.astype(np.float64) @ xs.astype(np.float64) + bm[:, None]).astype(np.float32)
    v = v.reshape(cout, 2, 2, h, wd).transpose(0, 3, 1, 4, 2).reshape(cout, 2 * h, 2 * wd)
    return v


def run(model, feeds, numerics="fp32", keep=False, host_prefix=None):
    """Evaluate the lowered graph for one crop.

    ``feeds`` maps ONNX input names to arrays (batch dimension optional).
    ``numerics`` is ``"fp32"`` (exact reference, FP64 accumulation) or
    ``"fp16"`` (device contract). Returns the model's outputs with batch 1, and
    with ``keep=True`` also every intermediate tensor.

    ``host_prefix`` names the last op of an FP32 host prefix: ops up to and
    including it run as ``"fp32"``, and FP16 ops after it round their inputs
    to FP16 (the staging of a CPU prefix output into the device grid).
    """
    import numpy as np

    if numerics not in ("fp32", "fp16"):
        raise ValueError("numerics must be fp32 or fp16")
    ins = {k: np.asarray(feeds[k], np.float32).reshape(v[1:] if len(v) > 1 else v) for k, v in model.inputs.items()}
    if not all(np.isfinite(v).all() for v in ins.values()):
        raise ValueError("non-finite input")
    img = ins.pop("inputImg")
    if host_prefix is not None and host_prefix not in {op.name for op in model.ops}:
        raise ValueError(f"unknown host_prefix op {host_prefix!r}")
    mode = "fp32" if host_prefix is not None else numerics
    t = {"inputImg": _f16(img) if mode == "fp16" else img, **ins}
    for op in model.ops:
        t[op.output] = _step(op, t, mode, numerics)
        if op.name == host_prefix:
            mode = numerics
    outs = {k: t[k].reshape(v).astype(np.float32) for k, v in model.outputs.items()}
    return (outs, t) if keep else outs


_W64 = {}


def _float64(w):
    """FP64 copy of a dense weight, converted once per weight array (the hint Gemm alone is
    344K values, converted on every request otherwise). Keyed by identity; the entry keeps
    the source array alive, so an id is never reused while cached."""
    import numpy as np

    hit = _W64.get(id(w))
    if hit is None or hit[0] is not w:
        if len(_W64) > 64:
            _W64.clear()
        hit = _W64[id(w)] = (w, np.asarray(w, np.float64))
        hit[1].setflags(write=False)
    return hit[1]


def _step(op, t, mode, numerics):
    """One lowered op on the tensors in ``t`` (``mode`` is the op's arithmetic)."""
    import numpy as np

    x = t[op.inputs[0]]
    res = t[op.inputs[1]] if op.kind == "conv" and len(op.inputs) > 1 else None
    if mode == "fp16" and op.kind in ("conv", "deconv2x2") and unit(op) != "fp32":
        x = _f16(x)
        res = None if res is None else _f16(res)
    if op.kind == "conv":
        return _conv(op, x, res, mode)
    if op.kind == "deconv2x2":
        return _deconv(op, x, mode)
    if op.kind == "dense":
        w, b = op.params["weight"], op.params["bias"]
        v = (_float64(w) @ x.astype(np.float64) + b).astype(np.float32)
        return np.clip(v, 0, 6) if op.params["act"] == "relu6" else v
    if op.kind == "avgpool":
        kh, kw = op.params["kernel"]
        c, oh, ow = op.shape
        crop = x[:, : oh * kh, : ow * kw].astype(np.float64)
        return crop.reshape(c, oh, kh, ow, kw).mean(axis=(2, 4)).astype(np.float32)
    if op.kind == "reshape":
        return x.reshape(op.shape)
    if op.kind == "scale":
        v = (x * t[op.inputs[1]][0]).astype(np.float32)
        return _f16(v) if numerics == "fp16" else v  # the hint plane enters the FP16 grid
    if op.kind == "concat":
        return np.concatenate([t[i] for i in op.inputs], axis=0)
    if op.kind == "slice":
        return x[op.params["start"] : op.params["end"]]
    if op.kind == "sigmoid":
        return (1.0 / (1.0 + np.exp(-x.astype(np.float64)))).astype(np.float32)
    raise AssertionError(op.kind)  # pragma: no cover - lower() only emits the kinds above


def run_tail(model, tensors, after, numerics="fp16", skip=(), outputs=None):
    """Finish the graph on the host from given tensors: every op after ``after``
    runs with ``numerics`` and may read any tensor in ``tensors`` (for example a
    device-produced feature map). Ops named in ``skip`` are not run; ``tensors``
    must then hold whatever of their outputs is needed (for example a
    device-produced heatmap). Returns the model's outputs with batch 1, or only the
    names in ``outputs``."""
    import numpy as np

    names = [op.name for op in model.ops]
    if after not in names or any(n not in names for n in skip):
        raise ValueError(f"unknown op {after!r} or skip {skip!r}")
    t = dict(tensors)
    tail = [op for op in model.ops[names.index(after) + 1 :] if op.name not in skip]
    if outputs is not None:  # only the ops the requested outputs depend on
        want, keep = set(outputs), []
        for op in reversed(tail):
            if op.output in want:
                keep.append(op)
                want.update(op.inputs)
        tail = keep[::-1]
    for op in tail:
        t[op.output] = _step(op, t, numerics, numerics)
    names = model.outputs if outputs is None else outputs
    return {k: t[k].reshape(model.outputs[k]).astype(np.float32) for k in names}


# ---------------------------------------------------------------- decode


def _refine(h, cx, cy, kmax=10):
    """Mercury's refine_center_of_distribution (window centre of mass)."""
    import numpy as np

    kx = max(0, min(cx, min(kmax, abs(cx - GRID) - 1)))
    ky = max(0, min(cy, min(kmax, abs(cy - GRID) - 1)))
    win = h[cy - ky : cy + ky + 1, cx - kx : cx + kx + 1].astype(np.float64)
    s = win.sum()
    if s == 0:
        return np.array([cx, cy], float)
    ys, xs = np.mgrid[cy - ky : cy + ky + 1, cx - kx : cx + kx + 1]
    return np.array([(win * (xs + 0.5)).sum() / s, (win * (ys + 0.5)).sum() / s])


def decode(outputs):
    """Decode raw outputs into crop-space keypoints and scalars.

    Returns ``grid`` (21, 2) in heatmap cells, ``crop_px`` (21, 2) in 128x128
    crop pixels (unflipped), ``peak`` (21,), ``rel_depth`` (21,) in hand-size
    units, ``is_hand`` probability, ``curls`` (5,) and ``curl_var`` (5,).
    """
    import numpy as np

    hm = np.asarray(outputs["heatmap_xy"]).reshape(JOINTS, GRID, GRID)
    grid = np.zeros((JOINTS, 2))
    for i in range(JOINTS):
        row, col = divmod(int(hm[i].argmax()), GRID)
        grid[i] = _refine(hm[i], col, row)
    dep = np.clip(np.asarray(outputs["heatmap_depth"]).reshape(JOINTS, GRID), 0, 1).astype(
        np.float64
    )
    com = (dep * np.arange(GRID)).sum(1) / np.maximum(dep.sum(1), 1e-5)
    extras = np.asarray(outputs["scalar_extras"]).reshape(-1)
    curls = np.asarray(outputs["curls"]).reshape(-1)
    return {
        "grid": grid,
        "crop_px": grid * (INPUT_SIZE / GRID),
        "peak": hm.reshape(JOINTS, -1).max(1),
        "rel_depth": ((com + 0.5) / GRID - 0.5) * 3.0,
        "is_hand": 1.0 / (1.0 + math.exp(-float(extras[0]))),
        "curls": curls[:5].astype(np.float64),
        "curl_var": np.abs(curls[5:]).astype(np.float64) + 0.01,
    }


def hint_from_grid(grid, flip=False):
    """Encode decoded keypoints as the next crop's ``lastKeypoints`` input.

    Mercury feeds predicted keypoints in normalized crop coordinates
    (x right, y up, range about [-1, 1]); this inverts ``decode`` for a
    hint placed on the same crop (the stationary-hand case).
    """
    import numpy as np

    x = np.asarray(grid, np.float64)[:, 0] / 11.0 - 1.0
    y = 1.0 - np.asarray(grid, np.float64)[:, 1] / 11.0
    if flip:
        x = -x
    return np.stack([x, y], 1).reshape(1, 42).astype(np.float32)


def normalize(crop_u8):
    """Mercury's per-crop normalization: std 0.25, mean 0.5. None if flat."""
    import numpy as np

    f = np.asarray(crop_u8, np.float32) / 255.0
    sd = f.std()
    if sd == 0:
        return None
    f = f * (0.25 / sd)
    return (f + (0.5 - f.mean())).astype(np.float32)


# ---------------------------------------------------------------- inventory


def _pad32(n):
    return -(-n // 32) * 32


def inventory(model):
    """Per-op work and storage summary for planning (no execution)."""
    rows = []
    for op in model.ops:
        row = {"name": op.name, "kind": op.kind, "unit": unit(op), "output_shape": list(op.shape)}
        if op.kind == "conv":
            w = op.params["weight"]
            cout, cin_g, kh, kw = w.shape
            oh, ow = op.shape[1:]
            row.update(
                conv_class=op.conv_class,
                kernel=[kh, kw],
                stride=list(op.params["stride"]),
                pads=list(op.params["pads"]),
                act=op.params["act"],
                residual=len(op.inputs) > 1,
                macs=cout * cin_g * kh * kw * oh * ow,
                weight_values=w.size + cout,
            )
            if op.conv_class != "depthwise":
                k = cin_g * kh * kw
                row["padded_hmx_macs"] = _pad32(cout) * _pad32(k) * _pad32(oh * ow)
        elif op.kind == "deconv2x2":
            w = op.params["weight"]
            cin, cout = w.shape[:2]
            h, wd = op.shape[1] // 2, op.shape[2] // 2
            row.update(
                macs=cin * cout * 4 * h * wd,
                padded_hmx_macs=_pad32(4 * cout) * _pad32(cin) * _pad32(h * wd),
                weight_values=w.size + cout,
            )
        elif op.kind == "dense":
            w = op.params["weight"]
            row.update(macs=w.size, weight_values=w.size + w.shape[0], act=op.params["act"])
        elif op.kind == "avgpool":
            row["kernel"] = list(op.params["kernel"])
        row["output_values"] = math.prod(op.shape)
        rows.append(row)
    return rows
