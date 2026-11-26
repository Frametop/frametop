/* Hardware-only fixture: read-only dump of worker-0 system registers and
   joint-TLB entries. It performs no TLB writes and touches only its output. */
#include "framenpu.h"

fnpu_u32 frame_job(const fnpu_context *ctx) {
    fnpu_u32 first = ctx->params[0], count = ctx->params[1];
    if (!count || count > 256 || first > 1024 || ctx->input_bytes ||
        ctx->output_bytes != 64 + count * 8)
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *out = fnpu_output(ctx);
    fnpu_u32 v;
    __asm__ volatile("%0=ssr" : "=r"(v)); out[0] = v;
    __asm__ volatile("%0=evb" : "=r"(v)); out[1] = v;
    __asm__ volatile("%0=cfgbase" : "=r"(v)); out[2] = v;
    __asm__ volatile("%0=rev" : "=r"(v)); out[3] = v;
    __asm__ volatile("%0=syscfg" : "=r"(v)); out[4] = v;
    __asm__ volatile("%0=r29" : "=r"(v)); out[5] = v;
    __asm__ volatile("%0=ccr" : "=r"(v)); out[6] = v;
    __asm__ volatile("%0=htid" : "=r"(v)); out[7] = v;
    __asm__ volatile("%0=stid" : "=r"(v)); out[8] = v;
    __asm__ volatile("%0=modectl" : "=r"(v)); out[9] = v;
    __asm__ volatile("%0=gp" : "=r"(v)); out[10] = v;
    __asm__ volatile("%0=ugp" : "=r"(v)); out[11] = v;
    for (fnpu_u32 i = 12; i < 16; i++) out[i] = 0;
    volatile unsigned long long *entries = (volatile unsigned long long *)(out + 16);
    for (fnpu_u32 i = 0; i < count; i++) {
        unsigned long long entry;
        __asm__ volatile("%0=tlbr(%1)" : "=r"(entry) : "r"(first + i));
        entries[i] = entry;
    }
    return FNPU_OK;
}
