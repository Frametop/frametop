"""run_int8_link.py LINK_DIR OUT.json [--graph G.npz] [--rounds N] --run-on-device: the whole INT8 student graph
(npu_a, or an exported int8_export.py graph) as one sealed linked image (build_int8_link.py) per frame. Root on
the Frame.

Protocol: framenpu's teacher link (tests/hardware_teacher_link.py), reused as is: lease, refusal record, the arena
saved, audited direct dispatch, the image written with a zero seal and read back, admin seal (7), the reservation
compared before and after every request, linked_completion as the validator, admin retire (8), window and
reservation restored, the arena compared with its start, DSP service and power health unchanged. No retry, no
recovery: a RecoveryRequired stops the run (recovery: spikes/int8 RESULTS.md, "Poison recovery").

Every request is planned on the host and checked by check_records (the image's check(), mirrored) before it is
sent, so the image never rejects one (a native rejection would fail linked_completion and poison the session).

Frames: an observe frame (1 crop, every tensor copied out and compared with int8_ref.run_npu, guards and halos
included), then 1, 2 and 4 crop frames (heat, kp_feat and pool out), every output compared, timed over rounds.
"""
import argparse, hashlib, json, os, struct, sys, time
from pathlib import Path
import numpy as np

# framenpu from this file's own tree (npu-hands/ here, app/ in ft-npud's root install), never a fixed home path:
# a root daemon importing this must not reach into a user's tree (npud/trust.py checks the install)
F = Path(__file__).resolve().parent.parent.parent / 'framenpu'
sys.path[:0] = [str(Path(__file__).resolve().parent), str(F / 'src'), str(F / 'tests'), str(F / 'tools')]   # also run as a script under -I
from int8_ref import quant_mult, run_npu, run_set, SHIFT_ADD   # beside this file: a symlink to the training tree's here

VT_LO, VT_HI = 0x31002000, 0x311E0000
NREC, RW = 48, 32
# int8_graph.c's VTCM layout from the top: a flags vector, the request copy (QV vectors), then the tensors over
# VT_USE (one worker) or two halves of VT_HALF (a split request: worker w's at VT_LO + w * VT_HALF)
QV = 1 + 2 * NREC
VT_USE = VT_HI - VT_LO - 128 * (QV + 1)
VT_HALF = VT_USE // 2 // 128 * 128
WIN_LO, WIN_HI = 0x1A40000, 0x1F00000
IG_MAGIC, IG_OPCODE = 0x49384752, 111
STEM, CONV, SPLIT, UPADD, OUT, FUSE = 1, 2, 3, 4, 5, 6
FUSE_N = 4      # int8_graph.c's
r128 = lambda n: (n + 127) // 128 * 128
# a request of 2+ crops packs a tensor whose map fills at most this many words of a 128-word vector block (7x7:
# 56) into one plane, the crops stacked with a zero row between them (int8_graph.c, "Packed planes")
PACK_WORDS = 64
# the DDR crop slots: 4 of SLOT x SLOT bytes; a graph's crops (crop_of: 224, or a smaller multiple of 32 such as 192)
# sit row-major at each slot's start, so a detector at 224 and landmarks at 192 share one crops area
SLOT = 224


def crop_of(ops):
    """a graph's crop size: its unshuffle op's 'crop' (192 for a student trained at 192), else 224"""
    c = int([o for o in ops if o['op'] == 'unshuffle'][0].get('crop', SLOT))
    if c % 32 or not 64 <= c <= SLOT:
        raise ValueError('crop %d: want a multiple of 32 from 64 to %d' % (c, SLOT))
    return c


def pack_rows(n, h):
    """height of n crops of h rows packed into one plane"""
    return n * (h + 1) - 1


def stacked(xs):
    """[C, h, w] crops -> [C, n*(h+1)-1, w], a zero row between crops: plane() of it is the packed plane."""
    C, h, w = xs[0].shape
    z = np.zeros((C, 1, w), xs[0].dtype)
    return np.concatenate(sum(([x, z] for x in xs), [])[:-1], 1)


def pin_of(h, w):
    return r128(2 + (h + 2) * (w + 1) + 128)


