/* Hardware-only fixture: VTCM vector throughput against row stride, in the
   access pattern of the network's 1x1 gather (read 16 vectors S apart, write
   them contiguously; next tile starts one vector later) and post (the reverse:
   contiguous reads, strided writes). Cached code on worker 0, plain pointers,
   4 independent loads in flight. VTCM from 0x31010000, at most 512 KiB (inside
   the borrowed span every Mercury request overwrites).
   Build with tests/entry_mw.S and handoff-tools/build_mw.py (direct dispatch).

   params: [0] stride S in vectors 1..248, [1] repetitions 1..64,
   [2] 0 strided reads / 1 strided writes, [3] nonce, [4] 1: contiguous buffer
   at 0x31130000 (as the replica's B tiles) instead of after the grid,
   [5] 1: a barrier after every 8 tiles (as the grouped 1x1 kernel).
   Output words: magic, S, reps, ticks lo/hi, cycles lo/hi, packets lo/hi. */
#include "framenpu.h"
#include <hexagon_types.h>
#include <hexagon_protos.h>

#define UNCACHED_CODE 0xf0a92000u
#define CACHED_CODE 0x87a92000u
#define ALIAS_DELTA 0x69000000u
#define STAGING 0x31000000u
#define AREA ((HVX_Vector *)0x31010000u)
#define MAGIC 0x53545244u /* 'STRD' */
#define TILES 128u

static __attribute__((noinline)) void sweep(fnpu_u32 s, fnpu_u32 reps, fnpu_u32 writes, fnpu_u32 hi,
                                            fnpu_u32 group) {
    HVX_Vector *grid = AREA, *buf = hi ? (HVX_Vector *)0x31130000u : AREA + 16 * 248 + TILES + 8;
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < reps; r++) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 t = 0; t < TILES; t++) {
            HVX_Vector *g = grid + t, *b = buf + 16 * (t & 7);
            if (!writes) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 p = 0; p < 16; p += 4) {
                    HVX_Vector a0 = g[p * s], a1 = g[(p + 1) * s], a2 = g[(p + 2) * s], a3 = g[(p + 3) * s];
                    b[p] = a0; b[p + 1] = a1; b[p + 2] = a2; b[p + 3] = a3;
                }
            } else {
#pragma clang loop unroll(disable)
                for (fnpu_u32 p = 0; p < 16; p += 4) {
                    HVX_Vector a0 = b[p], a1 = b[p + 1], a2 = b[p + 2], a3 = b[p + 3];
                    g[p * s] = a0; g[(p + 1) * s] = a1; g[(p + 2) * s] = a2; g[(p + 3) * s] = a3;
                }
            }
            if (group && (t & 7) == 7) __asm__ volatile("barrier" ::: "memory");
        }
        __asm__ volatile("barrier" ::: "memory");
    }
}

static __attribute__((noinline)) fnpu_u32 body(const fnpu_context *ctx) {
    const fnpu_u32 s = ctx->params[0], reps = ctx->params[1], writes = ctx->params[2];
    const HVX_Vector one = Q6_V_vsplat_R(0x3c003c00u);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 16 * 248 + TILES + 8 + 128; i++) AREA[i] = one;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 128; i++) ((HVX_Vector *)0x31130000u)[i] = one;
    __asm__ volatile("barrier" ::: "memory");
    unsigned long long t0, t1, c0, c1, k0, k1;
    __asm__ volatile("%0 = c31:30" : "=r"(t0)::"memory");
    __asm__ volatile("%0 = c15:14" : "=r"(c0)::"memory");
    __asm__ volatile("%0 = c19:18" : "=r"(k0)::"memory");
    sweep(s, reps, writes, ctx->params[4], ctx->params[5]);
    __asm__ volatile("%0 = c31:30" : "=r"(t1)::"memory");
    __asm__ volatile("%0 = c15:14" : "=r"(c1)::"memory");
    __asm__ volatile("%0 = c19:18" : "=r"(k1)::"memory");
    volatile fnpu_u32 *out = fnpu_output(ctx);
    t1 -= t0; c1 -= c0; k1 -= k0;
    out[0] = MAGIC; out[1] = s; out[2] = reps;
    out[3] = (fnpu_u32)t1; out[4] = (fnpu_u32)(t1 >> 32);
    out[5] = (fnpu_u32)c1; out[6] = (fnpu_u32)(c1 >> 32);
    out[7] = (fnpu_u32)k1; out[8] = (fnpu_u32)(k1 >> 32);
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
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 11)
        return FNPU_BAD_ARGUMENT;
    if (!ctx->params[0] || ctx->params[0] > 248u || !ctx->params[1] || ctx->params[1] > 64u ||
        ctx->params[2] > 1u || !ctx->params[3] || ctx->params[4] > 1u || ctx->params[5] > 1u || !(ctx->flags & FNPU_FLAG_DIRECT) ||
        ctx->input_bytes || ctx->output_bytes < 64u)
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
