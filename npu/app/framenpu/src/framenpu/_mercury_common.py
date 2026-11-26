"""Qualified Mercury grid, packed layouts and resident descriptors.

Promoted from the replay qualification helpers. tests/test_mercury_runtime.py
compares the packaged planner's tables and parameter bytes with that oracle.
"""

from dataclasses import dataclass, field
import functools
import math

R = 2
META_BYTES = 1024
WIN_LO, WIN_HI = 0x1A40000, 0x1F00000
WIN_HI_WIDE = 0x1FF0000  # RS_WIN_WIDE: up to the image reservation (canary-soaked arena)
XB_N = 512  # RS_CBIAS: expansion vectors at the top of the blob region
VTCM_SPAN_VECTORS = (0x311E0000 - 0x31002000) // 128
SPAN = VTCM_SPAN_VECTORS
# RS_RESIDENT: parameter images in the FeatureNN model-blob region (0x311e0000..0x316eea80),
# whose first vector (FR(SPAN)) holds the session tag. Offsets continue the FR() span.
RES_SPAN = (0x316EEA80 - 0x31002000) // 128
RES_LO = SPAN + 16
BLOB_FLAG = 0xB12014  # firmware "blob loaded" word, DDR offset (DSP 0xf0b12014)
RS_MAGIC, RS_OPCODE = 0x31535254, 111
KIND = {"pw": 1, "dw": 2, "load": 3, "store": 4, "s2": 5}
VTCM_FIRST = 4
MAX_CROPS = 3


def pack(matrix):
    import numpy as np

    m = np.asarray(matrix, dtype="<f2")
    if m.shape != (32, 32):
        raise ValueError("packed tile must be 32x32")
    return m.reshape(16, 2, 32).transpose(0, 2, 1).copy()


