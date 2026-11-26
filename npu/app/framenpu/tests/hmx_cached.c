/* Diagnostic cached/uncached instruction-fetch A/B. Build with entry_cached.S.
 * params[2]=0: uncached, 1: cached; params[3]=1 injects a guarded cached trap.
 * Other params and layout are hmx_baseline.c. Power policy is unchanged.
 * Run publication only after the SDK has installed its guarded window and
 * saved 8 KiB VTCM. Stage all 4 KiB of fresh code before any cached writes:
 * dcinva also cleans, so it cannot safely discard dirty aliases after upload.
 * Full 128-byte cached stores precede clean/invalidate of their cache lines.
 * The existing baseline then overwrites this temporary VTCM staging with input.
 * Arm worker-0 status word 14 before cached stores. The experimental entry
 * cleans/invalidates code on both normal and synchronous-fault return and sets
 * word 15. The host must validate these markers before DDR/power reclamation.
 * Controlled hardware results are in docs/HMX_CACHE.md; stalls remain unrecoverable.
 */
#define frame_job baseline_frame_job
#include "hmx_baseline.c"
#undef frame_job

#define UNCACHED_CODE 0xf0a92000u
#define CACHED_CODE 0x87a92000u
#define ALIAS_DELTA 0x69000000u

static __attribute__((noinline)) void publish_cached(void) {
    copy_vectors((void *)0x31000000u, (const void *)UNCACHED_CODE, 32);
    for (fnpu_u32 i = 0; i < 32; i++) {
        ((volatile HVX_Vector *)CACHED_CODE)[i] = ((const volatile HVX_Vector *)0x31000000u)[i];
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
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc) :: "memory");
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 9 ||
        !ctx->params[0] || ctx->params[0] > 512 ||
        !ctx->params[1] || ctx->params[1] > 4 || ctx->params[2] > 1 ||
        ctx->params[3] > 1 || (ctx->params[3] && !ctx->params[2]) ||
        (ctx->flags & (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT)) !=
            (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT) ||
        ctx->input_bytes != 4096 || ctx->output_bytes != 256 + ctx->params[1] * 2048 + 4096 ||
        (ctx->input_offset & 127) || (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    if (ctx->params[2] && pc >= UNCACHED_CODE && pc < UNCACHED_CODE + 4096) {
        *(volatile fnpu_u32 *)0xf0a93438u = 1;
        __asm__ volatile("barrier" ::: "memory");
        publish_cached();
        fnpu_u32 target = (fnpu_u32)frame_job;
        /* Keep alias arithmetic opaque: the compiler can otherwise recover
         * the original function identity and emit an uncached recursive call. */
        __asm__ volatile("%0 = sub(%0,%1)" : "+r"(target) : "r"(ALIAS_DELTA) : "memory");
        fnpu_u32 (*cached)(const fnpu_context *) =
            (fnpu_u32 (*)(const fnpu_context *))target;
        return cached(ctx);
    }
    if (ctx->params[3]) __asm__ volatile("trap0(#0)" ::: "memory");
    fnpu_u32 result = baseline_frame_job(ctx);
    if (result == FNPU_OK) ((volatile fnpu_u32 *)fnpu_output(ctx))[7] = pc;
    return result;
}
