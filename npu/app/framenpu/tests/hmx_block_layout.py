"""Geometry, packed layouts, device-gather emulation and CPU oracle for the
fused HMX spatial->projection block prototype (tests/hmx_block_probe.c).

Single source of truth for compile-time constants: the builder passes these
values as -D macros and the kernel re-derives and statically asserts them.
NumPy is only needed for payload/oracle helpers, not for the geometry table.

Layouts (all FP16, HMX-packed):
* packed tile: 16 vectors; vector r, word c holds rows (2r, 2r+1) at column c.
* planar-packed activation: vector [channel pair cp][position block b], word c
  holds channels (2cp, 2cp+1) at linear position 32b+c. Input bands carry one
  zero front guard block plus explicit halo rows (zero outside the image), so
  the device needs only column (x) masks. The block output is written in the
  same planar-packed layout without guards.
* stage-1 K index: k = tap*Cin + channel (tap = 3*ky + kx); K tiles of 32.
"""

from dataclasses import dataclass
import math

MAGIC = 0x31425346  # "FSB1"
OPCODE = 101
HEADER_BYTES = 640  # header vector, reserved zero vector, three bias vectors
META_BYTES = 384  # scalars, phase ticks, raw finite-flag vector
BIAS_SLOTS = 6
VTCM_BYTES = 8192
ARENA_BYTES = 0x20000
TAPS = 9


