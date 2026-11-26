"""The INT8 student graph's integer semantics, in numpy only (no torch): the test oracle for the Hexagon kernels
(npu-hands spikes/int8/conv_v2.c and its successors) and for int8_export.py.

The integer graph is what the Hexagon kernels must reproduce bit for bit; the numpy reference here (run_npu) is
their test oracle. Every tensor has zero point 0: uint8 after a ReLU (scale amax / 255), int8 otherwise (amax /
127). Ops, in order (OUT.npz's 'graph' JSON):

  unshuffle  the u8 224 crop as a 4x4 pixel unshuffle -> 16 x 56 x 56 (channel = i * 4 + j for pixel (4y+i, 4x+j)),
             scale 1/255
  conv       w int8 [K, C, k, k] (symmetric per output channel, -127..127), bias int32 [K], stride 1 or 2, zero pad
             k // 2; acc = bias + sum(w * x) in int32; out = clamp(requant(acc, M[k], sh[k]), lo, hi) with
             lo, hi = 0, 255 (ReLU, u8 out) or -128, 127 (s8 out)
  add        out = clamp(requant(requant(a << 20, Ma, sa) + requant(b << 20, Mb, sb), Mo, so), 0, 255)
             (ReLU; nearest x2 upsampling of a first when 'up')
  fuse       (two-view students, two_view.py mode f) the partner view's map as a per-channel bias. Crops come
             in pairs (crop c's partner is c ^ 1) and has2[c] says whether c's partner is real. With p the
             partner's src tensor: acc[o] = sum_i w[o, i] * sum_cells p[i] (w int8 [C, C], symmetric per output
             channel); bt[o] = requant(acc[o], M[o], sh[o]) when has2[c], else 0; out = clamp(requant(sat32(
             requant(x << 20, Ma, sa) + bt[o]), Mo, so), 0, 255) (sat32: the int32 saturating add HVX does)
  outputs    heat s8 [21, 28, 28], kp_feat s8 [kpd, 28, 28], pool u8 [g, 7, 7] (the CPU means it); a two-view
             graph also pool_pre, the stride-32 map before the fuse (its partner's heads read its mean)

requant(x, M, sh): M a Q31 multiplier (2^30 <= M < 2^31), sh its power-of-two exponent (real = M / 2^31 * 2^sh):
  y = srdhm(x << max(sh, 0), M), then a round-half-up right shift by r = max(-sh, 0): (y +sat 2^(r-1)) >> r
srdhm(a, b) = (a * b + 2^30) >> 31 in 64 bits, saturated to int32: gemmlowp's, and exactly what HVX's
vmpye + vmpyo:<<1:rnd:sat:shift pair gives (npu-hands spikes/int8/probe_rq.c: 4096 device values incl. the edges,
and the two formulas agree on 40M random pairs). Only the final shift differs from gemmlowp (round half up: one
saturating add and one vasr).
"""
import math

import numpy as np

SHIFT_ADD = 20


def quant_mult(real):
    """real > 0 -> (M, sh): real ~= M / 2^31 * 2^sh, 2^30 <= M < 2^31."""
    m, e = math.frexp(real)
    q = int(round(m * (1 << 31)))
    if q == 1 << 31:
        q //= 2
        e += 1
    return q, e


def srdhm(a, b):
    ab = a.astype(np.int64) * np.asarray(b, np.int64)
    return np.clip((ab + (1 << 30)) >> 31, -2 ** 31, 2 ** 31 - 1)


def requant(x, M, sh):
    """x int64/int32 array; M, sh arrays broadcast against x (per channel: shape [K, 1, 1])."""
    sh = np.asarray(sh, np.int64)
    ls, rs = np.maximum(sh, 0), np.maximum(-sh, 0)
    y = srdhm(np.clip(x.astype(np.int64) << ls, -2 ** 31, 2 ** 31 - 1), M)
    return np.clip(y + np.where(rs > 0, np.int64(1) << np.maximum(rs - 1, 0), 0), -2 ** 31, 2 ** 31 - 1) >> rs


def conv_int(x, w, b, M, sh, stride, relu):
    """x int32 [C, H, W], w int8 [K, C, k, k] -> int32 [K, H', W'] (requantized and clamped)."""
    K, C, k, _ = w.shape
    p = k // 2
    xp = np.pad(x.astype(np.int64), ((0, 0), (p, p), (p, p)))
    H, W = x.shape[1:]
    Ho, Wo = (H + 2 * p - k) // stride + 1, (W + 2 * p - k) // stride + 1
    cols = np.empty((C, k, k, Ho, Wo), np.int64)
    for dy in range(k):
        for dx in range(k):
            cols[:, dy, dx] = xp[:, dy:dy + stride * Ho:stride, dx:dx + stride * Wo:stride]
    acc = w.reshape(K, -1).astype(np.int64) @ cols.reshape(C * k * k, -1)
    assert np.abs(acc).max(initial=0) < 2 ** 31, 'int32 accumulator overflow'
    acc = acc.reshape(K, Ho, Wo) + b.astype(np.int64)[:, None, None]
    y = requant(acc, M[:, None, None], sh[:, None, None])
    return np.clip(y, 0, 255) if relu else np.clip(y, -128, 127)


def run_npu(ops, arrays, crop):
    """crop uint8 [224, 224] -> {name: int32 tensor} for every op (a one-view graph, or a two-view one alone)."""
    return run_set(ops, arrays, [crop], [0])[0]


