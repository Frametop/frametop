/* FeatureNN VTCM blob claim test (see memory featurenn-blob-claim).
 *
 * The firmware copies its 0x50ea80-byte blob from DDR 0x8707c400 to VTCM 0x311e0000
 * before a FeatureNN request whenever the flag at 0xf0b12014 is 0. The host clears
 * that flag before mode 1; this job never writes the flag or anything outside
 * the blob range.
 *
 * opcode 14. params = (mode, seed, _, _, cache).
 *   mode 0: compare VTCM blob range with its DDR source (uncached 0xf007c400).
 *   mode 1: fill the blob range with pattern(seed, vector index), then check it.
 *   mode 2: check the pattern only.
 * output: meta 256 B | per 64 KiB chunk (81): the number of nonzero words (0..32) in
 * the OR of (VTCM ^ expected) over the chunk, so 0 means the chunk matches exactly.
 * meta[5] = ticks, meta[6] = chunks that differ, meta[7] = phase (3 when done).
 */
#include "fnpu_readback.h"

typedef unsigned long long u64;
#define BLOB_VTCM 0x311e0000u
#define BLOB_DDR 0xf007c400u
#define BLOB_VECTORS 41429u /* 0x50ea80 / 128 */
_Static_assert(BLOB_VECTORS * 128u == 0x50ea80u, "blob size");
#define CHUNKS 81u

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}

static HVX_Vector pattern(fnpu_u32 seed, fnpu_u32 i) {
    HVX_Vector v = Q6_V_vsplat_R(seed ^ (i * 0x9e3779b9u));
    return Q6_Vw_vadd_VwVw(v, Q6_V_vsplat_R(i));
}

static __attribute__((noinline)) void copy_vectors(void *dst, const void *src, fnpu_u32 count) {
    for (fnpu_u32 i = 0; i < count; i++)
        ((volatile HVX_Vector *)dst)[i] = ((const volatile HVX_Vector *)src)[i];
    __asm__ volatile("barrier" ::: "memory");
}

#define FOLD 0x31001000u /* SDK-saved 8 KiB VTCM span */

/* Nonzero words of one vector (parked in VTCM; one scalar pass per chunk). */
static __attribute__((noinline)) fnpu_u32 nonzero_words(HVX_Vector v) {
    *(volatile HVX_Vector *)FOLD = v;
    __asm__ volatile("barrier" ::: "memory");
    const volatile fnpu_u32 *w = (const volatile fnpu_u32 *)FOLD;
    fnpu_u32 n = 0;
    for (fnpu_u32 k = 0; k < 32; k++) n += w[k] != 0;
    return n;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 14) return FNPU_UNSUPPORTED;
    fnpu_u32 mode = ctx->params[0], seed = ctx->params[1];
    if (mode > 2 || (ctx->flags & FNPU_FLAG_DIRECT) != FNPU_FLAG_DIRECT ||
        ctx->input_bytes != 0 || ctx->output_bytes != 256 + CHUNKS * 4 + 60 ||
        (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    unsigned char *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)out;
    volatile fnpu_u32 *count = (volatile fnpu_u32 *)(out + 256);
    meta[0] = 0x424c4f42u; meta[1] = mode; meta[2] = seed; meta[7] = 1;
    for (fnpu_u32 c = 0; c < CHUNKS; c++) count[c] = 0;
    __asm__ volatile("barrier" ::: "memory");
    u64 start = ticks();
    volatile HVX_Vector *vt = (volatile HVX_Vector *)BLOB_VTCM;
    const volatile HVX_Vector *dd = (const volatile HVX_Vector *)BLOB_DDR;
    if (mode == 1) {
        for (fnpu_u32 i = 0; i < BLOB_VECTORS; i++) vt[i] = pattern(seed, i);
        __asm__ volatile("barrier" ::: "memory");
    }
    meta[7] = 2;
    fnpu_u32 total = 0;
    for (fnpu_u32 c = 0; c < CHUNKS; c++) {
        fnpu_u32 lo = c << 9, hi = lo + 512 < BLOB_VECTORS ? lo + 512 : BLOB_VECTORS;
        HVX_Vector acc = Q6_V_vzero();
        if (mode == 0)
            for (fnpu_u32 i = lo; i < hi; i++) acc = Q6_V_vor_VV(acc, Q6_V_vxor_VV(vt[i], dd[i]));
        else
            for (fnpu_u32 i = lo; i < hi; i++) acc = Q6_V_vor_VV(acc, Q6_V_vxor_VV(vt[i], pattern(seed, i)));
        fnpu_u32 n = nonzero_words(acc);
        count[c] = n;
        total += n != 0;
    }
    meta[5] = (fnpu_u32)(ticks() - start);
    meta[6] = total;
    meta[7] = 3;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