@dataclass(frozen=True)
class Geometry:
    name: str
    gid: int
    width: int
    rows: int  # output rows in the band
    cin: int
    cmid: int  # mid channels computed by one job
    cout: int
    cmid_total: int  # mid channels of the whole layer (partial jobs)
    role: str

    @property
    def nk1(self):
        return math.ceil(TAPS * self.cin / 32)

    @property
    def nm(self):
        return math.ceil(self.cmid / 32)

    @property
    def no(self):
        return math.ceil(self.cout / 32)

    @property
    def nout(self):
        return self.rows * self.width

    @property
    def nj(self):
        return math.ceil(self.nout / 32)

    @property
    def nbin(self):
        band = (self.rows + 2) * self.width
        lo_max = self.nj - 1 + (2 * self.width + 33) // 32
        return max(1 + math.ceil(band / 32), lo_max + 2)

    @property
    def fused(self):
        return self.nm == 1

    @property
    def xt_offset(self):
        return HEADER_BYTES

    @property
    def src_offset(self):
        return HEADER_BYTES + self.nj * 128

    @property
    def w1_offset(self):
        return self.src_offset + (self.cin // 2) * self.nbin * 128

    @property
    def w2_offset(self):
        return self.w1_offset + self.nm * self.nk1 * 2048

    @property
    def input_bytes(self):
        return self.w2_offset + self.no * self.nm * 2048

    @property
    def output_bytes(self):
        return (
            META_BYTES + (self.cout // 2) * self.nj * 128 + (self.nm * 2048 if self.nm > 1 else 0)
        )

    @property
    def k_tiles_per_block(self):
        """HMX load pairs per output position block (stage 1 + projection)."""
        return self.nm * self.nk1 + self.no * self.nm

    @property
    def transactions_per_block(self):
        return self.nm + self.no

    def macros(self):
        return {
            "FB_GEOM": self.gid,
            "FB_W": self.width,
            "FB_R": self.rows,
            "FB_CIN": self.cin,
            "FB_NM": self.nm,
            "FB_CMID": self.cmid,
            "FB_COUT": self.cout,
            "FB_NBIN": self.nbin,
        }

    def check(self):
        if self.cin % 2 or self.cout % 2 or self.cmid % 2:
            raise ValueError("channel counts must be even (channel pairs)")
        if self.nm + self.no > BIAS_SLOTS:
            raise ValueError("bias slots exceeded")
        if self.input_bytes % 128 or self.output_bytes % 128:
            raise ValueError("unaligned arena layout")
        output_offset = (self.input_bytes + 127) & ~127
        if output_offset + self.output_bytes > ARENA_BYTES:
            raise ValueError(f"{self.name} exceeds the 128 KiB job arena")
        if self.width >= 2**15 or self.nj * 32 >= 2**15:
            raise ValueError("lane/x vector packs 16-bit fields")


GEOMETRIES = (
    Geometry(
        "w050_112",
        1,
        112,
        8,
        16,
        16,
        8,
        16,
        "half-width fused 3x3 at 112x112 (convdwclip_0+conv_10 shape), 8-row band, synthetic weights",
    ),
    Geometry(
        "w050_14",
        2,
        14,
        14,
        24,
        144,
        24,
        144,
        "half-width fused 3x3 at 14x14 (convclip_6+convdwclip_6+conv_22 shape), whole map, synthetic weights",
    ),
    Geometry(
        "w050_7",
        3,
        7,
        7,
        56,
        64,
        56,
        336,
        "half-width fused 3x3 at 7x7 (convclip_12+convdwclip_12+conv_34 shape); 64 of 336 mid channels per job (arena bound), synthetic weights",
    ),
    Geometry(
        "teacher_112",
        4,
        112,
        8,
        24,
        24,
        16,
        24,
        "public v20a convdwclip_0 embedded diagonally + conv_10 projection, 8-row band, real weights",
    ),
)
BY_NAME = {g.name: g for g in GEOMETRIES}
for _g in GEOMETRIES:
    _g.check()


def tap_shift(tap, width):
    """Guarded source offset s (lo block = j + s//32, byte shift 4*(s%32))."""
    dy, dx = divmod(tap, 3)
    return dy * width + dx + 31


# ----------------------------------------------------------------- NumPy helpers


def pack(matrix):
    import numpy as np

    m = np.asarray(matrix, dtype="<f2")
    if m.shape != (32, 32):
        raise ValueError("packed tile must be 32x32")
    return m.reshape(16, 2, 32).transpose(0, 2, 1).copy()


def unpack(raw):
    import numpy as np

    return (
        np.frombuffer(bytes(raw), dtype="<f2")
        .reshape(16, 32, 2)
        .transpose(0, 2, 1)
        .reshape(32, 32)
        .astype(np.float64)
    )


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


def band(image, y0, rows):
    """Rows y0-1 .. y0+rows of a [C,H,W] image with zero rows outside it."""
    import numpy as np

    c, h, w = image.shape
    out = np.zeros((c, rows + 2, w), dtype=image.dtype)
    for i in range(rows + 2):
        y = y0 - 1 + i
        if 0 <= y < h:
            out[:, i] = image[:, y]
    return out


def weight_tiles(g, w1, w2):
    """w1 [cmid, cin, 3, 3] and w2 [cout, cmid] -> packed A tiles."""
    import numpy as np

    k = np.zeros((g.nm * 32, g.nk1 * 32), dtype=np.float16)
    flat = np.transpose(np.asarray(w1, dtype=np.float16).reshape(g.cmid, g.cin, TAPS), (0, 2, 1))
    k[: g.cmid, : TAPS * g.cin] = flat.reshape(g.cmid, TAPS * g.cin)
    a1 = [
        pack(k[32 * m : 32 * m + 32, 32 * t : 32 * t + 32])
        for m in range(g.nm)
        for t in range(g.nk1)
    ]
    p = np.zeros((g.no * 32, g.nm * 32), dtype=np.float16)
    p[: g.cout, : g.cmid] = w2
    a2 = [
        pack(p[32 * o : 32 * o + 32, 32 * m : 32 * m + 32])
        for o in range(g.no)
        for m in range(g.nm)
    ]
    return a1, a2


def payload(g, band_input, w1, w2, b1, b2, *, y0=0, m0=0):
    """Complete input arena bytes. band_input is [cin, rows+2, width] FP16."""
    import numpy as np

    header = np.zeros(32, dtype="<u4")
    header[:13] = [
        MAGIC,
        g.gid,
        g.cin,
        g.cmid,
        g.cout,
        g.width,
        g.rows,
        g.nbin,
        g.nj,
        g.nm,
        g.nk1,
        m0,
        y0,
    ]
    pos = np.arange(g.nj * 32, dtype="<u4")
    xt = (pos << 16) | (pos % g.width)
    bias = np.zeros((BIAS_SLOTS, 32), dtype="<f2")
    b1p = np.zeros(g.nm * 32, dtype=np.float16)
    b1p[: g.cmid] = b1
    b2p = np.zeros(g.no * 32, dtype=np.float16)
    b2p[: g.cout] = b2
    for m in range(g.nm):
        bias[m] = b1p[32 * m : 32 * m + 32]
    for o in range(g.no):
        bias[g.nm + o] = b2p[32 * o : 32 * o + 32]
    src = planar(np.asarray(band_input, dtype=np.float16).reshape(g.cin, -1), g.nbin, 1)
    a1, a2 = weight_tiles(g, w1, w2)
    raw = (
        header.tobytes()
        + bytes(128)
        + bias.tobytes()
        + bytes(HEADER_BYTES - 256 - bias.nbytes)
        + xt.astype("<u4").tobytes()
        + src.tobytes()
        + b"".join(t.tobytes() for t in a1)
        + b"".join(t.tobytes() for t in a2)
    )
    if len(raw) != g.input_bytes:
        raise AssertionError("payload layout differs from geometry")
    return raw


def emulate_gather(g, src, j, t):
    """Bit-exact NumPy model of the device stage+pack for K tile t of block j.

    src is the planar array returned by planar(); returns the [16,32,2] tile.
    """
    import numpy as np

    words = src.view("<u4").reshape(g.cin // 2, g.nbin, 32)
    tile = np.zeros((16, 32), dtype="<u4")
    x = (32 * j + np.arange(32)) % g.width
    for rr in range(16):
        k = 32 * t + 2 * rr
        tap, ch = divmod(k, g.cin)
        if tap >= TAPS:
            continue
        s = tap_shift(tap, g.width)
        lo, r = j + s // 32, s % 32
        both = np.concatenate([words[ch // 2, lo], words[ch // 2, lo + 1]])
        v = both[r : r + 32].copy()
        dx = tap % 3
        if dx == 0:
            v[x == 0] = 0
        elif dx == 2:
            v[x == g.width - 1] = 0
        tile[rr] = v
    return tile.view("<f2").reshape(16, 32, 2)


def im2col(g, band_input, j):
    """Reference K x 32 matrix for block j directly from the band image."""
    import numpy as np

    out = np.zeros((g.nk1 * 32, 32))
    for c in range(32):
        p = 32 * j + c
        y, x = divmod(p, g.width)
        for tap in range(TAPS):
            ky, kx = divmod(tap, 3)
            yy, xx = y + ky, x + kx - 1
            if 0 <= xx < g.width and yy < g.rows + 2:
                out[tap * g.cin : (tap + 1) * g.cin, c] = band_input[:, yy, xx]
    return out


def oracle(g, band_input, w1, w2, b1, b2):
    """Expected mid/output with conservative per-value error allowances.

    Models: HMX accumulation then one half rounding at store; QFloat16 bias
    add rounded to half; clip6 on mid; linear projection. Allowances bound
    gross corruption only; they are not tracking-accuracy tolerances.
    """
    import numpy as np

    x = np.asarray(band_input, dtype=np.float16).astype(np.float64)
    w1h = np.asarray(w1, dtype=np.float16).astype(np.float64).reshape(g.cmid, g.cin, 3, 3)
    w2h = np.asarray(w2, dtype=np.float16).astype(np.float64)
    b1h = np.asarray(b1, dtype=np.float16).astype(np.float64)[:, None]
    b2h = np.asarray(b2, dtype=np.float16).astype(np.float64)[:, None]
    pad = np.zeros((g.cin, g.rows + 2, g.width + 2))
    pad[:, :, 1:-1] = x
    c1 = np.zeros((g.cmid, g.nout))
    cond1 = np.zeros_like(c1)
    for ky in range(3):
        for kx in range(3):
            patch = pad[:, ky : ky + g.rows, kx : kx + g.width].reshape(g.cin, -1)
            c1 += w1h[:, :, ky, kx] @ patch
            cond1 += np.abs(w1h[:, :, ky, kx]) @ np.abs(patch)
    h = lambda v: v.astype(np.float16).astype(np.float64)  # noqa: E731
    k1 = g.nk1 * 32
    gamma1 = (k1 * 2**-23) / (1 - k1 * 2**-23)
    r1 = h(c1)
    pre = h(r1 + b1h)
    mid = np.clip(pre, 0, 6)
    allow1 = gamma1 * cond1 + 2**-10 * np.abs(r1) + 2**-9 * (np.abs(r1) + np.abs(b1h)) + 2**-14
    c2 = w2h @ mid
    cond2 = np.abs(w2h) @ np.abs(mid)
    k2 = g.nm * 32
    gamma2 = (k2 * 2**-23) / (1 - k2 * 2**-23)
    r2 = h(c2)
    out = h(r2 + b2h)
    allow2 = (
        np.abs(w2h) @ allow1
        + gamma2 * cond2
        + 2**-10 * np.abs(r2)
        + 2**-9 * (np.abs(r2) + np.abs(b2h))
        + 2**-14
    )
    if np.max(cond1) > 60000 or np.max(cond2) > 60000 or not np.isfinite(out).all():
        raise ValueError("fixture exceeds half range bound")
    return {"mid": mid, "mid_allow": allow1, "out": out, "out_allow": allow2}


def exact_reference(g, band_input, w1, w2, b1, b2):
    """FP64 semantics with FP16-quantized inputs/weights and no other rounding."""
    import numpy as np

    x = np.asarray(band_input, dtype=np.float16).astype(np.float64)
    w1h = np.asarray(w1, dtype=np.float16).astype(np.float64).reshape(g.cmid, g.cin, 3, 3)
    pad = np.zeros((g.cin, g.rows + 2, g.width + 2))
    pad[:, :, 1:-1] = x
    c1 = sum(
        w1h[:, :, ky, kx] @ pad[:, ky : ky + g.rows, kx : kx + g.width].reshape(g.cin, -1)
        for ky in range(3)
        for kx in range(3)
    )
    mid = np.clip(c1 + np.asarray(b1, dtype=np.float64)[:, None], 0, 6)
    return np.asarray(w2, dtype=np.float64) @ mid + np.asarray(b2, dtype=np.float64)[:, None]


def synthetic_fixture(g, seed=0):
    """Deterministic dyadic synthetic activations/weights (no recorded data)."""
    import numpy as np

    rng = np.random.default_rng(1000 * g.gid + seed)
    image_rows = g.rows + 2
    x = (rng.integers(-24, 25, size=(g.cin, image_rows, g.width)) / 32).astype(np.float16)
    if g.rows == g.width:  # whole map: halo rows are the image's zero padding
        x[:, 0] = 0
        x[:, -1] = 0
    w1 = (rng.integers(-8, 9, size=(g.cmid, g.cin, 3, 3)) / (16 * math.sqrt(9 * g.cin))).astype(
        np.float16
    )
    w2 = (rng.integers(-8, 9, size=(g.cout, g.cmid)) / (8 * math.sqrt(g.cmid))).astype(np.float16)
    b1 = (rng.integers(-8, 9, size=g.cmid) / 32).astype(np.float16)
    b2 = (rng.integers(-8, 9, size=g.cout) / 32).astype(np.float16)
    return x, w1, w2, b1, b2


def teacher_fixture(g, graph, y0, seed=0):
    """Public convdwclip_0 (diagonal) + conv_10 weights; synthetic clip6-range input."""
    import numpy as np
    from framenpu.landmark import specifications, weights_and_bias

    specs = {s["name"]: s for s in specifications(graph)}
    dw, db = weights_and_bias(graph, specs["convdwclip_0"])
    pw, pb = weights_and_bias(graph, specs["conv_10"])
    dw = np.asarray(dw, dtype=np.float16).reshape(g.cmid, TAPS)
    w1 = np.zeros((g.cmid, g.cin, 3, 3), dtype=np.float16)
    for ch in range(g.cmid):
        w1[ch, ch] = dw[ch].reshape(3, 3)
    w2 = np.asarray(pw, dtype=np.float16).reshape(g.cout, g.cmid)
    rng = np.random.default_rng(7 + seed)
    image = (rng.integers(0, 193, size=(g.cin, 112, 112)) / 32).astype(np.float16)
    return band(image, y0, g.rows), w1, w2, np.asarray(db, np.float32), np.asarray(pb, np.float32)
