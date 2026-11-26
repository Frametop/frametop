/* Hardware-only fixture: does HVX throughput scale across workers 0/1/2?

   Built with tests/entry_mw.S (frame_job on workers 0, 1 and 2 in direct
   dispatch) and handoff-tools/build_mw.py. Each active worker runs the same HVX
   loop on its own 64 KiB VTCM slice at 0x31010000 + w * 0x10000, inside the
   borrowed FeatureNN scratch that every Mercury request already overwrites
   (worker 0's saved 8 KiB at 0x31000000 is only the cached-code staging area).

   params: [0] iterations 1..1000, [1] active workers 1..3, [2] 0 uncached code,
   1 cached code, [3] mode (0 contiguous copy, 1 the 1x1 post arithmetic,
   2 a 1x1-style strided gather), [4] nonce (nonzero, fresh per submission).

   Workers 1/2 run code only after worker 0 has published it (cached mode) and
   every active worker starts its timed loop after all have checked in. Every
   wait is bounded to 5 ms and fails with code 0x7e instead of hanging.
   Uncached code costs ~0.3 us per packet, so uncached mode allows 8 iterations.
   Handshake words carry the nonce, so stale arena contents cannot satisfy them.

   Output (uncached arena), words: worker w at 16*w: magic 'HVMW', iterations,
   w, mode, start lo/hi, end lo/hi, checksum, status. Handshake at 48..63. */
#include "framenpu.h"
#include <hexagon_types.h>
#include <hexagon_protos.h>

#define UNCACHED_CODE 0xf0a92000u
#define CACHED_CODE 0x87a92000u
#define ALIAS_DELTA 0x69000000u
#define STAGING 0x31000000u
#define SLICE(w) (0x31010000u + (w) * 0x10000u)
#define MAGIC 0x48564d57u
#define WAIT_TICKS 96000u /* 5 ms of the 19.2 MHz counter */
#define TIMED_OUT 0x7eu

#define PUB 48     /* worker 0: code published (nonce) */
#define READY 50   /* + w: worker w checked in (nonce) */
#define DONE 54    /* + w: worker w finished (nonce) */

static inline __attribute__((always_inline)) fnpu_u32 htid(void) {
    fnpu_u32 t;
    __asm__ volatile("%0 = htid" : "=r"(t));
    return t;
}

static inline __attribute__((always_inline)) unsigned long long ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return t;
}

static __attribute__((noinline)) fnpu_u32 wait_word(volatile fnpu_u32 *word, fnpu_u32 nonce) {
    const unsigned long long start = ticks();
    while (ticks() - start < WAIT_TICKS)
        if (*word == nonce) return 1;
    return *word == nonce;
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

/* mode 0: copy 256 contiguous vectors; 1: rs_pw_post arithmetic on 256
 * vectors (qf16 add of a bias, clip to [0, 6], finite flags); 2: gather 16
 * tiles of 16 vectors read at a 31-vector stride into a contiguous buffer. */
static __attribute__((noinline)) HVX_Vector run_loop(fnpu_u32 mode, fnpu_u32 iters, fnpu_u32 w) {
    volatile HVX_Vector *a = (volatile HVX_Vector *)SLICE(w), *b = a + 256;
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), six = Q6_V_vsplat_R(0x46004600u);
    HVX_Vector bias = Q6_V_vsplat_R(0x3c003c00u), fl = Q6_V_vzero();
#pragma clang loop unroll(disable)
    for (fnpu_u32 n = 0; n < iters; n++) {
        if (mode == 0) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 i = 0; i < 256; i++) b[i] = a[i];
        } else if (mode == 1) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 i = 0; i < 256; i++) {
                HVX_Vector v = a[i];
                fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
                v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias));
                v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), six);
                b[i] = v;
            }
        } else {
#pragma clang loop unroll(disable)
            for (fnpu_u32 t = 0; t < 16; t++)
#pragma clang loop unroll(disable)
                for (fnpu_u32 p = 0; p < 16; p++) b[16 * t + p] = a[(t + 31 * p) & 255];
        }
    }
    __asm__ volatile("barrier" ::: "memory");
    return Q6_V_vor_VV(fl, b[255]);
}

