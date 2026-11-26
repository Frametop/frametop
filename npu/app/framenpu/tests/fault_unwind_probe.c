/* Public-entry regression: nested live C frames, optionally trapping at leaf.
 * params=(depth 0..6, trap 0/1), opcode 10, no input, 128-byte output.
 * Build with the ordinary compiler/SDK entry, with and without --hmx.
 * HMX jobs dirty only the SDK's saved/restored 8 KiB VTCM span.
 */
#include "framenpu.h"

#define CANARY 0x13579bdfu
#define PENDING 0xdecafbadu

static __attribute__((noinline)) fnpu_u32 nested(const fnpu_context *ctx,
                                               volatile fnpu_u32 *out,
                                               fnpu_u32 level) {
    volatile fnpu_u32 frame[8];
    for (fnpu_u32 i = 0; i < 8; i++) frame[i] = CANARY ^ (level * 0x10101u) ^ i;
    fnpu_u32 sp, fp, index = out[5];
    __asm__ volatile("%0 = r29\n\t%1 = r30" : "=r"(sp), "=r"(fp) :: "memory");
    out[8 + index] = sp;
    out[16 + index] = fp;
    out[24 + index] = level;
    out[5] = index + 1;
    __asm__ volatile("barrier" ::: "memory");
    fnpu_u32 value = 0;
    if (level) value = nested(ctx, out, level - 1);
    else if (ctx->params[1]) __asm__ volatile("trap0(#37)" ::: "memory");
    /* Volatile frame use after the call prevents tail-call elimination. */
    value += frame[level & 7];
    out[6]++;
    return value;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 10 ||
        ctx->input_bytes || ctx->output_bytes != 128 ||
        ctx->params[0] > 6 || ctx->params[1] > 1)
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *out = fnpu_output(ctx);
    out[0] = 0x554e5744u;
    out[1] = 1;
    out[2] = ctx->params[0];
    out[3] = ctx->params[1];
    out[4] = ctx->flags & FNPU_FLAG_HMX;
    out[7] = PENDING;
    if (ctx->flags & FNPU_FLAG_HMX) {
        volatile fnpu_u32 *scratch = (volatile fnpu_u32 *)0x31000000u;
        for (fnpu_u32 i = 0; i < 2048; i++) scratch[i] = CANARY ^ i;
        __asm__ volatile("barrier" ::: "memory");
    }
    out[7] = nested(ctx, out, ctx->params[0]);
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
