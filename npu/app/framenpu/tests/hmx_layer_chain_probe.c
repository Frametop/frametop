/* Layer-chain executor prototype (v3): fused 3x3+1x1 HMX blocks over whole
 * feature maps, layer after layer, with weights, biases, tables and
 * activations in the persistent DDR arena and every operand staged in
 * borrowed VTCM.
 *
 * Geometry is runtime data: the job payload holds a header and one 32-word
 * descriptor per layer. Each layer's output is written straight into the
 * next layer's padded planar input layout, so layers chain on the device. A
 * job runs whole layers params[1] .. params[2]-1; the host splits a chain
 * into jobs at layer boundaries. A job that reaches the 20 ms hard budget
 * stops between blocks and reports the incomplete layer.
 *
 * Activation layout per channel pair: nbin 32-word blocks; position (y, x)
 * of the zero-padded map (y = -1 .. h) is at word pad + (y+1)*w + x, with
 * (pad + w) % 32 == 0 so output block j is one aligned vector. Halo rows,
 * guards and tail lanes are zero (host-zeroed; the kernel writes zeros past
 * h*w). Host tables per layer: gather words per B row pair (source vector
 * offset | byte shift << 16 | mask kind << 24) and x | position << 16 per
 * lane and block, as in v1/v2. Table contents only steer values: gather
 * offsets are masked so reads stay inside mapped VTCM, and every write
 * target is derived from validated descriptor fields.
 *
 * VTCM: the FeatureNN worker-0/1 scratch span 0x31002000-0x311e0000
 * (docs/VTCM_MAP.md) plus the saved 8 KiB; HMX tiles 2 KiB aligned, regions
 * 4 KiB aligned. Protections: worker 0, one HMX owner, arena window bounds
 * on every region, 20 ms hard budget, guarded fault control. */
#include "landmark_common.h"
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

#define LC_MAGIC 0x3158464cu /* "LFX1" */
#define LC_OPCODE 105u
#define LC_MAXL 16u
#define LC_DDR 0xf0000000u   /* uncached alias of the shared buffer */
#define LC_WIN_LO 0x1a40000u /* arena window (buffer offsets) */
#define LC_WIN_HI 0x1f00000u
#define LC_HARD 384000u      /* 20 ms at 19.2 MHz */
#define LC_META 1024u

/* Saved 8 KiB (vector index): out tile 0-15, scale 56-57, x|pos 58, flags 62. */
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define LC_SCALE ((volatile fnpu_u32 *)LV(56))
#define LC_XP LV(58)
#define LC_FLAGS LV(62)
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))
#define AL(v) (((v) + 31u) & ~31u)

/* 16 region/geometry words, then the VTCM plan computed by lc_get. */
typedef struct {
    fnpu_u32 w, h, cin, nm, no, cout, pad, nbin;
    fnpu_u32 off_in, off_w, off_bias, off_tbl, off_xt, off_out, nk1, zero;
    fnpu_u32 s1, s2, w2, bt, mid, bias, tbl, xt, src, nj;
} lc_desc;

static inline __attribute__((always_inline)) fnpu_u32 lc_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void lc_copy(volatile HVX_Vector *to, fnpu_u32 off, fnpu_u32 n) {
    const volatile HVX_Vector *from = (const volatile HVX_Vector *)(LC_DDR + off);
    fnpu_u32 i = 0;
#pragma clang loop unroll(disable)
    for (; i + 8 <= n; i += 8) {
        HVX_Vector v0 = from[i], v1 = from[i + 1], v2 = from[i + 2], v3 = from[i + 3];
        HVX_Vector v4 = from[i + 4], v5 = from[i + 5], v6 = from[i + 6], v7 = from[i + 7];
        to[i] = v0; to[i + 1] = v1; to[i + 2] = v2; to[i + 3] = v3;
        to[i + 4] = v4; to[i + 5] = v5; to[i + 6] = v6; to[i + 7] = v7;
    }
#pragma clang loop unroll(disable)
    for (; i < n; i++) to[i] = from[i];
}

/* One HMX load pair per K tile, accumulated into the open transaction. */
static __attribute__((noinline)) void lc_run(const volatile HVX_Vector *a,
                                             const volatile HVX_Vector *b, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 t = 0; t < n; t++)
        fnpu_hmx_load((const fnpu_u16 *)(a + 16 * t), (const fnpu_u16 *)(b + 16 * t));
}

static inline __attribute__((always_inline)) HVX_Vector lc_inf(HVX_Vector v) {
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u);
    return Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1);
}

