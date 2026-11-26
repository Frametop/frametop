/* Hardware-only fixture: the network's 1x1 gather and post HVX code, run alone.

   rs_gather and rs_pw_post below are verbatim copies of the RS_MHEADS/RS_PW_GROUP
   paths in tests/teacher_resident_probe.c, and run_op is that file's grouped
   rs_pw with the HMX tiles removed (the --pw-ablate 2 variant). They run on a
   full-resolution descriptor (cpi 16, cpo 16, nk 1, nbin 143, nj 132) in VTCM
   from 0x31010000 (inside the borrowed span every Mercury request overwrites),
   from cached code on worker 0. Build with tests/entry_mw.S and
   handoff-tools/build_mw.py; run with tests/hvx_net_run.py (direct dispatch).

   params: [0] repetitions 1..20, [1] phases (1 gather, 2 post, 3 both; bisection:
   4..6 an inline post over the same tiles, grouped (8 tiles, barrier): 4 tile
   load and strided store only, 5 plus the qf16 bias add and conversion, 6 plus
   clip and mask; one dependent chain per vector, no flags),
   [2] clip 0/1, [3] residual 0/1, [4] nonce. Output words: magic, reps,
   phases, ticks lo/hi, cycles lo/hi, packets lo/hi. */
#include "framenpu.h"
#include <hexagon_types.h>
#include <hexagon_protos.h>

#define UNCACHED_CODE 0xf0a92000u
#define CACHED_CODE 0x87a92000u
#define ALIAS_DELTA 0x69000000u
#define STAGING 0x31000000u
#define MAGIC 0x4e455448u /* 'NETH' */
#define RS_PW_GROUP 8

typedef struct {
    fnpu_u32 kind, cpi, cpo, nk, clip, nbin, blk0, nj, ws, dpar, npar, vin, vout, vres, vpar;
    fnpu_u32 bias, scr;
} rs_op;

#define P(x) ((volatile HVX_Vector *)(d->x))
/* the network's zero vector LV(57) and flags LV(62), placed in this fixture's area */
#define ZERO_V ((volatile HVX_Vector *)0x31010000u)
#define RS_FLAGS ((volatile HVX_Vector *)0x31010080u)
#define AREA 0x31010100u /* vector-aligned work area */

#ifndef HVX_NET_GATHER
#define HVX_NET_GATHER 0 /* 1: the --pw-pipe gather, 2: batched loads before stores */
#endif
#ifndef HVX_NET_POST
#define HVX_NET_POST 0 /* 1/2: the --pw-pipe post, clip / residual case only (4 KiB) */
#endif

