/* Experimental contiguous two-depth-block load, using only the SDK's saved
 * 8 KiB scratch. Activation and weight bases deliberately alias, so this tests
 * C = groups * (A0*A0 + A1*A1), not a distinct activation/weight working set.
 * params=(groups 1..256, transactions 1..4, mode 0 control / 1 batch).
 * Opcode/output timing layout are inherited from the correct baseline; word 7
 * is mode+1. The control emits two ordinary load pairs; batch emits the same
 * pair with r1=4095 to cover two contiguous 2 KiB blocks in one instruction.
 * No public SDK, opcode, clock setting, or scratch reservation changes.
 */
#define frame_job baseline_frame_job
#define fnpu_hmx_load diagnostic_load
#include "hmx_baseline.c"
#undef fnpu_hmx_load
#undef frame_job

void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);

void diagnostic_load(const fnpu_u16 *unused_a, const fnpu_u16 *unused_b) {
    (void)unused_a;
    (void)unused_b;
    const volatile fnpu_context *ctx = (const volatile fnpu_context *)0xf0a93000u;
    if (ctx->params[2] == 0) {
        fnpu_hmx_load((const fnpu_u16 *)0x31000000u, (const fnpu_u16 *)0x31000000u);
        fnpu_hmx_load((const fnpu_u16 *)0x31000800u, (const fnpu_u16 *)0x31000800u);
    } else {
        /* Both raw words are one packet. All implicit register operands stay
         * pinned in this block; 4095 encodes two complete depth blocks. */
        __asm__ volatile(
            "r0 = ##0x31000000\n"
            "r2 = r0\n"
            "r1 = #4095\n"
            ".word 0x920041e2\n"
            ".word 0x9202e1ef\n"
            ::: "r0", "r1", "r2", "memory");
    }
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI ||
        !ctx->params[0] || ctx->params[0] > 256 || ctx->params[2] > 1)
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 status = baseline_frame_job(ctx);
    if (status == FNPU_OK) {
        volatile fnpu_u32 *meta = fnpu_output(ctx);
        meta[7] = ctx->params[2] + 1;
        __asm__ volatile("barrier" ::: "memory");
    }
    return status;
}
