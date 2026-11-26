"""v30a (DeeJanuz/frametop-hands-models@03d2018, the live CPU landmark model) as framenpu
mercury.Op objects, plus its host reference.

The ncnn graph (frame-hands models/student-v30-soup12/hand.ncnn.*) is read with framenpu's
loader (MIT, NatTheDyke). Rewrites, all exact in real arithmetic:

* Split/Reshape/Squeeze are aliases: every tensor is named by its root blob.
* Each residual add folds into the 1x1 that makes its first operand (a second input, as
  framenpu's Mercury lowering does).
* maxpool2d_1 (2x2 stride 2) is a "conv" op with params pool="max": the planner emits it
  as a stride-2 kind-5 op with nk 2 and no clip.
* The stem reads gray replicated to three channels (trackd nets.cpp: PIXEL_GRAY2RGB, 1/255),
  so ``stem_gray`` sums its weights over the input channels: one plane, 3x3 stride 2,
  TF-SAME (pads 0 top/left, 1 bottom/right).

The device runs ``ops`` (stem rewritten by v30a_layout) up to FINAL (672 x 7 x 7); GAP and
the four heads run on the host (``heads``).
"""
import hashlib
import os
import sys
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

F = Path(__file__).resolve().parents[2] / 'framenpu'
if str(F / 'src') not in sys.path:
    sys.path.insert(0, str(F / 'src'))
from framenpu import mercury as M  # noqa: E402
from framenpu.convolution import load_landmark_graph  # noqa: E402
from framenpu.landmark import specifications, weights_and_bias  # noqa: E402

MODEL_DIR = Path(__file__).resolve().parents[3] / 'frame-hands/models/student-v30-soup12'
WEIGHTS_SHA = 'f26bb92f'  # prefix of v30a's hand.ncnn.bin sha256
STEM, FINAL = 'convclip_0', '79'
SIZE = 224
HEADS = (('linear_42', 'out0'), ('fcsigmoid_1', 'out1'), ('fcsigmoid_0', 'out2'), ('linear_43', 'out3'))
ALIASES = ('Split', 'Reshape', 'Squeeze')


@dataclass
class V30a:
    ops: list  # mercury.Op, device order (the stem still dense, three channels)
    heads: dict  # output name -> (weight [n, 672] float32, bias [n] float32, sigmoid)
    sha256: str
    detector: bool = False
    size: int = SIZE  # input crop side: 224 as trained, or a smaller one (load(size=...))

    def op(self, name):
        for op in self.ops:
            if op.name == name:
                return op
        raise KeyError(name)


