/* HVX FP16 depthwise probe v2: same layer semantics as tests/hvx_dw_probe.c
 * (stride-1 3x3/5x5 depthwise, bias, clip6, QFloat16 arithmetic, operands in
 * borrowed VTCM), restructured for single-thread HVX issue:
 *
 * - Guard-column layout: every row of a channel pair is ws = w + r words, r
 *   zero guard words then w values, so horizontal taps never cross into a
 *   neighbouring row's data and need no per-tap masks. Padded word index of
 *   (y, x), y = -r .. h-1+r, is r + (y+r)*ws + r + x; halo rows are zero.
 *   Output position q = y*ws + u (u < ws) is stored compactly per pair;
 *   guard lanes (u < r) and q >= h*ws are zeroed by a host mask table
 *   (one vector per block; it only steers values).
 * - Coefficients are splatted once per layer into VTCM vectors.
 * - One unaligned load per tap and block; four independent output blocks
 *   share each coefficient load.
 *
 * Payload header: [MAGIC, w, h, c, k, nbin, off_in, off_coef, off_bias,
 * off_mask, off_out]; channels a multiple of 8; output blocks rounded up to
 * a multiple of 4 (extra blocks are computed and masked to zero).
 * params[1] = compute repetitions (1..16). Protections as in v1. */
#include "landmark_common.h"

#define D2_MAGIC 0x32574448u /* "HDW2" */
#define D2_OPCODE 108u
#define D2_DDR 0xf0000000u
#define D2_WIN_LO 0x1a40000u
#define D2_WIN_HI 0x1f00000u
#define D2_META 1024u
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define D2_FLAGS LV(62)
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))

typedef long d2_uv __attribute__((__vector_size__(128), __aligned__(4)));

typedef struct {
    fnpu_u32 magic, w, h, c, k, nbin, off_in, off_coef, off_bias, off_mask, off_out;
    fnpu_u32 r, ws, cp, kk, nj, src, out, cv, coef, bias, mask, end;
} d2_desc;

static inline __attribute__((always_inline)) fnpu_u32 d2_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static inline __attribute__((always_inline)) fnpu_u32 d2_cycles(void) {
    unsigned long long t;
    __asm__ volatile("%0 = upcycle" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void d2_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from,
                                              fnpu_u32 n) {
    fnpu_u32 i = 0;
#pragma clang loop unroll(disable)
    for (; i + 4 <= n; i += 4) {
        HVX_Vector v0 = from[i], v1 = from[i + 1], v2 = from[i + 2], v3 = from[i + 3];
        to[i] = v0; to[i + 1] = v1; to[i + 2] = v2; to[i + 3] = v3;
    }
#pragma clang loop unroll(disable)
    for (; i < n; i++) to[i] = from[i];
}

static __attribute__((noinline)) void d2_zero(volatile HVX_Vector *to, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = Q6_V_vzero();
}

static __attribute__((noinline)) fnpu_u32 d2_get(const volatile fnpu_u32 *hdr, d2_desc *d) {
    fnpu_u32 *t = (fnpu_u32 *)d;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 11; i++) t[i] = hdr[i];
    d->r = d->k >> 1;
    d->ws = d->w + d->r;
    d->cp = d->c >> 1;
    d->kk = d->k * d->k;
    d->nj = (((d->h * d->ws + 31) >> 5) + 3) & ~3u;
    d->src = 0;
    d->out = d->cp * d->nbin;
    d->cv = d->out + d->cp * d->nj;
    d->coef = d->cv + d->cp * d->kk;
    d->bias = d->coef + (d->cp * d->kk + 31) / 32;
    d->mask = d->bias + (d->cp + 31) / 32;
    d->end = d->mask + d->nj;
    /* last word read: r + (nj*32-1) + (k-1)*ws + (k-1) - r + 31 (vmemu span) */
    fnpu_u32 last = d->nj * 32 + (d->k - 1) * (d->ws + 1) + 31;
    fnpu_u32 bad = (d->magic != D2_MAGIC) | (d->w - 1 > 127) | (d->h - 1 > 127) | (d->c - 8 > 1016) |
                   (d->c & 7) | ((d->k != 3) & (d->k != 5)) | (d->nbin * 32 <= last) | (d->nbin > 4096) |
                   (d->end > (FR_LIMIT - FR_BASE) / 128);
    fnpu_u32 n[5] = {d->cp * d->nbin, d->bias - d->coef, d->mask - d->bias, d->nj, d->cp * d->nj};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 5; i++) {
        fnpu_u32 lo = t[6 + i];
        bad |= (lo & 127) | (lo < D2_WIN_LO) | (lo > D2_WIN_HI) | (n[i] > (D2_WIN_HI - lo) / 128);
    }
    return bad;
}

