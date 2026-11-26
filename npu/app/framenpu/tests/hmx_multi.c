/* Hardware-only fixture: multi-worker HMX scaling (experiment #1).

   Built with tests/entry_mw.S, which (unlike the shipped entry.S) runs frame_job
   on workers 0, 1 and 2 concurrently in direct dispatch -- the master already
   resume(#7)s all three and joins them. Each worker self-identifies by HTID and
   drives the shipped pipelined HMX loop on its OWN VTCM slice
   (0x31000000 + htid*0xf0000, the firmware's own per-worker layout, confirmed in
   FUN_87005330). params[1]=active (1..3) gates how many workers actually run, so
   the same dispatch measures 1 vs 2 vs 3 concurrent HMX streams.

   Record each worker's reference-tick span (status words 10..13, returned in
   JobResult.workers). Slopes at two iteration counts remove fixed setup costs.
   Starts are not synchronized after scratch backup, and concurrent clears can
   interfere with the accumulator, so timing alone cannot establish parallel
   independent GEMMs or exclusive hardware ownership. Requires HMX + DIRECT.

   The first 64 output bytes are results. The next two 8 KiB slabs preserve
   worker-1/2 VTCM across a successful call (worker 0 is saved by entry_mw.S).
   These are borrowed scratch, not free persistent allocations. */
#include "framenpu.h"

void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static fnpu_u32 self_htid(void) {
    fnpu_u32 t;
    __asm__ volatile("%0 = htid" : "=r"(t));
    return t;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 7)
        return FNPU_UNSUPPORTED;
    fnpu_u32 iters = ctx->params[0];
    fnpu_u32 active = ctx->params[1];
    if (!iters || iters > 1000u || active < 1 || active > 3 ||
        !(ctx->flags & FNPU_FLAG_HMX) || !(ctx->flags & FNPU_FLAG_DIRECT) ||
        ctx->input_bytes || ctx->output_bytes < 64u + 2u * 8192u)
        return FNPU_BAD_ARGUMENT;

    fnpu_u32 w = self_htid();
    if (w >= active)
        return FNPU_OK; /* idle worker: returns fast, tiny tick span */

    fnpu_u32 base = 0x31000000u + w * 0xf0000u;
    volatile fnpu_u32 *out = fnpu_output(ctx);
    volatile fnpu_u32 *saved = out + 16 + (w ? w - 1 : 0) * 2048;
    volatile fnpu_u32 *scratch = (volatile fnpu_u32 *)base;
    if (w) {
        for (fnpu_u32 i = 0; i < 2048; i++) saved[i] = scratch[i];
        __asm__ volatile("barrier" ::: "memory");
    }
    volatile fnpu_u16 *ta = (volatile fnpu_u16 *)base;
    volatile fnpu_u16 *tb = (volatile fnpu_u16 *)(base + 0x800u);
    volatile fnpu_u16 *tc = (volatile fnpu_u16 *)(base + 0x1000u);
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)(base + 0x1800u);
    for (fnpu_u32 i = 0; i < 1024; i++) { ta[i] = 0x3c00u; tb[i] = 0x3c00u; }
    for (fnpu_u32 i = 0; i < 32; i++) scales[i] = 0x3c00u;
    for (fnpu_u32 i = 32; i < 64; i++) scales[i] = 0;
    __asm__ volatile("barrier" ::: "memory");

    fnpu_hmx_clear();
    for (fnpu_u32 n = 0; n < iters; n++)
        fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
    fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);

    out[w * 4 + 0] = 0x484d574bu; /* 'HMWK' */
    out[w * 4 + 1] = iters;
    out[w * 4 + 2] = (fnpu_u32)(int)tc[0];
    out[w * 4 + 3] = w;
    __asm__ volatile("barrier" ::: "memory");
    if (w) {
        for (fnpu_u32 i = 0; i < 2048; i++) scratch[i] = saved[i];
        __asm__ volatile("barrier" ::: "memory");
    }
    return FNPU_OK;
}
