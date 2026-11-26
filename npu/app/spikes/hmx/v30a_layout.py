"""v30a on framenpu's resident HMX kernel (k/teacher_resident_probe.c): ops, descriptors and the
window/VTCM plan. Opens no device.

Adapted from framenpu src/framenpu/_mercury_layout.py (MIT, NatTheDyke, 8c9baee): the grid,
parameter images, tile layout, VTCM allocator (_merged_crop) and tensor staging are reused from
it; build_plan is its copy with v30a's descriptors. v30a-specific placement:

* The stem (gray-folded, v30a_graph.stem_gray) runs on HMX as a 1x1 over host im2col columns of
  the FP16 crop, K = 10 (9 taps + a zero plane), TF-SAME: plane 3*dy+dx = img[2y+dy, 2x+dx],
  zero past the last row/column (stem_columns).
* The 112x112 expansion (convclip_1 -> convdwclip_1, 64 channels) runs per 32-channel group; the
  stride-2 outputs are the parts of one composite (DOWN), so 64x112x112 never exists.
* Depthwise ops are kind 2 (stride 1) or kind 5 (stride 2) with nk 3 or 5. Stride 2 keeps the odd
  decimation phase (no RS_S2_EVEN): TF-SAME padding. maxpool2d_1 is kind 5 with nk 2 and no clip.
* Kind 5's tmp word points at its own input: an RS_S2_DIRECT build validates tmp but never
  touches it, and a 64x112x112 scratch would not fit.
* The device stores FINAL (672 x 7 x 7); GAP and the heads run on the host (v30a_graph.finish).
"""
import struct
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np

F = Path(__file__).resolve().parents[2] / 'framenpu'
if str(F / 'src') not in sys.path:
    sys.path.insert(0, str(F / 'src'))
from framenpu import _mercury_common as RL  # noqa: E402
from framenpu import _mercury_layout as ML  # noqa: E402
from framenpu._mercury_layout import (MercuryPlan, grid, pw_image, read_tensor, stage_inputs,  # noqa: E402,F401
                                      tensor_bytes, _merged_crop, _tile_tensors)

import v30a_graph as G  # noqa: E402

STEM_COLS = 'stem.cols'
STEM_PLANES = 10
EXPAND, DOWN = '7', '8'  # convclip_1 (64 x 112 x 112, only ever per group) and convdwclip_1 outputs
GROUP = 32
MAX_DESCRIPTORS = ML.MAX_DESCRIPTORS
S2_PHASE = 1  # odd: TF-SAME


# The 5x5 stride-2 direct read (k/teacher_resident_probe.c, nk 5) loads up to 68 halves past its
# last input row's start: on small grids (a 160 input's 10x10 into convdwclip_11) that passes
# framenpu's nbin. Such an input grid gets the blocks it needs. Keyed by grid size, so every
# tensor of that size (its producer's descriptor, the allocation, the readback) agrees.
_NB_MIN = {}
_grid_nbin = RL.Grid.nbin
RL.Grid.nbin = property(lambda g: max(_grid_nbin.fget(g), _NB_MIN.get((g.w, g.h), 0)))


