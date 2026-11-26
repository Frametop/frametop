/* Hardware-only fixture: weight-resident streaming ceiling (experiment #2).

   The shipped load primitive (sdk/hmx.S fnpu_hmx_load) issues two adjacent raw
   HMX words with r2 = weight base preloaded:
     word A  0x920041e2   reads r0 (activation ptr), r1 = 2047 (length), r2 live
     word B  0x9202e1ef   reads r2 (weight ptr)
   Neither Ghidra 12.1.4 nor LLVM decode these mnemonics, so the split is by
   register use. The whole hot loop for each style is one asm block so r2 (the
   weight base) stays pinned across it -- the compiler cannot see that the raw
   words read r2, and reusing it mid-loop hangs worker 0.

     style 1  clear; loop (A + B); store         control = shipped pair, both words
     style 3  clear; B once; loop (A); store      weight-resident: load weights once
     style 5  clear; loop (A); store              A alone, B never issued

   style 3 vs 5 tests whether the activation word needs the weight word to have
   run first. Timing only; values unchecked. A fault is recorded as error 2 by
   the entry handler. Requires the HMX flag; VTCM layout matches gemm. */
#include "framenpu.h"

void fnpu_hmx_clear(void);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static fnpu_u32 stream(const fnpu_context *ctx) {
    fnpu_u32 iters = ctx->params[0];
    fnpu_u32 style = ctx->params[1];
    if (!iters || iters > 1000u || (style != 1 && style != 3 && style != 5) ||
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

    void *av = (void *)ta;
    void *wb = (void *)tb;
    fnpu_hmx_clear();
    if (style == 1) {
        __asm__ volatile(
            " r2 = %[wb]\n"
            " r4 = %[it]\n"
            "1:\n"
            " r0 = %[av]\n"
            " r1 = #2047\n"
            " .word 0x920041e2\n"   /* A */
            " .word 0x9202e1ef\n"   /* B */
            " r4 = add(r4, #-1)\n"
            " p0 = cmp.gt(r4, #0)\n"
            " if (p0) jump 1b\n"
            :: [av] "r"(av), [wb] "r"(wb), [it] "r"(iters)
            : "r0", "r1", "r2", "r4", "p0", "memory");
    } else if (style == 3) {
        __asm__ volatile(
            " r2 = %[wb]\n"
            " .word 0x9202e1ef\n"   /* B once: load weights */
            " r4 = %[it]\n"
            "1:\n"
            " r0 = %[av]\n"
            " r1 = #2047\n"
            " .word 0x920041e2\n"   /* A streams, r2 pinned = wb */
            " r4 = add(r4, #-1)\n"
            " p0 = cmp.gt(r4, #0)\n"
            " if (p0) jump 1b\n"
            :: [av] "r"(av), [wb] "r"(wb), [it] "r"(iters)
            : "r0", "r1", "r2", "r4", "p0", "memory");
    } else {
        __asm__ volatile(
            " r2 = %[wb]\n"
            " r4 = %[it]\n"
            "1:\n"
            " r0 = %[av]\n"
            " r1 = #2047\n"
            " .word 0x920041e2\n"   /* A only, no B ever */
            " r4 = add(r4, #-1)\n"
            " p0 = cmp.gt(r4, #0)\n"
            " if (p0) jump 1b\n"
            :: [av] "r"(av), [wb] "r"(wb), [it] "r"(iters)
            : "r0", "r1", "r2", "r4", "p0", "memory");
    }
    fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);

    volatile fnpu_u32 *out = fnpu_output(ctx);
    out[0] = 0x48535452u; /* 'HSTR' */
    out[1] = iters;
    out[2] = (fnpu_u32)(int)tc[0];
    out[3] = style;
    for (fnpu_u32 i = 4; i < 16; i++) out[i] = 0;
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode == 7) return stream(ctx);
    return FNPU_UNSUPPORTED;
}