def planar(tensor, blocks, guard):
    """[C, P] -> [C/2, blocks, 32 words of 2 halves] with `guard` leading zero blocks."""
    import numpy as np

    c, p = tensor.shape
    out = np.zeros((c // 2, blocks, 32, 2), dtype="<f2")
    flat = np.zeros((c, blocks * 32), dtype="<f2")
    flat[:, guard * 32 : guard * 32 + p] = tensor
    out[...] = flat.reshape(c // 2, 2, blocks, 32).transpose(0, 2, 3, 1)
    return out


def unplanar(raw, channels, blocks):
    import numpy as np

    v = np.frombuffer(bytes(raw), dtype="<f2").reshape(channels // 2, blocks, 32, 2)
    return v.transpose(0, 3, 1, 2).reshape(channels, blocks * 32).astype(np.float64)


@dataclass(frozen=True)
class Grid:
    w: int
    h: int

    @property
    def ws(self):
        return self.w + R

    @property
    def g0(self):
        g = (-R * self.ws) % 32
        return g + 32 if g < R else g

    @property
    def blk0(self):
        return (self.g0 + R * self.ws) // 32

    @property
    def nj(self):
        return math.ceil(self.h * self.ws / 32)

    @property
    def nj4(self):
        return (self.nj + 3) & ~3

    @property
    def nbin(self):
        last_read = 32 * (self.blk0 + self.nj4) + R * self.ws + R + 31
        whole = self.g0 + (self.h + 2 * R) * self.ws
        return max(last_read // 32 + 1, math.ceil(whole / 32))

    def word(self, y, x):
        return self.g0 + (y + R) * self.ws + R + x

    def interior(self):
        """Boolean per word (nbin*32): True where the word holds a value."""
        import numpy as np

        idx = np.arange(self.nbin * 32) - self.g0
        row, col = np.divmod(idx, self.ws)
        return (idx >= 0) & (row >= R) & (row < R + self.h) & (col >= R)

    def mask_bytes(self):
        """Per block blk0 .. blk0+nj4-1: all-ones words at interior lanes."""
        import numpy as np

        ok = self.interior()[32 * self.blk0 : 32 * (self.blk0 + self.nj4)]
        return np.where(ok, 0xFFFFFFFF, 0).astype("<u4").tobytes()

    def to_bytes(self, tensor):
        import numpy as np

        c = tensor.shape[0]
        idx, _ = _grid_index(self.w, self.h)
        # one u32 word per (pair, position): channel 2i+j is half j of the words of pair i
        pairs = np.asarray(tensor, "<f2").reshape(c // 2, 2, -1).transpose(0, 2, 1).copy()
        out = np.zeros((c // 2, self.nbin * 32), "<u4")
        out[:, idx] = pairs.view("<u4")[..., 0]
        return out.tobytes()

    def from_bytes(self, raw, channels):
        """-> ([C, h, w] float64, count of nonzero non-interior halves)."""
        import numpy as np

        flat = unplanar(raw, channels, self.nbin)
        idx, outside = _grid_index(self.w, self.h)
        stray = int(np.count_nonzero(flat[:, outside]))
        return flat[:, idx].reshape(channels, self.h, self.w), stray

    def tensor_bytes(self, channels):
        return channels // 2 * self.nbin * 128


@functools.lru_cache(maxsize=64)
def _grid_index(w, h):
    """(word of every (y, x) in row-major order, mask of non-interior words) of a grid."""
    import numpy as np

    g = Grid(w, h)
    idx = (g.word(0, 0) + np.arange(h)[:, None] * g.ws + np.arange(w)[None, :]).reshape(-1)
    outside = ~g.interior()
    idx.setflags(write=False)
    outside.setflags(write=False)
    return idx, outside


def _al(n, a=128):
    return (n + a - 1) // a * a


def _packed_pairs(values, pairs):
    """RS_CBIAS: FP16 values -> one word per channel pair (even | odd << 16), padded to
    whole vectors; the kernel splats word pr into bias vector pr."""
    import numpy as np

    h = np.zeros(2 * pairs, np.float16)
    h[: len(values)] = np.asarray(values, np.float32).astype(np.float16)
    w = h.view("<u2").astype("<u4")
    word = np.zeros(-(-pairs // 32) * 32, "<u4")
    word[:pairs] = w[0::2] | (w[1::2] << 16)
    return word.tobytes()


def _splat_pairs(values, pairs):
    """FP16 values -> one splatted vector per channel pair (word = even | odd << 16)."""
    import numpy as np

    h = np.zeros(2 * pairs, np.float16)
    h[: len(values)] = np.asarray(values, np.float32).astype(np.float16)
    w = h.view("<u2").astype("<u4")
    word = w[0::2] | (w[1::2] << 16)
    return np.repeat(word[:, None], 32, 1).astype("<u4").tobytes()


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


def replay_row(row, k, stride, win_hi=WIN_HI):
    """A descriptor as the RS_REPLAY kernel runs it for crop k: load and store
    window addresses (word 9) moved by k * stride, everything else unchanged."""
    row = list(row)
    if row[0] in (KIND["load"], KIND["store"]) and row[9] <= win_hi:
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
                if top and align > 1:  # the highest aligned start that fits
                    start = (at + size - n) // align * align
                    if start < at:
                        continue
                    rest = [(at, start - at)] if start > at else []
                    rest += [(start + n, at + size - start - n)] if at + size > start + n else []
                    self.free[i : i + 1] = rest
                    at = start
                elif top:
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


def native_ok(row, *, dw_blocks=2, s2_direct=None, pw_group=1, mheads=False, resident=False, tiles=False, deep8=False,
              cbias=False, win_hi=WIN_HI):
    """Mirror of rs_get in tests/teacher_resident_probe.c (True = accepted).
    s2_direct: the decimation phase (0 even, 1 odd) of an RS_S2_DIRECT build, which
    adds bounds on the direct kernel's reads and zero-lane store spill. pw_group: an
    RS_PW_GROUP build's 1x1 scratch, pw_group * (nk + no) * 16 vectors. mheads: an
    RS_MHEADS build, which also accepts kind 8 (3x3 HMX); kind 7 is regrid_ok. resident: an
    RS_RESIDENT build (word 21 = scratch offset of a resident op). tiles: an RS_TILES build
    (word 15 of kinds 1 and 2 = tile layout flags; see _mercury_layout.build_plan).
    cbias: an RS_CBIAS build (packed biases; resident images end XB_N below RES_SPAN).
    win_hi: the build's window top (WIN_HI_WIDE for RS_WIN_WIDE)."""
    if cbias and not resident:
        raise ValueError("cbias is an option of the resident build")
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
    top = RES_SPAN - XB_N if cbias else RES_SPAN
    bad = (big > 0x3FFF) or ((kind - 1) & m > 4 and not p3) or vpar + npar > (top if res else SPAN) or (dpar | align) & 127
    bad = bad or (resident and vpar >= RES_SPAN) or (res and vpar < RES_LO) or (res and kind in (3, 4))
    bad = bad or (res and (kind == 1 or p3) and rscr & 15)
    bad = bad or ((dpar - WIN_LO) & m) + 128 * npar > win_hi - WIN_LO or vpar == 0
    bad = bad or dpar < WIN_LO or dpar > win_hi or npar > (win_hi - dpar) // 128
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
