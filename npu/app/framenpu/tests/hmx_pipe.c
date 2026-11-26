/* Hardware-only fixture: decompose the per-tile HMX cost and measure the
   pipelined load/accumulate ceiling.

   Three styles over M iterations on resident VTCM tiles (worker 0):
     style 0  per-iter clear + load + store   (baseline = shipped primitive)
     style 1  clear once; M x load; store once (pure load/accumulate throughput)
     style 2  per-iter clear + load; store once (isolates store + its barrier)
   Then: (0-2) ~ per-iter store+barrier, (2-1) ~ per-iter clear, (1) ~ HMX MAC.

   fnpu_hmx_load accumulates into the HMX array when not preceded by clear, so
   style 1 sums M depth-32 products into one 32x32 accumulator; values are not
   checked (timing only). Requires the HMX flag. VTCM layout matches gemm. */
#include "framenpu.h"

void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static fnpu_u32 pipe(const fnpu_context *ctx) {
    fnpu_u32 iters = ctx->params[0];
    fnpu_u32 style = ctx->params[1];
    if (!iters || iters > 1000u || style > 2 ||
        !(ctx->flags & FNPU_FLAG_HMX) || ctx->input_bytes || ctx->output_bytes != 64)
        return FNPU_BAD_ARGUMENT;

    volatile fnpu_u16 *ta = (volatile fnpu_u16 *)0x31000000u;
    volatile fnpu_u16 *tb = (volatile fnpu_u16 *)0x31000800u;
    volatile fnpu_u16 *tc = (volatile fnpu_u16 *)0x31001000u;
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)0x31001800u;
    for (fnpu_u32 i = 0; i < 1024; i++) { ta[i] = 0x3c00u; tb[i] = 0x3c00u; }
    for (fnpu_u32 i = 0; i < 32; i++) scales[i] = 0x3c00u;
    for (fnpu_u32 i = 32; i < 64; i++) scales[i] = 0;
    __asm__ volatile("barrier" ::: "memory");

    if (style == 0) {
        for (fnpu_u32 n = 0; n < iters; n++) {
            fnpu_hmx_clear();
            fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
            fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
        }
    } else if (style == 1) {
        fnpu_hmx_clear();
        for (fnpu_u32 n = 0; n < iters; n++)
            fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
        fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
    } else {
        for (fnpu_u32 n = 0; n < iters; n++) {
            fnpu_hmx_clear();
            fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
        }
        fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
    }

    volatile fnpu_u32 *out = fnpu_output(ctx);
    out[0] = 0x48504950u; /* 'HPIP' */
    out[1] = iters;
    out[2] = (fnpu_u32)(int)tc[0]; /* dependency sink */
    out[3] = style;
    for (fnpu_u32 i = 4; i < 16; i++) out[i] = 0;
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode == 7) return pipe(ctx);
    return FNPU_UNSUPPORTED;
}
