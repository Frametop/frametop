/* Hardware-only fixture: resident-tile HMX throughput ceiling.

   One frame_job wakeup sets up fixed VTCM tiles once, then runs M back-to-back
   32x32x32 FP16 HMX products with NO per-tile upload and NO host round trip.
   The entry wrapper's c31:30 reference counters (status words 10..13) bracket
   frame_job, so running two M values isolates the steady-state per-GEMM cost
   and cancels fixed setup. This measures the HMX arithmetic (+ optional output
   conversion) ceiling, separate from transfer/power/benchmark overheads.

   Not a numerical test: tile values are fixed. Requires the HMX and direct
   flags. VTCM layout matches runtime.c gemm; only the saved/restored 8 KiB
   scratch is touched. */
#include "framenpu.h"
#include "fnpu_readback.h"

void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static fnpu_u32 bench(const fnpu_context *ctx) {
    fnpu_u32 iters = ctx->params[0];
    fnpu_u32 readback = ctx->params[1];
    if (!iters || iters > 1000u || readback > 1 ||
        !(ctx->flags & FNPU_FLAG_HMX) ||
        ctx->input_bytes || ctx->output_bytes != 64)
        return FNPU_BAD_ARGUMENT;

    volatile fnpu_u16 *ta = (volatile fnpu_u16 *)0x31000000u;
    volatile fnpu_u16 *tb = (volatile fnpu_u16 *)0x31000800u;
    volatile fnpu_u16 *tc = (volatile fnpu_u16 *)0x31001000u;
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)0x31001800u;
    for (fnpu_u32 i = 0; i < 1024; i++) { ta[i] = 0x3c00u; tb[i] = 0x3c00u; }
    for (fnpu_u32 i = 0; i < 32; i++) scales[i] = 0x3c00u;
    for (fnpu_u32 i = 32; i < 64; i++) scales[i] = 0;
    __asm__ volatile("barrier" ::: "memory");

    /* Accumulate a value that depends on the HMX output so the loop cannot be
       hoisted or dropped. */
    volatile float sink[32];
    for (fnpu_u32 i = 0; i < 32; i++) sink[i] = 0.0f;
    for (fnpu_u32 n = 0; n < iters; n++) {
        fnpu_hmx_clear();
        fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
        fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
        if (readback)
            read_tile(tc, (float *)sink, 1, 32, 32);
        else
            sink[0] += (float)(int)tc[0];
    }

    volatile fnpu_u32 *out = fnpu_output(ctx);
    float acc = 0.0f;
    for (fnpu_u32 i = 0; i < 32; i++) acc += sink[i];
    out[0] = 0x484d4258u; /* 'HMBX' */
    out[1] = iters;
    out[2] = (fnpu_u32)(int)acc;
    out[3] = readback;
    for (fnpu_u32 i = 4; i < 16; i++) out[i] = 0;
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode == 7) return bench(ctx);
    return FNPU_UNSUPPORTED;
}
