/* Teacher-tail depthwise layer (HVX, QFloat16): out = clip6(dw(x) + b), FP16,
 * stride 1 or 2, k = 3 or 5, on the grid layout of
 * tests/teacher_tail_layout.py. One layer per job.
 *
 * The grid's zero guard words/halo rows replace edge masks (as in
 * tests/hvx_dw2_probe.c): output word o takes input words
 * o + (dy-r)*ws + (dx-r). Coefficients are splatted once into VTCM vectors;
 * four output blocks share each coefficient load. Results go to a compact
 * VTCM buffer, masked to interior lanes. Stride 1: the interior blocks are
 * copied into the output grid in the arena. Stride 2 (TF-SAME padding of the
 * teacher, (0,1) for k3 and (1,2) for k5): output (y2, x2) is the stride-1
 * value at (2*y2+1, 2*x2+1); those words are gathered into a zeroed VTCM
 * half-resolution grid, which is copied out whole.
 *
 * Payload header: [MAGIC, c, k, stride, ws, nbin, blk0, nj4, nj, off_in,
 * off_coef, off_bias, off_mask, off_out, h, w2, h2, ws2, nbin2, blk02].
 * Meta as tests/teacher_pw_probe.c without the cycle count (word 7 = 0);
 * the coefficient splat is part of the prologue. */
#include "landmark_common.h"

#define TD_MAGIC 0x31574454u
#define TD_OPCODE 110u
#define TD_DDR 0xf0000000u
#define TD_WIN_LO 0x1a40000u
#define TD_WIN_HI 0x1f00000u
#define TD_META 1024u
#define TD_R 2u
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define TD_FLAGS LV(62)
#define TD_ACC(i) LV(24 + (i))
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))

typedef long td_uv __attribute__((__vector_size__(128), __aligned__(4)));

typedef struct {
    fnpu_u32 magic, c, k, stride, ws, nbin, blk0, nj4, nj, off_in, off_coef, off_bias, off_mask, off_out;
    fnpu_u32 h, w2, h2, ws2, nbin2, blk02;
    fnpu_u32 r, cp, kk, out, cv, coef, bias, mask, dec, src, end;
} td_desc;

static inline __attribute__((always_inline)) fnpu_u32 td_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void td_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from, fnpu_u32 n,
                                              fnpu_u32 ts, fnpu_u32 fs, fnpu_u32 rows) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < rows; r++, to += ts, from += fs) {
        fnpu_u32 i = 0;
#pragma clang loop unroll(disable)
        for (; i + 4 <= n; i += 4) {
            HVX_Vector v0 = from[i], v1 = from[i + 1], v2 = from[i + 2], v3 = from[i + 3];
            to[i] = v0; to[i + 1] = v1; to[i + 2] = v2; to[i + 3] = v3;
        }
#pragma clang loop unroll(disable)
        for (; i < n; i++) to[i] = from[i];
    }
}

static __attribute__((noinline)) void td_zero(volatile HVX_Vector *to, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = Q6_V_vzero();
}

