/* HVX FP16 depthwise probe: one stride-1 3x3/5x5 depthwise layer of the
 * public teacher (bias + clip6) computed with QFloat16 multiply/accumulate
 * from operands resident in borrowed VTCM. Measures the HVX depthwise rate
 * under the same relaxed FP16 contract the HMX path uses (no FP32 conversion,
 * no exact-IEEE zero identities), to price the unchanged teacher with HMX
 * pointwise layers and HVX depthwise layers.
 *
 * Layout per channel pair (one 32-bit word = channels 2c, 2c+1 at one
 * position): nbin 32-word blocks; position (y, x) of the zero-padded map
 * (y = -r .. h-1+r, r = k/2) is word pad + (y+r)*w + x with
 * (pad + r*w) % 32 == 0, so output block j is one aligned vector. Halo rows
 * and guards are zero; horizontal edges are masked per lane from the host
 * x|position table, which only steers values.
 *
 * Payload: header vector [MAGIC, w, h, c, k, pad, nbin, off_in, off_coef,
 * off_bias, off_xt, off_out]. Coefficients: one word per channel pair and tap
 * (row-major taps); bias: one word per pair. All regions lie in the arena
 * window and are copied to VTCM; the output is written back after timing.
 * Channels are a multiple of 8 (four pairs per inner step).
 * params[1] = compute repetitions (1..16). meta[7] = core cycles (upcycle)
 * across the compute phase, to relate the 19.2 MHz timer to the core clock. Protections: worker 0, descriptor
 * and window checks before any work, 20 ms hard budget, guarded fault. */
#include "landmark_common.h"

#define DW_MAGIC 0x31574448u /* "HDW1" */
#define DW_OPCODE 106u
#define DW_DDR 0xf0000000u
#define DW_WIN_LO 0x1a40000u
#define DW_WIN_HI 0x1f00000u
#define DW_META 1024u
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define DW_MASK(i) LV(16 + (i)) /* 16..20: per-dx edge masks */
#define DW_ACC(i) LV(24 + (i)) /* 24..27: accumulator hand-off */
#define DW_FLAGS LV(62)
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))

typedef struct {
    fnpu_u32 magic, w, h, c, k, pad, nbin, off_in, off_coef, off_bias, off_xt, off_out;
    fnpu_u32 r, cp, kk, nj, src, out, coef, bias, xt, end;
} dw_desc;

static inline __attribute__((always_inline)) fnpu_u32 dw_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static inline __attribute__((always_inline)) fnpu_u32 dw_cycles(void) {
    unsigned long long t;
    __asm__ volatile("%0 = upcycle" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void dw_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from,
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

static __attribute__((noinline)) void dw_zero(volatile HVX_Vector *to, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = Q6_V_vzero();
}

static __attribute__((noinline)) fnpu_u32 dw_get(const volatile fnpu_u32 *hdr, dw_desc *d) {
    fnpu_u32 *t = (fnpu_u32 *)d;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 12; i++) t[i] = hdr[i];
    d->r = d->k >> 1;
    d->cp = d->c >> 1;
    d->kk = d->k * d->k;
    d->nj = (d->w * d->h + 31) >> 5;
    d->src = 0;
    d->out = d->cp * d->nbin;
    d->coef = d->out + d->cp * d->nj;
    d->bias = d->coef + (d->cp * d->kk + 31) / 32;
    d->xt = d->bias + (d->cp + 31) / 32;
    d->end = d->xt + d->nj;
    fnpu_u32 amax = d->pad + 2 * d->r * d->w + 2 * d->r;
    fnpu_u32 bad = (d->magic != DW_MAGIC) | (d->w - 1 > 127) | (d->h - 1 > 127) | (d->c - 2 > 1022) |
                   (d->c & 7) | ((d->k != 3) & (d->k != 5)) | (d->pad < d->r) | (d->pad > 63) |
                   ((d->pad + d->r * d->w) & 31) | (d->nbin < (amax >> 5) + d->nj + 1) |
                   (d->nbin > 4096) | (d->end > (FR_LIMIT - FR_BASE) / 128);
    fnpu_u32 n[5] = {d->cp * d->nbin, d->bias - d->coef, d->xt - d->bias, d->nj, d->cp * d->nj};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 5; i++) {
        fnpu_u32 lo = t[7 + i];
        bad |= (lo & 127) | (lo < DW_WIN_LO) | (lo > DW_WIN_HI) | (n[i] > (DW_WIN_HI - lo) / 128);
    }
    return bad;
}

/* One full pass: every block, every channel pair, every tap. */
static __attribute__((noinline)) void dw_pass(const dw_desc *d) {
    HVX_Vector flags = DW_FLAGS[0];
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->coef);
    const volatile fnpu_u32 *bias = (const volatile fnpu_u32 *)FR(d->bias);
    HVX_Vector zero = Q6_V_vzero(), ones = Q6_V_vsplat_R(-1), wv = Q6_V_vsplat_R(d->w);
    HVX_Vector lo = Q6_V_vzero(), hi = Q6_V_vsplat_R(0x46004600u), exp = Q6_V_vsplat_R(0x7c007c00u);
    fnpu_u32 base = d->pad - d->r; /* padded word of tap (-r,-r) for output position 0 */