#if HVX_NET_POST
/* ---- from ~/frame-dsp-research/unshipped-patches/pw_pipe.patch (RS_PW_PIPE) ---- */
static inline __attribute__((always_inline)) HVX_Vector rs_pw_post_one(
    const HVX_Vector *t, const HVX_Vector *bias, const HVX_Vector *res, HVX_Vector *out,
    fnpu_u32 rr, fnpu_u32 nb, HVX_Vector mask, HVX_Vector m, const int clip, const int resid) {
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u);
    HVX_Vector v = t[rr];
    m = Q6_Vuh_vmax_VuhVuh(m, Q6_V_vand_VV(v, exp));
    v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias[rr]));
    __asm__("" : "+v"(v));
    if (clip) v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), Q6_V_vsplat_R(0x46004600u));
    if (resid) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, res[rr * nb]));
    __asm__("" : "+v"(v));
    m = Q6_Vuh_vmax_VuhVuh(m, Q6_V_vand_VV(v, exp));
    out[rr * nb] = Q6_V_vand_VV(v, mask);
    return m;
}
static inline __attribute__((always_inline)) HVX_Vector rs_pw_post_run(
    const HVX_Vector *t, const HVX_Vector *bias, const HVX_Vector *res, HVX_Vector *out,
    fnpu_u32 n, fnpu_u32 nb, HVX_Vector mask, const int clip, const int resid) {
    HVX_Vector m0 = Q6_V_vzero(), m1 = m0, m2 = m0, m3 = m0;
    fnpu_u32 rr = 0;
#pragma clang loop unroll(disable)
    for (; rr + 4 <= n; rr += 4) {
        m0 = rs_pw_post_one(t, bias, res, out, rr, nb, mask, m0, clip, resid);
        m1 = rs_pw_post_one(t, bias, res, out, rr + 1, nb, mask, m1, clip, resid);
        m2 = rs_pw_post_one(t, bias, res, out, rr + 2, nb, mask, m2, clip, resid);
        m3 = rs_pw_post_one(t, bias, res, out, rr + 3, nb, mask, m3, clip, resid);
    }
#pragma clang loop unroll(disable)
    for (; rr < n; rr++) m0 = rs_pw_post_one(t, bias, res, out, rr, nb, mask, m0, clip, resid);
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u),
                     m = Q6_Vuh_vmax_VuhVuh(Q6_Vuh_vmax_VuhVuh(m0, m1), Q6_Vuh_vmax_VuhVuh(m2, m3));
    return Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(m, exp), -1);
}
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile) {
    const fnpu_u32 nb = d->nbin, at = 16 * o * nb + d->blk0 + j;
    fnpu_u32 n = d->cpo - 16 * o;
    if (n > 16) n = 16;
    const HVX_Vector *t = (const HVX_Vector *)tile, *bias = (const HVX_Vector *)P(bias) + 16 * o;
    const HVX_Vector *res = d->vres ? (const HVX_Vector *)P(vres) + at : 0;
    HVX_Vector *out = (HVX_Vector *)P(vout) + at;
    const HVX_Vector mask = ((const HVX_Vector *)P(bias))[d->cpo + j];
    /* fixture: one specialization per build to fit 4 KiB (frame_job checks the case) */
    HVX_Vector fl = rs_pw_post_run(t, bias, res, out, n, nb, mask, HVX_NET_POST == 1, HVX_NET_POST == 2);
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], fl);
}
#else
/* ---- verbatim from tests/teacher_resident_probe.c (RS_PW_GROUP, RS_MHEADS kind 1) ---- */
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile) {
#define RS_TILE(rr) tile[rr]
    fnpu_u32 n = d->cpo - 16 * o, nb = d->nbin, at = 16 * o * nb + d->blk0 + j;
    if (n > 16) n = 16;
    const volatile HVX_Vector *bias = P(bias) + 16 * o, *res = d->vres ? P(vres) + at : 0;
    volatile HVX_Vector *out = P(vout) + at;
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), mask = P(bias)[d->cpo + j], fl = Q6_V_vzero();
    fnpu_u32 clip = d->clip;
#pragma clang loop unroll(disable)
    for (fnpu_u32 rr = 0; rr < n; rr++) {
        HVX_Vector v = RS_TILE(rr);
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias[rr]));
        if (clip) v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), Q6_V_vsplat_R(0x46004600u));
        if (res) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, res[rr * nb])); /* check: no qf16+hf fusion */
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        out[rr * nb] = Q6_V_vand_VV(v, mask);
    }
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], fl);
}
#undef RS_TILE
#endif

typedef long rs_uv __attribute__((__vector_size__(128), __aligned__(4)));
#define P3_KIND 8u
static __attribute__((noinline)) void rs_gather(const rs_op *d, volatile HVX_Vector *b, const volatile HVX_Vector *src) {
    fnpu_u32 p = 0;
    if (d->kind == P3_KIND) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 t = 0; t < 9; t++) {
            const int off = ((int)(t / 3) - 1) * (int)d->ws + (int)(t % 3) - 1;
            const char *s = (const char *)src + 4 * off;
#pragma clang loop unroll(disable)
            for (fnpu_u32 q = 0; q < d->cpi; q++, p++) b[p] = *(const rs_uv *)(s + 128 * d->nbin * q);
        }
    } else
#if HVX_NET_GATHER == 1
    {   /* the --pw-pipe copies through plain pointers: fields hoisted, loads overlapped */
        const fnpu_u32 cpi = d->cpi, nb = d->nbin, tot = d->nk * 16;
        const HVX_Vector *s = (const HVX_Vector *)src;
        HVX_Vector *bb = (HVX_Vector *)b;
#pragma clang loop unroll_count(4)
        for (; p < cpi; p++) bb[p] = s[p * nb];
        if (p < tot) {
            const HVX_Vector z = ZERO_V[0]; /* zero */
#pragma clang loop unroll(disable)
            for (; p < tot; p++) bb[p] = z;
        }
        return;
    }