def _reserve_s2_reads(ops):
    for op in ops:
        if op.conv_class != 'depthwise' or op.params['stride'] != (2, 2) or op.params['weight'].shape[2] != 5:
            continue
        h, w = op.shape[1] * 2, op.shape[2] * 2
        g = RL.Grid(w, h)
        oh, ow = op.shape[1:]
        last = 32 * g.blk0 + S2_PHASE + (S2_PHASE + 2 * oh) * g.ws + 2 * (32 * ((ow - 1) >> 5)) + 67
        if last // 32 + 1 > _grid_nbin.fget(g):
            _NB_MIN[(w, h)] = max(_NB_MIN.get((w, h), 0), last // 32 + 1)


def _part(name, k):
    return f'{name}.g{k}'


def device_ops(model):
    """Every op the device runs, in order: the stem as a K=10 1x1, the 112 expansion per group."""
    ops = list(model.ops)
    names = [op.name for op in ops]
    if names[:5] != ['convclip_0', 'convdwclip_0', 'conv_10', 'convclip_1', 'convdwclip_1']:
        raise ValueError('unexpected v30a prefix')
    stem = G.stem_gray(model)
    expand, down = ops[3], ops[4]
    if (stem.params['weight'].shape != (24, 1, 3, 3) or stem.params['stride'] != (2, 2)
            or stem.params['pads'] != (0, 0, 1, 1) or expand.output != EXPAND or down.inputs != (EXPAND,)
            or down.output != DOWN or expand.shape[0] % GROUP):
        raise ValueError('unexpected v30a prefix geometry')
    w = np.zeros((24, STEM_PLANES, 1, 1), np.float32)
    w[:, :9, 0, 0] = stem.params['weight'].reshape(24, 9)
    out = [replace(stem, inputs=(STEM_COLS,),
                   params=stem.params | {'weight': w, 'stride': (1, 1), 'pads': (0, 0, 0, 0)}),
           ops[1], ops[2]]
    for k in range(expand.shape[0] // GROUP):
        sl = slice(GROUP * k, GROUP * (k + 1))
        out.append(replace(expand, name=_part(expand.name, k), output=_part(EXPAND, k),
                           params=expand.params | {'weight': expand.params['weight'][sl],
                                                   'bias': expand.params['bias'][sl]},
                           shape=(GROUP, *expand.shape[1:])))
        out.append(replace(down, name=_part(down.name, k), inputs=(_part(EXPAND, k),), output=_part(DOWN, k),
                           params=down.params | {'weight': down.params['weight'][sl],
                                                 'bias': down.params['bias'][sl], 'group': GROUP},
                           shape=(GROUP, *down.shape[1:])))
    out += ops[5:]
    for op in out:
        _check(op)
    _reserve_s2_reads(out)
    return out


def _up(c):
    return -(-c // 32) * 32


def _pad32(op):
    """Every device tensor rounded up to whole 32-channel blocks, the extra channels made by zero
    weights and biases (so exactly 0; ReLU6, max pool and the residual adds keep them 0). The
    depthwise kernel writes only a tensor's own channel pairs, and a 1x1 reads its input's
    whole blocks (zero weights times what the rest of the block held: NaN stays NaN), so a
    24-channel depthwise output feeding a 1x1 read stale VTCM. The 1x1s cost the same (HMX
    tiles are 32 wide either way); only the depthwise ops on 24, 144 and 16 channels grow."""
    p = op.params
    w, b = p['weight'], p['bias']
    cout, cin = w.shape[:2]
    co = _up(cout)
    ci = cin if op.inputs[0] == STEM_COLS or op.conv_class != 'pointwise' else _up(cin)
    if (co, ci) == (cout, cin):
        return op
    w2 = np.zeros((co, ci, *w.shape[2:]), np.float32)
    w2[:cout, :cin] = w
    b2 = np.zeros(co, np.float32)
    b2[:cout] = b
    extra = {'group': co} if op.conv_class == 'depthwise' else {}
    return replace(op, params=p | {'weight': w2, 'bias': b2} | extra, shape=(co, *op.shape[1:]))


def composites(model):
    n = model.op('convclip_1').shape[0] // GROUP
    return {DOWN: [(_part(DOWN, k), 'device') for k in range(n)]}


def shapes(model):
    shape = {op.output: op.shape for op in model.ops}
    shape[STEM_COLS] = (STEM_PLANES, *model.op(G.STEM).shape[1:])
    for op in device_ops(model):
        shape[op.output] = op.shape
    return shape


def _check(op):
    p = op.params
    if op.kind != 'conv' or op.conv_class not in ('pointwise', 'depthwise'):
        raise ValueError(f'{op.name}: not a resident op')
    if op.conv_class == 'pointwise':
        if p['weight'].shape[1] % 2 or op.shape[0] % 2:
            raise ValueError(f'{op.name}: odd channel count')
        return
    k, s = p['weight'].shape[2], p['stride'][0]
    if len(op.inputs) != 1 or op.shape[0] % 2:
        raise ValueError(f'{op.name}: depthwise with a residual or odd channels')
    if p.get('pool') == 'max':
        if (k, p['stride'], p['act'], p['pads']) != (2, (2, 2), None, (0, 0, 0, 0)):
            raise ValueError(f'{op.name}: the pool must be 2x2 stride 2')
        return
    if k not in (3, 5) or p['act'] != 'relu6' or p['pads'] != G._same_pads(k, s) or s not in (1, 2):
        raise ValueError(f'{op.name}: resident depthwise is 3x3/5x5 TF-SAME + ReLU6')


def dw_image(op, gin, cbias=False):
    """framenpu's dw_image with kk = k * k (3x3, 5x5, or the max pool's 2x2 of zeros)."""
    w, b = op.params['weight'], op.params['bias']
    c, kk = w.shape[0], w.shape[2] * w.shape[3]
    h = np.asarray(w.reshape(c, kk), np.float16).view('<u2').astype('<u4')
    coef = np.zeros(((c // 2 * kk + 31) // 32) * 32, '<u4')
    coef[: c // 2 * kk] = (h[0::2] | (h[1::2] << 16)).reshape(-1)
    image = coef.tobytes() + ML._bias(b, c // 2, cbias) + gin.mask_bytes()
    return image, c // 2 * kk


def stem_columns(crop01):
    """Host im2col of the FP16 1 x S x S crop (0..1) for the TF-SAME stride-2 stem:
    plane 3*dy+dx holds img[2y+dy, 2x+dx] (zero past the edge); plane 9 is zero."""
    img = np.asarray(crop01, np.float16)
    n = img.shape[-1]
    img = img.reshape(n, n)
    h = n // 2
    pad = np.zeros((n + 2, n + 2), np.float16)
    pad[: n, : n] = img
    cols = np.zeros((STEM_PLANES, h, h), np.float16)
    for dy in range(3):
        for dx in range(3):
            cols[3 * dy + dx] = pad[dy: dy + n: 2, dx: dx + n: 2]
    return cols


def expected_tensors(model, t):
    """An v30a_graph.run(keep=True) result plus the rewritten tensors (columns, group slices)."""
    t = dict(t)
    t[STEM_COLS] = stem_columns(t['in0']).astype(np.float32)
    for k in range(model.op('convclip_1').shape[0] // GROUP):
        sl = slice(GROUP * k, GROUP * (k + 1))
        t[_part(EXPAND, k)] = t[EXPAND][sl]
        t[_part(DOWN, k)] = t[DOWN][sl]
    return t


def build_plan(model, mode='merged', stop_after=None, start=None, batch=1, pw_group=1, replay=False,
               resident=False, tiles=False, deep8=False, res_base=None, win_base=None, cbias=False, win_hi=None,
               compact=False):
    """compact: store only the vector blocks of each plane that hold values (k/ kind 4 with nj).
    framenpu _mercury_layout.build_plan for v30a (same options and checks; no heads, hint or
    detector). start/stop_after name device ops (device_ops) to truncate the request."""
    win_hi = RL.WIN_HI if win_hi is None else win_hi
    if win_hi not in (RL.WIN_HI, RL.WIN_HI_WIDE):
        raise ValueError(f'window top {win_hi:#x}')
    if cbias and not resident:
        raise ValueError('packed biases are an option of the resident plan')
    res_top = RL.RES_SPAN - RL.XB_N if cbias else RL.RES_SPAN
    if mode not in ('merged', 'qualify'):
        raise ValueError(mode)
    if batch < 1 or (batch > 1 and mode != 'merged'):
        raise ValueError('batch > 1 needs a merged plan')
    if replay and (mode != 'merged' or batch > RL.MAX_CROPS):
        raise ValueError(f'replay is a merged plan of at most {RL.MAX_CROPS} crops')
    if resident and not replay:
        raise ValueError('resident parameters need a replay plan')
    if tiles and not resident:
        raise ValueError('tile layout is an option of the resident plan')
    if deep8 and not (tiles and pw_group == 8):
        raise ValueError('deep8 is an option of the 8-group tiles plan')
    program = device_ops(model)
    order = [op.name for op in program]
    a = order.index(start) if start else 0
    b = order.index(stop_after) if stop_after else len(order) - 1
    if a > b:
        raise ValueError('start after stop_after')
    ops = program[a: b + 1]
    final = ops[-1].output
    shape = shapes(model)
    pairs = lambda n: shape[n][0] // 2  # noqa: E731
    vec = lambda n: pairs(n) * grid(shape[n]).nbin  # noqa: E731
    comps = composites(model)
    produced = {op.output for op in ops}
    consumed = {i for op in ops for i in op.inputs}
    free = {}
    for parent, parts in comps.items():
        dev = [n for n, src in parts if src == 'device']
        made = [n for n in dev if n in produced]
        if mode != 'merged' or not made:
            continue
        if parent in consumed and len(made) != len(dev):
            raise ValueError(f'{parent}: request produces only some of its parts')
        offsets, at = {}, 0
        for n, _ in parts:
            offsets[n] = at
            at += vec(n)
        free[parent] = offsets
    parent_of = {n: par for par, offs in free.items() for n in offs}
    made = produced | set(free)
    external = [n for n in dict.fromkeys(i for op in ops for i in op.inputs) if n not in made]
    tile = _tile_tensors(ops, shape, external, free, parent_of, [], False) if tiles else set()

    images = {op.name: (pw_image(op, grid(shape[op.inputs[0]]), pw_group, cbias) if op.conv_class == 'pointwise'
                        else dw_image(op, grid(shape[op.inputs[0]]), cbias)) for op in ops}
    deep = set()
    if deep8:
        for op in ops:
            if op.conv_class != 'pointwise':
                continue
            cout, cin = op.params['weight'].shape[:2]
            nk, no = -(-cin // 32), -(-cout // 32)
            if op.inputs[0] in tile and no <= nk and grid(shape[op.inputs[0]]).nj >= 64:
                image, scratch = images[op.name]
                images[op.name] = (image, max(scratch, 32 * no * 16))
                deep.add(op.name)

    table_at = RL.WIN_LO if win_base is None else win_base
    if table_at & 4095 or not RL.WIN_LO <= table_at < win_hi:
        raise ValueError(f'window base {table_at:#x}')
    cursor = RL._al(table_at + 128 * MAX_DESCRIPTORS * (len(ops) if mode == 'qualify' else 1), 4096)
    par_at, fixed = {}, {}
    for op in ops:
        par_at[op.name] = cursor
        fixed[cursor] = images[op.name][0]
        cursor = RL._al(cursor + len(images[op.name][0]))
    stored = [final] if mode == 'merged' else [op.output for op in ops]
    crops = []
    for _ in range(batch):
        where = {}
        for name in dict.fromkeys(external + stored):
            cursor = RL._al(cursor, 4096)
            where[name] = cursor
            cursor += vec(name) * 128
        crops.append(where)
    where = crops[0]
    if cursor > win_hi:
        raise ValueError(f'window plan ends at {cursor:#x}')

    table, jobs, peak = [], [], 0

    def words(*w):
        table.append(RL._words(*w))

    cur = dict(where)
    res_at, res_next = {}, RL.RES_LO if res_base is None else res_base
    if not RL.RES_LO <= res_next < res_top:
        raise ValueError(f'resident base {res_next}')

    def load(name, to):
        words(RL.KIND['load'], 0, 0, 0, 0, 0, 0, 0, 0, cur[name], vec(name), 0, 0, 0, to)

    def store(name, frm):
        if compact:  # only the blocks that hold values: read back with read_tensor(compact=True)
            g = grid(shape[name])
            words(RL.KIND['store'], pairs(name), 0, 0, 0, g.nbin, g.blk0, g.nj, 0, cur[name], pairs(name) * g.nj,
                  0, 0, 0, frm)
        else:
            words(RL.KIND['store'], 0, 0, 0, 0, 0, 0, 0, 0, cur[name], vec(name), 0, 0, 0, frm)

    def emit(op, at, vt):
        nonlocal res_next
        src = op.inputs[0]
        res = op.inputs[1] if len(op.inputs) > 1 else None
        dst = op.output
        gin, gout = grid(shape[src]), grid(shape[dst])
        image, scratch = images[op.name]
        npar = len(image) // 128
        pw = op.conv_class == 'pointwise'
        if resident:
            if op.name not in res_at:
                at0 = RL._al(res_next * 128, 2048 if pw else 128) // 128
                if at0 + npar > res_top:
                    raise ValueError('resident parameter images exceed the blob region')
                res_at[op.name], res_next = at0, at0 + npar
            vpar = res_at[op.name]
            scr = vt.take(scratch, align=16 if pw else 1)
            tail = (scr,)
        else:
            vpar = vt.take(npar + scratch, align=16 if pw else 1)
            tail = ()
        if pw:
            cout, cin = op.params['weight'].shape[:2]
            words(RL.KIND['pw'], cin // 2, cout // 2, (cin // 2 + 15) >> 4, int(op.params['act'] == 'relu6'),
                  gin.nbin, gin.blk0, gin.nj, gin.ws, par_at[op.name], npar, at[src], at[dst],
                  at[res] if res else 0, vpar,
                  *((int(src in tile) | int(dst in tile) << 1 | int(res in tile) << 2 | int(op.name in deep) << 3,)
                    + (0,) * 5 + tail if tail else ()))
        else:
            cp, k = pairs(src), op.params['weight'].shape[2]
            s2 = op.params['stride'] == (2, 2)
            extra = (gout.nbin, gout.blk0, gout.ws, gout.h, gout.w, at[src]) if s2 else ()
            if tail:
                extra = (extra or (int(dst in tile) << 1,) + (0,) * 5) + tail
            words(RL.KIND['s2' if s2 else 'dw'], cp, cp, k, int(op.params['act'] == 'relu6'),
                  gin.nbin, gin.blk0, gin.nj4, gin.ws, par_at[op.name], npar, at[src], at[dst], 0, vpar, *extra)
        if resident:
            vt.give(scr, scratch)
        else:
            vt.give(vpar, npar + scratch)

    if mode == 'merged':
        key = lambda n: parent_of.get(n, n)  # noqa: E731
        last_use = {}
        for i, op in enumerate(ops):
            for n in op.inputs:
                last_use[key(n)] = i
        vt = RL._Vtcm()
        for k in range(batch):
            cur.clear()
            cur.update(crops[k])
            _merged_crop(ops, external, free, comps, parent_of, final, last_use, key, vec, vt, load, store, emit,
                         stored, tile)
        job = RL.Job('rs', first=0, end=len(table), ops=[op.name for op in ops], stores=stored)
        job.vtcm_peak = peak = vt.peak
        jobs.append(job)
    else:
        for op in ops:
            first = len(table)
            vt, at = RL._Vtcm(), {}
            for n in dict.fromkeys(op.inputs):
                at[n] = vt.take(vec(n))
                load(n, at[n])
            at[op.output] = vt.take(vec(op.output))
            emit(op, at, vt)
            store(op.output, at[op.output])
            job = RL.Job('rs', first=first, end=len(table), ops=[op.name], stores=[op.output])
            job.vtcm_peak = vt.peak
            peak = max(peak, vt.peak)
            jobs.append(job)
    stride = 0
    if replay:
        per = len(table) // batch
        if batch > 1:
            stride = crops[1][final] - crops[0][final]
        for k in range(batch):
            if any(crops[k][n] - crops[0][n] != k * stride for n in crops[0]):
                raise ValueError('crop slots are not a fixed stride apart')
            for n in range(per):
                got = struct.unpack('<32I', table[k * per + n])
                want = RL.replay_row(struct.unpack('<32I', table[n]), k, stride, win_hi)
                if list(got) != want:
                    raise ValueError(f"crop {k} descriptor {n} is not crop 0's replayed")
        table = table[:per]
        jobs[0].end = per
    if any(j.end - j.first > MAX_DESCRIPTORS for j in jobs):
        raise ValueError(f'a request exceeds {MAX_DESCRIPTORS} descriptors')
    plan = RL.Plan(mode, jobs, b''.join(table), table_at, fixed, where, cursor, ops, shape)
    if replay:
        plan.max_crops, plan.crop_stride = batch, stride
    return MercuryPlan(mode, plan, ops, final, where, shape, peak, len(table), external, [], comps, True,
                       tuple(crops) if batch > 1 else (), replay, resident, res_next if resident else 0, cursor,
                       cbias, win_hi)


def native_ok(row, *, dw_blocks=2, s2_direct=None, pw_group=1, mheads=False, resident=False, tiles=False,
              deep8=False, cbias=False, win_hi=RL.WIN_HI):
    """Mirror of rs_get in k/teacher_resident_probe.c (True = accepted): framenpu's
    _mercury_common.native_ok (MIT, NatTheDyke) with this kernel's kind-5 extensions, nk 5
    (clip 1; the direct read reaches 68 words from row 2(oh-1)+P-2) and nk 2 (the max pool, clip 0)."""
    SPAN, RES_SPAN, RES_LO, XB_N, WIN_LO = RL.SPAN, RL.RES_SPAN, RL.RES_LO, RL.XB_N, RL.WIN_LO
    if cbias and not resident:
        raise ValueError('cbias is an option of the resident build')
    m = 0xFFFFFFFF
    w = [int(x) for x in row[:15]]
    kind, cpi, cpo, k, clip, nb, blk0, nj, ws, dpar, npar, vin, vout, vres, vpar = w
    rscr = int(row[21]) if resident else 0
    big = rscr
    for i, x in enumerate(w):
        if i != 9 and not (resident and i == 14):
            big |= x
    s2 = kind == 5
    for x in row[15:21]:
        big |= int(x)
    if s2:
        onb, oblk, ows, oh, ow, tmp = (int(x) for x in row[15:21])
    p3 = mheads and kind == 8
    align = ((vpar | npar) << 3) if kind == 1 or p3 else 0
    res = rscr != 0
    top = RES_SPAN - XB_N if cbias else RES_SPAN
    bad = (big > 0x3FFF) or ((kind - 1) & m > 4 and not p3) or vpar + npar > (top if res else SPAN) or (dpar | align) & 127
    bad = bad or (resident and vpar >= RES_SPAN) or (res and vpar < RES_LO) or (res and kind in (3, 4))
    bad = bad or (res and (kind == 1 or p3) and rscr & 15)
    bad = bad or ((dpar - WIN_LO) & m) + 128 * npar > win_hi - WIN_LO or vpar == 0
    bad = bad or dpar < WIN_LO or dpar > win_hi or npar > (win_hi - dpar) // 128
    if kind == 4 and nj:  # compact store (npu-hands): nj blocks from blk0 of each of cpi planes
        bad = bad or cpi == 0 or nb == 0 or blk0 + nj > nb or npar != cpi * nj or vpar + cpi * nb > SPAN
    if kind in (3, 4) or bad:
        return not bad
    out_nb = onb if s2 else nb
    if s2:
        if (oh == 0 or ow == 0 or (oh | ow | ows | ws) > 255 or ows != ow + 2 or ws != 2 * ow + 2 or onb == 0
                or tmp == 0 or tmp + cpo * nb > SPAN or vres != 0 or clip != int(k != 2) or k not in (2, 3, 5)):
            return False
        if 2 * oh * ws > 32 * nj or 32 * oblk + (oh - 1) * ows + 2 + ow > 32 * onb:
            return False
        if s2_direct is not None:
            ph, xl = s2_direct, 32 * ((ow - 1) >> 5)
            last = (32 * blk0 + ph + (ph + 2 * oh) * ws + 2 * xl + 67 if k == 5
                    else 32 * blk0 + 1 + ph + (ph + 2 * oh - 1) * ws + 2 * xl + 65)
            if last >= 32 * nb or 32 * oblk + 2 + (oh - 1) * ows + xl + 32 > 32 * onb:
                return False
    kk, reach, no = k * k, (k >> 1) * (ws + 1), (cpo + 15) >> 4
    pw = kind == 1 or p3
    bias = vpar + (no * k * 16 if pw else (cpi * kk + 31) // 32)
    scr = rscr if res else vpar + npar
    bad = (cpi == 0 or nj == 0 or vin == 0 or vout == 0 or clip > 1 or blk0 + nj > nb or vin + cpi * nb > SPAN
           or vout + cpo * out_nb > SPAN or vres + cpo * nb > SPAN
           or bias + (-(-cpo // 32) if cbias else cpo) + nj > vpar + npar or (cbias and cpo + nj > XB_N)
           or scr + ((pw_group * (k + no) if pw_group > 1 else k) * 16 if pw else cpi * kk) > SPAN)
    if p3:
        bad = bad or k != (9 * cpi + 15) >> 4 or 32 * blk0 < ws + 1 or 32 * (blk0 + nj) + ws + 1 > 32 * nb
    elif pw:
        bad = bad or k != (cpi + 15) >> 4
    else:
        bad = (bad or cpi != cpo or (k not in (3, 5) and not (k == 2 and s2)) or nj & (dw_blocks - 1)
               or 32 * blk0 < reach or 32 * (blk0 + nj) + reach > 32 * nb)
    if tiles and not s2:
        lay = int(row[15])
        lim = (15 if deep8 else 7) if kind == 1 else 2 if kind == 2 else 0
        bad = (bad or lay & ~lim or (lay & 8 and (not lay & 1 or no > k or scr + 32 * 16 * no > SPAN))
               or (lay & 1 and (cpi | vin) & 15) or (lay & 2 and (cpo | vout) & 15)
               or (lay & 4 and ((cpo | vres) & 15 or vres == 0)))
    return not bad


def read_compact(mplan, get, name):
    """read_tensor for a compact store (build_plan(compact=True)): get(window_address, n) -> bytes of
    pairs x nj vectors; the blocks are put back at blk0 of full planes (zeros elsewhere), so the
    stray count covers the stored blocks only."""
    g, c = mplan.grid(name), mplan.shape[name][0]
    pairs = c // 2
    raw = np.frombuffer(get(mplan.where[name], pairs * g.nj * 128), np.uint8).reshape(pairs, g.nj, 128)
    full = np.zeros((pairs, g.nbin, 128), np.uint8)
    full[:, g.blk0: g.blk0 + g.nj] = raw
    return g.from_bytes(full.tobytes(), c)
