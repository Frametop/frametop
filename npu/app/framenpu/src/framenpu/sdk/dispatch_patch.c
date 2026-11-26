/* Reversible ABI 9 dispatch edit, executed only through the ordinary worker
 * callback. Input: 96 expected bytes followed by 96 replacement bytes. The
 * installed firmware supplies the original bytes; no firmware is bundled.
 */
#include "framenpu.h"

fnpu_u32 frame_job(const fnpu_context *ctx) {
    volatile fnpu_u32 *control = (volatile fnpu_u32 *)0xf0a91000u;
    volatile fnpu_u32 *status = (volatile fnpu_u32 *)0xf0a93400u;
    volatile fnpu_u32 *uncached = (volatile fnpu_u32 *)0xf00005a0u;
    volatile fnpu_u32 *code = (volatile fnpu_u32 *)0x870005a0u;
    const fnpu_u32 *input = fnpu_input(ctx);
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI ||
        ctx->flags || ctx->input_bytes != 192 || ctx->output_bytes ||
        ctx->input_offset != 0 || ctx->arena_bytes < 192)
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 ready = 0;
    fnpu_u32 started, now;
    __asm__ volatile("%0 = c30" : "=r"(started));
    /* This firmware's c31:30 reference counter runs at 19.2 MHz. Allow at
     * most 20 ms for the stock master and other callbacks to return. */
    do {
        if (control[0] == 3 && status[16] == ctx->sequence &&
            status[32] == ctx->sequence) {
            ready = 1;
            break;
        }
        __asm__ volatile("pause(#64)\n\t%0 = c30" : "=r"(now) : : "memory");
    } while ((fnpu_u32)(now - started) < 384000u);
    if (!ready || control[1] != 1 || control[2] != 4)
        return FNPU_BAD_ARGUMENT;
    for (fnpu_u32 i = 0; i < 24; i++)
        if (uncached[i] != input[i])
            return FNPU_BAD_ARGUMENT;
    /* The master is in its acknowledgment loop and workers 1/2 have returned.
     * Use the cached execution alias for instruction/data cache maintenance.
     * Sequence follows Hexagon's self-modifying-code protocol (also LLVM XRay).
     */
    for (fnpu_u32 i = 0; i < 24; i++) {
        volatile fnpu_u32 *address = code + i;
        fnpu_u32 value = input[24 + i];
        __asm__ volatile(
            "icinva(%0)\n\tisync\n\tmemw(%0) = %1\n\t"
            "dccleaninva(%0)\n\tsyncht\n\t"
            : : "r"(address), "r"(value) : "memory");
    }
    __asm__ volatile("isync" ::: "memory");
    for (fnpu_u32 i = 0; i < 24; i++)
        if (uncached[i] != input[24 + i])
            return FNPU_EXCEPTION;
    return FNPU_OK;
}