#endif
#if HVX_NET_GATHER == 2
    {   /* four loads into separate registers, then four stores: the empty asm keeps
         * the compiler from pairing each load with its store in one packet
         * (v.cur = vmem; vmem = v), which stalls every copy for a full load latency */
        const fnpu_u32 cpi = d->cpi, nb = d->nbin, tot = d->nk * 16;
        const HVX_Vector *s = (const HVX_Vector *)src;
        HVX_Vector *bb = (HVX_Vector *)b;
#pragma clang loop unroll(disable)
        for (; p + 4 <= cpi; p += 4) {
            HVX_Vector a0 = s[0], a1 = s[nb], a2 = s[2 * nb], a3 = s[3 * nb];
            __asm__("" : "+v"(a0), "+v"(a1), "+v"(a2), "+v"(a3));
            bb[p] = a0; bb[p + 1] = a1; bb[p + 2] = a2; bb[p + 3] = a3;
            s += 4 * nb;
        }
#pragma clang loop unroll(disable)
        for (; p < cpi; p++, s += nb) bb[p] = *s;
        if (p < tot) {
            const HVX_Vector z = ZERO_V[0]; /* zero */
#pragma clang loop unroll(disable)
            for (; p < tot; p++) bb[p] = z;
        }
        return;
    }
#endif
#pragma clang loop unroll(disable)
    for (; p < d->cpi; p++) b[p] = src[p * d->nbin];
#pragma clang loop unroll(disable)
    for (; p < d->nk * 16; p++) b[p] = ZERO_V[0]; /* zero */
}
/* ---- end verbatim ---- */

/* rs_pw (RS_PW_GROUP) with the HMX tiles removed; phases selects gather/post. */
static __attribute__((noinline)) void run_op(const rs_op *d, fnpu_u32 phases) {
    const fnpu_u32 nk = d->nk, no = (d->cpo + 15) >> 4, nj = d->nj;
    volatile HVX_Vector *bt = P(scr), *ot = bt + RS_PW_GROUP * nk * 16;
#pragma clang loop unroll(disable)
    for (fnpu_u32 j0 = 0; j0 < nj; j0 += RS_PW_GROUP) {
        const fnpu_u32 g = nj - j0 < RS_PW_GROUP ? nj - j0 : RS_PW_GROUP;
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g && (phases & 1); jj++) {
            const volatile HVX_Vector *src = P(vin) + d->blk0 + j0 + jj;
            volatile HVX_Vector *b = bt + jj * nk * 16;
            rs_gather(d, b, src);
        }
        __asm__ volatile("barrier" ::: "memory");
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g && (phases & 2); jj++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 o = 0; o < no; o++) rs_pw_post(d, o, j0 + jj, ot + (jj * no + o) * 16);
        }
    }
}

static __attribute__((noinline)) void fill(volatile HVX_Vector *v, fnpu_u32 n, fnpu_u32 word) {
    const HVX_Vector x = Q6_V_vsplat_R(word);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) v[i] = x;
}

