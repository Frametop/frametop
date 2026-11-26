"""Synthetic conv chains for the resident HMX kernel: ops, plan, host reference. Opens no device.

A chain is one input tensor (C x H x H) and a list of ops with random weights:
  p3  dense 3x3, padding 1 (kernel kind 8: HMX with a 9-tap gather)
  pw  1x1 (kind 1: HMX)
  dw  3x3 depthwise stride 1 (kind 2: HVX qf16)
Each request times every descriptor (meta words 96.., 19.2 MHz ticks), so one request gives the
cost of the load, each op and the store.

Reuses, without editing them: npu-hands/spikes/hmx/v30a_layout.py (build_plan, native_ok; itself
adapted from framenpu _mercury_layout.py, MIT, NatTheDyke, 8c9baee) and framenpu's Op, grid and
pw_image. The kind-8 descriptor follows framenpu _mercury_layout.py's heads plan (tap3 + KIND_P3):
build_plan emits the op as a 1x1 over 9*cin inputs and `plan` rewrites its kind and pair count.
"""
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

# an installed tree (ship/hnet/mkinstall.sh) holds framenpu and spikes/hmx beside probe/; this repo reads
# them from npu-hands next to it. Not Path.home(): root runs this.
_TREE = Path(__file__).resolve().parents[1]
NPU = _TREE if (_TREE / 'framenpu/src').is_dir() else Path(__file__).resolve().parents[2] / 'npu-hands'
for p in (NPU / 'spikes/hmx', NPU / 'framenpu/src'):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))
from framenpu import mercury as M  # noqa: E402
from framenpu import _mercury_common as C  # noqa: E402
import v30a_layout as L  # noqa: E402

KIND_P3 = 8
OPTS = dict(replay=True, pw_group=8, resident=True, tiles=True, deep8=True, cbias=True, win_hi=C.WIN_HI_WIDE,
            compact=True)
CHECK = dict(s2_direct=L.S2_PHASE, pw_group=8, mheads=True, resident=True, tiles=True, deep8=True, cbias=True,
             win_hi=C.WIN_HI_WIDE)
X = 'x'


def chain(c, h, spec, seed=0):
    """spec: list of (kind, cout) with kind in p3/pw/dw (dw keeps its channel count).
    Returns a model stand-in for v30a_layout.build_plan plus the dense weights for `reference`."""
    rng = np.random.default_rng(seed)
    ops, dense, shape, cin, src = [], [], {X: (c, h, h)}, c, X
    for i, (kind, cout) in enumerate(spec):
        name, out = f'{i}.{kind}', f't{i}'
        bias = rng.uniform(0.0, 0.2, cout).astype(np.float32)
        if kind == 'p3':
            w = (rng.standard_normal((cout, cin, 3, 3)) * (1.0 / np.sqrt(9 * cin))).astype(np.float32)
            m = w.reshape(cout, cin, 9).transpose(0, 2, 1).reshape(cout, 9 * cin, 1, 1)  # tap-major K
            params = {'weight': np.ascontiguousarray(m), 'bias': bias, 'stride': (1, 1), 'pads': (0, 0, 0, 0),
                      'group': 1, 'act': 'relu6', 'taps': 9}
        elif kind == 'pw':
            w = (rng.standard_normal((cout, cin, 1, 1)) * (1.0 / np.sqrt(cin))).astype(np.float32)
            params = {'weight': w, 'bias': bias, 'stride': (1, 1), 'pads': (0, 0, 0, 0), 'group': 1, 'act': 'relu6'}
        elif kind == 'dw':
            if cout != cin:
                raise ValueError('dw keeps the channel count')
            w = (rng.standard_normal((cout, 1, 3, 3)) / 3.0).astype(np.float32)
            params = {'weight': w, 'bias': bias, 'stride': (1, 1), 'pads': (1, 1, 1, 1), 'group': cout, 'act': 'relu6'}
        else:
            raise ValueError(kind)
        ops.append(M.Op(name, 'conv', (src,), out, params, (cout, h, h)))
        dense.append((kind, w, bias))
        shape[out] = (cout, h, h)
        src, cin = out, cout
    return SimpleNamespace(dev_ops=ops, shape=shape, dense=dense, c=c, h=h, spec=list(spec))


