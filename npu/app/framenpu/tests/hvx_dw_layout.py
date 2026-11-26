"""Host layouts, fixtures and oracle for the HVX FP16 depthwise probe
(tests/hvx_dw_probe.c). Opens no device.

The input is planar-packed FP16 (one 128-byte vector per channel pair and
32-position block); position (y, x) of the zero-padded map, y = -r .. h-1+r,
is word pad + (y+r)*w + x with (pad + r*w) % 32 == 0 and pad >= r. The output
is compact: block j of pair c is vector c*nj + j, positions past h*w zero.
"""

from dataclasses import dataclass
import math

from hmx_block_layout import planar, unplanar

MAGIC = 0x31574448  # "HDW1"
OPCODE = 106
META_BYTES = 1024
WIN_LO, WIN_HI = 0x1A40000, 0x1F00000
VTCM_SPAN_VECTORS = (0x311E0000 - 0x31002000) // 128


@dataclass(frozen=True)
class DwLayer:
    name: str
    c: int
    w: int
    h: int
    k: int

    @property
    def r(self):
        return self.k // 2

    @property
    def pad(self):
        pad = (-self.r * self.w) % 32
        return pad + 32 if pad < self.r else pad

    @property
    def nj(self):
        return math.ceil(self.w * self.h / 32)

    @property
    def nbin(self):
        amax = self.pad + 2 * self.r * self.w + 2 * self.r
        return max(
            (amax >> 5) + self.nj + 1, math.ceil((self.pad + (self.h + 2 * self.r) * self.w) / 32)
        )

    @property
    def macs(self):
        return self.c * self.w * self.h * self.k * self.k

    def vtcm_vectors(self):
        cp = self.c // 2
        return (
            cp * self.nbin
            + cp * self.nj
            + (cp * self.k * self.k + 31) // 32
            + (cp + 31) // 32
            + self.nj
        )


# Every stride-1 depthwise layer of the public v20a teacher (stride-2
# convdwclip_1/_3/_5/_11 are not covered by this probe).
TEACHER_STRIDE1 = (
    DwLayer("convdwclip_0", 24, 112, 112, 3),
    DwLayer("convdwclip_2", 96, 56, 56, 3),
    DwLayer("convdwclip_4", 144, 28, 28, 5),
    DwLayer("convdwclip_6", 288, 14, 14, 3),
    DwLayer("convdwclip_7", 288, 14, 14, 3),
    DwLayer("convdwclip_8", 288, 14, 14, 5),
    DwLayer("convdwclip_9", 384, 14, 14, 5),
    DwLayer("convdwclip_10", 384, 14, 14, 5),
    DwLayer("convdwclip_12", 672, 7, 7, 5),
    DwLayer("convdwclip_13", 672, 7, 7, 5),
    DwLayer("convdwclip_14", 672, 7, 7, 5),
    DwLayer("convdwclip_15", 672, 7, 7, 3),
)
# Teacher stride-2 depthwise MACs, for scaling the measured rate.
STRIDE2_MACS = 64 * 56 * 56 * 9 + 96 * 28 * 28 * 25 + 144 * 14 * 14 * 9 + 384 * 7 * 7 * 25


def fixture(layer, seed=0):
    """Deterministic dyadic activations in [0, 6) and depthwise weights/bias."""
    import numpy as np

    rng = np.random.default_rng(9000 + seed)
    x = (rng.integers(0, 49, size=(layer.c, layer.h, layer.w)) / 8).astype(np.float16)
    wt = (rng.integers(-16, 17, size=(layer.c, layer.k, layer.k)) / (16 * layer.k)).astype(
        np.float16
    )
    b = (rng.integers(-8, 9, size=layer.c) / 16).astype(np.float16)
    return x, wt, b


def input_bytes(layer, x):
    import numpy as np

    flat = np.zeros((layer.c, layer.nbin * 32), np.float16)
    for y in range(layer.h):
        start = layer.pad + (y + layer.r) * layer.w
        flat[:, start : start + layer.w] = x[:, y]
    return planar(flat, layer.nbin, 0).tobytes()