static __attribute__((noinline)) fnpu_u32 td_get(const volatile fnpu_u32 *hdr, td_desc *d) {
    fnpu_u32 *t = (fnpu_u32 *)d;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 20; i++) t[i] = hdr[i];
    fnpu_u32 s2 = d->stride == 2;
    d->r = d->k >> 1;
    d->cp = d->c >> 1;
    d->kk = d->k * d->k;
    d->out = 0;
    d->cv = d->cp * d->nj4;
    d->coef = d->cv + d->cp * d->kk;
    d->bias = d->coef + (d->cp * d->kk + 31) / 32;
    d->mask = d->bias + (d->cp + 31) / 32;
    d->dec = d->mask + d->nj4;
    d->src = d->dec + (s2 ? d->cp * d->nbin2 : 0);
    d->end = d->src + d->cp * d->nbin;
    fnpu_u32 w = d->ws - TD_R;
    fnpu_u32 last = 32 * (d->blk0 + d->nj4) + d->r * d->ws + d->r + 31; /* last word any tap load touches */
    fnpu_u32 first = 32 * d->blk0;                                       /* must cover r rows + r words back */
    fnpu_u32 bad = (d->magic != TD_MAGIC) | (d->c - 8 > 1016) | (d->c & 7) | ((d->k != 3) & (d->k != 5)) |
                   ((d->stride - 1) > 1) | ((d->h | d->ws | d->h2 | d->ws2 | d->w2) > 255) |
                   ((d->nbin | d->nj4 | d->blk0 | d->nbin2 | d->blk02) > 4095) |
                   (d->nj4 & 3) | (d->nj4 == 0) | (d->nj > d->nj4) | (d->nj * 32 < d->h * d->ws) |
                   (first < d->r * d->ws + d->r) | (d->nbin * 32 <= last) |
                   (d->end > (FR_LIMIT - FR_BASE) / 128);
    if (s2)
        bad |= (d->w2 * 2 != w) | (d->h2 * 2 != d->h) | (d->ws2 != d->w2 + TD_R) | (d->nbin2 - 1 > 4095) |
               (32 * d->blk02 + (d->h2 - 1) * d->ws2 + TD_R + d->w2 > 32 * d->nbin2);
    fnpu_u32 n[5] = {d->cp * d->nbin, d->bias - d->coef, d->mask - d->bias, d->nj4, d->cp * (s2 ? d->nbin2 : d->nbin)};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 5; i++) {
        fnpu_u32 lo = t[9 + i];
        bad |= (lo & 127) | (lo < TD_WIN_LO) | (lo > TD_WIN_HI) | (n[i] > (TD_WIN_HI - lo) / 128);
    }
    return bad;
}

static __attribute__((noinline)) void td_splat(const td_desc *d) {
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->coef);
    volatile HVX_Vector *cv = FR(d->cv);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cp * d->kk; i++) cv[i] = Q6_V_vsplat_R(coef[i]);
}

static __attribute__((noinline)) void td_pass(const td_desc *d) {
    const volatile fnpu_u32 *bias = (const volatile fnpu_u32 *)FR(d->bias);
    const HVX_Vector *mask = (const HVX_Vector *)FR(d->mask);
    HVX_Vector zero = Q6_V_vzero(), lo = Q6_V_vzero(), hi = Q6_V_vsplat_R(0x46004600u);
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), flags = TD_FLAGS[0];
    const fnpu_u32 k = d->k, step = 4 * (d->ws - k), nb = 128 * d->nbin;
    const fnpu_u32 base = 4 * (32 * d->blk0 - d->r * d->ws - d->r);
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cp; c++) {
        const char *s = (const char *)FR(d->src) + c * nb + base;
        const HVX_Vector *cv = (const HVX_Vector *)FR(d->cv) + c * d->kk;
        volatile HVX_Vector *o = FR(d->out) + c * d->nj4;
        HVX_Vector b = Q6_V_vsplat_R(bias[c]);
#pragma clang loop unroll(disable)
        for (fnpu_u32 j = 0; j < d->nj4; j += 4) {
            HVX_Vector a0 = Q6_Vqf16_vadd_VhfVhf(zero, zero), a1 = a0, a2 = a0, a3 = a0;
            const char *p = s + 128 * j;
            const HVX_Vector *w = cv;
#pragma clang loop unroll(disable)
            for (fnpu_u32 dy = 0; dy < k; dy++, p += step) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 dx = 0; dx < k; dx++, p += 4, w++) {
                    HVX_Vector cw = *w;
                    a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(*(const td_uv *)p, cw));
                    a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(*(const td_uv *)(p + 128), cw));
                    a2 = Q6_Vqf16_vadd_Vqf16Vqf16(a2, Q6_Vqf16_vmpy_VhfVhf(*(const td_uv *)(p + 256), cw));
                    a3 = Q6_Vqf16_vadd_Vqf16Vqf16(a3, Q6_Vqf16_vmpy_VhfVhf(*(const td_uv *)(p + 384), cw));
                }
            }
            TD_ACC(0)[0] = a0; TD_ACC(1)[0] = a1; TD_ACC(2)[0] = a2; TD_ACC(3)[0] = a3;
