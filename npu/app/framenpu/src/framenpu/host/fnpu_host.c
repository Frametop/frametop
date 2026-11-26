/* FrameNPU host kernels: the CPU side of a Mercury request, in C.
 *
 * Loaded with ctypes by framenpu/_native.py. Freestanding: no libc, no Python API,
 * no allocation, no global state. Every function reads and writes only the caller's
 * buffers, whose sizes the caller has checked. Built by tools/build_host_native.py
 * with -ffp-contract=off so that every product and sum rounds exactly as written,
 * on every architecture.
 *
 * Numerics follow framenpu.mercury's NumPy reference:
 *   - FP32 -> FP16 is round-to-nearest-even (NumPy's astype(float16)).
 *   - dense: FP64 products and sums of FP32 values, plus the FP32 bias in FP64, then
 *     one rounding to FP32 (the reference's FP64 matmul; only the summation order may
 *     differ, which can move a result by one FP32 ulp in rare cases).
 *   - conv1x1 on a 1x1 map (the reference's FP32 matmul): FP64 accumulation, rounded
 *     to FP32, plus the FP32 bias in FP32. This is at least as exact as an FP32 BLAS,
 *     whose summation order the reference does not fix either.
 *   - average pools of FP16-valued inputs: the FP64 sum of at most 36 FP16 values
 *     is exact, so the result equals NumPy's mean exactly.
 *   - conv1x1 on a spatial FP16 map (the reference's "hmx" unit): FP64 products and sums
 *     of FP16 values rounded once to FP16, plus the FP16 bias in FP32, rounded to FP16.
 */
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long usize;

#define EXPORT __attribute__((visibility("default")))

/* The compiler may lower loops to these; the library carries its own (no_builtin keeps
 * them from being lowered to calls to themselves). */