def plane(x, h, w):
    """[C, h, w] (u8 or s8 values) -> [C/4][PIN][4] bytes, the guard-column plane."""
    C = x.shape[0]
    p = np.zeros((C // 4, pin_of(h, w), 4), np.uint8)
    g = x.astype(np.int64).astype(np.uint8).reshape(C // 4, 4, h, w).transpose(0, 2, 3, 1)
    for y in range(h):
        s = 1 + (y + 1) * (w + 1) + 1
        p[:, s:s + w] = g[:, y]
    return p.tobytes()


def phases(x, h, w):
    """[C, 2h, 2w] -> the stride-2 block [4][C/4][PIN(h, w)]."""
    return b''.join(plane(np.ascontiguousarray(x[:, py::2, px::2]), h, w) for py in (0, 1) for px in (0, 1))


# ------------------------------------------------------------------ the synthetic npu_a graph

def _acc(x, w, b, st):
    K, C, k, _ = w.shape
    p = k // 2
    xp = np.pad(x.astype(np.int64), ((0, 0), (p, p), (p, p)))
    H, W = x.shape[1:]
    Ho, Wo = (H + 2 * p - k) // st + 1, (W + 2 * p - k) // st + 1
    cols = np.empty((C, k, k, Ho, Wo), np.int64)
    for dy in range(k):
        for dx in range(k):
            cols[:, dy, dx] = xp[:, dy:dy + st * Ho:st, dx:dx + st * Wo:st]
    return (w.reshape(K, -1).astype(np.int64) @ cols.reshape(C * k * k, -1)).reshape(K, Ho, Wo) + b[:, None, None]


def synthetic(seed=7, crop=SLOT, widths=(32, 64, 128), depths=(1, 1, 1), dec=32, kpd=32):
    """npu_a's ops in int8_export's format (or another student_net arch: per-stage widths and residual depths,
    decoder and keypoint-feature widths), random weights, each conv's M/sh set from its real accumulator range so
    activations spread over the u8/s8 range instead of saturating. -> (ops, arrays, crop)."""
    rng = np.random.default_rng(seed)
    side = crop
    crop = rng.integers(0, 256, (side, side), dtype=np.uint8)
    ops, arrays = [dict(op='unshuffle', name='in', r=4, scale=1 / 255)], {}
    if side != SLOT:
        ops[0]['crop'] = side
    t = run_npu(ops, arrays, crop)
    scale = {'in': 1 / 255}

    def conv(name, src, K, k, st, relu):
        x = t[src]
        C = x.shape[0]
        w = rng.integers(-127, 128, (K, C, k, k), dtype=np.int8)
        b = rng.integers(-3000, 3000, K).astype(np.int32)
        a = _acc(x, w, b, st)
        lo = np.percentile(np.abs(a), 99, axis=(1, 2)) + 1
        M, sh = zip(*(quant_mult((200 if relu else 110) / float(v)) for v in lo))
        assert max(sh) <= 0
        arrays.update({name + '.w': w, name + '.b': b, name + '.M': np.array(M, np.int32),
                       name + '.sh': np.array(sh, np.int32)})
        ops.append(dict(op='conv', name=name, src=src, k=k, stride=st, cin=C, cout=K, relu=relu, scale=1.0))
        t[name] = _one(ops[-1], arrays, t)
        scale[name] = float(rng.uniform(0.01, 0.05))
        return name

    def add(name, a, b):
        sa, sb, so = scale[a], scale[b], float(rng.uniform(0.02, 0.06))
        tm = 2 * max(sa, sb)
        (Ma, sha), (Mb, shb), (Mo, sho) = quant_mult(sa / tm), quant_mult(sb / tm), quant_mult(tm / ((1 << SHIFT_ADD) * so))
        ops.append(dict(op='add', name=name, a=a, b=b, up=True, Ma=Ma, sha=sha, Mb=Mb, shb=shb, Mo=Mo, sho=sho, scale=so))
        t[name] = _one(ops[-1], arrays, t)
        scale[name] = so
        return name

    x, f = 'in', []
    for i, (c, d) in enumerate(zip(widths, depths)):
        x = conv('s%d.0' % i, x, c, 3, 2, True)
        for j in range(d):     # a residual block after QAT's merge: one plain 3x3 + ReLU
            x = conv('s%d.%d' % (i, j + 1), x, c, 3, 1, True)
        f.append(x)
    y = conv('top', f[2], dec, 1, 1, True)
    y = add('up16', y, conv('up16.lat', f[1], dec, 1, 1, False))
    y = conv('up16.fuse', y, dec, 3, 1, True)
    y = add('up8', y, conv('up8.lat', f[0], dec, 1, 1, False))
    conv('heat', y, 21, 1, 1, False)
    conv('kp_feat', y, kpd, 1, 1, False)
    ops.append(dict(op='outputs', heat='heat', kp_feat='kp_feat', pool=f[2]))
    return ops, arrays, crop


def _one(o, arrays, t):
    """run_npu for one op on the tensors so far."""
    from int8_ref import conv_int, requant
    if o['op'] == 'conv':
        n = o['name']
        return conv_int(t[o['src']], arrays[n + '.w'], arrays[n + '.b'], arrays[n + '.M'], arrays[n + '.sh'],
                        o['stride'], o['relu'])
    a, b = t[o['a']], t[o['b']]
    a = a.repeat(2, 1).repeat(2, 2)[:, :b.shape[1], :b.shape[2]]
    s = requant(a.astype(np.int64) << SHIFT_ADD, o['Ma'], o['sha']) + requant(b.astype(np.int64) << SHIFT_ADD, o['Mb'], o['shb'])
    return np.clip(requant(s, o['Mo'], o['sho']), 0, 255)


# ------------------------------------------------------------------ the plan: DDR window, VTCM, records

def conv_scratch(T, C, K, P):
    return T * C + 4 * K + P // 32 + 4 * (P // 32) + 1 + (12 * K + 127) // 128 + K // 4 * (r32(T * C) // 32)


def fuse_scratch(C, K):
    return C + K // 4 * (r32(C) // 32) + (8 * K + 127) // 128 + 1 + 4 * FUSE_N


def pairs_mask(n, has2):
    """FUSE's has2 bits for crops 0..n-1 (has2: a mask over the set, or a list of 0/1 per crop): only crops whose
    partner c ^ 1 is in the set"""
    m = has2 if isinstance(has2, int) else sum(1 << c for c, v in enumerate(has2) if v)
    return m & sum(1 << c for c in range(n) if (c ^ 1) < n)


class Plan:
    """The graph's tensors, weights and records for n crops. Tensors live in VTCM, each n copies a crop stride
    apart, allocated first-fit over their live ranges; weights, tables, crops and outputs in the DDR window.
    Weights start at base; crops (4 slots of SLOT x SLOT) and then outputs at io_at, or right after the weights. Two graphs
    share a window (ft-npud: landmarks and detector) as weights A, weights B, then one crops and outputs area:
    Plan.pair."""

    def __init__(self, ops, arrays, base=WIN_LO, io_at=None):
        self.ops = [o for o in ops if o['op'] in ('conv', 'add', 'fuse')]
        self.two_view = any(o['op'] == 'fuse' for o in self.ops)   # crops come in pairs (c, c ^ 1)
        self.outputs = [o for o in ops if o['op'] == 'outputs'][0]
        self.out_names = [v for k, v in self.outputs.items() if k != 'op']     # in the graph's order
        self.arrays = arrays
        self.crop = crop_of(ops)
        sh = {'in': (16, self.crop // 4, self.crop // 4)}
        for o in self.ops:
            if o['op'] == 'conv':
                C, H, W = sh[o['src']]
                st = o['stride']
                sh[o['name']] = (o['cout'], H // st, W // st)
            elif o['op'] == 'fuse':
                sh[o['name']] = sh[o['src']]
            else:
                sh[o['name']] = sh[o['b']]
        self.shape = sh
        self.kpad = {o['name']: r4(o['cout']) for o in self.ops if o['op'] == 'conv'}
        # every stride-2 conv reads a phase block: 'in' from STEM, the others through a SPLIT
        self.s2src = {o['src'] for o in self.ops if o['op'] == 'conv' and o['stride'] == 2}
        # the DDR window: per conv its padded weights and table
        cur, self.ddr, self.blobs = base, {}, []
        for o in self.ops:
            if o['op'] == 'fuse':       # a k = 1 conv's weight layout, then its [K M][K sh] table
                n = o['name']
                w = arrays[n + '.w']
                K, C = w.shape
                assert K == C and not C % 4, 'fuse: a square weight of 4k channels'
                g = np.ascontiguousarray(w.reshape(K // 4, 4, C // 4, 4).transpose(0, 2, 1, 3)).reshape(K // 4, C * 4)
                g = np.concatenate([g, np.zeros((K // 4, (r32(C) - C) * 4), np.int8)], 1)
                tbl = arrays[n + '.M'].astype(np.int32).tobytes() + arrays[n + '.sh'].astype(np.int32).tobytes()
                for key, data in ((n + '.w', g.tobytes()), (n + '.q', tbl + bytes(r128(len(tbl)) - len(tbl)))):
                    self.ddr[key] = cur
                    self.blobs.append((cur, data))
                    cur += r128(len(data))
                continue
            if o['op'] != 'conv':
                continue
            n, K, k = o['name'], self.kpad[o['name']], o['k']
            w = arrays[n + '.w']
            C, T = w.shape[1], k * k
            wp = np.zeros((K, C, k, k), np.int8); wp[:w.shape[0]] = w
            G = T * C
            g = np.ascontiguousarray(wp.reshape(K // 4, 4, C // 4, 4, T).transpose(0, 4, 2, 1, 3)).reshape(K // 4, G * 4)
            g = np.concatenate([g, np.zeros((K // 4, (r32(G) - G) * 4), np.int8)], 1)
            b = np.zeros(K, np.int32); b[:w.shape[0]] = arrays[n + '.b']
            M = np.full(K, 1 << 30, np.int32); M[:w.shape[0]] = arrays[n + '.M']
            s = np.full(K, -1, np.int32); s[:w.shape[0]] = arrays[n + '.sh']
            tbl = b.tobytes() + M.tobytes() + s.tobytes()
            for key, data in ((n + '.w', g.tobytes()), (n + '.q', tbl + bytes(r128(len(tbl)) - len(tbl)))):
                self.ddr[key] = cur
                self.blobs.append((cur, data))
                cur += r128(len(data))
        self.weights_end = cur
        assert io_at is None or io_at >= cur, 'the crops would overlap the weights'
        self.crops_at = cur if io_at is None else io_at
        self.ddr_free = self.crops_at + 4 * SLOT * SLOT
        assert self.ddr_free <= WIN_HI

    @classmethod
    def pair(cls, a, b):
        """two graphs ((ops, arrays) each) in one window: their weights back to back, one crops and outputs area"""
        wa = cls(*a).weights_end
        io = cls(*b, base=wa).weights_end
        return cls(*a, io_at=io), cls(*b, base=wa, io_at=io)

    def vt_bytes(self, name):
        C, H, W = self.shape[name]
        return 4 * (C // 4) * pin_of(H // 2, W // 2) * 4 if name in self.s2src else r4(C) // 4 * pin_of(H, W) * 4

    def map_of(self, nm):
        """a tensor's (planes, h, w) a crop: a phase copy NAME/2 has 4 planes a channel group at half size"""
        base = nm.split('/')[0]
        C, H, W = self.shape[base]
        if nm == 'in' or nm.endswith('/2'):
            return 4 * (C // 4), H // 2, W // 2
        return r4(C) // 4, H, W

    def packed(self, nm, n):
        """whether a request of n crops packs tensor nm into one plane ('in' never: STEM writes per crop)"""
        _, h, w = self.map_of(nm)
        return n >= 2 and nm != 'in' and h * (w + 1) <= PACK_WORDS

    def tensor_bytes(self, nm, n):
        """VTCM bytes of tensor nm for n crops, and its crop stride (0 when packed)"""
        pl, h, w = self.map_of(nm)
        if self.packed(nm, n):
            return pl * pin_of(pack_rows(n, h), w) * 4, 0
        one = pl * pin_of(h, w) * 4
        return one * n, one

    def records(self, n, observe):
        """-> (header + records bytes, {tensor: VTCM offset}, {output: (DDR offset, bytes)}, ops); crop c's
        tensors are a crop stride (its bytes) apart, except a packed tensor (Plan.packed), one plane of every crop."""
        recs, where, ddr, scratch, _ = self.section(n, observe)
        head = struct.pack('<4I', IG_MAGIC, len(recs), 0, scratch) + bytes(112)
        data = head + b''.join(struct.pack('<32I', *r) for r in recs)
        check_records(data)
        return data, where, ddr, [r[0] for r in recs]

    def split(self, n0, n1, nonce=1):
        """a two-worker request: worker 0 runs crops 0..n0-1 over the lower VTCM half, worker 1 crops n0..n0+n1-1
        over the upper one (int8_graph.c ig_run) -> (bytes, (where0, where1), (ddr0, ddr1), ops0 + ops1); worker
        1's outputs follow worker 0's in DDR. set_nonce() gives each request its own nonce."""
        assert not self.two_view or n0 % 2 == 0, 'a two-view split keeps each pair on one worker'
        r0, w0, d0, s0, end = self.section(n0, False, VT_HALF)
        r1, w1, d1, s1, _ = self.section(n1, False, VT_HALF, crop0=n0, out_at=end)
        assert len(r0) <= NREC and len(r1) <= NREC
        head = struct.pack('<8I', IG_MAGIC, len(r0), 0, s0, len(r1), nonce, 0, s1) + bytes(96)
        data = head + b''.join(struct.pack('<32I', *r) for r in r0 + r1)
        check_records(data)
        return data, (w0, w1), (d0, d1), [r[0] for r in r0 + r1]

    def section(self, n, observe, use=VT_USE, crop0=0, out_at=None):
        """one worker's records for n crops (from crop slot crop0, outputs from out_at or ddr_free) over a VTCM
        span of use bytes -> (records, where, ddr, scratch bytes, end of its outputs)"""
        recs, steps = [], []          # steps: (record, reads, writes)
        sh = self.shape
        scratch = 64 * 28 * 28 + 128
        for o in self.ops:
            if o['op'] == 'conv':
                C, H, W = sh[o['src']]
                k = o['k']
                h, w = sh[o['name']][1:]
                if self.packed(o['name'], n):
                    h = pack_rows(n, h)
                P = r128(h * (w + 1))
                scratch = max(scratch, conv_scratch(k * k, C, self.kpad[o['name']], P) * 128)
            elif o['op'] == 'fuse':
                C = sh[o['src']][0]
                scratch = max(scratch, fuse_scratch(C, C) * 128)
        scratch = r128(scratch)
        # tensors: 'in' (the phase block STEM writes), every op's output, and a phase copy (NAME/2) of each
        # stride-2 source other than 'in'
        order = [('stem', 'in')]
        for o in self.ops:
            if o['op'] == 'conv' and o['stride'] == 2 and o['src'] != 'in':
                order.append(('split', o['src']))
            order.append(('op', o))
        size, reads = {}, {}
        life = {}
        for i, (kind, x) in enumerate(order):
            if kind == 'stem':
                life['in'] = [i, i]
            elif kind == 'split':
                C, H, W = sh[x]
                life[x + '/2'] = [i, i]
                life[x][1] = i
            else:
                o = x
                nm = o['name']
                C, H, W = sh[nm]
                life[nm] = [i, i]
                srcs = [o['src']] if o['op'] in ('conv', 'fuse') else [o['a'], o['b']]
                for s in srcs:
                    s = s + '/2' if o['op'] == 'conv' and o['stride'] == 2 and s != 'in' else s
                    life[s][1] = i
        outs = list(life) if observe else self.out_names
        for nm in outs:
            life[nm][1] = len(order)
        # first fit by start time
        placed = []
        where = {}
        stride = {}
        for nm in life:
            size[nm], stride[nm] = self.tensor_bytes(nm, n)
        for nm in sorted(life, key=lambda q: life[q][0]):
            need = size[nm]
            s0, e0 = life[nm]
            busy = sorted((a, a + b) for (a, b, s, e) in placed if not (e < s0 or s > e0))
            off = scratch
            for a, b in busy:
                if off + need <= a:
                    break
                off = max(off, b)
            assert off + need <= use, 'VTCM plan overflows at %s (%d crops)' % (nm, n)
            where[nm] = off
            placed.append((off, need, s0, e0))
        rec = lambda **f: _record(n=n, **f)
        for kind, x in order:
            if kind == 'stem':
                recs.append(rec(op=STEM, h=self.crop // 8, w=self.crop // 8, C=16, dst=where['in'],
                                dst_s=stride['in'], ddr=self.crops_at + crop0 * SLOT * SLOT, ddr_s=SLOT * SLOT))
            elif kind == 'split':
                C, H, W = sh[x]
                pk = n if self.packed(x + '/2', n) else 0
                assert not self.packed(x, n)
                recs.append(rec(op=SPLIT, h=H // 2, w=W // 2, C=C, src=where[x], src_s=stride[x], dst=where[x + '/2'],
                                dst_s=stride[x + '/2'], pk=pk))
            elif x['op'] == 'conv':
                o, nm = x, x['name']
                src = o['src'] if o['src'] == 'in' or o['stride'] == 1 else o['src'] + '/2'
                C = sh[o['src']][0]
                h, w = sh[nm][1:]
                pk = self.packed(nm, n)
                assert pk == self.packed(src, n), 'a packed conv reads and writes packed planes (%s)' % nm
                recs.append(_record(n=1 if pk else n, op=CONV, h=pack_rows(n, h) if pk else h, w=w, C=C,
                                    K=self.kpad[nm], k=o['k'], st=o['stride'], sgn=int(not o['relu']),
                                    src=where[src], src_s=stride[src], dst=where[nm], dst_s=stride[nm],
                                    ddr=self.ddr[nm + '.w'], q=self.ddr[nm + '.q'], pk=n if pk else 0, hc=h if pk else 0))
            elif x['op'] == 'fuse':
                o, nm = x, x['name']
                C, h, w = sh[nm]
                pk = self.packed(nm, n)
                assert pk == self.packed(o['src'], n)
                recs.append(rec(op=FUSE, h=h, w=w, C=C, K=C, src=where[o['src']], src_s=stride[o['src']], dst=where[nm],
                                dst_s=stride[nm], ddr=self.ddr[nm + '.w'], q=self.ddr[nm + '.q'],
                                rq=[o['Ma'], o['sha'], 0, 0, o['Mo'], o['sho']], pk=n if pk else 0))
            else:
                o, nm = x, x['name']
                C, h, w = sh[nm]
                assert not self.packed(nm, n) and not self.packed(o['b'], n)
                rq = [o['Ma'], o['sha'], o['Mb'], o['shb'], o['Mo'], o['sho']]
                recs.append(rec(op=UPADD, h=h, w=w, C=C, src=where[o['a']], src_s=stride[o['a']], dst=where[nm],
                                dst_s=stride[nm], b=where[o['b']], b_s=stride[o['b']], rq=rq,
                                pk=n if self.packed(o['a'], n) else 0))
        # ddr[nm] = (offset, bytes a crop); a packed output is one plane of every crop: (offset, its bytes)
        ddr, cur = {}, self.ddr_free if out_at is None else out_at
        for nm in outs:
            if stride[nm]:
                ddr[nm] = (cur, stride[nm])
                recs.append(rec(op=OUT, src=where[nm], src_s=stride[nm], ddr=cur, ddr_s=stride[nm], q=stride[nm],
                                h=1, w=1, C=4))
            else:
                ddr[nm] = (cur, size[nm])
                recs.append(_record(n=1, op=OUT, src=where[nm], ddr=cur, q=size[nm], h=1, w=1, C=4))
            cur += size[nm] if not stride[nm] else stride[nm] * n
        assert cur <= WIN_HI and len(recs) <= NREC, (hex(cur), len(recs))
        return recs, where, ddr, scratch, cur


GO, STARTED, DONE, DONE_FAULT = 0x474f4f47, 0x53545254, 0x444f4e45, 0xfa017ed1   # int8_graph.c's flags


def nonce_ok(x):
    """int8_graph.c nonce_ok(): never 0, nor a value whose flag is a resting 0 or reads as the fault mark"""
    return 0 < x <= 0xFFFFFFFF and x not in (GO, STARTED, DONE, DONE ^ DONE_FAULT)


def next_nonce(x):
    """the nonce after x (any u32): the next one nonce_ok() accepts"""
    while True:
        x = x % 0xFFFFFFFF + 1
        if nonce_ok(x):
            return x


def halves(n):
    """a split request's crops: worker 0's, worker 1's"""
    return (n + 1) // 2, n // 2


def meta_ok(output, n, n1):
    """the image's 256-byte metadata for a request of n worker-0 records and n1 worker-1 ones: [2] the records
    worker 1 ran (short after its fault), [3] its ticks, [8 + i] worker 0's record ticks, the rest 0"""
    m = struct.unpack('<64I', output)
    return (m[0] == IG_MAGIC and m[1] == n and m[2] == n1 and bool(m[3]) == bool(n1) and
            not any(m[4:8]) and not any(m[8 + n:]))


def set_nonce(data, nonce):
    """a split request's bytes with its nonce (header word 5) set: every request its own (nonce_ok)"""
    assert struct.unpack_from('<I', data, 16)[0] and nonce_ok(nonce)
    return data[:20] + struct.pack('<I', nonce) + data[24:]


def r4(n):
    return (n + 3) // 4 * 4


def r32(n):
    return (n + 31) // 32 * 32


def _record(*, op, n, h=0, w=0, C=0, K=0, k=0, st=0, sgn=0, src=0, dst=0, ddr=0, q=0, b=0, src_s=0, dst_s=0,
            b_s=0, rq=(0,) * 6, ddr_s=0, pk=0, hc=0, has2=0):
    r = [op, h | (w << 16), C, K, k | (st << 4) | (sgn << 8), src, dst, ddr, q, b, n, src_s, dst_s, b_s,
         *[v & 0xFFFFFFFF for v in rq], ddr_s, pk, hc, has2] + [0] * 8
    return r


def set_has2(data, has2):
    """a request's bytes with every FUSE record's has2 bits (int8_graph.c record [23]) for this frame: has2 a mask
    over the request's crops (bit c: crop c's partner c ^ 1 is real). Worker 1's crops of a split request follow
    worker 0's, whose count is its first record's (STEM)."""
    q = list(struct.unpack('<%dI' % (len(data) // 4), data))
    n0, n1 = q[1], q[4]
    for i in range(n0 + n1):
        at = RW * (i + 1)
        if q[at] == FUSE:
            c0 = 0 if i < n0 else q[RW + 10]
            q[at + 23] = pairs_mask(q[at + 10], has2 >> c0)
    out = struct.pack('<%dI' % len(q), *q)
    check_records(out)
    return out


def check_records(data):
    """int8_graph.c's header and check(), mirrored: refuse anything the image would reject."""
    q = struct.unpack('<%dI' % (len(data) // 4), data)
    n0, scr, scr_b, n1, nonce, scr1, scr1_b = q[1:8]
    vt_span = VT_HALF if n1 else VT_USE     # every record's VTCM extent: its own worker's span
    n = n0 + n1
    assert q[0] == IG_MAGIC and 0 < n0 <= NREC and n1 <= NREC and len(data) == 128 * (n + 1), 'header'
    assert not (scr | scr_b | scr1 | scr1_b) & 127 and scr <= vt_span and scr_b <= vt_span - scr, 'header'
    assert ((nonce_ok(nonce) and scr1 <= vt_span and scr1_b <= vt_span - scr1) if n1 else not (nonce or scr1 or scr1_b)), 'header'
    assert not any(q[8:RW]), 'header'

    def fits(off, n, stride, size, base, span):
        if (off | stride | size) & 127 or off < base or off - base > span or n > 4 or stride > span:
            return False
        end = off - base + (n - 1) * stride
        return end <= span and size <= span - end
    for i in range(n):
        r = q[RW * (i + 1):RW * (i + 2)]
        scr_b = q[3] if i < n0 else scr1_b
        op, h, w, C, K, nc = r[0], r[1] & 0xFFFF, r[1] >> 16, r[2], r[3], r[10]
        k, st = r[4] & 15, (r[4] >> 4) & 15
        pk, hc = r[21], r[22]
        ok = (0 < h <= 64 and 0 < w <= 64 and 0 < nc <= 4 and not C % 4 and 0 < C <= 512 and
              not any(r[24 if op == FUSE else 23:]) and
              (not pk or (2 <= pk <= 4 and op in (CONV, SPLIT, UPADD, FUSE))) and (not hc or (op == CONV and pk)))
        PO, C4 = pin_of(h, w), C // 4
        if ok and op == STEM:
            ok = (C == 16 and 2 * w <= 64 and fits(r[7], nc, r[20], r128(64 * h * w), WIN_LO, WIN_HI - WIN_LO) and
                  fits(r[6], nc, r[12], 4 * C4 * PO * 4, 0, vt_span) and 64 * h * w + 128 <= scr_b)
        elif ok and op == CONV:
            T, P = k * k, r128(h * (w + 1))
            G = T * C
            ok = (K and not K % 4 and K <= 128 and k in (1, 3) and st in (1, 2) and not (k == 1 and st != 1) and
                  not r[4] >> 9 and conv_scratch(T, C, K, P) * 128 <= scr_b and
                  fits(r[5], nc, r[11], (4 if st == 2 else 1) * C4 * PO * 4, 0, vt_span) and
                  fits(r[6], nc, r[12], K // 4 * PO * 4, 0, vt_span) and
                  fits(r[7], 1, 0, K // 4 * r32(G) * 4, WIN_LO, WIN_HI - WIN_LO) and
                  fits(r[8], 1, 0, r128(12 * K), WIN_LO, WIN_HI - WIN_LO) and
                  (not pk or (nc == 1 and hc and h == pack_rows(pk, hc))))
        elif ok and op == SPLIT:
            src = 2 * w <= 32 and fits(r[5], nc, r[11], C4 * pin_of(2 * h, 2 * w) * 4, 0, vt_span)
            if pk:
                ok = (src and nc == pk and not r[12] and pack_rows(pk, h) <= 64 and
                      fits(r[6], 1, 0, 4 * C4 * pin_of(pack_rows(pk, h), w) * 4, 0, vt_span))
            else:
                ok = src and fits(r[6], nc, r[12], 4 * C4 * PO * 4, 0, vt_span)
        elif ok and op == UPADD:
            s = [struct.unpack('<i', struct.pack('<I', r[j]))[0] for j in (15, 17, 19)]
            ha, wa = (h + 1) // 2, (w + 1) // 2
            a_ok = (fits(r[5], 1, 0, C4 * pin_of(pack_rows(pk, ha), wa) * 4, 0, vt_span) and nc == pk and not r[11] and
                    pack_rows(pk, ha) <= 64) if pk else fits(r[5], nc, r[11], C4 * pin_of(ha, wa) * 4, 0, vt_span)
            ok = (all(-31 <= v <= 0 for v in s) and w <= 32 and a_ok and
                  fits(r[9], nc, r[13], C4 * PO * 4, 0, vt_span) and fits(r[6], nc, r[12], C4 * PO * 4, 0, vt_span))
        elif ok and op == FUSE:
            s = [struct.unpack('<i', struct.pack('<I', r[j]))[0] for j in (15, 19)]
            P1 = pin_of(pack_rows(pk, h), w) if pk else PO
            ok = (K == C and K <= 128 and w <= 32 and all(-31 <= v <= 0 for v in s) and not r[16] and not r[17] and
                  not r[23] >> nc and all((c ^ 1) < nc for c in range(nc) if r[23] >> c & 1) and
                  fuse_scratch(C, K) * 128 <= scr_b and
                  (not pk or (nc == pk and not r[11] and not r[12] and pack_rows(pk, h) <= 64)) and
                  fits(r[5], 1 if pk else nc, r[11], C4 * P1 * 4, 0, vt_span) and
                  fits(r[6], 1 if pk else nc, r[12], C4 * P1 * 4, 0, vt_span) and
                  fits(r[7], 1, 0, K // 4 * r32(C) * 4, WIN_LO, WIN_HI - WIN_LO) and
                  fits(r[8], 1, 0, r128(8 * K), WIN_LO, WIN_HI - WIN_LO))
        elif ok and op == OUT:
            ok = r[8] and fits(r[5], nc, r[11], r[8], 0, vt_span) and fits(r[7], nc, r[20], r[8], WIN_LO, WIN_HI - WIN_LO)
        else:
            ok = False
        assert ok, 'record %d (op %d) would be rejected: %s' % (i, op, r[:21])


def expected(plan, t, nm):
    C, H, W = plan.shape[nm.split('/')[0]]
    x = t[nm.split('/')[0]]
    if nm == 'in' or nm.endswith('/2'):
        return phases(x, H // 2, W // 2)
    if x.shape[0] % 4:
        x = np.concatenate([x, np.zeros((4 - x.shape[0] % 4,) + x.shape[1:], x.dtype)])
    return plane(x, H, W)


def expected_packed(plan, ts, nm):
    """the bytes of tensor nm packed from the crops' int8_ref tensors ts (int8_graph.c "Packed planes")"""
    C, H, W = plan.shape[nm.split('/')[0]]
    xs = [t[nm.split('/')[0]] for t in ts]
    if nm.endswith('/2'):
        h, w = H // 2, W // 2
        return b''.join(plane(stacked([np.ascontiguousarray(x[:, py::2, px::2]) for x in xs]), pack_rows(len(xs), h), w)
                        for py in (0, 1) for px in (0, 1))
    if C % 4:
        xs = [np.concatenate([x, np.zeros((4 - C % 4,) + x.shape[1:], x.dtype)]) for x in xs]
    return plane(stacked(xs), pack_rows(len(xs), H), W)


# ------------------------------------------------------------------ device

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('link', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--graph', type=Path)
    ap.add_argument('--rounds', type=int, default=20)
    ap.add_argument('--crops', default='1,2,4')
    ap.add_argument('--plan-only', action='store_true')
    ap.add_argument('--held-power', action='store_true', help='power HMX once for the session (device._hold_power)')
    ap.add_argument('--paced', type=float, default=0, help='then SECONDS of 4-crop frames at --hz: host CPU and jitter')
    ap.add_argument('--hz', type=float, default=60)
    ap.add_argument('--run-on-device', action='store_true')
    ap.add_argument('--crop', type=int, default=SLOT, help='the synthetic graph\'s crop size (192: a 192 student)')
    ap.add_argument('--split', action='store_true',
                    help='also run each 2+ crop frame split over two workers (needs a build_int8_link.py --mw link)')
    a = ap.parse_args()
    if a.graph:
        z = np.load(a.graph)
        ops = json.loads(str(z['graph']))
        arrays = {k: z[k] for k in z.files if k != 'graph'}
        side = crop_of(ops)
        crop = np.random.default_rng(3).integers(0, 256, (side, side), dtype=np.uint8)
    else:
        ops, arrays, crop = synthetic(crop=a.crop)
    plan = Plan(ops, arrays)
    ncrops = [int(c) for c in a.crops.split(',')]
    rng = np.random.default_rng(11)
    crops = [crop] + [rng.integers(0, 256, (plan.crop, plan.crop), dtype=np.uint8) for _ in range(3)]
    ref_cache = {}

    def refs_of(n, mask):
        """int8_ref's tensors of crops 0..n-1 as one set (a two-view graph's pairs (c, c ^ 1), has2 bits mask)"""
        if (n, mask) not in ref_cache:
            ts = run_set(ops, arrays, crops[:n], [mask >> c & 1 for c in range(n)])
            for t in ts:
                for o in plan.ops:
                    if o['op'] == 'conv' and o['stride'] == 2 and o['src'] != 'in':
                        t[o['src'] + '/2'] = t[o['src']]
            ref_cache[n, mask] = ts
        return ref_cache[n, mask]

    def with_has2(req, mask):
        return (set_has2(req[0], mask),) + tuple(req[1:]) if plan.two_view else req
    full = lambda n: pairs_mask(n, 0xF) if plan.two_view else 0
    # (kind, crops, request, has2 bits); a two-view graph also runs 4 crops with only crops 0 and 3 fused
    frames = [('observe', 1, plan.records(1, True), 0)] + \
        [('frame', n, with_has2(plan.records(n, False), full(n)), full(n)) for n in ncrops]
    if plan.two_view and 4 in ncrops:
        frames.append(('frame-m9-', 4, with_has2(plan.records(4, False), 9), 9))
    if a.split:
        ok = [n for n in ncrops if n >= 2 and not (plan.two_view and halves(n)[0] % 2)]
        frames += [('split', n, with_has2(plan.split(*halves(n)), full(n)), full(n)) for n in ok]
        if plan.two_view and 4 in ok:
            frames.append(('split-m9-', 4, with_has2(plan.split(*halves(4)), 9), 9))
    report = {'format': 'npu-hands-int8-link-run', 'graph': str(a.graph or 'synthetic npu_a seed 7'),
              'held_power': a.held_power,
              'plans': {'%s%d' % (k, n): {'records': len(f[3]), 'request_bytes': len(f[0]),
                                          'vtcm_top': max(max(w.values()) for w in (f[1] if k.startswith('split') else [f[1]]))}
                        for k, n, f, _ in frames}}
    sat = {nm: round(float(np.mean((t == 0) | (t == 255) | (t == -128) | (t == 127))), 3) for nm, t in refs_of(1, 0)[0].items()
           if '/' not in nm}
    report['ref_saturation'] = sat
    if a.plan_only:
        print(json.dumps(report, indent=1))
        return
    if not a.run_on_device or os.geteuid() != 0 or not 2 <= a.rounds <= 200:
        ap.error('Frame root, --run-on-device and rounds in 2..200 required')

    from framenpu import Device, Job, JobFailed, RecoveryRequired
    from framenpu.resident_gemm import _lease, RECORD
    from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
    from hardware_hmx import PowerSnapshot, audited_device
    from hardware_resident import settled_health
    from hardware_landmark import service
    from hmx_multi_run import failure_snapshot
    import hardware_teacher_link as TL

    rec = json.loads((a.link / 'link.json').read_text())
    loader = Job.load(a.link / 'teacher_link_loader.fjob')
    image = (a.link / 'int8_link.bin').read_bytes()
    if a.split and rec['loader'].get('workers') != 2:
        ap.error('--split needs the two-worker loader (build_int8_link.py --mw)')
    if (hashlib.sha256(loader.code).hexdigest() != rec['loader']['sha256'] or
            hashlib.sha256(image).hexdigest() != rec['linked']['sha256'] or not 0 < len(image) <= TL.SEAL_AT - TL.CODE or
            not loader.requires_hmx or not loader.supports_direct):
        ap.error('link build record differs')
    report['link'] = rec

    class Link:     # LinkRunner's seal/admin/reservation checks, unchanged
        _check_reservation, admin, seal = TL.LinkRunner._check_reservation, TL.LinkRunner.admin, TL.LinkRunner.seal

    nonce = [int.from_bytes(os.urandom(4), 'little')]

    def frame(lk, data, phases=None):
        seen = []
        n1 = struct.unpack_from('<I', data, 16)[0]
        if n1:
            nonce[0] = next_nonce(nonce[0])
            data = set_nonce(data, nonce[0])

        def validate(res):
            TL.linked_completion(res, len(image), output_bytes=256)
            lk._check_reservation(True)
            seen.append(res)
        t0 = time.perf_counter()
        kw = {'_held_power': lk.held} if lk.held is not None else {}
        res = lk.dev._submit(loader, data, opcode=IG_OPCODE, params=(1,), output_size=256, use_direct=True,
                             _completion_validator=validate, _prepare=lambda t: lk._check_reservation(True),
                             _phase_times=phases, **kw)
        wall = (time.perf_counter() - t0) * 1e3
        m = struct.unpack('<64I', res.output)
        n = struct.unpack_from('<I', data, 4)[0]
        if not meta_ok(res.output, n, n1):
            raise ValueError('image metadata differs: %s' % [hex(v) for v in m[:10]])
        frame.worker1_us = m[3] / 19.2
        return wall, res.kernel_reference_ticks / 19.2e3, [v / 19.2 for v in m[8:8 + n]]

    def compare(mem, where, ddr, n, names, refs, c0=0):
        """crops c0..c0+n-1's outputs at ddr against int8_ref's refs (the frame's whole set); a split request:
        both sections (where, ddr pairs)"""
        if isinstance(ddr, tuple):
            n0, n1 = halves(n)
            return {**compare(mem, where[0], ddr[0], n0, list(ddr[0]), refs),
                    **{'w1:' + k: v for k, v in compare(mem, where[1], ddr[1], n1, list(ddr[1]), refs, c0=n0).items()}}
        bad = {}
        for nm in names:
            off, size = ddr[nm]
            if plan.packed(nm, n):
                got, want = bytes(mem[off:off + size]), expected_packed(plan, refs[c0:c0 + n], nm)
                assert len(want) == size, (nm, len(want), size)
                d = sum(x != y for x, y in zip(got, want))
                if d:
                    bad['%s[packed]' % nm] = d
                continue
            for c in range(n):
                got = bytes(mem[off + c * size:off + (c + 1) * size])
                want = expected(plan, refs[c0 + c], nm)
                assert len(want) == size, (nm, len(want), size)
                d = sum(x != y for x, y in zip(got, want))
                if d:
                    bad['%s[%d]' % (nm, c)] = d
        return bad

    report['audits'] = {'restored_jobs': 0, 'restored_hmx_jobs': 0}
    report['service_before'] = service()
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report['initial_health'] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    mem = observer._transport.memory
                    if bytes(mem[CONTROL:CONTROL + 8]) == RECORD:
                        raise ValueError('resident refusal record present')
                    before = bytes(mem[ARENA_START:ARENA_END])
                window = before[WIN_LO - ARENA_START:WIN_HI - ARENA_START]
                reservation = before[TL.CODE - ARENA_START:TL.CODE + TL.CODE_BYTES - ARENA_START]
                unsafe, retired = False, True
                try:
                    with audited_device(power, report['audits'])(dispatch='direct') as dev:
                        lk = Link()
                        lk.dev, lk.mem, lk.loader, lk.image, lk.sealed = dev, dev._transport.memory, loader, image, False
                        lk.reservation = image + bytes(TL.CODE_BYTES - len(image))
                        lk.held = None
                        mem = lk.mem
                        mem[WIN_LO:WIN_HI] = bytes(WIN_HI - WIN_LO)
                        for off, blob in plan.blobs:
                            mem[off:off + len(blob)] = blob
                        for c in range(4):
                            at = plan.crops_at + c * SLOT * SLOT
                            mem[at:at + plan.crop ** 2] = crops[c].tobytes()
                        try:
                            report['seal_ms'] = lk.seal()
                            if a.held_power:
                                lk.held = dev._hold_power()
                            runs = {}
                            for kind, n, (data, where, ddr, ops_), mask in frames:
                                names = list(ddr)
                                refs = refs_of(n, mask)
                                w, wrap, per = frame(lk, data)
                                bad = compare(mem, where, ddr, n, names, refs)
                                row = {'crops': n, 'records': len(ops_), 'mismatch': bad, 'first_wall_ms': round(w, 3),
                                       'first_wrapper_ms': round(wrap, 4)}
                                if kind == 'observe':
                                    row['record_us'] = [round(v, 1) for v in per]
                                    row['record_ops'] = ops_
                                    runs['observe'] = row
                                    if bad:
                                        break
                                    continue
                                walls, wraps, pers, phs = [], [], [], []
                                for _ in range(a.rounds):
                                    ph = {}
                                    w, wrap, per = frame(lk, data, ph)
                                    walls.append(w); wraps.append(wrap); pers.append(per); phs.append(ph)
                                row['phases_ms_median'] = {k: round(float(np.median([p.get(k, 0) for p in phs])), 3)
                                                           for k in phs[0]}
                                row['mismatch_after'] = compare(mem, where, ddr, n, names, refs)
                                row['wall_ms_median'] = round(float(np.median(walls)), 3)
                                row['wrapper_ms_median'] = round(float(np.median(wraps)), 4)
                                row['wrapper_ms_max'] = round(float(np.max(wraps)), 4)
                                row['body_ms_median'] = round(float(np.median([sum(p) for p in pers])) / 1e3, 4)
                                row['record_us_median'] = [round(float(v), 1) for v in np.median(np.array(pers), 0)]
                                row['record_ops'] = ops_
                                if kind.startswith('split'):
                                    row['worker1_us_last'] = round(frame.worker1_us, 1)
                                runs['%s%d' % (kind, n)] = row
                                sys.stderr.write('done %d crops\n' % n)
                            report['runs'] = runs
                            if a.paced and not any(r.get('mismatch') or r.get('mismatch_after') for r in runs.values()):
                                last = [f for f in frames if f[1] == max(ncrops) and not f[0].endswith('-')][-1]
                                data = last[2][0]
                                period, t_next, rows = 1 / a.hz, time.perf_counter(), []
                                c0, w0 = time.process_time(), time.perf_counter()
                                while time.perf_counter() - w0 < a.paced:
                                    t_next += period
                                    c1 = time.process_time()
                                    w, wrap, _ = frame(lk, data)
                                    rows.append((w, wrap, (time.process_time() - c1) * 1e3))
                                    slack = t_next - time.perf_counter()
                                    if slack > 0:
                                        time.sleep(slack)
                                    else:
                                        t_next = time.perf_counter()
                                el = time.perf_counter() - w0
                                ws_, wr_, cp_ = (np.array([r[i] for r in rows]) for i in range(3))
                                report['paced'] = {
                                    'hz': a.hz, 'crops': max(ncrops), 'seconds': round(el, 2), 'frames': len(rows),
                                    'achieved_hz': round(len(rows) / el, 2),
                                    'cpu_percent_of_core': round(100 * (time.process_time() - c0) / el, 1),
                                    'cpu_ms_per_frame_p50': round(float(np.median(cp_)), 3),
                                    'wall_ms_p50_p95_p99_max': [round(float(np.percentile(ws_, q)), 3) for q in (50, 95, 99, 100)],
                                    'wrapper_ms_p50_p95_p99_max': [round(float(np.percentile(wr_, q)), 3) for q in (50, 95, 99, 100)]}
                                report['paced']['mismatch'] = compare(mem, *last[2][1:3], max(ncrops), list(last[2][2]),
                                                                      refs_of(max(ncrops), last[3]))
                        except RecoveryRequired:
                            unsafe = True
                            raise
                        finally:
                            if lk.held is not None:
                                held, lk.held = lk.held, None
                                if unsafe or dev._transport.poisoned:
                                    held.abandon()  # never gate clocks in an unknown state
                                else:
                                    dev._release_power(held)
                            if lk.sealed:
                                retired = False
                                if not unsafe and not dev._transport.poisoned:
                                    report['retire_ms'] = lk.admin(8)
                                    retired = True
                except RecoveryRequired:
                    unsafe = True
                    raise
                finally:
                    report['window_restored'] = False
                    after = None
                    if not unsafe and retired:
                        with Device() as observer:
                            mem = observer._transport.memory
                            mem[WIN_LO:WIN_HI] = window
                            mem[TL.CODE:TL.CODE + TL.CODE_BYTES] = reservation
                            after = bytes(mem[ARENA_START:ARENA_END])
                        report['window_restored'] = True
                if after != before:
                    raise ValueError('arena differs after window/reservation restoration')
                report['final_health'] = settled_health(power, baseline)
                report['service_after'] = service()
                if report['service_before'] != report['service_after']:
                    raise ValueError('DSP service changed')
                report['all_checks_passed'] = True
            except BaseException as exc:
                report['error'] = '%s: %s' % (type(exc).__name__, exc)
                report['after_failure'] = failure_snapshot(power)
    finally:
        lease.close()
        a.out.write_text(json.dumps(report, indent=1) + '\n')
    s = {k: {f: v.get(f) for f in ('crops', 'mismatch', 'wrapper_ms_median', 'body_ms_median', 'wall_ms_median')}
         for k, v in report.get('runs', {}).items()}
    print(json.dumps({'all_checks_passed': report.get('all_checks_passed', False), 'error': report.get('error'),
                      'runs': s, 'paced': report.get('paced')}, indent=1))


if __name__ == '__main__':
    main()
