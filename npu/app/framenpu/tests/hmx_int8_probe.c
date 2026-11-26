/* INT8 HMX probe: fixed-point clear, uint8 activation / int8 weight loads, and the
 * uint8 convert, using encodings one field away from the proven FP16 words:
 *
 *   0xa6e0c011 mxclracc                      (FP16: 0xa6e0c013 mxclracc.hf)
 *   0x920041e0 activation.ub=mxmem(r0,r1):deep (FP16: 0x920041e2 activation.hf)
 *   0x9202e1e0 weight.b=mxmem(r2,r1)          (FP16: 0x9202e1ef weight.hf)
 *   0xa6e0d710 cvt.ub=acc(r0)                 (FP16: 0xa6e0da10 cvt.hf)
 *   bias=mxmem2(r0) and mxmem(r0,r1)=cvt are the FP16 words unchanged.
 *
 * opcode 11. params = (mode, configs K 1..24, depth D 0..512, cvt Rs value).
 *   mode 0: convert only (accumulator cleared, no loads), one convert per config.
 *   mode 1: D activation/weight load pairs per config, then the convert.
 *   mode 2: the same with the proven FP16 clear/load/store (timing comparison;
 *           the config's bias block is the FP16 scale block).
 * input:  act 2048 B | weight 2048 B | K bias blocks of 256 B
 * output: meta 256 B | per config, the whole 4 KiB VTCM span 0x31001000-0x31001fff
 *         after the store. Before each store that span is filled with 0xa5 and
 *         the config's bias block is written at 0x31001800, so the dump shows how
 *         many bytes the store wrote and where.
 * Only the SDK's saved/restored 8 KiB VTCM span (0x31000000) is touched. meta[7]
 * holds the phase reached, so a stall can be located from the output.
 */
#include "fnpu_readback.h"

typedef unsigned long long u64;
void fnpu_i8_clear(void);
void fnpu_i8_load(const void *act, const void *wei);
void fnpu_i8_store(void *out, const void *bias, fnpu_u32 cvt_rs);
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

__asm__(
    ".text\n"
    ".global fnpu_i8_clear\n"
    "fnpu_i8_clear:\n"
    " .word 0xa6e0c011\n"
    " { jumpr r31 }\n"
    ".global fnpu_i8_load\n"
    "fnpu_i8_load:\n"
    " { r2 = r1 }\n"
    " { r1 = #2047 }\n"
    " .word 0x920041e0\n"
    " .word 0x9202e1e0\n"
    " { jumpr r31 }\n"
    ".global fnpu_i8_store\n"
    "fnpu_i8_store:\n"
    " { r3 = r0 }\n"
    " { r0 = r1 }\n"
    " .word 0x9200c3fe\n"
    " { r0 = r2 }\n"
    " .word 0xa6e0d710\n"
    " { r0 = r3 }\n"
    " { r1 = #0 }\n"
    " .word 0xa6e0c118\n"
    " { barrier }\n"
    " { jumpr r31 }\n");

#define ACT 0x31000000u
#define WEI 0x31000800u
#define OUT 0x31001000u
#define BIAS 0x31001800u

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}

static __attribute__((noinline)) void copy_vectors(void *dst, const void *src, fnpu_u32 count) {
    for (fnpu_u32 i = 0; i < count; i++)
        ((volatile HVX_Vector *)dst)[i] = ((const volatile HVX_Vector *)src)[i];
    __asm__ volatile("barrier" ::: "memory");
}

static __attribute__((noinline)) void fill_span(void) {
    volatile HVX_Vector *span = (volatile HVX_Vector *)OUT;
    HVX_Vector canary = Q6_V_vsplat_R(0xa5a5a5a5u);
    for (fnpu_u32 i = 0; i < 32; i++) span[i] = canary;
    __asm__ volatile("barrier" ::: "memory");
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 11) return FNPU_UNSUPPORTED;
    fnpu_u32 mode = ctx->params[0], configs = ctx->params[1], depth = ctx->params[2];
    fnpu_u32 cvt_rs = ctx->params[3];
    if (mode > 2 || !configs || configs > 24 || depth > 512 || (mode && !depth) ||
        (mode == 0 && depth) || cvt_rs > 0xffffu ||
        (ctx->flags & (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT)) !=
            (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT) ||
        ctx->input_bytes != 4096 + configs * 256 ||
        ctx->output_bytes != 256 + configs * 4096 ||
        (ctx->input_offset & 127) || (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    const unsigned char *in = fnpu_input(ctx);
    unsigned char *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)out;
    meta[0] = 0x484d3849u; meta[1] = mode; meta[2] = configs; meta[3] = depth;
    meta[4] = cvt_rs; meta[7] = 1;
    __asm__ volatile("barrier" ::: "memory");

    u64 start = ticks();
    copy_vectors((void *)ACT, in, 32);
    meta[7] = 2;
    for (fnpu_u32 k = 0; k < configs; k++) {
        fill_span();
        copy_vectors((void *)BIAS, in + 4096 + k * 256, 2);
        meta[6] = k; meta[7] = 3;
        __asm__ volatile("barrier" ::: "memory");
        u64 begin = ticks();
        if (mode == 2) {
            fnpu_hmx_clear();
            meta[7] = 4;
            for (fnpu_u32 d = 0; d < depth; d++)
                fnpu_hmx_load((const fnpu_u16 *)ACT, (const fnpu_u16 *)WEI);
            meta[7] = 5;
            fnpu_hmx_store((fnpu_u16 *)OUT, (const void *)BIAS);
        } else {
            fnpu_i8_clear();
            meta[7] = 4;
            for (fnpu_u32 d = 0; d < depth; d++)
                fnpu_i8_load((const void *)ACT, (const void *)WEI);
            meta[7] = 5;
            fnpu_i8_store((void *)OUT, (const void *)BIAS, cvt_rs);
        }
        u64 end = ticks();
        meta[7] = 6;
        copy_vectors(out + 256 + k * 4096, (const void *)OUT, 32);
        volatile fnpu_u32 *record = meta + 16 + k * 2;
        record[0] = (fnpu_u32)begin; record[1] = (fnpu_u32)(end - begin);
        /* Yield only after a completed store; never clear an active gate or turn
         * off HMX because this local deadline elapsed. */
        if ((fnpu_u32)(ticks() - start) > 288000u) return FNPU_BAD_ARGUMENT;
    }
    meta[5] = (fnpu_u32)(ticks() - start);
    meta[7] = 7;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
