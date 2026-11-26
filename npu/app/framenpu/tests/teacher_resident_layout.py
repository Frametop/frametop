"""Host side of the resident teacher tail (tests/teacher_resident_probe.c,
opcode 111): op tables, VTCM and window plans, parameter images and a
lane-level simulator of the kernel. Opens no device.

A plan is a sequence of jobs. A resident job runs a range of 128-byte
descriptors (1x1, stride-1 depthwise, load, store) with tensors kept in VTCM
between ops; the three stride-2 depthwise layers run as
tests/teacher_dw_probe.c jobs (opcode 110) from and to the window.

Plan modes:
- "resident": one resident job per stride-1 segment (4 jobs + 3 stride-2
  jobs); every parameter image lives in the window, written once; only
  tensors that cross a job boundary (and the final tensor) are stored.
- "qualify": one job per compute op, every output stored and read back, and
  each job's parameters staged into a shared window area just before it.
Both use the same kernel and per-op arithmetic, so their outputs must agree
bit for bit.

"merged" runs all 42 compute ops, including stride 2, in one linked request
(or two with split_after). "merged-observe" also stores the three stride-2
outputs for comparison with qualification. Kind 5 descriptors use words
15..20 for output nbin, blk0, ws, h, w and a full-resolution temporary grid.
"""

from dataclasses import dataclass, field

import teacher_tail_layout as T

RS_MAGIC, RS_OPCODE = 0x31535254, 111  # "TRS1"
SPAN = T.VTCM_SPAN_VECTORS
RES_SPAN = (0x316EEA80 - 0x31002000) // 128  # RS_RESIDENT: the FeatureNN blob region continues FR()
RES_LO = SPAN + 16  # FR(SPAN) holds the session tag
KIND = {"pw": 1, "dw": 2, "load": 3, "store": 4, "s2": 5}
VTCM_FIRST = 4  # vector 0 is never allocated (vres = 0 means "no residual")
FINAL = "79"
INPUTS = ("8", "10")


def _al(n, a=128):
    return (n + a - 1) // a * a


# ---------------------------------------------------------------- parameter images


def _splat_pairs(values, pairs):
    """FP16 values -> one splatted vector per channel pair (word = even | odd << 16)."""
    import numpy as np

    h = np.zeros(2 * pairs, np.float16)
    h[: len(values)] = np.asarray(values, np.float32).astype(np.float16)
    w = h.view("<u2").astype("<u4")
    word = w[0::2] | (w[1::2] << 16)
    return np.repeat(word[:, None], 32, 1).astype("<u4").tobytes()