__attribute__((visibility("hidden"), no_builtin("memset"))) void *memset(void *d, int c,
                                                                         usize n) {
    unsigned char *p = d;
    unsigned long v = (unsigned char)c * 0x0101010101010101ul;
    while (n && ((usize)p & 7u)) {
        *p++ = (unsigned char)c;
        n--;
    }
    for (; n >= 8; n -= 8, p += 8)
        *(unsigned long *)p = v;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

__attribute__((visibility("hidden"), no_builtin("memcpy"))) void *memcpy(void *d, const void *s,
                                                                         usize n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static u32 bits(float f) {
    union { float f; u32 u; } v = {f};
    return v.u;
}

static float from_bits(u32 u) {
    union { u32 u; float f; } v = {u};
    return v.f;
}

static int finite32(float f) { return (bits(f) & 0x7f800000u) != 0x7f800000u; }
static int finite16(u16 h) { return (h & 0x7c00u) != 0x7c00u; }

/* FP32 -> FP16, round to nearest even, IEEE overflow to infinity. */
static u16 f2h(float f) {
    u32 x = bits(f), sign = (x >> 16) & 0x8000u, ax = x & 0x7fffffffu;
    if (ax > 0x7f800000u)
        return (u16)(sign | 0x7e00u);  /* NaN (callers reject it) */
    if (ax >= 0x477ff000u)
        return (u16)(sign | 0x7c00u);  /* >= 65520 rounds to infinity */
    if (ax >= 0x38800000u) {          /* normal FP16 */
        u32 m = ax + 0xfffu + ((ax >> 13) & 1u);
        return (u16)(sign | ((m - 0x38000000u) >> 13));
    }
    u32 e = ax >> 23;
    if (e < 102u)
        return (u16)sign;              /* below 2^-25 rounds to zero */
    u32 mant = (ax & 0x7fffffu) | 0x800000u, shift = 126u - e;
    u32 q = mant >> shift, rem = mant & ((1u << shift) - 1u), half = 1u << (shift - 1u);
    if (rem > half || (rem == half && (q & 1u)))
        q++;
    return (u16)(sign | q);
}

/* FP64 -> FP16 with a single rounding (NumPy's float64.astype(float16)): round to odd
 * into FP32 first, whose 24 bits leave enough guard bits that the RNE step to FP16 is
 * the correctly rounded result. */
static u16 d2h(double d) {
    if (d != d)
        return 0x7e00u;
    double a = d < 0.0 ? -d : d;
    if (a >= 65520.0)
        return (u16)((d < 0.0 ? 0x8000u : 0u) | 0x7c00u);
    float f = (float)d;
    if ((double)f != d) {
        u32 u = bits(f);
        double af = f < 0.0f ? -(double)f : (double)f;
        if (af > a)
            u -= 1u;                   /* truncate: one step toward zero */
        f = from_bits(u | 1u);         /* sticky bit: odd */
    }
    return f2h(f);
}

/* FP16 -> FP32, exact. */
static float h2f(u16 h) {
    u32 s = (u32)(h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    if (e == 0) {
        float v = (float)m * 5.9604644775390625e-8f;  /* m * 2^-24, exact */
        return from_bits(bits(v) | s);
    }
    if (e == 31)
        return from_bits(s | 0x7f800000u | (m << 13));
    return from_bits(s | ((e + 112u) << 23) | (m << 13));
}

static float relu6(float v) { return v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v); }

/* ---------------------------------------------------------------- exported */

EXPORT i32 fnpu_abi(void) { return 4; }

/* out[i] = base[word[i]] for i = 0 .. n-1, in order: one 32-bit load per entry (volatile:
 * never merged, widened, reordered or skipped). Read-only; used on a PROT_READ mapping of
 * the HMX power registers. The caller validates every word index against the mapping. */
EXPORT void fnpu_read32(const volatile u32 *base, const i32 *word, i32 n, u32 *out) {
    for (i32 i = 0; i < n; i++)
        out[i] = base[word[i]];
}

/* Count of non-finite values in x[0..n). */
EXPORT i32 fnpu_nonfinite32(const float *x, i32 n) {
    i32 bad = 0;
    for (i32 i = 0; i < n; i++)
        bad += !finite32(x[i]);
    return bad;
}

/* y = FP16(x), returning the count of non-finite results (overflow included). */
EXPORT i32 fnpu_to_f16(const float *x, u16 *y, i32 n) {
    i32 bad = 0;
    for (i32 i = 0; i < n; i++) {
        y[i] = f2h(x[i]);
        bad += !finite16(y[i]);
    }
    return bad;
}

/* y = FP32(x) for FP16 x, exact. */
EXPORT void fnpu_from_f16(const u16 *x, float *y, i32 n) {
    for (i32 i = 0; i < n; i++)
        y[i] = h2f(x[i]);
}

/* Stride-2 stem im2col of an FP16 128x128 image (layout.stem_columns): plane
 * 3*dy+dx holds image[2y-1+dy][2x-1+dx], zero outside; plane 9 is zero. */
EXPORT void fnpu_stem_columns(const u16 *img, u16 *cols) {
    for (i32 dy = 0; dy < 3; dy++)
        for (i32 dx = 0; dx < 3; dx++) {
            u16 *plane = cols + (3 * dy + dx) * 4096;
            for (i32 y = 0; y < 64; y++) {
                i32 iy = 2 * y - 1 + dy;
                for (i32 x = 0; x < 64; x++) {
                    i32 ix = 2 * x - 1 + dx;
                    plane[64 * y + x] =
                        (iy >= 0 && iy < 128 && ix >= 0 && ix < 128) ? img[128 * iy + ix] : 0;
                }
            }
        }
    for (i32 i = 0; i < 4096; i++)
        cols[9 * 4096 + i] = 0;
}

/* Grid packing (Grid.to_bytes). t: c planes of hw FP16 values; idx: the word of each
 * position; out: c/2 pairs of nwords u32 words, channel 2p+j in half j of pair p. */
EXPORT void fnpu_grid_pack(const u16 *t, i32 c, i32 hw, const i32 *idx, i32 nwords, u16 *out) {
    for (i32 i = 0; i < c * nwords; i++)
        out[i] = 0;
    for (i32 p = 0; p < c / 2; p++)
        for (i32 j = 0; j < 2; j++) {
            const u16 *src = t + (2 * p + j) * hw;
            u16 *dst = out + 2 * p * nwords + j;
            for (i32 k = 0; k < hw; k++)
                dst[2 * idx[k]] = src[k];
        }
}

/* Grid unpacking (Grid.from_bytes) into c FP32 planes of hw values. stats[0] = nonzero
 * halves outside the interior (stray), stats[1] = non-finite interior values. */
EXPORT void fnpu_grid_unpack(const u16 *raw, i32 c, i32 hw, const i32 *idx, i32 nwords,
                             float *out, i32 *stats) {
    i32 nonzero = 0, inner = 0, bad = 0;
    for (i32 i = 0; i < c * nwords; i++)
        nonzero += (raw[i] & 0x7fffu) != 0;
    for (i32 p = 0; p < c / 2; p++)
        for (i32 j = 0; j < 2; j++) {
            const u16 *src = raw + 2 * p * nwords + j;
            float *dst = out + (2 * p + j) * hw;
            for (i32 k = 0; k < hw; k++) {
                u16 h = src[2 * idx[k]];
                inner += (h & 0x7fffu) != 0;
                bad += !finite16(h);
                dst[k] = h2f(h);
            }
        }
    stats[0] = nonzero - inner;
    stats[1] = bad;
}

/* For each of nb inputs (rows of x, n_in each): acc[b][i] = sum over k = 0 .. n_in-1, in
 * order, of FP64(wt[k][i]) * FP64(x[b][k]); wt is FP32, transposed (n_in x n_out). The product
 * of two FP32 values is exact in FP64, so widening here equals FP64 weights. One pass over the
 * weights serves every input (these layers are bound by weight bandwidth); each output's sum
 * keeps the same order, so a batch equals nb single calls bit for bit. (Holding blocks of
 * outputs in registers measured slower on the Frame.) */
static void dot_columns(const float *wt, const float *x, i32 nb, i32 n_out, i32 n_in,
                        double *acc) {
    for (i32 i = 0; i < nb * n_out; i++)
        acc[i] = 0.0;
    double *a0 = acc, *a1 = acc + n_out, *a2 = acc + 2 * n_out;
    for (i32 k = 0; k < n_in; k++) {
        const float *r = wt + (usize)k * (usize)n_out;
        double x0 = (double)x[k];
        if (nb == 1) {
            for (i32 i = 0; i < n_out; i++)
                a0[i] += (double)r[i] * x0;
        } else if (nb == 2) {
            double x1 = (double)x[n_in + k];
            for (i32 i = 0; i < n_out; i++) {
                double w = (double)r[i];
                a0[i] += w * x0;
                a1[i] += w * x1;
            }
        } else {
            double x1 = (double)x[n_in + k], x2 = (double)x[2 * n_in + k];
            for (i32 i = 0; i < n_out; i++) {
                double w = (double)r[i];
                a0[i] += w * x0;
                a1[i] += w * x1;
                a2[i] += w * x2;
            }
        }
    }
}

/* Dense (mercury._step "dense") of nb inputs: y[b] = FP32(sum FP64(w) * FP64(x[b]) +
 * FP64(bias)), then relu6. wt: the FP32 weight transposed (n_in x n_out); x: nb x n_in;
 * acc: nb x n_out doubles of scratch; y: nb x n_out. 1 <= nb <= 3. */
EXPORT void fnpu_dense(const float *wt, const float *b, const float *x, i32 nb, i32 n_out,
                       i32 n_in, i32 act_relu6, double *acc, float *y) {
    dot_columns(wt, x, nb, n_out, n_in, acc);
    for (i32 j = 0; j < nb * n_out; j++) {
        float v = (float)(acc[j] + (double)b[j % n_out]);
        y[j] = act_relu6 ? relu6(v) : v;
    }
}

/* 1x1 conv of a 1x1 map ("fp32" unit) of nb inputs: y[b] = FP32(sum FP64 w*x[b]) + bias in
 * FP32, then relu6. Same layout and scratch as fnpu_dense. */
EXPORT void fnpu_conv1x1(const float *wt, const float *b, const float *x, i32 nb, i32 n_out,
                         i32 n_in, i32 act_relu6, double *acc, float *y) {
    dot_columns(wt, x, nb, n_out, n_in, acc);
    for (i32 j = 0; j < nb * n_out; j++) {
        float v = (float)acc[j] + b[j % n_out];
        y[j] = act_relu6 ? relu6(v) : v;
    }
}

/* Average pool (mercury._step "avgpool"): non-overlapping kh x kw windows of the top-left
 * oh*kh x ow*kw corner of c planes of h x w; FP64 sum / count, rounded to FP32. */
EXPORT void fnpu_avgpool(const float *x, i32 c, i32 h, i32 w, i32 kh, i32 kw, i32 oh, i32 ow,
                         float *y) {
    double n = (double)(kh * kw);
    for (i32 ch = 0; ch < c; ch++)
        for (i32 oy = 0; oy < oh; oy++)
            for (i32 ox = 0; ox < ow; ox++) {
                double s = 0.0;
                for (i32 dy = 0; dy < kh; dy++)
                    for (i32 dx = 0; dx < kw; dx++)
                        s += (double)x[((usize)ch * h + oy * kh + dy) * w + ox * kw + dx];
                y[((usize)ch * oh + oy) * ow + ox] = (float)(s / n);
            }
}

/* Hint planes (Gemm_58 -> Reshape_63 -> Mul_70) of nb crops:
 * out[b] = FP16(FP32(relu6 dense(kp[b])) * flag[b]). Returns the count of non-finite FP16
 * values. kp: nb x n_in; acc: nb x n_out doubles; tmp, out: nb x n_out. */
EXPORT i32 fnpu_hint(const float *wt, const float *b, const float *kp, const float *flag, i32 nb,
                     i32 n_out, i32 n_in, i32 act_relu6, double *acc, float *tmp, u16 *out) {
    fnpu_dense(wt, b, kp, nb, n_out, n_in, act_relu6, acc, tmp);
    i32 bad = 0;
    for (i32 j = 0; j < nb * n_out; j++) {
        out[j] = f2h(tmp[j] * flag[j / n_out]);
        bad += !finite16(out[j]);
    }
    return bad;
}

/* 1x1 conv of an FP16 map (mercury._conv, unit "hmx"): y[o][p] = FP16(FP32(FP16(sum over
 * k = 0 .. cin-1, in order, of FP64(w[o][k]) * FP64(x[k][p]))) + b[o]), then relu6. w (cout x
 * cin), b and x (cin planes of hw) hold FP16 values as FP32; acc: hw doubles; y: cout x hw. */
EXPORT void fnpu_conv1x1_f16(const float *w, const float *b, const float *x, i32 cout, i32 cin,
                             i32 hw, i32 act_relu6, double *acc, float *y) {
    for (i32 o = 0; o < cout; o++) {
        for (i32 p = 0; p < hw; p++)
            acc[p] = 0.0;
        const float *wr = w + (usize)o * (usize)cin;
        for (i32 k = 0; k < cin; k++) {
            double wk = (double)wr[k];
            const float *xr = x + (usize)k * (usize)hw;
            for (i32 p = 0; p < hw; p++)
                acc[p] += wk * (double)xr[p];
        }
        float *yr = y + (usize)o * (usize)hw;
        for (i32 p = 0; p < hw; p++) {
            float v = h2f(f2h(h2f(d2h(acc[p])) + b[o]));
            yr[p] = act_relu6 ? relu6(v) : v;
        }
    }
}