/* Bias (QFloat16), finite flags, tail mask; clip6 for mid tiles. */
static __attribute__((noinline)) void lc_post(const lc_desc *d, volatile HVX_Vector *from,
                                              volatile HVX_Vector *to, fnpu_u32 stride,
                                              fnpu_u32 slot, fnpu_u32 rows, fnpu_u32 clip) {
    const volatile fnpu_u32 *bias = (const volatile fnpu_u32 *)FR(d->bias) + 16 * slot;
    HVX_Vector flags = LC_FLAGS[0];
    HVX_Vector tail = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(Q6_V_vsplat_R(d->w * d->h),
                                                      Q6_Vuw_vlsr_VuwR(LC_XP[0], 16)),
                                    Q6_V_vsplat_R(-1), Q6_V_vzero());
    HVX_Vector lo = Q6_V_vsplat_R(clip ? 0 : 0xfc00fc00u), hi = Q6_V_vsplat_R(clip ? 0x46004600u : 0x7c007c00u);
#pragma clang loop unroll(disable)
    for (fnpu_u32 rr = 0; rr < rows; rr++) {
        HVX_Vector v = from[rr];
        flags = Q6_V_vor_VV(flags, lc_inf(v));
        v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, Q6_V_vsplat_R(bias[rr])));
        flags = Q6_V_vor_VV(flags, lc_inf(v));
        v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi);
        to[rr * stride] = Q6_V_vand_VV(v, tail);
    }
    LC_FLAGS[0] = flags;
}

/* Fetch descriptor l, validate everything that bounds memory accesses (arena
 * window for every region, output extent, VTCM end) and plan the layer's
 * VTCM. Returns nonzero if unacceptable. Overlap between regions inside the
 * window only affects this job's own data. */
static __attribute__((noinline)) fnpu_u32 lc_get(const volatile fnpu_u32 *hdr, fnpu_u32 l, lc_desc *d) {
    fnpu_u32 *t = (fnpu_u32 *)d;
    const volatile fnpu_u32 *s = hdr + 32 + 32 * l;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 16; i++) t[i] = s[i];
    d->nj = (d->w * d->h + 31) >> 5;
    d->s1 = d->nk1 * 16;
    d->s2 = d->nm * 16;
    d->w2 = d->nm * d->s1;
    d->bt = AL(d->w2 + d->no * d->s2);
    d->mid = AL(d->bt + d->s1);
    d->bias = AL(d->mid + d->s2);
    d->tbl = d->bias + (d->nm + d->no + 1) / 2;
    d->xt = d->tbl + (d->nk1 + 1) / 2;
    d->src = d->xt + d->nj;
    fnpu_u32 bad = (d->w - 1 > 127) | (d->h - 1 > 127) | (d->cin - 2 > 62) | (d->cout - 2 > 62) |
                   (d->nm - 1 > 10) | (d->no - 1 > 1) | (d->cout > 32 * d->no) | d->zero |
                   (d->nk1 != (9 * d->cin + 31) >> 5) | (d->pad - 1 > 31) | ((d->pad + d->w) & 31) |
                   (d->nbin < d->nj + ((d->pad + 2 * d->w + 1) >> 5) + 2) |
                   (d->src + d->cin / 2 * d->nbin > (FR_LIMIT - FR_BASE) / 128);
    fnpu_u32 n[6] = {d->cin / 2 * d->nbin, d->w2 + d->no * d->s2, d->tbl - d->bias,
                     d->xt - d->tbl, d->nj, d->cout / 2 * d->nbin};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 6; i++) {
        fnpu_u32 lo = t[8 + i];
        bad |= (lo & 127) | (lo < LC_WIN_LO) | (lo > LC_WIN_HI) | (n[i] > (LC_WIN_HI - lo) / 128);
    }
    return bad;
}

static __attribute__((noinline)) void lc_trap(const fnpu_context *c) { fault_control(c); }

static __attribute__((noinline)) void lc_block(const lc_desc *d, fnpu_u32 j, const fnpu_context *c) {
    HVX_Vector zero = Q6_V_vzero(), ones = Q6_V_vsplat_R(-1);
    HVX_Vector xp = FR(d->xt)[j], x = Q6_V_vand_VV(xp, Q6_V_vsplat_R(0xffff));
    LC_XP[0] = xp;
    HVX_Vector left = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, zero), zero, ones);
    HVX_Vector right = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, Q6_V_vsplat_R(d->w - 1)), zero, ones);
    const volatile HVX_Vector *src = FR(d->src + j);
    const volatile fnpu_u32 *tbl = (const volatile fnpu_u32 *)FR(d->tbl);
    volatile HVX_Vector *bt = FR(d->bt);
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < d->s1; r++) { /* pack all K tiles of block j */
        fnpu_u32 word = tbl[r], kind = word >> 24;
        const volatile HVX_Vector *p = src + (word & 0x3fffu);
        HVX_Vector v = Q6_V_valign_VVR(p[1], p[0], word >> 16);
        HVX_Vector mask = kind == 0 ? left : kind == 2 ? right : ones;
        bt[r] = kind == 3 ? zero : Q6_V_vand_VV(v, mask);
    }
    __asm__ volatile("barrier" ::: "memory");