def plan(model, batch=1):
    """The resident replay plan of a chain, every descriptor checked against the kernel's mirror."""
    saved = L.device_ops, L.shapes, L.composites
    L.device_ops, L.shapes, L.composites = (lambda m: list(m.dev_ops)), (lambda m: dict(m.shape)), (lambda m: {})
    try:
        mp = L.build_plan(model, batch=batch, **OPTS)
    finally:
        L.device_ops, L.shapes, L.composites = saved
    rows = [list(r) for r in struct.iter_unpack('<32I', mp.plan.table)]
    by_par = {}
    for op in model.dev_ops:
        if op.params.get('taps') == 9:
            by_par[op.params['weight'].shape[1] // 2] = op  # the row's cpi as emitted (9 * cin / 2)
    for r in rows:
        if r[0] == C.KIND['pw'] and r[1] in by_par and r[3] == (r[1] + 15) >> 4 and r[1] % 9 == 0 and _is_p3(model, r):
            r[0], r[1] = KIND_P3, r[1] // 9
    table = b''.join(struct.pack('<32I', *r) for r in rows)
    mp.plan.table = table
    for k in range(batch):
        for r in rows:
            if not L.native_ok(C.replay_row(r, k, mp.plan.crop_stride, mp.win_hi), **CHECK):
                raise ValueError(f'descriptor rejected: {r[:22]}')
    return mp


def _is_p3(model, row):
    """A 1x1 row belongs to a p3 op when its parameter image is that op's (matched by image length)."""
    for op in model.dev_ops:
        if op.params.get('taps') == 9:
            cout, k9 = op.params['weight'].shape[:2]
            if row[1] == k9 // 2 and row[2] == cout // 2:
                return True
    return False


def describe(model, mp):
    """Per descriptor: kind, name, MACs, HMX output tiles and load pairs (the cost model's inputs)."""
    g = L.grid(model.shape[X])
    out = [{'d': 'load', 'vectors': model.shape[X][0] // 2 * g.nbin}]
    cin = model.c
    for op, (kind, _, _) in zip(model.dev_ops, model.dense):
        cout = op.shape[0]
        px = model.h * model.h
        d = {'d': kind, 'name': op.name, 'cin': cin, 'cout': cout, 'h': model.h, 'nj': g.nj}
        if kind == 'dw':
            d |= {'macs': px * cout * 9, 'pairs_blocks': cout // 2 * g.nj}
        else:
            taps = 9 if kind == 'p3' else 1
            nk, no = -(-taps * cin // 32), -(-cout // 32)
            d |= {'macs': px * cout * cin * taps, 'out_tiles': g.nj * no, 'depth': nk,
                  'load_pairs': g.nj * no * nk, 'gather_vectors': g.nj * taps * cin // 2}
        out.append(d)
        cin = cout
    out.append({'d': 'store', 'vectors': cin // 2 * g.nj})
    return out


def _f16(x):
    return np.asarray(x, np.float16).astype(np.float32)


def reference(model, x):
    """Host float32 forward with each op's output rounded to FP16 (the device stores FP16):
    close to the device, not bit-exact (accumulation order and width differ)."""
    t = _f16(x)
    for kind, w, b in model.dense:
        c, h, _ = t.shape
        if kind == 'pw':
            y = np.einsum('oc,chw->ohw', w[:, :, 0, 0], t)
        else:
            pad = np.zeros((c, h + 2, h + 2), np.float32)
            pad[:, 1:-1, 1:-1] = t
            y = np.zeros((w.shape[0], h, h), np.float32)
            for dy in range(3):
                for dx in range(3):
                    win = pad[:, dy: dy + h, dx: dx + h]
                    if kind == 'dw':
                        y += w[:, 0, dy, dx][:, None, None] * win
                    else:
                        y += np.einsum('oc,chw->ohw', w[:, :, dy, dx], win)
        t = _f16(np.clip(y + b[:, None, None], 0.0, 6.0))
    return t


# (name, C, H, ops). Sized so tensors + scratch fit the 15,296-vector VTCM span and the parameter
# images fit the window; `python3 synth_plan.py` checks every plan on the host.
CONFIGS = [
    ('c32h112', 32, 112, [('p3', 32)]),
    ('c32h56', 32, 56, [('p3', 32), ('p3', 32), ('pw', 32), ('pw', 128), ('pw', 32), ('dw', 32), ('pw', 32)]),
    ('c64h56', 64, 56, [('p3', 64), ('p3', 64), ('pw', 64), ('dw', 64), ('pw', 64)]),
    ('c64h28', 64, 28, [('p3', 64), ('p3', 64), ('pw', 64), ('pw', 256), ('pw', 64), ('dw', 64), ('pw', 64)]),
    ('c128h28', 128, 28, [('p3', 128), ('p3', 128), ('pw', 128), ('pw', 512), ('pw', 128), ('dw', 128), ('pw', 128)]),
    ('c128h14', 128, 14, [('p3', 128), ('p3', 128), ('pw', 128), ('pw', 512), ('pw', 128), ('dw', 128), ('pw', 128)]),
    ('c192h14', 192, 14, [('p3', 192), ('p3', 192), ('pw', 192), ('pw', 768), ('pw', 192), ('dw', 192), ('pw', 192)]),
    ('c256h14', 256, 14, [('p3', 256), ('p3', 256), ('pw', 256), ('pw', 768), ('pw', 256), ('dw', 256), ('pw', 256)]),
    ('c256h7', 256, 7, [('p3', 256), ('p3', 256), ('pw', 256), ('pw', 768), ('pw', 256), ('dw', 256), ('pw', 256)]),
    ('c512h7', 512, 7, [('pw', 512), ('pw', 768), ('pw', 512), ('dw', 512), ('pw', 512)]),
]


if __name__ == '__main__':
    for name, c, h, spec in CONFIGS:
        try:
            m = chain(c, h, spec)
            mp = plan(m)
            par = sum(len(v) for v in mp.plan.fixed.values())
            print(f'{name}: ok, {mp.descriptors} descriptors, VTCM peak {mp.vtcm_peak}/{C.SPAN}, '
                  f'params {par / 1e6:.2f} MB, window end {mp.win_end:#x}')
        except Exception as e:  # noqa: BLE001
            print(f'{name}: {type(e).__name__}: {e}')
