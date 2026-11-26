"""The H-net (train/hnet.py) as resident kernel descriptors, with synthetic weights. Opens no device.

The graph is the device half of hnet.HNet after BatchNorm folding and the RepVGG merge: per stage a
2x2 max pool (kernel kind 5, nk 2), then dense 3x3 convs (kind 8); the decoder's nearest x2 upsampling
is a regrid (kind 7, four phases), each lateral a 1x1 with the upsampled map as its residual input
(kind 1: bias, clip, then the add), each fuse a dense 3x3; the heatmap and keypoint-feature heads are
one 1x1 without a clip. The request stores that head tensor; the pooled stage-4 features (256 channels
at 7x7, 256 vectors) would be a second store, which v30a_layout.build_plan cannot express: its cost is
added from the measured store rate.

Like synth_plan, the plan comes from npu-hands/spikes/hmx/v30a_layout.build_plan (adapted there from
framenpu _mercury_layout.py, MIT, NatTheDyke), which only knows 1x1 and depthwise ops. The 3x3 convs go
in as 1x1 ops over 9*cin inputs and the upsampling as a placeholder 1x1; `plan` then rewrites those rows
(kind 8, and kind 7 as framenpu's regrid_words does for its heads) and checks every row against the
kernel's mirror.
"""
import struct
from types import SimpleNamespace

import numpy as np

import synth_plan as S
from synth_plan import C, L, M, X

KIND_RG, KIND_P3 = 7, 8

ARCHS = {
    'hx_m': dict(widths=(64, 128, 224, 224), depths=(1, 1, 1, 1), inp=224),
    'hx_a160': dict(widths=(64, 128, 256, 256), depths=(1, 1, 1, 1), inp=160),
    'hx_s': dict(widths=(64, 128, 192, 192), depths=(0, 1, 1, 0), inp=224),
    'hx_s160': dict(widths=(64, 128, 192, 192), depths=(0, 1, 1, 0), inp=160),
    # flat: three stages and one 1x1 head (84 heatmap + 32 keypoint + 64 global channels), nothing upsampled
    'hx_f': dict(flat=True, widths=(64, 128, 256), depths=(1, 1, 3), inp=224),
    'hx_f160': dict(flat=True, widths=(64, 128, 256), depths=(1, 1, 3), inp=160),
    'hx_f192': dict(flat=True, widths=(64, 128, 256), depths=(1, 1, 3), inp=192),
    'hx_f128': dict(flat=True, widths=(64, 128, 256), depths=(1, 1, 3), inp=128),
    'hx_f224w': dict(flat=True, widths=(64, 128, 224), depths=(1, 1, 3), inp=224),
    # a wider field at the same weights and MACs: five residual 3x3 at 192 channels instead of three at 256
    # (receptive field 256 px against 192; resident weights 4.4 MB against 4.8)
    'hx_fd': dict(flat=True, widths=(64, 128, 192), depths=(1, 1, 5), inp=224),
    'hx_fd160': dict(flat=True, widths=(64, 128, 192), depths=(1, 1, 5), inp=160),
    'hx_f0': dict(flat=True, widths=(64, 128, 256), depths=(0, 1, 3), inp=224),
}