def fuse_int(x, p, w, M, sh, Ma, sa, Mo, so, has2):
    """the fuse op (above) for one crop: x its own map, p its partner's (u8 [C, H, W]); M, sh [C]."""
    if has2:
        acc = w.astype(np.int64) @ p.reshape(p.shape[0], -1).astype(np.int64).sum(1)
        assert np.abs(acc).max(initial=0) < 2 ** 31, 'int32 accumulator overflow'
        bt = requant(acc, M, sh)[:, None, None]
    else:
        bt = 0
    s = np.clip(requant(x.astype(np.int64) << SHIFT_ADD, Ma, sa) + bt, -2 ** 31, 2 ** 31 - 1)
    return np.clip(requant(s, Mo, so), 0, 255)


def run_set(ops, arrays, crops, has2):
    """a set's crops (pairs: crop c's partner is c ^ 1) and has2 per crop -> [{name: int32 tensor}] per crop. Every
    op but fuse reads its own crop only."""
    ts = [{} for _ in crops]
    for o in ops:
        for c, (t, crop) in enumerate(zip(ts, crops)):
            if o['op'] == 'unshuffle':
                r = o['r']
                H, W = crop.shape
                t['in'] = crop.astype(np.int32).reshape(H // r, r, W // r, r).transpose(1, 3, 0, 2).reshape(r * r, H // r, W // r)
            elif o['op'] == 'conv':
                n = o['name']
                t[n] = conv_int(t[o['src']], arrays[n + '.w'], arrays[n + '.b'], arrays[n + '.M'], arrays[n + '.sh'],
                                o['stride'], o['relu'])
            elif o['op'] == 'add':
                a, b = t[o['a']], t[o['b']]
                if o['up']:
                    a = a.repeat(2, 1).repeat(2, 2)[:, :b.shape[1], :b.shape[2]]
                s = requant(a.astype(np.int64) << SHIFT_ADD, o['Ma'], o['sha']) + \
                    requant(b.astype(np.int64) << SHIFT_ADD, o['Mb'], o['shb'])
                t[o['name']] = np.clip(requant(s, o['Mo'], o['sho']), 0, 255)
            elif o['op'] == 'fuse':
                n, real = o['name'], bool(has2[c]) and (c ^ 1) < len(crops)
                p = ts[c ^ 1][o['src']] if real else None
                t[n] = fuse_int(t[o['src']], p, arrays[n + '.w'], arrays[n + '.M'], arrays[n + '.sh'], o['Ma'],
                                o['sha'], o['Mo'], o['sho'], real)
    return ts


def _features(ops, arrays, t):
    """the CPU part's inputs from one crop's integer outputs: (screen [21, 2], pool g, pre-fusion pool, k [21 kpd])"""
    sc = {o['name']: o['scale'] for o in ops if 'scale' in o}
    out = [o for o in ops if o['op'] == 'outputs'][0]
    f32 = np.float32

    def pool(nm):
        x = t[out[nm]]
        return (x.astype(f32) * f32(sc[out[nm]])).reshape(x.shape[0], -1).mean(1)
    heat = t[out['heat']].astype(f32) * f32(sc[out['heat']])
    kf = t[out['kp_feat']].astype(f32) * f32(sc[out['kp_feat']])
    g = pool('pool')
    K, H, W = heat.shape
    h = heat.reshape(K, -1)
    h = np.exp(h - h.max(1, keepdims=True))
    h /= h.sum(1, keepdims=True)
    hm = h.reshape(K, H, W)
    grid = arrays['head.grid'].astype(f32)
    screen = np.stack([(hm.sum(1) * grid[:W]).sum(1), (hm.sum(2) * grid[:H]).sum(1)], 1)
    k = (h @ kf.reshape(kf.shape[0], -1).T).reshape(-1)          # [21 * kpd], keypoint-major as the einsum
    return screen, g, pool('pool_pre') if 'pool_pre' in out else g, k


def heads(ops, arrays, t, tp=None, has2=0):
    """The CPU part in float32 numpy (student_net.StudentNet.forward after npu), from the integer outputs and the
    export's head.* arrays: (screen [21, 2] crop pixels, pres, right, world [63]). The spec for C clients.
    A two-view student (head.world_p.w present) also reads its partner's outputs tp when has2: the partner's
    pre-fusion pool and keypoint features through head.world_p, head.pres_p and head.right_p (no biases)."""
    f32 = np.float32
    screen, g, _, k = _features(ops, arrays, t)

    def lin(n, x):
        return arrays['head.%s.w' % n].astype(f32) @ x + arrays['head.%s.b' % n].astype(f32)

    hid, pres, right = lin('world0', np.concatenate([g, k])), lin('pres', g), lin('right', g)
    if has2 and tp is not None and 'head.world_p.w' in arrays:
        _, _, gp, kp = _features(ops, arrays, tp)
        hid = hid + arrays['head.world_p.w'].astype(f32) @ np.concatenate([gp, kp])
        pres = pres + arrays['head.pres_p.w'].astype(f32) @ gp
        right = right + arrays['head.right_p.w'].astype(f32) @ gp
    sig = lambda v: f32(1) / (f32(1) + np.exp(-v))
    world = lin('world1', np.maximum(hid, 0))
    return screen, float(sig(pres)[0]), float(sig(right)[0]), world
