"""Host side of the FP16 teacher-tail run: the public v20a teacher from the
56x56 stage to the heads (conv_12 .. convdwclip_15, then pooling/heads on the
host), executed layer by layer by tests/teacher_pw_probe.c (HMX 1x1, fused
bias/clip/residual) and tests/teacher_dw_probe.c (HVX depthwise, stride 1/2).
Opens no device.

Grid layout, shared by every tensor at one resolution: per channel pair,
nbin 32-word blocks; word G + (y+R)*ws + R + x holds (y, x), ws = w + R,
R = 2 guard words per row and R halo rows above and below; G is chosen so
the interior start I0 = G + R*ws is block aligned (blk0 = I0/32). Every
non-interior word is zero, so depthwise taps never need edge masks.

Numerical contract (the "device" model, emulated in `emulate_*`): FP16
storage; HMX 1x1 = FP16 x FP16 with wide accumulation, FP16 store, FP16 bias
add, optional clip6, optional FP16 residual add; depthwise = QFloat16
products/sums (modelled as FP32 accumulation in tap order, then FP16),
FP16 bias add, clip6.
"""

from dataclasses import dataclass
import math

from hmx_block_layout import pack, planar, unplanar

R = 2
PW_MAGIC, PW_OPCODE = 0x31575054, 109  # "TPW1"
DW_MAGIC, DW_OPCODE = 0x31574454, 110  # "TDW1"
META_BYTES = 1024
WIN_LO, WIN_HI = 0x1A40000, 0x1F00000
VTCM_SPAN_VECTORS = (0x311E0000 - 0x31002000) // 128


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
        flat = np.zeros((c, self.nbin * 32), np.float16)
        for y in range(self.h):
            s = self.word(y, 0)
            flat[:, s : s + self.w] = tensor[:, y]
        return planar(flat, self.nbin, 0).tobytes()

    def from_bytes(self, raw, channels):
        """-> ([C, h, w] float64, count of nonzero non-interior halves)."""
        import numpy as np

        flat = unplanar(raw, channels, self.nbin)
        ok = self.interior()
        stray = int(np.count_nonzero(flat[:, ~ok]))
        out = np.stack(
            [flat[:, self.word(y, 0) : self.word(y, 0) + self.w] for y in range(self.h)], 1
        )
        return out, stray

    def tensor_bytes(self, channels):
        return channels // 2 * self.nbin * 128


@dataclass(frozen=True)
class Op:
    kind: str  # "pw" or "dw"
    name: str  # graph layer name
    src: str  # input blob
    dst: str  # output blob (after a fused residual add, if any)
    res: str | None = None  # residual blob added after bias (pw only)
    add: str | None = None  # name of the fused BinaryOp


def tail_ops(specs):
    """Executable op list from conv_12 onward, with residual adds fused into
    the preceding projection and Split/Reshape/Squeeze aliases resolved."""
    by_name = {s["name"]: s for s in specs}
    alias = {}
    for s in specs:
        if s["operator"] == "Split":
            for o in s["output"]:
                alias[o] = s["input"][0]
    root = lambda b: root(alias[b]) if b in alias else b  # noqa: E731
    ops, started = [], False
    adds = {root(s["input"][0]): s for s in specs if s["operator"] == "BinaryOp"}
    for s in specs:
        if s["name"] == "conv_12":
            started = True
        if not started or s["operator"] not in ("Convolution", "ConvolutionDepthWise"):
            continue
        src, dst = root(s["input"][0]), s["output"][0]
        if s["operator"] == "ConvolutionDepthWise":
            ops.append(Op("dw", s["name"], src, dst))
            continue
        add = adds.get(dst)
        if add is not None:
            other = [root(b) for b in add["input"] if root(b) != dst]
            ops.append(Op("pw", s["name"], src, add["output"][0], other[0], add["name"]))
        else:
            ops.append(Op("pw", s["name"], src, dst))
    del by_name
    return ops, root


def shapes(specs):
    out = {}
    for s in specs:
        for b in s["output"]:
            out[b] = tuple(s["output_shape"])
    return out


def grid_of(shape):
    return Grid(shape[2], shape[1])


# ---------------------------------------------------------------- weights


