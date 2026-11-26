"""Host layouts, arena plan, fixtures and checks for the layer-chain executor
prototype (tests/hmx_layer_chain_probe.c). Opens no device.

Activation buffers are planar-packed FP16 (one 128-byte vector per channel
pair and 32-position block). Position (y, x) of the zero-padded map,
y = -1 .. h, is word pad + (y+1)*w + x, with pad = 32 - w % 32 so output
block j is the aligned block (pad + w)//32 + j. Every word that is not an
interior position is zero. Layer L writes buffer L+1, which is layer L+1's
input, so a chain runs on the device without host repacking.
"""

from dataclasses import dataclass
import math

from hmx_block_layout import Geometry, TAPS, oracle, planar, unplanar, weight_tiles

MAGIC = 0x3158464C  # "LFX1"
OPCODE = 105
META_BYTES = 1024
MAX_LAYERS = 16
DDR_ALIAS = 0xF0000000
WIN_LO, WIN_HI = 0x1A40000, 0x1F00000  # buffer offsets inside the resident arena
VTCM_SPAN_VECTORS = (0x311E0000 - 0x31002000) // 128
HARD_TICKS = 384000


@dataclass(frozen=True)
class Layer:
    w: int
    h: int
    cin: int
    cmid: int
    cout: int

    @property
    def pad(self):
        return 32 - self.w % 32

    @property
    def nj(self):
        return math.ceil(self.w * self.h / 32)

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
    def nbin(self):
        need_in = self.nj + (self.pad + 2 * self.w + 1) // 32 + 2
        need_out = self.nj + (self.pad + self.w) // 32 + 1
        whole = math.ceil((self.pad + (self.h + 2) * self.w) / 32)
        return max(need_in, need_out, whole)

    def geometry(self, gid=0):
        return Geometry(
            f"chain{self.w}",
            gid,
            self.w,
            self.h,
            self.cin,
            self.cmid,
            self.cout,
            self.cmid,
            "chain",
        )

    def vtcm_vectors(self):
        """Mirror of the kernel's lc_get plan (vectors from 0x31002000)."""
        al = lambda v: (v + 31) & ~31  # noqa: E731
        s1, s2 = self.nk1 * 16, self.nm * 16
        bt = al(self.nm * s1 + self.no * s2)
        bias = al(al(bt + s1) + s2)
        tbl = bias + (self.nm + self.no + 1) // 2
        src = tbl + (self.nk1 + 1) // 2 + self.nj
        return src + self.cin // 2 * self.nbin

    def gather_table(self):
        """Per B row pair: source vector offset | byte shift << 16 | kind << 24."""
        import numpy as np

        words = np.zeros(((self.nk1 + 1) // 2) * 32, dtype="<u4")
        for r in range(self.nk1 * 16):
            tap, ch = divmod(2 * r, self.cin)
            if tap >= TAPS:
                words[r] = 3 << 24
                continue
            s = self.pad - 1 + (tap // 3) * self.w + tap % 3
            words[r] = ((ch // 2) * self.nbin + s // 32) | ((s % 32) * 4) << 16 | (tap % 3) << 24
        if int(words.max() & 0xFFFF) >= 0x4000:
            raise ValueError("gather offset exceeds the kernel's mask")
        return words.tobytes()

    def position_table(self):
        import numpy as np

        pos = np.arange(self.nj * 32, dtype="<u4")
        return ((pos << 16) | (pos % self.w)).astype("<u4").tobytes()


# Stride-1 fused3_w050 runs (3x3 Cin->Cmid + clip6, 1x1 Cmid->Cout); residual
# adds between equal-width blocks are not part of this prototype.
CHAINS = {
    "c14": (
        Layer(14, 14, 24, 144, 24),
        Layer(14, 14, 24, 144, 24),
        Layer(14, 14, 24, 144, 32),
        Layer(14, 14, 32, 192, 32),
        Layer(14, 14, 32, 192, 32),
    ),
    "c7": (Layer(7, 7, 56, 336, 56),) * 4,
}


def check_chain(layers):
    for a, b in zip(layers, layers[1:]):
        if (a.w, a.h, a.cout, a.nbin) != (b.w, b.h, b.cin, b.nbin):
            raise ValueError("layer output layout differs from next input")
    for layer in layers:
        if layer.vtcm_vectors() > VTCM_SPAN_VECTORS:
            raise ValueError("layer exceeds the borrowed VTCM span")
        if layer.cin > 64 or layer.cout > 64 or layer.nm > 11 or layer.no > 2:
            raise ValueError("layer outside the kernel limits")


def plan(layers):
    """Arena offsets: activation buffers 0..N, then per-layer W1, W2, bias."""
    check_chain(layers)
    cursor = WIN_LO
    out = {"act": [], "w": [], "bias": [], "tbl": [], "xt": []}

    def take(size):
        nonlocal cursor
        off = cursor
        cursor += (size + 4095) & ~4095
        if cursor > WIN_HI:
            raise ValueError("chain exceeds the arena window")
        return off

    channels = [layers[0].cin] + [layer.cout for layer in layers]
    for c in channels:
        out["act"].append((take(c // 2 * layers[0].nbin * 128), c // 2 * layers[0].nbin * 128))
    for layer in layers:  # W1 tiles immediately followed by W2 tiles
        out["w"].append(take((layer.nm * layer.nk1 + layer.no * layer.nm) * 2048))
        out["bias"].append(take((layer.nm + layer.no + 1) // 2 * 128))
        out["tbl"].append(take((layer.nk1 + 1) // 2 * 128))
        out["xt"].append(take(layer.nj * 128))
    out["end"] = cursor
    return out


def fixture(layers, seed=0):
    """Deterministic dyadic synthetic input and weights (no recorded data)."""
    import numpy as np

    rng = np.random.default_rng(4242 + seed)
    first = layers[0]
    x0 = (rng.integers(-24, 25, size=(first.cin, first.h, first.w)) / 32).astype(np.float16)
    weights = []
    for layer in layers:
        w1 = (
            rng.integers(-8, 9, size=(layer.cmid, layer.cin, 3, 3))
            / (16 * math.sqrt(9 * layer.cin))
        ).astype(np.float16)
        w2 = (
            rng.integers(-8, 9, size=(layer.cout, layer.cmid)) / (8 * math.sqrt(layer.cmid))
        ).astype(np.float16)
        b1 = (rng.integers(-8, 9, size=layer.cmid) / 32).astype(np.float16)
        b2 = (rng.integers(-8, 9, size=layer.cout) / 32).astype(np.float16)
        weights.append((w1, w2, b1, b2))
    return x0, weights


def to_buffer(image, layer):
    """[C, h, w] -> padded planar bytes for a layer-shaped buffer."""
    import numpy as np

    c = image.shape[0]
    flat = np.zeros((c, layer.nbin * 32), dtype=np.float16)
    for y in range(layer.h):
        start = layer.pad + (y + 1) * layer.w
        flat[:, start : start + layer.w] = image[:, y]
    return planar(flat, layer.nbin, 0).tobytes()


def from_buffer(raw, channels, layer):
    """Padded planar bytes -> ([C, h+2, w] band with halo rows, nonzero padding count)."""
    import numpy as np

    flat = unplanar(raw, channels, layer.nbin)
    lo = layer.pad
    band = flat[:, lo : lo + (layer.h + 2) * layer.w].reshape(channels, layer.h + 2, layer.w)
    interior = np.zeros(flat.shape, dtype=bool)
    interior[:, lo + layer.w : lo + (layer.h + 1) * layer.w] = True
    return band, int(np.count_nonzero(flat[~interior]))


def bias_bytes(layer, b1, b2):
    import numpy as np

    slots = np.zeros(((layer.nm + layer.no + 1) // 2 * 2, 32), dtype="<f2")
    b1p = np.zeros(layer.nm * 32, np.float16)
    b1p[: layer.cmid] = b1
    b2p = np.zeros(layer.no * 32, np.float16)
    b2p[: layer.cout] = b2
    for m in range(layer.nm):
        slots[m] = b1p[32 * m : 32 * m + 32]
    for o in range(layer.no):
        slots[layer.nm + o] = b2p[32 * o : 32 * o + 32]
    return slots.tobytes()


def arena_image(layers, x0, weights):
    """{offset: bytes} for every buffer (activation buffers after 0 are zero)."""
    p = plan(layers)
    writes = {}
    for i, (off, size) in enumerate(p["act"]):
        writes[off] = to_buffer(x0, layers[0]) if i == 0 else bytes(size)
    for i, (layer, (w1, w2, b1, b2)) in enumerate(zip(layers, weights)):
        a1, a2 = weight_tiles(layer.geometry(), w1, w2)
        writes[p["w"][i]] = b"".join(t.tobytes() for t in a1 + a2)
        writes[p["bias"][i]] = bias_bytes(layer, b1, b2)
        writes[p["tbl"][i]] = layer.gather_table()
        writes[p["xt"][i]] = layer.position_table()
    if any(len(writes[off]) != size for off, size in p["act"]):
        raise AssertionError("activation buffer size differs from plan")
    return p, writes


def descriptors(layers, p):
    import numpy as np

    if not 1 <= len(layers) <= MAX_LAYERS:
        raise ValueError("bad chain")
    words = np.zeros(32 + 32 * len(layers), dtype="<u4")
    words[:2] = [MAGIC, len(layers)]
    for i, layer in enumerate(layers):
        words[32 + 32 * i : 48 + 32 * i] = [
            layer.w,
            layer.h,
            layer.cin,
            layer.nm,
            layer.no,
            layer.cout,
            layer.pad,
            layer.nbin,
            p["act"][i][0],
            p["w"][i],
            p["bias"][i],
            p["tbl"][i],
            p["xt"][i],
            p["act"][i + 1][0],
            layer.nk1,
            0,
        ]
    return words.tobytes()


def check_layer(layer, raw_in, raw_out, w1, w2, b1, b2):
    """Device output of one layer against the oracle of its actual device input."""
    import numpy as np

    g = layer.geometry()
    band, stray_in = from_buffer(raw_in, layer.cin, layer)
    ref = oracle(g, band.astype(np.float16), w1, w2, b1, b2)
    out_band, stray = from_buffer(raw_out, layer.cout, layer)
    out = out_band[:, 1:-1].reshape(layer.cout, -1)
    err = np.abs(out - ref["out"])
    if stray_in or stray or not np.isfinite(out).all() or np.any(err > ref["out_allow"]):
        raise ValueError(
            f"layer output differs: stray={stray_in}/{stray} "
            f"max_ratio={float(np.max(err / ref['out_allow'])):.3g}"
        )
    return {
        "values": int(out.size),
        "max_abs": float(err.max()),
        "max_error_to_bound_ratio": float(np.max(err / ref["out_allow"])),
        "output_abs_max": float(np.abs(out).max()),
    }


def emulate_gather(layer, raw_in, j):
    """Bit-exact model of the kernel's table-driven pack for block j -> [nk1*32, 32]."""
    import numpy as np

    words = np.frombuffer(raw_in, "<u4")
    table = np.frombuffer(layer.gather_table(), "<u4")
    xp = np.frombuffer(layer.position_table(), "<u4")[32 * j : 32 * j + 32]
    x = xp & 0xFFFF
    rows = np.zeros((layer.nk1 * 16, 32), "<u4")
    for r in range(layer.nk1 * 16):
        word = int(table[r])
        kind, off, shift = word >> 24, word & 0x3FFF, (word >> 16) & 0x7F
        if kind == 3:
            continue
        base = (off + j) * 32 + shift // 4
        v = words[base : base + 32].copy()
        if kind == 0:
            v[x == 0] = 0
        elif kind == 2:
            v[x == layer.w - 1] = 0
        rows[r] = v
    tiles = rows.reshape(layer.nk1, 16, 32).view("<f2").reshape(layer.nk1, 16, 32, 2)
    return tiles.transpose(0, 1, 3, 2).reshape(layer.nk1 * 32, 32).astype(np.float64)