def coef_bytes(layer, wt):
    import numpy as np

    kk = layer.k * layer.k
    words = np.zeros(((layer.c // 2 * kk + 31) // 32) * 32, "<u4")
    h = wt.reshape(layer.c, kk).astype("<f2").view("<u2").astype("<u4")
    words[: layer.c // 2 * kk] = (h[0::2] | (h[1::2] << 16)).reshape(-1)
    return words.tobytes()


def bias_bytes(layer, b):
    import numpy as np

    words = np.zeros(((layer.c // 2 + 31) // 32) * 32, "<u4")
    h = b.astype("<f2").view("<u2").astype("<u4")
    words[: layer.c // 2] = h[0::2] | (h[1::2] << 16)
    return words.tobytes()


def position_bytes(layer):
    import numpy as np

    pos = np.arange(layer.nj * 32, dtype="<u4")
    return ((pos << 16) | (pos % layer.w)).astype("<u4").tobytes()


def plan(layer):
    sizes = {
        "in": layer.c // 2 * layer.nbin * 128,
        "coef": ((layer.c // 2 * layer.k * layer.k + 31) // 32) * 128,
        "bias": ((layer.c // 2 + 31) // 32) * 128,
        "xt": layer.nj * 128,
        "out": layer.c // 2 * layer.nj * 128,
    }
    out, cursor = {}, WIN_LO
    for name, size in sizes.items():
        out[name] = (cursor, size)
        cursor += (size + 4095) & ~4095
    if cursor > WIN_HI or layer.vtcm_vectors() > VTCM_SPAN_VECTORS:
        raise ValueError(f"{layer.name} exceeds the window or VTCM span")
    out["end"] = cursor
    return out


def arena_image(layer, x, wt, b):
    p = plan(layer)
    data = {
        "in": input_bytes(layer, x),
        "coef": coef_bytes(layer, wt),
        "bias": bias_bytes(layer, b),
        "xt": position_bytes(layer),
        "out": bytes(p["out"][1]),
    }
    for name, raw in data.items():
        if len(raw) != p[name][1]:
            raise AssertionError(f"{name} size differs from plan")
    return p, {p[name][0]: raw for name, raw in data.items()}


def descriptor(layer, p):
    import numpy as np

    words = np.zeros(32, "<u4")
    words[:12] = [
        MAGIC,
        layer.w,
        layer.h,
        layer.c,
        layer.k,
        layer.pad,
        layer.nbin,
        p["in"][0],
        p["coef"][0],
        p["bias"][0],
        p["xt"][0],
        p["out"][0],
    ]
    return words.tobytes()


def oracle(layer, x, wt, b):
    """FP64 reference of clip6(dwconv(x) + b) and a per-value error bound for
    QFloat16 products/sums, FP16 rounding of the sum, bias add and output."""
    import numpy as np

    r, xs = layer.r, x.astype(np.float64)
    padded = np.zeros((layer.c, layer.h + 2 * r, layer.w + 2 * r))
    padded[:, r:-r, r:-r] = xs
    acc = np.zeros((layer.c, layer.h, layer.w))
    mag = np.zeros_like(acc)
    for dy in range(layer.k):
        for dx in range(layer.k):
            term = (
                wt[:, dy, dx, None, None].astype(np.float64)
                * padded[:, dy : dy + layer.h, dx : dx + layer.w]
            )
            acc += term
            mag += np.abs(term)
    pre = acc + b.astype(np.float64)[:, None, None]
    eps = 2.0**-10
    allow = (layer.k * layer.k + 3) * eps * mag + 2 * eps * np.abs(pre) + 2.0**-24
    return np.clip(pre, 0, 6), allow


def check_output(layer, raw, x, wt, b):
    import numpy as np

    got = unplanar(raw, layer.c, layer.nj)
    tail = got[:, layer.w * layer.h :]
    out = got[:, : layer.w * layer.h].reshape(layer.c, layer.h, layer.w)
    ref, allow = oracle(layer, x, wt, b)
    err = np.abs(out - ref)
    if np.any(tail) or not np.isfinite(out).all() or np.any(err > allow):
        raise ValueError(
            f"{layer.name} output differs: tail={int(np.count_nonzero(tail))} "
            f"max_ratio={float(np.max(err / allow)):.3g}"
        )
    return {
        "max_abs": float(err.max()),
        "max_error_to_bound_ratio": float(np.max(err / allow)),
        "output_abs_max": float(np.abs(out).max()),
    }


def emulate(layer, x, wt, b):
    """Lane-level model of the kernel's addressing/masking (FP64 arithmetic),
    from the packed buffers alone; returns the compact output tensor."""
    import numpy as np

    raw = np.frombuffer(input_bytes(layer, x), "<f2").reshape(layer.c // 2, layer.nbin * 32, 2)
    pos = np.arange(layer.nj * 32)
    xl = pos % layer.w
    out = np.zeros((layer.c, layer.nj * 32))
    for c in range(layer.c):
        a = layer.pad - layer.r
        acc = np.zeros(layer.nj * 32)
        for dy in range(layer.k):
            for dx in range(layer.k):
                vals = raw[c // 2, a + pos, c % 2].astype(np.float64)
                ok = (xl + dx - layer.r >= 0) & (xl + dx - layer.r < layer.w)
                acc += np.where(ok, vals, 0) * float(wt[c, dy, dx])
                a += 1
            a += layer.w - layer.k
        out[c] = np.clip(acc + float(b[c]), 0, 6) * (pos < layer.w * layer.h)
    return out[:, : layer.w * layer.h].reshape(layer.c, layer.h, layer.w)


# ---- v2 (tests/hvx_dw2_probe.c): guard-column rows, pre-splatted coefficients ----

MAGIC2 = 0x32574448  # "HDW2"
OPCODE2 = 108


def v2_shape(layer):
    """(ws, nj, nbin): row stride, output blocks (multiple of 4), input blocks."""
    ws = layer.w + layer.r
    nj = ((math.ceil(layer.h * ws / 32)) + 3) & ~3
    last = nj * 32 + (layer.k - 1) * (ws + 1) + 31
    nbin = max(last // 32 + 1, math.ceil((2 * layer.r + (layer.h + 2 * layer.r) * ws) / 32))
    return ws, nj, nbin


def v2_vtcm_vectors(layer):
    ws, nj, nbin = v2_shape(layer)
    cp, kk = layer.c // 2, layer.k * layer.k
    return cp * nbin + cp * nj + cp * kk + (cp * kk + 31) // 32 + (cp + 31) // 32 + nj


def v2_input_bytes(layer, x):
    import numpy as np

    ws, nj, nbin = v2_shape(layer)
    flat = np.zeros((layer.c, nbin * 32), np.float16)
    for y in range(layer.h):
        start = layer.r + (y + layer.r) * ws + layer.r
        flat[:, start : start + layer.w] = x[:, y]
    return planar(flat, nbin, 0).tobytes()


def v2_mask_bytes(layer):
    import numpy as np

    ws, nj, _ = v2_shape(layer)
    q = np.arange(nj * 32)
    ok = ((q % ws) >= layer.r) & (q < layer.h * ws)
    return np.where(ok, 0xFFFFFFFF, 0).astype("<u4").tobytes()


def v2_plan(layer):
    ws, nj, nbin = v2_shape(layer)
    sizes = {
        "in": layer.c // 2 * nbin * 128,
        "coef": ((layer.c // 2 * layer.k * layer.k + 31) // 32) * 128,
        "bias": ((layer.c // 2 + 31) // 32) * 128,
        "mask": nj * 128,
        "out": layer.c // 2 * nj * 128,
    }
    out, cursor = {}, WIN_LO
    for name, size in sizes.items():
        out[name] = (cursor, size)
        cursor += (size + 4095) & ~4095
    if cursor > WIN_HI or v2_vtcm_vectors(layer) > VTCM_SPAN_VECTORS:
        raise ValueError(f"{layer.name} exceeds the window or VTCM span (v2)")
    out["end"] = cursor
    return out


def v2_arena_image(layer, x, wt, b):
    p = v2_plan(layer)
    data = {
        "in": v2_input_bytes(layer, x),
        "coef": coef_bytes(layer, wt),
        "bias": bias_bytes(layer, b),
        "mask": v2_mask_bytes(layer),
        "out": bytes(p["out"][1]),
    }
    for name, raw in data.items():
        if len(raw) != p[name][1]:
            raise AssertionError(f"{name} size differs from v2 plan")
    return p, {p[name][0]: raw for name, raw in data.items()}


def v2_descriptor(layer, p):
    import numpy as np

    ws, nj, nbin = v2_shape(layer)
    words = np.zeros(32, "<u4")
    words[:11] = [
        MAGIC2,
        layer.w,
        layer.h,
        layer.c,
        layer.k,
        nbin,
        p["in"][0],
        p["coef"][0],
        p["bias"][0],
        p["mask"][0],
        p["out"][0],
    ]
    return words.tobytes()


def v2_decode(layer, raw):
    """Compact guard-column output -> ([C, h, w], nonzero count in masked lanes)."""
    import numpy as np

    ws, nj, _ = v2_shape(layer)
    got = unplanar(raw, layer.c, nj)
    q = np.arange(nj * 32)
    ok = ((q % ws) >= layer.r) & (q < layer.h * ws)
    stray = int(np.count_nonzero(got[:, ~ok]))
    out = got[:, : layer.h * ws].reshape(layer.c, layer.h, ws)[:, :, layer.r :]
    return out, stray


def v2_check_output(layer, raw, x, wt, b):
    import numpy as np

    out, stray = v2_decode(layer, raw)
    ref, allow = oracle(layer, x, wt, b)
    err = np.abs(out - ref)
    if stray or not np.isfinite(out).all() or np.any(err > allow):
        raise ValueError(
            f"{layer.name} v2 output differs: stray={stray} max_ratio={float(np.max(err / allow)):.3g}"
        )
    return {
        "max_abs": float(err.max()),
        "max_error_to_bound_ratio": float(np.max(err / allow)),
        "output_abs_max": float(np.abs(out).max()),
    }


def v2_emulate(layer, x, wt, b):
    """Lane model of the v2 kernel's addressing (FP64 arithmetic) from the
    packed input alone; returns the compact output bytes' decoded tensor."""
    import numpy as np

    ws, nj, nbin = v2_shape(layer)
    raw = np.frombuffer(v2_input_bytes(layer, x), "<f2").reshape(layer.c // 2, nbin * 32, 2)
    q = np.arange(nj * 32)
    ok = ((q % ws) >= layer.r) & (q < layer.h * ws)
    out = np.zeros((layer.c, nj * 32))
    for c in range(layer.c):
        acc = np.zeros(nj * 32)
        for dy in range(layer.k):
            for dx in range(layer.k):
                acc += raw[c // 2, q + dy * ws + dx, c % 2].astype(np.float64) * float(
                    wt[c, dy, dx]
                )
        out[c] = np.where(ok, np.clip(acc + float(b[c]), 0, 6), 0)
    return out[:, : layer.h * ws].reshape(layer.c, layer.h, ws)[:, :, layer.r :]
