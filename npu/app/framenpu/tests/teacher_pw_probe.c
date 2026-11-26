/* Teacher-tail 1x1 layer (HMX): out = [clip6](W x + b) [+ residual], FP16,
 * on the grid layout of tests/teacher_tail_layout.py. One layer per job.
 *
 * Prologue copies the A tiles (ordered [o][t]), bias slots, interior mask,
 * the input tensor and the optional residual from the arena window into
 * borrowed VTCM. Per interior block: pack nk B tiles (16 planar channel-pair
 * vectors each, zero past cin), one barrier, then per output tile clear /
 * nk loads / store / post into a compact VTCM output. Epilogue copies the
 * output blocks into the grid tensor in the arena (halo/guard words stay
 * host-zeroed; masked lanes are written as zero).
 *
 * Payload header: [MAGIC, cin, cout, nk, no, clip, nbin, blk0, nj, off_in,
 * off_w, off_bias, off_mask, off_res (0 = none), off_out]. Every region is
 * checked against the arena window, every VTCM extent against the span.
 * Meta: [0] magic [1] pc [2] blocks [4] prologue [5] compute [6] epilogue
 * ticks [7] compute core cycles; flags vector at words 64..95. */
#include "landmark_common.h"
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

#define PW_MAGIC 0x31575054u
#define PW_OPCODE 109u
#define PW_DDR 0xf0000000u
#define PW_WIN_LO 0x1a40000u
#define PW_WIN_HI 0x1f00000u
#define PW_META 1024u
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define PW_SCALE LV(56)
#define PW_FLAGS LV(62)
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))
#define AL(v) (((v) + 31u) & ~31u)

typedef struct {
    fnpu_u32 magic, cin, cout, nk, no, clip, nbin, blk0, nj, off_in, off_w, off_bias, off_mask, off_res, off_out;
    fnpu_u32 cpi, cpo, bt, bias, mask, out, src, res, end;
} pw_desc;

static inline __attribute__((always_inline)) fnpu_u32 pw_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static inline __attribute__((always_inline)) fnpu_u32 pw_cycles(void) {
    unsigned long long t;
    __asm__ volatile("%0 = upcycle" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void pw_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from,
                                              fnpu_u32 n, fnpu_u32 ts, fnpu_u32 fs, fnpu_u32 rows) {
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

static __attribute__((noinline)) void pw_zero(volatile HVX_Vector *to, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = Q6_V_vzero();
}

static __attribute__((noinline)) void pw_run(const volatile HVX_Vector *a, const volatile HVX_Vector *b, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 t = 0; t < n; t++) fnpu_hmx_load((const fnpu_u16 *)(a + 16 * t), (const fnpu_u16 *)(b + 16 * t));
}

static __attribute__((noinline)) fnpu_u32 pw_get(const volatile fnpu_u32 *hdr, pw_desc *d) {
    fnpu_u32 *t = (fnpu_u32 *)d;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 15; i++) t[i] = hdr[i];
    d->cpi = d->cin >> 1;
    d->cpo = d->cout >> 1;
    d->bt = AL(d->no * d->nk * 16);
    d->bias = AL(d->bt + d->nk * 16);
    d->mask = d->bias + (d->no + 1) / 2;
    d->out = d->mask + d->nj;
    d->src = d->out + d->cpo * d->nj;
    d->res = d->src + d->cpi * d->nbin;
    d->end = d->res + (d->off_res ? d->cpo * d->nbin : 0);
    fnpu_u32 bad = (d->magic != PW_MAGIC) | (d->cin - 2 > 1022) | (d->cout - 2 > 1022) | ((d->cin | d->cout) & 1) |
                   (d->nk != (d->cin + 31) >> 5) | (d->no != (d->cout + 31) >> 5) | (d->clip > 1) |
                   (d->nbin - 1 > 4095) | (d->nj == 0) | (d->blk0 + d->nj > d->nbin) |
                   (d->end > (FR_LIMIT - FR_BASE) / 128);
    fnpu_u32 n[6] = {d->cpi * d->nbin, d->no * d->nk * 16, (d->no + 1) / 2, d->nj, d->off_res ? d->cpo * d->nbin : 0,
                     d->cpo * d->nbin};
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 6; i++) {
        fnpu_u32 lo = t[9 + i];
        if (i == 4 && !lo) continue;
        bad |= (lo & 127) | (lo < PW_WIN_LO) | (lo > PW_WIN_HI) | (n[i] > (PW_WIN_HI - lo) / 128);
    }
    return bad;
}