def _same_pads(k, stride):
    """(top, left, bottom, right) of TF-SAME on an even input"""
    if stride == 1:
        return (k // 2,) * 4
    lo = (k - 2) // 2
    return (lo, lo, k - 2 - lo, k - 2 - lo)


def load(model_dir=MODEL_DIR, size=SIZE):
    """size: the input crop side. The graph is fully convolutional up to the GAP, so a side whose
    every stride-2 output stays even (160: 80 40 20 10 5) runs the same weights; ncnn's shapes
    (224) are scaled."""
    model_dir = Path(model_dir)
    if size % 32 or not 64 <= size <= SIZE:
        raise ValueError(f'input side {size}')
    raw = (model_dir / 'hand.ncnn.bin').read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    if not sha.startswith(WEIGHTS_SHA):
        raise ValueError(f'not v30a: {sha[:12]}')
    g = load_landmark_graph(model_dir / 'hand.ncnn.param', model_dir / 'hand.ncnn.bin')
    specs = specifications(g)
    root, ops, made_by, heads = {}, [], {}, {}
    name = lambda b: root.get(b, b)  # noqa: E731
    for s in specs:
        op_t = s['operator']
        if op_t == 'Input':
            continue
        if op_t in ALIASES:
            for o in s['output']:
                root[o] = name(s['input'][0])
            continue
        src = [name(b) for b in s['input']]
        out = s['output'][0]
        if op_t in ('Convolution', 'ConvolutionDepthWise'):
            c_out = s['output_shape'][0]
            k = s['kernel'][0]
            stride = s['stride'][0]
            if s['kernel'] != (k, k) or s['stride'] != (stride, stride) or s['dilation'] != (1, 1):
                raise ValueError(f'{s["name"]}: unexpected geometry')
            pads = _same_pads(k, stride)
            if sorted(s['padding']) != sorted(pads):
                raise ValueError(f'{s["name"]}: padding {s["padding"]} is not TF-SAME')
            w, b = weights_and_bias(g, s)
            dw = op_t == 'ConvolutionDepthWise'
            c_in = 1 if dw else s['input_shape'][0]
            w = np.asarray(w, np.float32).reshape(c_out, c_in, k, k)
            ops.append(M.Op(s['name'], 'conv', tuple(src), out,
                            {'weight': w, 'bias': np.asarray(b, np.float32), 'stride': (stride, stride),
                             'pads': pads, 'group': c_out if dw else 1,
                             'act': 'relu6' if s['activation'] == 'clip' else None},
                            tuple(s['output_shape'])))
            made_by[out] = len(ops) - 1
        elif op_t == 'Pooling' and s['name'] == 'maxpool2d_1':
            p = s['params']
            if (p.get(0), p.get(1), p.get(2), p.get(3, 0)) != (0, 2, 2, 0):
                raise ValueError('maxpool2d_1: expected 2x2 stride-2 max pooling, no padding')
            c = s['output_shape'][0]
            ops.append(M.Op(s['name'], 'conv', tuple(src), out,
                            {'weight': np.zeros((c, 1, 2, 2), np.float32), 'bias': np.zeros(c, np.float32),
                             'stride': (2, 2), 'pads': (0, 0, 0, 0), 'group': c, 'act': None, 'pool': 'max'},
                            tuple(s['output_shape'])))
            made_by[out] = len(ops) - 1
        elif op_t == 'BinaryOp':
            if s['params'].get(0, 0) != 0 or len(src) != 2:
                raise ValueError(f'{s["name"]}: only adds')
            i = made_by.pop(src[0])
            a = ops[i]
            if a.conv_class != 'pointwise' or a.params['act'] is not None or len(a.inputs) != 1:
                raise ValueError(f'{s["name"]}: first operand is not a linear 1x1')
            if any(src[0] in o.inputs for o in ops[i + 1:]):
                raise ValueError(f'{s["name"]}: {src[0]} has other users')
            # the fused op runs where the add was (maxpool2d_1 comes after conv_12 in ncnn's order)
            del ops[i]
            made_by = {b: j - (j > i) for b, j in made_by.items()}
            ops.append(replace(a, inputs=(a.inputs[0], src[1]), output=out))
            made_by[out] = len(ops) - 1
        elif op_t == 'Pooling' and s['name'] == 'gap_0':
            if src != [FINAL]:
                raise ValueError('gap_0 does not read ' + FINAL)
        elif op_t == 'InnerProduct':
            w, b = weights_and_bias(g, s)
            n = s['output_shape'][0]
            heads[out] = (np.asarray(w, np.float32).reshape(n, -1), np.asarray(b, np.float32),
                          s['activation'] == 'sigmoid')
        else:
            raise ValueError(f'{s["name"]}: unsupported {op_t}')
    if ops[-1].output != FINAL or sorted(heads) != sorted(o for _, o in HEADS):
        raise ValueError('unexpected v30a tail')
    if size != SIZE:
        for i, op in enumerate(ops):
            c, h, w = op.shape
            if (h * size) % SIZE or (w * size) % SIZE:
                raise ValueError(f'{op.name}: {h}x{w} does not scale to a {size} input')
            ops[i] = replace(op, shape=(c, h * size // SIZE, w * size // SIZE))
    return V30a(ops, heads, sha, size=size)


def load_bytes(param, raw, size=SIZE):
    """load() from the two ncnn files' bytes (ft-npud's graph file, npud/v30a_serve.py): written to a fresh
    private directory (mkdtemp: 0700, the caller's) and read back by framenpu's loader"""
    import tempfile
    with tempfile.TemporaryDirectory(prefix='v30a-') as d:
        for name, b in (('hand.ncnn.param', param), ('hand.ncnn.bin', raw)):
            fd = os.open(Path(d) / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as f:
                f.write(b)
        return load(d, size)


def stem_gray(model):
    """The stem's weights summed over its three (identical) input channels: [24, 1, 3, 3]"""
    stem = model.op(STEM)
    w = stem.params['weight'].astype(np.float64).sum(axis=1, keepdims=True).astype(np.float32)
    return replace(stem, params=stem.params | {'weight': w})


def normalize(gray_u8):
    """S x S uint8 gray crop (S even) -> [1, S, S] float32 in 0..1 (the gray-folded stem's input)"""
    x = np.asarray(gray_u8)
    if x.ndim != 2 or x.shape[0] != x.shape[1] or x.shape[0] % 2:
        raise ValueError(f'crop shape {x.shape}')
    return (x.astype(np.float32) / 255.0)[None]


def _maxpool(x):
    c, h, w = x.shape
    return x[:, : h // 2 * 2, : w // 2 * 2].reshape(c, h // 2, 2, w // 2, 2).max(axis=(2, 4))


def step(op, t, numerics):
    """One op of ``ops`` (or a v30a_layout rewrite) on the tensors in t, as mercury._step."""
    if op.params.get('pool') == 'max':
        x = t[op.inputs[0]]
        return M._f16(_maxpool(M._f16(x))) if numerics == 'fp16' else _maxpool(x)
    return M._step(op, t, numerics, numerics)


def run(model, gray_u8, numerics='fp32', keep=False, gray_stem=True):
    """Host forward to FINAL and the heads. numerics 'fp32' (FP64 accumulation) or 'fp16'
    (framenpu's device contract). gray_stem=False runs the original three-channel stem."""
    x = normalize(gray_u8)
    ops = list(model.ops)
    if gray_stem:
        ops[0] = stem_gray(model)
    else:
        x = np.repeat(x, 3, axis=0)
    t = {'in0': M._f16(x) if numerics == 'fp16' else x}
    for op in ops:
        t[op.output] = step(op, t, numerics)
    outs = finish(model, t[FINAL])
    return (outs, t) if keep else outs


def finish(model, final):
    """GAP + the four heads on the host (FP64), from the 672 x S/32 x S/32 map."""
    v = np.asarray(final, np.float64).reshape(final.shape[0], -1).mean(axis=1)
    outs = {}
    for name, (w, b, sig) in model.heads.items():
        y = w.astype(np.float64) @ v + b
        outs[name] = (1.0 / (1.0 + np.exp(-y)) if sig else y).astype(np.float32)
    return outs
