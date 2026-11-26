/* Cached/uncached instruction-fetch A/B for the INT8 probe, after hmx_cached.c.
 * Build with tests/entry_cached.S. params[4]=0: uncached, 1: cached; the other
 * params and layout are hmx_int8_probe.c. The wrapper's PC goes to meta[8].
 * The host must check worker-0 status words 14/15 (cleanup markers) before
 * reclaiming DDR or power, as tests/hmx_cached_run.py does.
 */
#define frame_job int8_frame_job
#include "hmx_int8_probe.c"
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
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 11 ||
        ctx->params[4] > 1)
        return FNPU_BAD_ARGUMENT;
    if (ctx->params[4] && pc >= UNCACHED_CODE && pc < UNCACHED_CODE + 4096) {
        *(volatile fnpu_u32 *)0xf0a93438u = 1;
        __asm__ volatile("barrier" ::: "memory");
        publish_cached();
        fnpu_u32 target = (fnpu_u32)frame_job;
        __asm__ volatile("%0 = sub(%0,%1)" : "+r"(target) : "r"(ALIAS_DELTA) : "memory");
        fnpu_u32 (*cached)(const fnpu_context *) = (fnpu_u32 (*)(const fnpu_context *))target;
        return cached(ctx);
    }
    fnpu_u32 result = int8_frame_job(ctx);
    if (result == FNPU_OK) ((volatile fnpu_u32 *)fnpu_output(ctx))[8] = pc;
    return result;
}