def pw_weights(graph, spec):
    """-> (A tile bytes ordered [o][t], bias bytes, nk, no)."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    cout, cin = spec["output_shape"][0], spec["input_shape"][0]
    w = np.asarray(w, np.float16).reshape(cout, cin)
    nk, no = math.ceil(cin / 32), math.ceil(cout / 32)
    m = np.zeros((no * 32, nk * 32), np.float16)
    m[:cout, :cin] = w
    tiles = b"".join(
        pack(m[32 * o : 32 * o + 32, 32 * t : 32 * t + 32]).tobytes()
        for o in range(no)
        for t in range(nk)
    )
    bias = np.zeros(no * 32, np.float16)
    bias[:cout] = np.asarray(b, np.float32).astype(np.float16)
    slots = np.zeros(((no + 1) // 2 * 2, 32), np.float16)
    slots[:no] = bias.reshape(no, 32)
    return tiles, slots.tobytes(), nk, no


def dw_weights(graph, spec):
    """-> (coefficient words [pair][tap], bias words [pair]) as bytes."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    c, (kh, kw) = spec["output_shape"][0], spec["kernel"]
    kk = kh * kw
    h = np.asarray(w, np.float16).reshape(c, kk).view("<u2").astype("<u4")
    coef = np.zeros(((c // 2 * kk + 31) // 32) * 32, "<u4")
    coef[: c // 2 * kk] = (h[0::2] | (h[1::2] << 16)).reshape(-1)
    hb = np.asarray(b, np.float32).astype(np.float16).view("<u2").astype("<u4")
    bias = np.zeros(((c // 2 + 31) // 32) * 32, "<u4")
    bias[: c // 2] = hb[0::2] | (hb[1::2] << 16)
    return coef.tobytes(), bias.tobytes()


# ---------------------------------------------------------------- numerics


def f16(x):
    import numpy as np

    return np.asarray(x, np.float64).astype(np.float16).astype(np.float64)


def emulate_pw(graph, spec, x, res=None):
    """Device-contract model of one 1x1 layer (+ fused residual)."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    cout, cin = spec["output_shape"][0], spec["input_shape"][0]
    _, h, wd = x.shape
    wm = np.asarray(w, np.float16).astype(np.float64).reshape(cout, cin)
    acc = f16(wm @ f16(x).reshape(cin, -1))
    v = f16(acc + f16(np.asarray(b, np.float32))[:, None])
    if spec["activation"] == "clip":
        v = np.clip(v, 0, 6)
    if res is not None:
        v = f16(v + f16(res).reshape(cout, -1))
    return v.reshape(cout, h, wd)


def emulate_dw(graph, spec, x):
    """Device-contract model of one depthwise layer (stride 1 or 2)."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    c, h, wd = x.shape
    kh, kw = spec["kernel"]
    r = kh // 2
    wt = np.asarray(w, np.float16).astype(np.float32).reshape(c, kh, kw)
    pad = np.zeros((c, h + 2 * r, wd + 2 * r), np.float32)
    pad[:, r:-r, r:-r] = f16(x)
    acc = np.zeros((c, h, wd), np.float32)
    for dy in range(kh):
        for dx in range(kw):
            acc = acc + (wt[:, dy, dx, None, None] * pad[:, dy : dy + h, dx : dx + wd]).astype(
                np.float32
            )
    v = np.clip(f16(f16(acc) + f16(np.asarray(b, np.float32))[:, None, None]), 0, 6)
    if spec["stride"] == (2, 2):
        v = v[:, 1::2, 1::2]
    return v


def bound_pw(graph, spec, x, res=None):
    """Per-value allowance for one device 1x1 layer given its actual input."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    cout, cin = spec["output_shape"][0], spec["input_shape"][0]
    wm = np.abs(np.asarray(w, np.float16).astype(np.float64).reshape(cout, cin))
    mag = wm @ np.abs(x).reshape(cin, -1)
    e = 2.0**-10
    allow = 4 * e * mag + 3 * e * (mag + np.abs(np.asarray(b, np.float64))[:, None]) + 2.0**-20
    if res is not None:
        allow = allow + 2 * e * np.abs(res).reshape(cout, -1)
    return allow.reshape(cout, *x.shape[1:])


def bound_dw(graph, spec, x):
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    c, h, wd = x.shape
    kh, kw = spec["kernel"]
    r = kh // 2
    wt = np.abs(np.asarray(w, np.float16).astype(np.float64).reshape(c, kh, kw))
    pad = np.zeros((c, h + 2 * r, wd + 2 * r))
    pad[:, r:-r, r:-r] = np.abs(x)
    mag = np.zeros((c, h, wd))
    for dy in range(kh):
        for dx in range(kw):
            mag += wt[:, dy, dx, None, None] * pad[:, dy : dy + h, dx : dx + wd]
    e = 2.0**-10
    allow = (
        (kh * kw + 4) * e * mag
        + 3 * e * np.abs(np.asarray(b, np.float64))[:, None, None]
        + 2.0**-20
    )
    if spec["stride"] == (2, 2):
        allow = allow[:, 1::2, 1::2]
    return allow


def heads(graph, specs, last):
    """FP32 global average pool + four heads from the last tensor (host)."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    pooled = (
        np.asarray(last, np.float32)
        .reshape(last.shape[0], -1)
        .mean(1, dtype=np.float64)
        .astype(np.float32)
    )
    out = {}
    for s in specs:
        if s["operator"] != "InnerProduct":
            continue
        w, b = weights_and_bias(graph, s)
        v = np.asarray(w, np.float32).reshape(s["output_shape"][0], -1).astype(
            np.float64
        ) @ pooled + np.asarray(b, np.float64)
        if s["activation"] == "sigmoid":
            v = 1 / (1 + np.exp(-v))
        out[s["output"][0]] = v
    return out


def compare(actual, reference):
    import numpy as np

    a = np.asarray(actual, np.float64).ravel()
    r = np.asarray(reference, np.float64).ravel()
    e = a - r
    return {
        "max_abs": float(np.abs(e).max()),
        "rms": float(np.sqrt(np.mean(e * e))),
        "reference_max_abs": float(np.abs(r).max()),
    }


def landmark_error(out0, ref0):
    """Screen landmarks: 21 x (x, y, z); report 2D displacement in crop pixels."""
    import numpy as np

    a = np.asarray(out0, np.float64).reshape(21, 3)
    r = np.asarray(ref0, np.float64).reshape(21, 3)
    d = np.hypot(*(a[:, :2] - r[:, :2]).T)
    return {
        "max_px": float(d.max()),
        "mean_px": float(d.mean()),
        "max_z": float(np.abs(a[:, 2] - r[:, 2]).max()),
    }


# ---------------------------------------------------------------- descriptors / plan


def allocate(ops, shape_of, first_inputs):
    """Activation buffer per blob in the arena window, reusing freed buffers
    of equal size; returns ({blob: (offset, bytes)}, staging offset, end)."""
    last_use = {}
    for i, op in enumerate(ops):
        for b in (op.src, op.res):
            if b:
                last_use[b] = i
    size = lambda b: grid_of(shape_of[b]).tensor_bytes(shape_of[b][0])  # noqa: E731
    cursor, free, where = WIN_LO, [], {}

    def take(n):
        nonlocal cursor
        for i, (off, m) in enumerate(free):
            if m >= n:
                return free.pop(i)
        off, cursor = cursor, cursor + ((n + 4095) & ~4095)
        return off, (n + 4095) & ~4095

    for b in first_inputs:
        where[b] = take(size(b))
    for i, op in enumerate(ops):
        where[op.dst] = take(size(op.dst))
        for b in {op.src, op.res} - {None}:
            if last_use.get(b) == i and b not in first_inputs:
                free.append(where[b])
    stage = cursor
    return where, stage


def pw_descriptor(spec, grid, nk, no, where_in, stage, sizes, where_res, where_out):
    import numpy as np

    words = np.zeros(32, "<u4")
    cin, cout = spec["input_shape"][0], spec["output_shape"][0]
    words[:15] = [
        PW_MAGIC,
        cin,
        cout,
        nk,
        no,
        int(spec["activation"] == "clip"),
        grid.nbin,
        grid.blk0,
        grid.nj,
        where_in,
        stage,
        stage + sizes[0],
        stage + sizes[0] + sizes[1],
        where_res or 0,
        where_out,
    ]
    return words.tobytes()


def dw_descriptor(spec, gin, gout, where_in, stage, sizes, where_out):
    import numpy as np

    words = np.zeros(32, "<u4")
    c = spec["input_shape"][0]
    s2 = spec["stride"] == (2, 2)
    words[:20] = [
        DW_MAGIC,
        c,
        spec["kernel"][0],
        2 if s2 else 1,
        gin.ws,
        gin.nbin,
        gin.blk0,
        gin.nj4,
        gin.nj,
        where_in,
        stage,
        stage + sizes[0],
        stage + sizes[0] + sizes[1],
        where_out,
        gin.h,
        gout.w,
        gout.h,
        gout.ws,
        gout.nbin,
        gout.blk0,
    ]
    return words.tobytes()


# ---------------------------------------------------------------- kernel addressing models


def _halves(raw):
    import numpy as np

    return np.frombuffer(bytes(raw), "<f2").astype(np.float64)


def model_pw(desc, mem):
    """Lane-level model of teacher_pw_probe.c addressing (FP64 arithmetic,
    FP16 rounding at the device's store points). `mem(off, n)` returns arena
    bytes; returns {offset: bytes} the kernel would write."""
    import numpy as np

    d = np.frombuffer(desc, "<u4")[:15].astype(np.int64)
    (
        _,
        cin,
        cout,
        nk,
        no,
        clip,
        nbin,
        blk0,
        nj,
        off_in,
        off_w,
        off_bias,
        off_mask,
        off_res,
        off_out,
    ) = d
    cpi, cpo = cin // 2, cout // 2
    src = _halves(mem(off_in, cpi * nbin * 128)).reshape(cpi, nbin, 32, 2)
    res = _halves(mem(off_res, cpo * nbin * 128)).reshape(cpo, nbin, 32, 2) if off_res else None
    mask = np.frombuffer(mem(off_mask, nj * 128), "<u4").reshape(nj, 32) != 0
    bias = _halves(mem(off_bias, (no + 1) // 2 * 128)).reshape(-1, 32)
    from hmx_block_layout import unpack

    raw_w = mem(off_w, no * nk * 2048)
    A = np.block(
        [
            [unpack(raw_w[2048 * (o * nk + t) : 2048 * (o * nk + t + 1)]) for t in range(nk)]
            for o in range(no)
        ]
    )
    out = np.zeros((cpo, nbin, 32, 2))
    for j in range(nj):
        B = np.zeros((nk * 32, 32))
        blk = src[:, blk0 + j]  # [cpi, 32 lanes, 2]
        B[:cin] = blk.transpose(0, 2, 1).reshape(cin, 32)
        acc = f16(A @ B)[:cout]
        v = f16(acc + f16(bias.reshape(-1)[:cout])[:, None])
        if clip:
            v = np.clip(v, 0, 6)
        if res is not None:
            v = f16(v + res[:, blk0 + j].transpose(0, 2, 1).reshape(cout, 32))
        v = np.where(mask[j][None, :], v, 0)
        out[:, blk0 + j] = v.reshape(cpo, 2, 32).transpose(0, 2, 1)
    whole = bytearray(mem(off_out, cpo * nbin * 128))
    o16 = np.frombuffer(bytes(whole), "<f2").reshape(cpo, nbin, 32, 2).copy()
    o16[:, blk0 : blk0 + nj] = out[:, blk0 : blk0 + nj].astype(np.float16)
    return {int(off_out): o16.tobytes()}


def model_dw(desc, mem):
    """Lane-level model of teacher_dw_probe.c addressing."""
    import numpy as np

    d = np.frombuffer(desc, "<u4")[:20].astype(np.int64)
    (
        _,
        c,
        k,
        stride,
        ws,
        nbin,
        blk0,
        nj4,
        nj,
        off_in,
        off_coef,
        off_bias,
        off_mask,
        off_out,
        h,
        w2,
        h2,
        ws2,
        nbin2,
        blk02,
    ) = d
    cp, r, kk = c // 2, k // 2, k * k
    src = _halves(mem(off_in, cp * nbin * 128)).reshape(cp, nbin * 32, 2)
    coef = (
        _halves(mem(off_coef, ((cp * kk + 31) // 32) * 128))
        .reshape(-1, 2)[: cp * kk]
        .reshape(cp, kk, 2)
    )
    bias = _halves(mem(off_bias, ((cp + 31) // 32) * 128)).reshape(-1, 2)[:cp]
    mask = np.frombuffer(mem(off_mask, nj4 * 128), "<u4") != 0
    o = 32 * blk0 + np.arange(nj4 * 32)
    acc = np.zeros((cp, nj4 * 32, 2), np.float32)
    t = 0
    for dy in range(k):
        for dx in range(k):
            acc = acc + (src[:, o + (dy - r) * ws + (dx - r)] * coef[:, t][:, None, :]).astype(
                np.float32
            )
            t += 1
    v = np.clip(f16(f16(acc) + f16(bias)[:, None, :]), 0, 6) * mask[None, :, None]
    if stride == 1:
        whole = (
            np.frombuffer(bytes(mem(off_out, cp * nbin * 128)), "<f2")
            .reshape(cp, nbin * 32, 2)
            .copy()
        )
        whole[:, 32 * blk0 : 32 * (blk0 + nj)] = v[:, : 32 * nj].astype(np.float16)
        return {int(off_out): whole.tobytes()}
    dec = np.zeros((cp, nbin2 * 32, 2), np.float16)
    for y in range(h2):
        for x in range(w2):
            dec[:, 32 * blk02 + y * ws2 + R + x] = v[:, (2 * y + 1) * ws + R + 2 * x + 1]
    return {int(off_out): dec.tobytes()}