/* Splat every (pair, tap) coefficient word into its own vector, once. */
static __attribute__((noinline)) void d2_splat(const d2_desc *d) {
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->coef);
    volatile HVX_Vector *cv = FR(d->cv);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cp * d->kk; i++) cv[i] = Q6_V_vsplat_R(coef[i]);
}

static __attribute__((noinline)) void d2_pass(const d2_desc *d) {
    const volatile fnpu_u32 *bias = (const volatile fnpu_u32 *)FR(d->bias);
    const HVX_Vector *mask = (const HVX_Vector *)FR(d->mask);
    HVX_Vector zero = Q6_V_vzero(), lo = Q6_V_vzero(), hi = Q6_V_vsplat_R(0x46004600u);
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), flags = D2_FLAGS[0];
    const fnpu_u32 k = d->k, step = 4 * (d->ws - k), nb = 128 * d->nbin;
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cp; c++) {
        const char *s = (const char *)FR(d->src) + c * nb; /* tap (0,0) of q=0 is word r - r = 0 */
        const HVX_Vector *cv = (const HVX_Vector *)FR(d->cv) + c * d->kk;
        volatile HVX_Vector *o = FR(d->out) + c * d->nj;
        HVX_Vector b = Q6_V_vsplat_R(bias[c]);
#pragma clang loop unroll(disable)
        for (fnpu_u32 j = 0; j < d->nj; j += 4) {
            HVX_Vector a0 = Q6_Vqf16_vadd_VhfVhf(zero, zero), a1 = a0, a2 = a0, a3 = a0;
            const char *p = s + 128 * j;
            const HVX_Vector *w = cv;
#pragma clang loop unroll(disable)
            for (fnpu_u32 dy = 0; dy < k; dy++, p += step) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 dx = 0; dx < k; dx++, p += 4, w++) {
                    HVX_Vector cw = *w;
                    a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(*(const d2_uv *)p, cw));
                    a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(*(const d2_uv *)(p + 128), cw));
                    a2 = Q6_Vqf16_vadd_Vqf16Vqf16(a2, Q6_Vqf16_vmpy_VhfVhf(*(const d2_uv *)(p + 256), cw));
                    a3 = Q6_Vqf16_vadd_Vqf16Vqf16(a3, Q6_Vqf16_vmpy_VhfVhf(*(const d2_uv *)(p + 384), cw));
                }
            }
            HVX_Vector acc[4] = {a0, a1, a2, a3};
#pragma clang loop unroll(full)
            for (fnpu_u32 q = 0; q < 4; q++) {
                HVX_Vector v = Q6_Vhf_equals_Vqf16(acc[q]);
                flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, b));
                flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                o[j + q] = Q6_V_vand_VV(Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi), mask[j + q]);
            }
        }
    }
    D2_FLAGS[0] = flags;
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *hdr = fnpu_input(c);
    fnpu_u32 reps = c->params[1];
    d2_desc ds[1];
    d2_desc *d = ds;
    if (!valid(c) || c->opcode != D2_OPCODE || c->params[0] != 1 || reps - 1 > 15 || c->params[2] ||
        c->input_bytes != 128 || c->output_bytes != D2_META || d2_get(hdr, d))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    d2_zero((volatile HVX_Vector *)meta, D2_META / 128);
    fault_control(c);
    fnpu_u32 t0 = d2_ticks();
    d2_copy(FR(d->src), (const volatile HVX_Vector *)(D2_DDR + d->off_in), d->cp * d->nbin);
    d2_copy(FR(d->coef), (const volatile HVX_Vector *)(D2_DDR + d->off_coef), d->bias - d->coef);
    d2_copy(FR(d->bias), (const volatile HVX_Vector *)(D2_DDR + d->off_bias), d->mask - d->bias);
    d2_copy(FR(d->mask), (const volatile HVX_Vector *)(D2_DDR + d->off_mask), d->nj);
    fnpu_u32 ta = d2_ticks();
    d2_splat(d);
    fnpu_u32 t1 = d2_ticks(), y1 = d2_cycles();
    d2_zero(D2_FLAGS, 1);
    fnpu_u32 done = 0;
#pragma clang loop unroll(disable)
    while (done < reps) { /* stops early past 20 ms; host requires done == reps */
        d2_pass(d);
        done++;
        if (d2_ticks() - t0 > 384000u) break;
    }
    fnpu_u32 y2 = d2_cycles(), t2 = d2_ticks();
    d2_copy((volatile HVX_Vector *)(D2_DDR + d->off_out), FR(d->out), d->cp * d->nj);
    fnpu_u32 t3 = d2_ticks(), pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = D2_MAGIC; meta[1] = pc; meta[2] = done; meta[3] = d->nj;
    meta[4] = ta - t0; meta[5] = t2 - t1; meta[6] = t3 - t2; meta[7] = y2 - y1; meta[8] = t1 - ta;
    ((volatile HVX_Vector *)meta)[2] = D2_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