/* The timed part; runs from the cached alias in cached mode. */
static __attribute__((noinline)) fnpu_u32 worker_body(const fnpu_context *ctx, fnpu_u32 w) {
    volatile fnpu_u32 *out = fnpu_output(ctx);
    const fnpu_u32 iters = ctx->params[0], active = ctx->params[1], mode = ctx->params[3];
    const fnpu_u32 nonce = ctx->params[4];
    volatile HVX_Vector *slice = (volatile HVX_Vector *)SLICE(w);
    const HVX_Vector one = Q6_V_vsplat_R(0x3c003c00u); /* FP16 1.0: no subnormals */
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 512; i++) slice[i] = one;
    __asm__ volatile("barrier" ::: "memory");
    out[READY + w] = nonce;
    __asm__ volatile("barrier" ::: "memory");
    for (fnpu_u32 k = 0; k < active; k++)
        if (!wait_word(out + READY + k, nonce)) return TIMED_OUT;
    unsigned long long t0 = ticks();
    HVX_Vector r = run_loop(mode, iters, w);
    unsigned long long t1 = ticks();
    volatile HVX_Vector *keep = (volatile HVX_Vector *)SLICE(w) + 511;
    *keep = r;
    __asm__ volatile("barrier" ::: "memory");
    volatile fnpu_u32 *row = out + 16 * w;
    row[0] = MAGIC; row[1] = iters; row[2] = w; row[3] = mode;
    row[4] = (fnpu_u32)t0; row[5] = (fnpu_u32)(t0 >> 32);
    row[6] = (fnpu_u32)t1; row[7] = (fnpu_u32)(t1 >> 32);
    row[8] = ((volatile fnpu_u32 *)keep)[0];
    row[9] = 1;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 9)
        return FNPU_BAD_ARGUMENT;
    const fnpu_u32 iters = ctx->params[0], active = ctx->params[1], cached = ctx->params[2];
    const fnpu_u32 mode = ctx->params[3], nonce = ctx->params[4];
    if (!iters || iters > (cached ? 1000u : 8u) || active < 1 || active > 3 || cached > 1 || mode > 2 ||
        !nonce || !(ctx->flags & FNPU_FLAG_DIRECT) || ctx->input_bytes || ctx->output_bytes < 256u)
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    if (pc < UNCACHED_CODE || pc >= UNCACHED_CODE + 4096) return FNPU_BAD_ARGUMENT; /* entered uncached only */
    const fnpu_u32 w = htid();
    if (w >= active) return FNPU_OK;
    volatile fnpu_u32 *out = fnpu_output(ctx);
    fnpu_u32 (*body)(const fnpu_context *, fnpu_u32) = worker_body;
    if (cached) {
        fnpu_u32 target = (fnpu_u32)worker_body;
        __asm__ volatile("%0 = sub(%0,%1)" : "+r"(target) : "r"(ALIAS_DELTA) : "memory");
        body = (fnpu_u32 (*)(const fnpu_context *, fnpu_u32))target;
    }
    if (w == 0) {
        if (cached) publish_cached();
        __asm__ volatile("barrier" ::: "memory");
        out[PUB] = nonce;
        __asm__ volatile("barrier" ::: "memory");
    } else if (!wait_word(out + PUB, nonce)) {
        return TIMED_OUT;
    }
    fnpu_u32 status = body(ctx, w);
    __asm__ volatile("barrier" ::: "memory");
    out[DONE + w] = status == FNPU_OK ? nonce : status;
    __asm__ volatile("barrier" ::: "memory");
    if (w == 0) {
        for (fnpu_u32 k = 1; k < active; k++)
            if (!wait_word(out + DONE + k, nonce) && status == FNPU_OK) status = TIMED_OUT;
        if (cached) /* leave no published lines behind */
            for (fnpu_u32 offset = 0; offset < 4096; offset += 32) {
                fnpu_u32 address = CACHED_CODE + offset;
                __asm__ volatile("icinva(%0)\n\tisync" :: "r"(address) : "memory");
            }
    }
    return status;
}