#pragma clang loop unroll(disable)
    for (fnpu_u32 j = 0; j < d->nj; j++) {
        HVX_Vector xp = FR(d->xt)[j];
        HVX_Vector x = Q6_V_vand_VV(xp, Q6_V_vsplat_R(0xffff));
        HVX_Vector tail = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(Q6_V_vsplat_R(d->w * d->h), Q6_Vuw_vlsr_VuwR(xp, 16)),
                                        ones, zero);
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < d->k; i++) {
            HVX_Vector xd = Q6_Vw_vadd_VwVw(x, Q6_V_vsplat_R(i - d->r));
            DW_MASK(i)[0] = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VuwVuw(wv, xd), ones, zero);
        }
        const HVX_Vector *srcj = (const HVX_Vector *)FR(d->src + j);
        volatile HVX_Vector *outj = FR(d->out + j);
        const fnpu_u32 nb = d->nbin, kk = d->kk;
#pragma clang loop unroll(disable)
        for (fnpu_u32 c = 0; c < d->cp; c += 4) { /* four independent channel pairs */
            const HVX_Vector *s0 = srcj + c * nb;
            const fnpu_u32 *cf = (const fnpu_u32 *)coef + c * kk;
            HVX_Vector a0 = Q6_Vqf16_vadd_VhfVhf(zero, zero), a1 = a0, a2 = a0, a3 = a0;
            fnpu_u32 a = base, t = 0;
#pragma clang loop unroll(disable)
            for (fnpu_u32 dy = 0; dy < d->k; dy++, a += d->w - d->k) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 dx = 0; dx < d->k; dx++, a++, t++) {
                    const HVX_Vector *p = s0 + (a >> 5);
                    fnpu_u32 sh = (a & 31) * 4;
                    HVX_Vector m = DW_MASK(dx)[0];
                    HVX_Vector v0 = Q6_V_vand_VV(Q6_V_valign_VVR(p[1], p[0], sh), m);
                    HVX_Vector v1 = Q6_V_vand_VV(Q6_V_valign_VVR(p[nb + 1], p[nb], sh), m);
                    HVX_Vector v2 = Q6_V_vand_VV(Q6_V_valign_VVR(p[2 * nb + 1], p[2 * nb], sh), m);
                    HVX_Vector v3 = Q6_V_vand_VV(Q6_V_valign_VVR(p[3 * nb + 1], p[3 * nb], sh), m);
                    a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(v0, Q6_V_vsplat_R(cf[t])));
                    a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(v1, Q6_V_vsplat_R(cf[kk + t])));
                    a2 = Q6_Vqf16_vadd_Vqf16Vqf16(a2, Q6_Vqf16_vmpy_VhfVhf(v2, Q6_V_vsplat_R(cf[2 * kk + t])));
                    a3 = Q6_Vqf16_vadd_Vqf16Vqf16(a3, Q6_Vqf16_vmpy_VhfVhf(v3, Q6_V_vsplat_R(cf[3 * kk + t])));
                }
            }
            DW_ACC(0)[0] = a0; DW_ACC(1)[0] = a1; DW_ACC(2)[0] = a2; DW_ACC(3)[0] = a3;
#pragma clang loop unroll(disable)
            for (fnpu_u32 q = 0; q < 4; q++) {
                HVX_Vector v = Q6_Vhf_equals_Vqf16(DW_ACC(q)[0]);
                flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, Q6_V_vsplat_R(bias[c + q])));
                flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                outj[(c + q) * d->nj] = Q6_V_vand_VV(Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi), tail);
            }
        }
    }
    DW_FLAGS[0] = flags;
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *hdr = fnpu_input(c);
    fnpu_u32 reps = c->params[1];
    dw_desc ds[1];
    dw_desc *d = ds;
    if (!valid(c) || c->opcode != DW_OPCODE || c->params[0] != 1 || reps - 1 > 15 || c->params[2] ||
        c->input_bytes != 128 || c->output_bytes != DW_META || dw_get(hdr, d))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    dw_zero((volatile HVX_Vector *)meta, DW_META / 128);
    fault_control(c);
    fnpu_u32 t0 = dw_ticks();
    dw_copy(FR(d->src), (const volatile HVX_Vector *)(DW_DDR + d->off_in), d->cp * d->nbin);
    dw_copy(FR(d->coef), (const volatile HVX_Vector *)(DW_DDR + d->off_coef), d->bias - d->coef);
    dw_copy(FR(d->bias), (const volatile HVX_Vector *)(DW_DDR + d->off_bias), d->xt - d->bias);
    dw_copy(FR(d->xt), (const volatile HVX_Vector *)(DW_DDR + d->off_xt), d->nj);
    fnpu_u32 t1 = dw_ticks(), y1 = dw_cycles();
    dw_zero(DW_FLAGS, 1);
    fnpu_u32 done = 0;
#pragma clang loop unroll(disable)
    while (done < reps) { /* stops early past 20 ms; host requires done == reps */
        dw_pass(d);
        done++;
        if (dw_ticks() - t0 > 384000u) break;
    }
    fnpu_u32 y2 = dw_cycles(), t2 = dw_ticks();
    dw_copy((volatile HVX_Vector *)(DW_DDR + d->off_out), FR(d->out), d->cp * d->nj);
    fnpu_u32 t3 = dw_ticks(), pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = DW_MAGIC; meta[1] = pc; meta[2] = done; meta[3] = d->nj;
    meta[4] = t1 - t0; meta[5] = t2 - t1; meta[6] = t3 - t2; meta[7] = y2 - y1;
    ((volatile HVX_Vector *)meta)[2] = DW_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