/* Bias, clip, residual and interior mask for output tile o of block j. */
static __attribute__((noinline)) void pw_post(const pw_desc *d, fnpu_u32 o, fnpu_u32 j) {
    HVX_Vector flags = PW_FLAGS[0], mask = FR(d->mask)[j], exp = Q6_V_vsplat_R(0x7c007c00u);
    HVX_Vector lo = Q6_V_vsplat_R(d->clip ? 0 : 0xfc00fc00u), hi = Q6_V_vsplat_R(d->clip ? 0x46004600u : 0x7c007c00u);
    const volatile fnpu_u32 *bias = (const volatile fnpu_u32 *)FR(d->bias) + 16 * o;
    fnpu_u32 rows = d->cpo - 16 * o;
#pragma clang loop unroll(disable)
    for (fnpu_u32 rr = 0; rr < (rows > 16 ? 16 : rows); rr++) {
        fnpu_u32 pr = 16 * o + rr;
        HVX_Vector v = LV(0)[rr];
        flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, Q6_V_vsplat_R(bias[rr])));
        v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi);
        if (d->off_res) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, FR(d->res)[pr * d->nbin + d->blk0 + j]));
        flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        FR(d->out)[pr * d->nj + j] = Q6_V_vand_VV(v, mask);
    }
    PW_FLAGS[0] = flags;
}

static __attribute__((noinline)) void pw_trap(const fnpu_context *c) { fault_control(c); }

static __attribute__((noinline)) void pw_block(const pw_desc *d, fnpu_u32 j, const fnpu_context *c) {
    volatile HVX_Vector *bt = FR(d->bt);
    const volatile HVX_Vector *src = FR(d->src) + d->blk0 + j;
#pragma clang loop unroll(disable)
    for (fnpu_u32 p = 0; p < d->nk * 16; p++) bt[p] = p < d->cpi ? src[p * d->nbin] : Q6_V_vzero();
    __asm__ volatile("barrier" ::: "memory");
#pragma clang loop unroll(disable)
    for (fnpu_u32 o = 0; o < d->no; o++) {
        fnpu_hmx_clear();
        if (c->params[5]) { pw_run(FR(o * d->nk * 16), bt, 1); pw_trap(c); }
        pw_run(FR(o * d->nk * 16), bt, d->nk);
        fnpu_hmx_store((fnpu_u16 *)LV(0), (const void *)PW_SCALE);
        pw_post(d, o, j);
    }
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *hdr = fnpu_input(c);
    pw_desc ds[1];
    pw_desc *d = ds;
    if (!valid(c) || c->opcode != PW_OPCODE || c->params[0] != 1 || c->params[1] || c->params[2] ||
        c->input_bytes != 128 || c->output_bytes != PW_META || pw_get(hdr, d))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    pw_zero((volatile HVX_Vector *)meta, PW_META / 128);
    pw_zero(PW_FLAGS, 1);
    LV(56)[0] = Q6_V_vsplat_R(0x3c00u);
    pw_zero(LV(57), 1);
    fnpu_u32 t0 = pw_ticks();
    pw_copy(FR(0), (const volatile HVX_Vector *)(PW_DDR + d->off_w), d->no * d->nk * 16, 0, 0, 1);
    pw_copy(FR(d->bias), (const volatile HVX_Vector *)(PW_DDR + d->off_bias), (d->no + 1) / 2, 0, 0, 1);
    pw_copy(FR(d->mask), (const volatile HVX_Vector *)(PW_DDR + d->off_mask), d->nj, 0, 0, 1);
    pw_copy(FR(d->src), (const volatile HVX_Vector *)(PW_DDR + d->off_in), d->cpi * d->nbin, 0, 0, 1);
    if (d->off_res) pw_copy(FR(d->res), (const volatile HVX_Vector *)(PW_DDR + d->off_res), d->cpo * d->nbin, 0, 0, 1);
    fnpu_u32 t1 = pw_ticks(), y1 = pw_cycles(), j = 0;
#pragma clang loop unroll(disable)
    for (; j < d->nj; j++) {
        pw_block(d, j, c);
        if (pw_ticks() - t0 > 384000u) break;
    }
    fnpu_u32 y2 = pw_cycles(), t2 = pw_ticks();
    pw_copy((volatile HVX_Vector *)(PW_DDR + d->off_out) + d->blk0, FR(d->out), d->nj, d->nbin, d->nj, d->cpo);
    fnpu_u32 t3 = pw_ticks(), pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = PW_MAGIC; meta[1] = pc; meta[2] = j;
    meta[4] = t1 - t0; meta[5] = t2 - t1; meta[6] = t3 - t2; meta[7] = y2 - y1;
    ((volatile HVX_Vector *)meta)[2] = PW_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