#pragma clang loop unroll(disable)
            for (fnpu_u32 q = 0; q < 4; q++) {
                HVX_Vector v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(Q6_Vhf_equals_Vqf16(TD_ACC(q)[0]), b));
                flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                o[j + q] = Q6_V_vand_VV(Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi), mask[j + q]);
            }
        }
    }
    TD_FLAGS[0] = flags;
}

/* Stride 2: gather stride-1 words (2*y2+1, 2*x2+1) into the half grid. */
static __attribute__((noinline)) void td_decimate(const td_desc *d) {
    const fnpu_u32 cp = d->cp, h2 = d->h2, w2 = d->w2, ws = d->ws, ws2 = d->ws2;
    const fnpu_u32 sstep = 32 * d->nj4, tstep = 32 * d->nbin2;
    const volatile fnpu_u32 *s0 = (const volatile fnpu_u32 *)FR(d->out) + ws + TD_R + 1;
    volatile fnpu_u32 *t0 = (volatile fnpu_u32 *)FR(d->dec) + 32 * d->blk02 + TD_R;
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < cp; c++, s0 += sstep, t0 += tstep) {
        const volatile fnpu_u32 *s = s0;
        volatile fnpu_u32 *t = t0;
#pragma clang loop unroll(disable)
        for (fnpu_u32 y = 0; y < h2; y++, s += 2 * ws, t += ws2) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 x = 0; x < w2; x++) t[x] = s[2 * x];
        }
    }
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *hdr = fnpu_input(c);
    td_desc ds[1];
    td_desc *d = ds;
    if (!valid(c) || c->opcode != TD_OPCODE || c->params[0] != 1 || c->params[1] || c->params[2] ||
        c->input_bytes != 128 || c->output_bytes != TD_META || td_get(hdr, d))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    td_zero((volatile HVX_Vector *)meta, TD_META / 128);
    fault_control(c);
    fnpu_u32 t0 = td_ticks();
    fnpu_u32 cl[4][3] = {{d->src, d->off_in, d->cp * d->nbin}, {d->coef, d->off_coef, d->bias - d->coef},
                         {d->bias, d->off_bias, d->mask - d->bias}, {d->mask, d->off_mask, d->nj4}};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 4; i++)
        td_copy(FR(cl[i][0]), (const volatile HVX_Vector *)(TD_DDR + cl[i][1]), cl[i][2], 0, 0, 1);
    td_splat(d); /* counted as prologue */
    td_zero(TD_FLAGS, 1);
    fnpu_u32 t1 = td_ticks();
    td_pass(d);
    if (d->stride == 2) {
        td_zero(FR(d->dec), d->cp * d->nbin2);
        td_decimate(d);
    }
    fnpu_u32 t2 = td_ticks(), s2 = d->stride == 2;
    td_copy((volatile HVX_Vector *)(TD_DDR + d->off_out) + (s2 ? 0 : d->blk0), FR(s2 ? d->dec : d->out),
            s2 ? d->nbin2 : d->nj, s2 ? d->nbin2 : d->nbin, s2 ? d->nbin2 : d->nj4, d->cp);
    fnpu_u32 t3 = td_ticks(), pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = TD_MAGIC; meta[1] = pc; meta[2] = d->nj4;
    meta[4] = t1 - t0; meta[5] = t2 - t1; meta[6] = t3 - t2;
    ((volatile HVX_Vector *)meta)[2] = TD_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
