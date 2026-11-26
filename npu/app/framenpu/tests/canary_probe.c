/* Hardware-only fixture: write/verify a deterministic canary pattern across the
   persistent-arena candidate, to test whether stock firmware (gaze/FeatureNN)
   disturbs it during a real soak. It touches ONLY a caller-bounded sub-span that
   must lie at or above the verified safe floor; it never writes firmware memory.

   The arena candidate is DDR buffer 0x1a22000..0x2000000 (above every reserved
   5 MiB per-worker FeatureNN slot; see docs/MEMMAP.md). Addressing uses the whole-
   buffer aliases: uncached 0xf0000000+off (straight to DDR) or cached 0x87000000+off
   (DSP's own view, with explicit clean/invalidate). The job arena/code/status all
   live below 0xb22000 and never overlap the span. */
#include "framenpu.h"
#include <hexagon_protos.h>

#define FNPU_BUF_SIZE   0x2000000u
#define FNPU_SAFE_FLOOR 0x1a22000u
#define ALIAS_UNCACHED  0xf0000000u
#define ALIAS_CACHED    0x87000000u

/* Deterministic, offset-sensitive pattern. Any stray write is detectable and
   localizable because the expected value depends on the byte offset. */
static fnpu_u32 pattern(fnpu_u32 seed, fnpu_u32 off) {
    fnpu_u32 v = seed * 0x01000193u;
    v ^= off * 0x9e3779b1u;
    v ^= v >> 15;
    v *= 0x85ebca77u;
    v ^= v >> 13;
    return v;
}

static fnpu_u32 canary(const fnpu_context *ctx) {
    fnpu_u32 mode = ctx->params[0];      /* 1 = fill, 2 = verify */
    fnpu_u32 seed = ctx->params[1];
    fnpu_u32 off = ctx->params[2];       /* byte offset from buffer base */
    fnpu_u32 len = ctx->params[3];       /* bytes, this chunk */
    fnpu_u32 cached = ctx->params[4];    /* 0 uncached alias, 1 cached alias */
    if ((mode != 1 && mode != 2) || cached > 1 || (off & 127) || (len & 127) ||
        len == 0 || len > 0x200000u || off < FNPU_SAFE_FLOOR ||
        off > FNPU_BUF_SIZE || len > FNPU_BUF_SIZE - off ||
        ctx->input_bytes || ctx->output_bytes != 64)
        return FNPU_BAD_ARGUMENT;

    volatile fnpu_u32 *out = fnpu_output(ctx);
    fnpu_u32 base = (cached ? ALIAS_CACHED : ALIAS_UNCACHED) + off;
    fnpu_u32 words = len >> 2;
    fnpu_u32 mismatches = 0, first_off = 0xffffffffu, first_exp = 0, first_got = 0;

    if (mode == 1) {
        volatile fnpu_u32 *p = (volatile fnpu_u32 *)base;
        for (fnpu_u32 i = 0; i < words; i++) p[i] = pattern(seed, off + i * 4);
        if (cached) {
            for (fnpu_u32 a = base; a < base + len; a += 32)
                __asm__ volatile("dccleaninva(%0)" ::"r"(a) : "memory");
            __asm__ volatile("syncht" ::: "memory");
        }
    } else {
        if (cached) {
            for (fnpu_u32 a = base; a < base + len; a += 32)
                __asm__ volatile("dcinva(%0)" ::"r"(a) : "memory");
            __asm__ volatile("syncht" ::: "memory");
        }
        const volatile fnpu_u32 *p = (const volatile fnpu_u32 *)base;
        for (fnpu_u32 i = 0; i < words; i++) {
            fnpu_u32 exp = pattern(seed, off + i * 4), got = p[i];
            if (got != exp) {
                if (!mismatches) { first_off = off + i * 4; first_exp = exp; first_got = got; }
                mismatches++;
            }
        }
    }

    out[0] = 0x434e5259u; /* 'CNRY' */
    out[1] = mode;
    out[2] = mismatches;
    out[3] = first_off;
    out[4] = first_exp;
    out[5] = first_got;
    out[6] = words;
    out[7] = off;
    out[8] = len;
    out[9] = cached;
    for (fnpu_u32 i = 10; i < 16; i++) out[i] = 0;
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode == 1) return canary(ctx);
    return FNPU_UNSUPPORTED;
}