#pragma clang loop unroll(disable)
    for (fnpu_u32 m = 0; m < d->nm; m++) {
        fnpu_hmx_clear();
        if (c->params[5]) { lc_run(FR(m * d->s1), bt, 1); lc_trap(c); }
        lc_run(FR(m * d->s1), bt, d->nk1);
        fnpu_hmx_store((fnpu_u16 *)FR(d->mid + 16 * m), (const void *)LC_SCALE);
        lc_post(d, FR(d->mid + 16 * m), FR(d->mid + 16 * m), 1, m, 16, 1);
    }
    __asm__ volatile("barrier" ::: "memory");
    volatile HVX_Vector *out = (volatile HVX_Vector *)(LC_DDR + d->off_out) + ((d->pad + d->w) >> 5) + j;
#pragma clang loop unroll(disable)
    for (fnpu_u32 o = 0; o < d->no; o++) {
        fnpu_hmx_clear();
        lc_run(FR(d->w2 + o * d->s2), FR(d->mid), d->nm);
        fnpu_hmx_store((fnpu_u16 *)LV(0), (const void *)LC_SCALE);
        fnpu_u32 rows = d->cout / 2 - 16 * o;
        lc_post(d, LV(0), out + 16 * o * d->nbin, d->nbin, d->nm + o, rows > 16 ? 16 : rows, 0);
    }
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *hdr = fnpu_input(c);
    fnpu_u32 nl = hdr[1], l0 = c->params[1], l1 = c->params[2];
    if (!valid(c) || c->opcode != LC_OPCODE || c->params[0] != 1 || hdr[0] != LC_MAGIC ||
        nl - 1 >= LC_MAXL || l0 >= l1 || l1 > nl || c->input_bytes != 128 + nl * 128 ||
        c->output_bytes != LC_META)
        return FNPU_BAD_ARGUMENT;
    lc_desc ds[1];
    lc_desc *d = ds;
    volatile fnpu_u32 *meta = fnpu_output(c);
#pragma clang loop unroll(disable)
    for (fnpu_u32 l = 0; l < nl; l++) /* all descriptors are checked before any work */
        if (lc_get(hdr, l, d)) return FNPU_BAD_ARGUMENT;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < LC_META / 128; i++) ((volatile HVX_Vector *)meta)[i] = Q6_V_vzero();
    LV(56)[0] = Q6_V_vsplat_R(0x3c00u);
    LV(56)[1] = Q6_V_vzero();
    LC_FLAGS[0] = Q6_V_vzero();
    fnpu_u32 start = lc_ticks(), l = l0, j = 0, blocks = 0;
    for (; l < l1; l++) {
        lc_get(hdr, l, d);
        fnpu_u32 t0 = lc_ticks();
        lc_copy(FR(0), d->off_w, d->w2 + d->no * d->s2);
        lc_copy(FR(d->bias), d->off_bias, d->tbl - d->bias);
        lc_copy(FR(d->tbl), d->off_tbl, d->xt - d->tbl);
        lc_copy(FR(d->xt), d->off_xt, d->nj);
        lc_copy(FR(d->src), d->off_in, d->cin / 2 * d->nbin);
        fnpu_u32 t1 = lc_ticks();
        meta[32 + l] = t1 - t0; /* prologue ticks */
        for (j = 0; j < d->nj; j++) {
            lc_block(d, j, c);
            blocks++;
            if (lc_ticks() - start > LC_HARD) goto finish;
        }
        meta[48 + l] = lc_ticks() - t1; /* block ticks */
    }
    j = 0;
finish:;
    fnpu_u32 end = lc_ticks(), pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = LC_MAGIC; meta[1] = pc; meta[2] = nl; meta[3] = l0; meta[4] = l1;
    meta[5] = l; meta[6] = j; meta[7] = blocks; meta[8] = start; meta[9] = end;
    ((volatile HVX_Vector *)meta)[2] = LC_FLAGS[0]; /* words 64..95 */
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