def graph(arch, seed=0, cin=16, dec=64, head=54, weights=None):
    """The device ops of an arch (head: 21 heatmaps + 32 keypoint features, rounded up to a channel pair).
    weights: {op name: (weight, bias)} of a trained network (train/fold.py) instead of random ones."""
    cfg = ARCHS[arch]
    rng = np.random.default_rng(seed)
    h = cfg['inp'] // 4
    ops, ref, shape = [], [], {X: (cin, h, h)}

    def add(kind, name, ins, cout, hw, w, b, act, **extra):
        out = f't{len(ops)}'
        params = {'weight': w, 'bias': b, 'stride': (1, 1), 'pads': (0, 0, 0, 0), 'group': 1,
                  'act': 'relu6' if act else None} | extra
        ops.append(M.Op(name, 'conv', tuple(ins), out, params, (cout, hw, hw)))
        shape[out] = (cout, hw, hw)
        return out

    def p3(src, cout, name):
        c, hw, _ = shape[src]
        w = (rng.standard_normal((cout, c, 3, 3)) / np.sqrt(9 * c)).astype(np.float32)
        b = rng.uniform(0.0, 0.2, cout).astype(np.float32)
        if weights:
            w, b = (np.asarray(v, np.float32) for v in weights[name])
            if w.shape != (cout, c, 3, 3):
                raise ValueError(f'{name}: weight {w.shape}')
        m = np.ascontiguousarray(w.reshape(cout, c, 9).transpose(0, 2, 1).reshape(cout, 9 * c, 1, 1))
        out = add('p3', name, [src], cout, hw, m, b, True, taps=9)
        ref.append(('p3', (src,), out, w, b, True))
        return out

    def pw(src, cout, name, act=True, res=None):
        c, hw, _ = shape[src]
        w = (rng.standard_normal((cout, c, 1, 1)) / np.sqrt(c)).astype(np.float32)
        b = rng.uniform(0.0, 0.2, cout).astype(np.float32)
        if weights:
            w, b = (np.asarray(v, np.float32) for v in weights[name])
            if w.shape != (cout, c, 1, 1):
                raise ValueError(f'{name}: weight {w.shape}')
        ins = (src,) + ((res,) if res else ())
        out = add('pw', name, ins, cout, hw, w, b, act)
        ref.append(('pw', ins, out, w, b, act))
        return out

    def pool(src, name):
        c, hw, _ = shape[src]
        out = f't{len(ops)}'
        params = {'weight': np.zeros((c, 1, 2, 2), np.float32), 'bias': np.zeros(c, np.float32), 'stride': (2, 2),
                  'pads': (0, 0, 0, 0), 'group': c, 'act': None, 'pool': 'max'}
        ops.append(M.Op(name, 'conv', (src,), out, params, (c, hw // 2, hw // 2)))
        shape[out] = (c, hw // 2, hw // 2)
        ref.append(('pool', (src,), out, None, None, False))
        return out

    def up(src, name):
        c, hw, _ = shape[src]
        # a placeholder 1x1 for build_plan (taps 0 keeps it out of the tile layout); `plan` makes it kind 7
        out = add('up', name, [src], c, 2 * hw, np.zeros((c, c, 1, 1), np.float32), np.zeros(c, np.float32), False,
                  taps=0)
        ref.append(('up', (src,), out, None, None, False))
        return out

    t, feats = X, []
    for i, (c, n) in enumerate(zip(cfg['widths'], cfg['depths'])):
        if i:
            t = pool(t, f's{i}.pool')
        t = p3(t, c, f's{i}.conv')
        for k in range(n):
            t = p3(t, c, f's{i}.res{k}')
        feats.append(t)
    if cfg.get('flat'):
        pw(t, 180, 'head', act=False)
        return SimpleNamespace(dev_ops=ops, shape=shape, ref=ref, arch=arch, pooled=None)
    y = pw(feats[3], dec, 'top')
    y = p3(pw(feats[2], dec, 'up16.lat', res=up(y, 'up16.up')), dec, 'up16.fuse')
    y = p3(pw(feats[1], dec, 'up8.lat', res=up(y, 'up8.up')), dec, 'up8.fuse')
    pw(y, head, 'head', act=False)
    return SimpleNamespace(dev_ops=ops, shape=shape, ref=ref, arch=arch, pooled=feats[3])


def _rows(mp):
    return [list(r) for r in struct.iter_unpack('<32I', mp.plan.table)]


def plan(model, batch=1):
    saved = L.device_ops, L.shapes, L.composites
    L.device_ops, L.shapes, L.composites = (lambda m: list(m.dev_ops)), (lambda m: dict(m.shape)), (lambda m: {})
    try:
        mp = L.build_plan(model, batch=batch, **S.OPTS)
    finally:
        L.device_ops, L.shapes, L.composites = saved
    rows = _rows(mp)
    compute = [r for r in rows if r[0] in (C.KIND['pw'], C.KIND['dw'], C.KIND['s2'])]
    if len(compute) != len(model.dev_ops):
        raise ValueError('compute rows do not match the ops')
    for r, op, (kind, *_) in zip(compute, model.dev_ops, model.ref):
        if kind == 'p3':
            if r[0] != C.KIND['pw'] or r[1] % 9 or any(r[15:21]):
                raise ValueError(f'{op.name}: unexpected 3x3 row {r[:22]}')
            r[0], r[1] = KIND_P3, r[1] // 9
        elif kind == 'up':
            gin, gout = L.grid(model.shape[op.inputs[0]]), L.grid(op.shape)
            phases = [w for a in (0, 1) for b in (0, 1) for w in (gin.word(0, 0), gout.word(a, b))]
            new = [KIND_RG, op.shape[0] // 2, 4, gin.h, gin.w, gin.nbin, gout.nbin, r[11], r[12], gin.ws,
                   2 * gout.ws, 2, 0, 0, *phases]
            r[:] = new + [0] * (32 - len(new))
    mp.plan.table = b''.join(struct.pack('<32I', *r) for r in rows)
    for k in range(batch):
        for r in rows:
            row = C.replay_row(r, k, mp.plan.crop_stride, mp.win_hi)
            if not (C.regrid_ok(row) if r[0] == KIND_RG else L.native_ok(row, **S.CHECK)):
                raise ValueError(f'descriptor rejected: {r[:24]}')
    return mp


def describe(model, mp):
    """One entry per descriptor of a crop, in table order."""
    out, it = [], iter(zip(model.dev_ops, model.ref))
    for r in _rows(mp):
        if r[0] == C.KIND['load']:
            out.append({'d': 'load', 'vectors': r[10]})
        elif r[0] == C.KIND['store']:
            out.append({'d': 'store', 'vectors': r[10]})
        else:
            op, (kind, ins, *_ ) = next(it)
            cin, hw = model.shape[ins[0]][0], model.shape[ins[0]][1]
            cout, g = op.shape[0], L.grid(model.shape[ins[0]])
            d = {'d': kind, 'name': op.name, 'cin': cin, 'cout': cout, 'h': hw, 'nj': g.nj}
            if kind in ('p3', 'pw'):
                taps = 9 if kind == 'p3' else 1
                nk, no = -(-taps * cin // 32), -(-cout // 32)
                d |= {'macs': hw * hw * cout * cin * taps, 'out_tiles': g.nj * no, 'depth': nk,
                      'load_pairs': g.nj * no * nk, 'gather_vectors': g.nj * taps * cin // 2}
            out.append(d)
    return out


def _conv3(a, w):
    c, h, _ = a.shape
    pad = np.zeros((c, h + 2, h + 2), np.float32)
    pad[:, 1:-1, 1:-1] = a
    y = np.zeros((w.shape[0], h, h), np.float32)
    for dy in range(3):
        for dx in range(3):
            y += np.einsum('oc,chw->ohw', w[:, :, dy, dx], pad[:, dy: dy + h, dx: dx + h])
    return y


def reference(model, x):
    """Host float32 forward, every tensor rounded to FP16 as the device stores it."""
    t = {X: S._f16(x)}
    for kind, ins, out, w, b, act in model.ref:
        a = t[ins[0]]
        if kind == 'p3':
            y = _conv3(a, w) + b[:, None, None]
        elif kind == 'pw':
            y = np.einsum('oc,chw->ohw', w[:, :, 0, 0], a) + b[:, None, None]
        elif kind == 'pool':
            c, h, _ = a.shape
            y = a.reshape(c, h // 2, 2, h // 2, 2).max((2, 4))
        else:
            y = a.repeat(2, 1).repeat(2, 2)
        if act:
            y = np.clip(y, 0.0, 6.0)
        if len(ins) > 1:
            y = y + t[ins[1]]
        t[out] = S._f16(y)
    return t[model.dev_ops[-1].output]


CONFIGS = [(a, graph(a)) for a in ARCHS]

if __name__ == '__main__':
    import sys
    for name, m in CONFIGS:
        for batch in (1, 3):
            try:
                mp = plan(m, batch)
                par = sum(len(v) for v in mp.plan.fixed.values())
                macs = sum(d.get('macs', 0) for d in describe(m, mp))
                print(f'{name} x{batch}: ok, {mp.descriptors} descriptors, VTCM peak {mp.vtcm_peak}/{C.SPAN}, '
                      f'params {par / 1e6:.2f} MB, resident end {mp.res_end}/{C.RES_SPAN}, '
                      f'window end {mp.win_end:#x}/{C.WIN_HI_WIDE:#x}, {macs / 1e6:.0f}M MACs')
            except Exception as e:  # noqa: BLE001
                print(f'{name} x{batch}: {type(e).__name__}: {e}')
    if len(sys.argv) > 1:
        m = dict(CONFIGS)[sys.argv[1]]
        x = np.random.default_rng(7).uniform(0, 1, m.shape[X]).astype(np.float32)
        y = reference(m, x)
        print('reference', y.shape, float(y.min()), float(y.max()))