static __attribute__((noinline)) fnpu_u32 body(const fnpu_context *ctx) {
    const fnpu_u32 reps = ctx->params[0], phases = ctx->params[1];
    rs_op ds[1];
    rs_op *d = ds;
    /* the full-resolution 1x1 op's fields; grids and buffers laid out in AREA */
    d->kind = 1; d->cpi = 16; d->cpo = 16; d->nk = 1; d->clip = ctx->params[2]; d->nbin = 143;
    d->blk0 = 5; d->nj = 132; d->ws = 66; d->dpar = 0; d->npar = 176; d->vpar = 0;
    const fnpu_u32 grid = 16 * 143 + 8; /* vectors per grid incl. blk0 + nj slack */
    d->vin = AREA;
    d->vout = d->vin + 128 * grid;
    d->vres = ctx->params[3] ? d->vout + 128 * grid : 0;
    d->bias = d->vout + 128 * (2 * grid);
    d->scr = d->bias + 128 * 160;            /* 16 bias + 132 masks (+ slack) */
    fill(P(vin), grid, 0x3c003c00u);         /* FP16 1.0 */
    fill((volatile HVX_Vector *)(d->vout + 128 * grid), grid, 0x3c003c00u); /* residual grid */
    fill(P(bias), 16, 0x3c003c00u);
    fill(P(bias) + 16, 144, 0xffffffffu);    /* masks */
    fill(ZERO_V, 2, 0);
    __asm__ volatile("barrier" ::: "memory");
    unsigned long long t0, t1, c0, c1, k0, k1;
    __asm__ volatile("%0 = c31:30" : "=r"(t0)::"memory");
    __asm__ volatile("%0 = c15:14" : "=r"(c0)::"memory");
    __asm__ volatile("%0 = c19:18" : "=r"(k0)::"memory");
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < reps; r++) {
        if (phases <= 3) {
            run_op(d, phases);
        } else {
            const fnpu_u32 nb = d->nbin, level = phases - 3;
            const HVX_Vector *bias = (const HVX_Vector *)P(bias), *tile = (const HVX_Vector *)P(scr);
            const HVX_Vector top = Q6_V_vsplat_R(0x46004600u);
#pragma clang loop unroll(disable)
            for (fnpu_u32 j = 0; j < d->nj; j++) {
                if (j && (j & 7) == 0) __asm__ volatile("barrier" ::: "memory");
                const HVX_Vector *t = tile + 16 * (j & 7), mask = bias[d->cpo + j];
                HVX_Vector *out = (HVX_Vector *)P(vout) + d->blk0 + j;
#pragma clang loop unroll(disable)
                for (fnpu_u32 rr = 0; rr < 16; rr++) {
                    HVX_Vector v = t[rr];
                    if (level >= 2) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias[rr]));
                    if (level >= 3) v = Q6_V_vand_VV(Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), top), mask);
                    out[rr * nb] = v;
                }
            }
        }
        __asm__ volatile("barrier" ::: "memory");
    }
    __asm__ volatile("barrier" ::: "memory");
    __asm__ volatile("%0 = c31:30" : "=r"(t1)::"memory");
    __asm__ volatile("%0 = c15:14" : "=r"(c1)::"memory");
    __asm__ volatile("%0 = c19:18" : "=r"(k1)::"memory");
    volatile fnpu_u32 *out = fnpu_output(ctx);
    t1 -= t0; c1 -= c0; k1 -= k0;
    out[0] = MAGIC; out[1] = reps; out[2] = phases;
    out[3] = (fnpu_u32)t1; out[4] = (fnpu_u32)(t1 >> 32);
    out[5] = (fnpu_u32)c1; out[6] = (fnpu_u32)(c1 >> 32);
    out[7] = (fnpu_u32)k1; out[8] = (fnpu_u32)(k1 >> 32);
    out[9] = ((volatile fnpu_u32 *)P(vout))[5 * 32];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}

static __attribute__((noinline)) void publish_cached(void) {
    volatile HVX_Vector *stage = (volatile HVX_Vector *)STAGING;
    const volatile HVX_Vector *code = (const volatile HVX_Vector *)UNCACHED_CODE;
    for (fnpu_u32 i = 0; i < 32; i++) stage[i] = code[i];
    __asm__ volatile("barrier" ::: "memory");
    for (fnpu_u32 i = 0; i < 32; i++) {
        ((volatile HVX_Vector *)CACHED_CODE)[i] = stage[i];
        __asm__ volatile("barrier" ::: "memory");
        for (fnpu_u32 part = 0; part < 128; part += 32) {
            fnpu_u32 address = CACHED_CODE + i * 128 + part;
            __asm__ volatile("dccleaninva(%0)\n\tsyncht" :: "r"(address) : "memory");
        }
    }
    for (fnpu_u32 offset = 0; offset < 4096; offset += 32) {
        fnpu_u32 address = CACHED_CODE + offset;
        __asm__ volatile("icinva(%0)\n\tisync" :: "r"(address) : "memory");
    }
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 10)
        return FNPU_BAD_ARGUMENT;
    if (!ctx->params[0] || ctx->params[0] > 20u || !ctx->params[1] || ctx->params[1] > 6u ||
        ctx->params[2] > 1u || ctx->params[3] > 1u || !ctx->params[4] ||
        !(ctx->flags & FNPU_FLAG_DIRECT) || ctx->input_bytes || ctx->output_bytes < 64u ||
        (HVX_NET_POST && (ctx->params[2] != (HVX_NET_POST == 1) || ctx->params[3] != (HVX_NET_POST == 2))))
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 w;
    __asm__ volatile("%0 = htid" : "=r"(w));
    if (w) return FNPU_OK; /* worker 0 only */
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    if (pc < UNCACHED_CODE || pc >= UNCACHED_CODE + 4096) return FNPU_BAD_ARGUMENT;
    publish_cached();
    fnpu_u32 target = (fnpu_u32)body;
    __asm__ volatile("%0 = sub(%0,%1)" : "+r"(target) : "r"(ALIAS_DELTA) : "memory");
    fnpu_u32 status = ((fnpu_u32 (*)(const fnpu_context *))target)(ctx);
    for (fnpu_u32 offset = 0; offset < 4096; offset += 32) {
        fnpu_u32 address = CACHED_CODE + offset;
        __asm__ volatile("icinva(%0)\n\tisync" :: "r"(address) : "memory");
    }
    return status;
}