def rs_params(graph, spec, kind, gin):
    """Parameter image (bytes) and scratch vectors for one resident compute op."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    _, b = weights_and_bias(graph, spec)
    if kind == "pw":
        tiles, _, nk, _ = T.pw_weights(graph, spec)
        cpo = spec["output_shape"][0] // 2
        image = tiles + _splat_pairs(b, cpo) + gin.mask_bytes()[: gin.nj * 128]
        image += bytes(-len(image) % 2048)  # B tiles after it stay 2 KiB aligned
        return image, nk * 16
    coef, _ = T.dw_weights(graph, spec)
    cp, kk = spec["input_shape"][0] // 2, spec["kernel"][0] ** 2
    image = coef + _splat_pairs(b, cp) + gin.mask_bytes()
    del np
    return image, cp * kk


def s2_params(graph, spec, gin):
    coef, bias = T.dw_weights(graph, spec)
    return coef, bias, gin.mask_bytes()


# ---------------------------------------------------------------- plan


@dataclass
class Job:
    kind: str  # "rs" or "s2"
    first: int = 0  # descriptor range (rs)
    end: int = 0
    op: object = None  # T.Op (s2)
    desc: bytes = b""  # opcode-110 descriptor (s2)
    ops: list = field(default_factory=list)  # compute op names, in order
    stores: list = field(default_factory=list)  # tensors written to the window
    staged: dict = field(default_factory=dict)  # window writes needed just before the job
    vtcm_peak: int = 0


@dataclass
class Plan:
    mode: str
    jobs: list
    table: bytes  # all descriptors
    table_at: int
    fixed: dict  # window writes done once (resident parameters)
    where: dict  # tensor -> window offset
    end: int  # first window byte past the plan
    ops: list
    shape_of: dict
    max_crops: int = 1  # RS_REPLAY: a request may run 1..max_crops crops of the table
    crop_stride: int = 0  # bytes between crop k's and crop k+1's load/store addresses


MAX_CROPS = 3  # RS_MAX_CROPS
WIN_HI_WIDE = 0x1FF0000  # RS_WIN_WIDE window top
XB_N = 512  # RS_CBIAS: expansion vectors at the top of the blob region (RS_XB = RES_SPAN - XB_N)


def replay_row(row, k, stride, win_hi=None):
    """A descriptor as the RS_REPLAY kernel runs it for crop k: load and store
    window addresses (word 9) moved by k * stride, everything else unchanged."""
    row = list(row)
    if row[0] in (KIND["load"], KIND["store"]) and row[9] <= (T.WIN_HI if win_hi is None else win_hi):
        row[9] += k * stride
    return row


class _Vtcm:
    def __init__(self):
        self.free = [(VTCM_FIRST, SPAN - VTCM_FIRST)]
        self.peak = 0  # most vectors in use at once

    def take(self, n, top=False, align=1):
        """First fit from the bottom (start aligned to `align` vectors), or
        (long-lived tensors) last fit from the top."""
        order = range(len(self.free) - 1, -1, -1) if top else range(len(self.free))
        for i in order:
            at, size = self.free[i]
            pad = -at % align
            if size >= n + pad:
                if top:
                    self.free[i : i + 1] = [(at, size - n)] if size > n else []
                    at += size - n
                else:
                    rest = [(at, pad)] if pad else []
                    rest += [(at + pad + n, size - pad - n)] if size > pad + n else []
                    self.free[i : i + 1] = rest
                    at += pad
                self.peak = max(self.peak, SPAN - VTCM_FIRST - sum(s for _, s in self.free))
                return at
        raise ValueError(f"VTCM plan does not fit ({n} vectors; free {self.free})")

    def give(self, at, n):
        self.free.append((at, n))
        self.free.sort()
        merged = []
        for a, s in self.free:
            if merged and merged[-1][0] + merged[-1][1] == a:
                merged[-1] = (merged[-1][0], merged[-1][1] + s)
            else:
                merged.append((a, s))
        self.free = merged


def _words(*w):
    import numpy as np

    out = np.zeros(32, "<u4")
    out[: len(w)] = w
    return out.tobytes()


def segments(ops, mode):
    """Job partition: lists of op indices; stride-2 ops alone."""
    groups, cur = [], []
    for i, op in enumerate(ops):
        s2 = op.kind == "dw" and op.stride2
        if s2 or mode == "qualify":
            if cur:
                groups.append(cur)
                cur = []
            groups.append([i])
        else:
            cur.append(i)
    if cur:
        groups.append(cur)
    return groups


@dataclass(frozen=True)
class _Op:
    kind: str
    name: str
    src: str
    dst: str
    res: str | None
    spec: dict
    stride2: bool


def build_plan(graph, specs, mode, *, split_after=None):
    if mode not in ("resident", "qualify", "merged", "merged-observe"):
        raise ValueError(mode)
    merged = mode.startswith("merged")
    if split_after is not None and not merged:
        raise ValueError("split_after requires a merged plan")
    by = {s["name"]: s for s in specs}
    tail, _ = T.tail_ops(specs)
    shape_of = T.shapes(specs)
    ops = [
        _Op(
            o.kind,
            o.name,
            o.src,
            o.dst,
            o.res,
            by[o.name],
            o.kind == "dw" and by[o.name]["stride"] == (2, 2),
        )
        for o in tail
    ]
    if merged:
        groups = [list(range(len(ops)))]
        if split_after is not None:
            cut = next((i + 1 for i, o in enumerate(ops) if o.name == split_after), None)
            if cut is None or cut == len(ops):
                raise ValueError("split_after must name a non-final tail op")
            groups = [list(range(cut)), list(range(cut, len(ops)))]
    else:
        groups = segments(ops, mode)
    job_of = {i: g for g, idx in enumerate(groups) for i in idx}
    grid = lambda b: T.grid_of(shape_of[b])  # noqa: E731
    pairs = lambda b: shape_of[b][0] // 2  # noqa: E731
    vec = lambda b: pairs(b) * grid(b).nbin  # noqa: E731

    # tensors that must be in the window: inputs, outputs used by a later job, the final, observed
    producer = {o.dst: job_of[i] for i, o in enumerate(ops)} | {b: -1 for b in INPUTS}
    users = {}
    for i, o in enumerate(ops):
        for b in (o.src, o.res):
            if b:
                users.setdefault(b, []).append(job_of[i])
    stored = {b for b in producer if b == FINAL or any(u != producer[b] for u in users.get(b, []))}
    if mode == "qualify":
        stored |= {o.dst for o in ops}
    if mode == "merged-observe":
        stored |= {o.dst for o in ops if o.stride2}
    stored |= set(INPUTS)

    # parameter images
    images = {}
    for i, o in enumerate(ops):
        if o.stride2 and not merged:
            coef, bias, mask = s2_params(graph, o.spec, grid(o.src))
            images[i] = (coef + bias + mask, 0, (len(coef), len(bias)))
        else:
            image, scratch = rs_params(graph, o.spec, o.kind, grid(o.src))
            images[i] = (image, scratch, None)

    # window: table | parameters (fixed) or staging | tensors
    n_desc_bound = sum(4 * len(g) + 2 for g in groups)  # generous upper bound
    table_at = T.WIN_LO
    cursor = _al(table_at + 128 * n_desc_bound, 4096)
    par_at = {}
    if mode != "qualify":
        for i in range(len(ops)):
            par_at[i] = cursor
            cursor = _al(cursor + len(images[i][0]))
        fixed = {par_at[i]: images[i][0] for i in range(len(ops))}
    else:
        stage = cursor
        cursor = _al(stage + max(len(im[0]) for im in images.values()), 4096)
        par_at = {i: stage for i in range(len(ops))}
        fixed = {}

    # tensor slots, reused after the last job that touches them
    last_job = {}
    for b in stored:
        js = [producer[b]] + users.get(b, [])
        last_job[b] = max(js)
    where, slot, free, live = {}, {}, [], []
    for b in sorted(stored, key=lambda b: (producer[b], b)):
        for x in [x for x in live if last_job[x] < producer[b]]:
            live.remove(x)
            free.append(slot[x])
        size = _al(vec(b) * 128, 4096)
        hit = next((k for k, (a, n) in enumerate(free) if n >= size), None)
        if hit is None:
            slot[b] = (cursor, size)
            cursor += size
        else:
            slot[b] = free.pop(hit)  # whole slot, so it is returned whole
        where[b] = slot[b][0]
        live.append(b)
    if cursor > T.WIN_HI:
        raise ValueError(f"window plan ends at {cursor:#x}")

    # descriptors
    table, jobs = [], []
    for g, idx in enumerate(groups):
        if ops[idx[0]].stride2 and not merged:
            i = idx[0]
            o = ops[i]
            image, _, sizes = images[i]
            desc = T.dw_descriptor(
                o.spec, grid(o.src), grid(o.dst), where[o.src], par_at[i], list(sizes), where[o.dst]
            )
            job = Job("s2", op=o, desc=desc, ops=[o.name], stores=[o.dst])
            if mode == "qualify":
                job.staged = {par_at[i]: image}
            jobs.append(job)
            continue
        job = Job("rs", first=len(table), ops=[ops[i].name for i in idx])
        vt, at = _Vtcm(), {}
        last_use = {}
        for i in idx:
            for b in (ops[i].src, ops[i].res):
                if b:
                    last_use[b] = i
        for i in idx:
            o = ops[i]
            for b in (o.src, o.res):
                if b and b not in at:
                    at[b] = vt.take(vec(b))
                    table.append(
                        _words(
                            KIND["load"], 0, 0, 0, 0, 0, 0, 0, 0, where[b], vec(b), 0, 0, 0, at[b]
                        )
                    )
            later = [k for k in idx if k > i and o.dst in (ops[k].src, ops[k].res)]
            at[o.dst] = vt.take(vec(o.dst), top=bool(later) and later[-1] > i + 1)
            image, scratch, _ = images[i]
            npar = len(image) // 128
            vpar = vt.take(npar + scratch, align=16 if o.kind == "pw" else 1)  # HMX tiles: 2 KiB
            gin = grid(o.src)
            tmp = vt.take(vec(o.src)) if o.stride2 else None
            if o.kind == "pw":
                cin, cout = o.spec["input_shape"][0], o.spec["output_shape"][0]
                table.append(
                    _words(
                        KIND["pw"],
                        cin // 2,
                        cout // 2,
                        (cin // 2 + 15) >> 4,
                        int(o.spec["activation"] == "clip"),
                        gin.nbin,
                        gin.blk0,
                        gin.nj,
                        gin.ws,
                        par_at[i],
                        npar,
                        at[o.src],
                        at[o.dst],
                        at[o.res] if o.res else 0,
                        vpar,
                    )
                )
            else:
                cp = pairs(o.src)
                gout = grid(o.dst)
                extra = (gout.nbin, gout.blk0, gout.ws, gout.h, gout.w, tmp) if o.stride2 else ()
                table.append(
                    _words(
                        KIND["s2" if o.stride2 else "dw"],
                        cp,
                        cp,
                        o.spec["kernel"][0],
                        1,
                        gin.nbin,
                        gin.blk0,
                        gin.nj4,
                        gin.ws,
                        par_at[i],
                        npar,
                        at[o.src],
                        at[o.dst],
                        0,
                        vpar,
                        *extra,
                    )
                )
            if mode == "qualify":
                job.staged = {par_at[i]: image}
            vt.give(vpar, npar + scratch)
            if tmp is not None:
                vt.give(tmp, vec(o.src))
            if o.dst in stored:
                table.append(
                    _words(
                        KIND["store"],
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0,
                        where[o.dst],
                        vec(o.dst),
                        0,
                        0,
                        0,
                        at[o.dst],
                    )
                )
                job.stores.append(o.dst)
            for b in {o.src, o.res} - {None}:
                if last_use[b] == i:
                    vt.give(at.pop(b), vec(b))
            if o.dst not in last_use:  # not read again in this job (stored above)
                vt.give(at.pop(o.dst), vec(o.dst))
        job.end = len(table)
        job.vtcm_peak = vt.peak
        jobs.append(job)
        if job.end - job.first > 160:
            raise ValueError("job exceeds 160 descriptors")
    if len(table) > n_desc_bound:
        raise ValueError("table bound")
    return Plan(mode, jobs, b"".join(table), table_at, fixed, where, cursor, ops, shape_of)


def setup_writes(plan):
    """Window writes required before any job of the plan: the op table and,
    for resident plans, every parameter image. Harness and simulator both use
    exactly this list."""
    return {plan.table_at: plan.table} | dict(plan.fixed)


def payload(plan, crops=1):
    """The request words; crops > 1 (an RS_REPLAY plan) adds words 3, 4."""
    import numpy as np

    most = getattr(plan, "max_crops", 1)  # plan stand-ins without the field: one crop
    if not 1 <= crops <= most:
        raise ValueError(f"crops must be 1..{most}")
    w = np.zeros(32, "<u4")
    w[:3] = [RS_MAGIC, plan.table_at, len(plan.table) // 128]
    if crops > 1:
        w[3:5] = [crops, plan.crop_stride]
    return w.tobytes()


# ---------------------------------------------------------------- lane-level simulator


def _cbias_expand(vt, base, cpo, nj):
    """RS_CBIAS rs_cbias: splat the packed pair words at vector `base` and copy the nj
    masks after them to RS_XB; returns RS_XB (the op's expanded bias base)."""
    import numpy as np

    xb = RES_SPAN - XB_N
    nwv = -(-cpo // 32)
    words = vt[base : base + nwv].view("<u4").reshape(-1)[:cpo]
    masks = vt[base + nwv : base + nwv + nj].copy()
    vt[xb : xb + cpo] = np.repeat(words[:, None], 32, 1).view("<f2").reshape(cpo, 32, 2)
    vt[xb + cpo : xb + cpo + nj] = masks
    return xb


S2_ORDERS = ("rows", "pairs", "pairs_rev")


def _s2_store_order(oh, ow, order):
    """(y, x0) of the kernel's stride-2 stores in issue order: "rows" row by row (no
    RS_POST_BATCH), "pairs" two rows per column block (RS_POST_BATCH; an odd last row
    pairs with itself), "pairs_rev" the same with the blocks last to first (RS_S2_REV)."""
    blocks = list(range(0, ow, 32))
    if order == "rows":
        return [(y, x0) for y in range(oh) for x0 in blocks]
    if order not in S2_ORDERS:
        raise ValueError(f"s2 store order {order!r}")
    if order == "pairs_rev":
        blocks = blocks[::-1]
    out = []
    for y in range(0, oh, 2):
        for x0 in blocks:
            out += [(y, x0), (min(y + 1, oh - 1), x0)]
    return out


def _s2_direct(row, vt, phase, bias_at=None, order="rows"):
    """Word-level model of rs_s2 under RS_S2_DIRECT (tests/teacher_resident_probe.c):
    the kernel's word offsets, 32-word loads, deals and zero-lane stores in the kernel's
    store order (_s2_store_order), with the simulator's arithmetic. Raises if any access
    leaves the bounds the native check promises (reads inside the input planes, writes
    inside the output tensor)."""
    import numpy as np

    kind, cpi, cpo, k, clip, nb, blk0, nj, ws, dpar, npar, vin, vout, vres, vpar = (
        int(x) for x in row[:15]
    )
    onb, oblk, ows, oh, ow, _tmp = (int(x) for x in row[15:21])
    words = vt.reshape(-1, 2)  # word i = (vector i // 32, lane i % 32)
    kk = 9
    coef = vt[vpar:].reshape(-1, 2)[: cpi * kk].reshape(cpi, kk, 2).astype(np.float32)
    b0 = vpar + (cpi * kk + 31) // 32 if bias_at is None else bias_at
    bias = vt[b0 : b0 + cpi, 0, :]
    vt[vout : vout + cpo * onb] = 0
    off0 = 32 * blk0 + 1 + phase + phase * ws - ws
    lo_in, hi_in = 32 * vin, 32 * (vin + cpi * nb)
    lo_out, hi_out = 32 * vout, 32 * (vout + cpo * onb)
    for c in range(cpi):
        base = 32 * (vin + nb * c) + off0
        out = 32 * (vout + onb * c) + 32 * oblk + 2
        vals = {}
        for y in range(oh):
            for x0 in range(0, ow, 32):
                acc = np.zeros((32, 2), np.float32)
                for dy in range(3):
                    p = base + 2 * y * ws + 2 * x0 + dy * ws
                    if p < lo_in or p + 66 > hi_in:
                        raise ValueError("direct stride-2 read outside the input")
                    taps = (
                        words[p : p + 64 : 2],
                        words[p + 1 : p + 65 : 2],
                        words[p + 2 : p + 66 : 2],
                    )
                    for dx, tap in enumerate(taps):
                        acc = acc + (tap.astype(np.float32) * coef[c, 3 * dy + dx]).astype(
                            np.float32
                        )
                v = np.clip(
                    (acc.astype(np.float64).astype(np.float16).astype(np.float64) + bias[c]).astype(
                        np.float16
                    ),
                    0,
                    6,
                )
                v[min(32, ow - x0) :] = 0
                vals[y, x0] = v
        # the inputs and outputs are separate tensors: every value first, then the stores
        for y, x0 in _s2_store_order(oh, ow, order):
            q = out + y * ows + x0
            if q < lo_out or q + 32 > hi_out:
                raise ValueError("direct stride-2 store outside the output")
            words[q : q + 32] = vals[y, x0]


def simulate(plan, window, *, s2_phase=1, s2_direct=False, crops=1, resident=None, load=True, tiles=False,
             deep8=False, cbias=False, win_hi=None, s2_order="rows"):
    """Run a plan against `window` (bytearray covering WIN_LO..WIN_HI) with the
    kernel's addressing; VTCM starts each resident job filled with NaN. The
    caller performs setup_writes(plan) first, exactly as the harness does; the
    table is read back from the window for every job. s2_phase 1 is the
    teacher's (2y+1, 2x+1) decimation; 0 is the RS_S2_EVEN (Mercury) build.
    s2_direct models an RS_S2_DIRECT build's stride-2 data path word by word
    (_s2_direct); its arithmetic is the stride-1 path's, so results must match.
    crops > 1 runs an RS_REPLAY plan's rs jobs once per crop, as the kernel does:
    replay_row addressing, and VTCM carried from crop to crop (not refilled).
    resident: an RS_RESIDENT plan's persistent VTCM, a (RES_SPAN, 32, 2) float16 array
    whose blob region survives between calls; the borrowed span is refilled with NaN
    each job. Resident images are copied from the window only when `load` (crop 0),
    as the kernel does when the region is not the session's. tiles models an RS_TILES
    build: word 15 of kinds 1 and 2 puts the input (1), output (2) or residual (4) in
    B-tile layout, vector (pair p, block j) at j * pairs + p; a tile output is not
    cleared first. Other builds ignore the word. cbias models an RS_CBIAS build (packed
    biases expanded per op at RES_SPAN - XB_N; resident only); win_hi an RS_WIN_WIDE
    build's window top (the window bytearray then covers WIN_LO..win_hi). s2_order: the
    direct stride-2 kernel's store order (_s2_store_order): "pairs" for an RS_POST_BATCH
    build, "pairs_rev" with RS_S2_REV; the default "rows" is the order without either."""
    import numpy as np
    from hmx_block_layout import unpack

    if s2_phase not in (0, 1):
        raise ValueError("s2_phase must be 0 or 1")

    def vecs(off, n):
        return np.frombuffer(
            bytes(window[off - T.WIN_LO : off - T.WIN_LO + 128 * n]), "<f2"
        ).reshape(n, 32, 2)

    def put(off, data):
        window[off - T.WIN_LO : off - T.WIN_LO + len(data)] = data

    def f16(x):
        return np.asarray(x, np.float64).astype(np.float16)

    for job in plan.jobs:
        for off, data in job.staged.items():
            put(off, data)
        tab = np.frombuffer(
            bytes(window[plan.table_at - T.WIN_LO : plan.table_at - T.WIN_LO + len(plan.table)]),
            "<u4",
        ).reshape(-1, 32)
        if job.kind == "s2":
            for off, data in T.model_dw(
                job.desc, lambda o, n: bytes(window[o - T.WIN_LO : o - T.WIN_LO + n])
            ).items():
                put(off, data)
            continue
        if not 1 <= crops <= plan.max_crops:
            raise ValueError(f"crops must be 1..{plan.max_crops}")
        if resident is not None:
            if resident.shape != (RES_SPAN, 32, 2):
                raise ValueError("resident VTCM must be (RES_SPAN, 32, 2)")
            vt = resident
            vt[:SPAN] = np.nan
        else:
            vt = np.full((SPAN, 32, 2), np.nan, np.float16)
        rows = [
            (k, np.asarray(replay_row(r, k, plan.crop_stride, win_hi), np.uint32) if k else r)
            for k in range(crops)
            for r in tab[job.first : job.end]
        ]
        for crop, row in rows:
            if row[0] == 6:
                import teacher_heads_layout as H

                if not H.native_ok(row):
                    raise ValueError("invalid heads descriptor")
                dpar, npar, vpar = (int(row[i]) for i in (9, 10, 14))
                vt[vpar : vpar + npar] = vecs(dpar, npar)
                H.simulate_op(row, vt)
                continue
            if row[0] == 7:  # RS_MHEADS regrid: exact word copy between grids
                if not regrid_ok(row):
                    raise ValueError("invalid regrid descriptor")
                w = [int(x) for x in row]
                np_, nph, oh, ow, snb, dnb, vin, vout, sws, drow, dcol, fl = w[1:13]
                dp = nph * np_ if fl & 2 else np_
                vt[vout : vout + dp * dnb] = 0
                words = vt.reshape(-1, 2)
                for t in range(nph):
                    sb, db = w[14 + 2 * t], w[15 + 2 * t]
                    for q in range(np_):
                        sp = t * np_ + q if fl & 1 else q
                        dq = t * np_ + q if fl & 2 else q
                        s0, d0 = 32 * (vin + sp * snb) + sb, 32 * (vout + dq * dnb) + db
                        for y in range(oh):
                            words[d0 + y * drow : d0 + y * drow + ow * dcol : dcol] = words[
                                s0 + y * sws : s0 + y * sws + ow
                            ]
                continue
            if not native_ok(
                row, s2_direct=s2_phase if s2_direct else None, mheads=True, resident=resident is not None,
                tiles=tiles, deep8=deep8, cbias=cbias, win_hi=win_hi,
            ):
                raise ValueError("invalid resident descriptor")
            kind, cpi, cpo, k, clip, nb, blk0, nj, ws, dpar, npar, vin, vout, vres, vpar = (
                int(x) for x in row[:15]
            )
            if kind == 3:
                vt[vpar : vpar + npar] = vecs(dpar, npar)
                continue
            if kind == 4:
                put(dpar, vt[vpar : vpar + npar].tobytes())
                continue
            if not (resident is not None and int(row[21])) or (load and crop == 0):
                vt[vpar : vpar + npar] = vecs(dpar, npar)
            dst = vout
            lay = int(row[15]) if tiles and kind in (1, 2) else 0
            xb = None
            if cbias:
                if resident is None:
                    raise ValueError("cbias needs the resident VTCM")
                pw_ = kind in (1, 8)
                base = vpar + (((cpo + 15) >> 4) * k * 16 if pw_ else (cpi * k * k + 31) // 32)
                xb = _cbias_expand(vt, base, cpo, nj)
            if kind == 5 and s2_direct:
                _s2_direct(row, vt, s2_phase, xb, s2_order)
                continue
            if kind == 5:
                onb, oblk, ows, oh, ow, vout = (int(x) for x in row[15:21])
                vt[dst : dst + cpo * onb] = 0
            elif not lay & 2:
                vt[vout : vout + cpo * nb] = 0
            if kind in (1, 8):
                if (vpar | npar) % 16:
                    raise ValueError("HMX tiles not 2 KiB aligned")
                no = (cpo + 15) >> 4
                bias = vpar + no * k * 16 if xb is None else xb
                raw = vt[vpar : vpar + no * k * 16].tobytes()
                A = np.block(
                    [
                        [unpack(raw[2048 * (o * k + t) : 2048 * (o * k + t + 1)]) for t in range(k)]
                        for o in range(no)
                    ]
                )
                words = vt.reshape(-1, 2)
                for j in range(nj):
                    B = np.zeros((k * 32, 32))
                    if kind == 8:  # tap t of pair q at word offset (t//3 - 1) ws + t%3 - 1
                        rows = []
                        for t in range(9):
                            off = (t // 3 - 1) * ws + t % 3 - 1
                            for q in range(cpi):
                                at = 32 * (vin + q * nb + blk0 + j) + off
                                rows.append(words[at : at + 32])
                        blk = np.stack(rows)  # [9 cpi, 32, 2]
                    elif lay & 1:
                        blk = vt[vin + j * cpi : vin + (j + 1) * cpi]
                    else:
                        blk = vt[vin + blk0 + j : vin + blk0 + j + cpi * nb : nb]  # [cpi, 32, 2]
                    B[: 2 * len(blk)] = blk.astype(np.float64).transpose(0, 2, 1).reshape(2 * len(blk), 32)
                    acc = f16(A @ B)[: 2 * cpo].reshape(cpo, 2, 32).transpose(0, 2, 1)
                    v = f16(acc.astype(np.float64) + vt[bias : bias + cpo].astype(np.float64))
                    if clip:
                        v = np.clip(v, 0, 6).astype(np.float16)
                    if vres:
                        rv = (
                            vt[vres + j * cpo : vres + (j + 1) * cpo]
                            if lay & 4
                            else vt[vres + blk0 + j : vres + blk0 + j + cpo * nb : nb]
                        )
                        v = f16(v.astype(np.float64) + rv.astype(np.float64))
                    m = vt[bias + cpo + j].view("<u4").reshape(32) != 0
                    ov = np.where(m[None, :, None], v, 0)
                    if lay & 2:
                        vt[vout + j * cpo : vout + (j + 1) * cpo] = ov
                    else:
                        vt[vout + blk0 + j : vout + blk0 + j + cpo * nb : nb] = ov
            else:
                kk, r = k * k, k // 2
                coef = vt[vpar:].reshape(-1, 2)[: cpi * kk].reshape(cpi, kk, 2).astype(np.float32)
                bias = vpar + (cpi * kk + 31) // 32 if xb is None else xb
                src = vt[vin : vin + cpi * nb].reshape(cpi, nb * 32, 2).astype(np.float32)
                o = 32 * blk0 + np.arange(nj * 32)
                acc = np.zeros((cpi, nj * 32, 2), np.float32)
                t = 0
                for dy in range(k):
                    for dx in range(k):
                        acc = acc + (
                            src[:, o + (dy - r) * ws + (dx - r)] * coef[:, t][:, None, :]
                        ).astype(np.float32)
                        t += 1
                bv = vt[bias : bias + cpi, 0, :].astype(np.float64)  # splatted: lane 0 suffices
                v = np.clip(f16(f16(acc).astype(np.float64) + bv[:, None, :]), 0, 6)
                m = (vt[bias + cpi : bias + cpi + nj].view("<u4") != 0).reshape(-1)
                out = np.where(m[None, :, None], v, 0).astype(np.float16)
                if lay & 2:  # (pair c, block j) at j * cpi + c
                    vt[vout : vout + nj * cpi] = (
                        out.reshape(cpi, nj, 32, 2).transpose(1, 0, 2, 3).reshape(-1, 32, 2)
                    )
                    continue
                flat = vt[vout : vout + cpi * nb].reshape(cpi, nb * 32, 2)
                flat[:, 32 * blk0 : 32 * (blk0 + nj)] = out
                vt[vout : vout + cpi * nb] = flat.reshape(-1, 32, 2)
                if kind == 5:
                    target = vt[dst : dst + cpo * onb].reshape(cpo, onb * 32, 2)
                    for y in range(oh):
                        start = 32 * blk0 + (2 * y + s2_phase) * ws + 2 + s2_phase
                        to = 32 * oblk + y * ows + 2
                        target[:, to : to + ow] = flat[:, start : start + 2 * ow : 2]
    return window


# ---------------------------------------------------------------- native check mirror


def native_ok(row, *, dw_blocks=2, s2_direct=None, pw_group=1, mheads=False, resident=False, tiles=False, deep8=False,
              cbias=False, win_hi=None):
    """Mirror of rs_get in tests/teacher_resident_probe.c (True = accepted).
    s2_direct: the decimation phase (0 even, 1 odd) of an RS_S2_DIRECT build, which
    adds bounds on the direct kernel's reads and zero-lane store spill. pw_group: an
    RS_PW_GROUP build's 1x1 scratch, pw_group * (nk + no) * 16 vectors. mheads: an
    RS_MHEADS build, which also accepts kind 8 (3x3 HMX); kind 7 is regrid_ok. resident: an
    RS_RESIDENT build (word 21 = scratch offset of a resident op). tiles: an RS_TILES build
    (word 15 of kinds 1 and 2 = tile layout flags; see _mercury_layout.build_plan)."""
    if dw_blocks not in (2, 4):
        raise ValueError("dw_blocks must be 2 or 4")
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
    if cbias and not resident:
        raise ValueError("cbias is an option of the resident build")
    hi = T.WIN_HI if win_hi is None else win_hi
    top = RES_SPAN - XB_N if cbias else RES_SPAN
    bad = (big > 0x3FFF) or ((kind - 1) & m > 4 and not p3) or vpar + npar > (top if res else SPAN) or (dpar | align) & 127
    bad = bad or (resident and vpar >= RES_SPAN) or (res and vpar < RES_LO) or (res and kind in (3, 4))
    bad = bad or (res and (kind == 1 or p3) and rscr & 15)
    bad = bad or ((dpar - T.WIN_LO) & m) + 128 * npar > hi - T.WIN_LO or vpar == 0
    bad = bad or dpar < T.WIN_LO or dpar > hi or npar > (hi - dpar) // 128
    if kind in (3, 4) or bad:
        return not bad
    out_nb = onb if s2 else nb
    if s2:
        bad = (
            oh == 0
            or ow == 0
            or (oh | ow | ows | ws) > 255
            or ows != ow + 2
            or ws != 2 * ow + 2
            or onb == 0
            or tmp == 0
            or tmp + cpo * nb > SPAN
            or vres != 0
            or clip != 1
        )
        if bad:
            return False
        if 2 * oh * ws > 32 * nj or 32 * oblk + (oh - 1) * ows + 2 + ow > 32 * onb:
            return False
        if s2_direct is not None:
            ph, xl = s2_direct, 32 * ((ow - 1) >> 5)
            if 32 * blk0 + 1 + ph + (ph + 2 * oh - 1) * ws + 2 * xl + 65 >= 32 * nb:
                return False
            if 32 * oblk + 2 + (oh - 1) * ows + xl + 32 > 32 * onb:
                return False
    kk, reach, no = k * k, (k >> 1) * (ws + 1), (cpo + 15) >> 4
    pw = kind == 1 or p3
    bias = vpar + (no * k * 16 if pw else (cpi * kk + 31) // 32)
    scr = rscr if res else vpar + npar
    bad = (
        cpi == 0
        or nj == 0
        or vin == 0
        or vout == 0
        or clip > 1
        or blk0 + nj > nb
        or vin + cpi * nb > SPAN
        or vout + cpo * out_nb > SPAN
        or vres + cpo * nb > SPAN
        or bias + (-(-cpo // 32) if cbias else cpo) + nj > vpar + npar
        or (cbias and cpo + nj > XB_N)
        or scr + ((pw_group * (k + no) if pw_group > 1 else k) * 16 if pw else cpi * kk) > SPAN
    )
    if p3:
        bad = bad or k != (9 * cpi + 15) >> 4 or 32 * blk0 < ws + 1 or 32 * (blk0 + nj) + ws + 1 > 32 * nb
    elif pw:
        bad = bad or k != (cpi + 15) >> 4
    else:
        bad = (
            bad
            or cpi != cpo
            or k not in (3, 5)
            or nj & (dw_blocks - 1)
            or 32 * blk0 < reach
            or 32 * (blk0 + nj) + reach > 32 * nb
        )
    if tiles and not s2:
        lay = int(row[15])
        lim = (15 if deep8 else 7) if kind == 1 else 2 if kind == 2 else 0
        bad = (
            bad
            or lay & ~lim
            or (lay & 8 and (not lay & 1 or no > k or scr + 32 * 16 * no > SPAN))
            or (lay & 1 and (cpi | vin) & 15)
            or (lay & 2 and (cpo | vout) & 15)
            or (lay & 4 and ((cpo | vres) & 15 or vres == 0))
        )
    return not bad


def regrid_ok(row):
    """Mirror of rg_bad in tests/teacher_resident_probe.c (True = accepted)."""
    w = [int(x) for x in row[:32]]
    big = 0
    for x in w:
        big |= x
    np_, nph, oh, ow, snb, dnb, vin, vout, sws, drow, dcol, fl = w[1:13]
    if (
        w[0] != 7 or big > 0x3FFF or np_ == 0 or nph == 0 or nph > 9 or oh == 0 or ow == 0
        or snb == 0 or dnb == 0 or vin == 0 or vout == 0 or fl > 3 or w[13] != 0 or dcol == 0
    ):
        return False
    sp = nph * np_ if fl & 1 else np_
    dp = nph * np_ if fl & 2 else np_
    if vin + sp * snb > SPAN or vout + dp * dnb > SPAN:
        return False
    if vin < vout + dp * dnb and vout < vin + sp * snb:
        return False
    for t in range(nph):
        if w[14 + 2 * t] + (oh - 1) * sws + ow > 32 * snb:
            return False
        if w[15 + 2 * t] + (oh - 1) * drow + (ow - 1) * dcol >= 32 * dnb:
            return False
    return True
