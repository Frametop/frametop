/* Streaming INT8 vs FP16 HMX tiles, in the pattern of the network's 1x1 kernel
 * (teacher_resident_probe.c rs_pw): per output tile, clear, nk load pairs with
 * distinct VTCM operands, then a store to the tile's own output buffer.
 *
 * opcode 12. params = (kind 1 INT8 / 2 FP16, tiles T 1..512, nk 1..16, flags, cache).
 * flags bit 0: read each tile back right after its store (the ungrouped network's
 * post dependency); bit 1: scale block at 0x31001c00 (the network's RS_SCALE = LV(56))
 * instead of 0x31001800; bit 2: fill A/B with a non-trivial pattern (outputs then
 * not checked by the host); bit 3: a scalar VTCM read (0x31001f80) feeds every load's
 * address; bit 4: a scalar stack read feeds every load's address (rs_pw reloads
 * d->vpar from its stack descriptor before each load).
 * Pools of 64 A tiles, 64 B tiles and 64 output tiles (2 KiB each) live in the
 * FeatureNN worker scratch 0x31002000.. (used within this job only); load i of
 * the run uses A/B tile i % 64, tile t stores to output t % 64. A and B are
 * filled with ones (uint8/int8 1, or FP16 1.0) before timing, so INT8 tiles are
 * nk (FP16 scale 16, bias 0) and FP16 tiles are 32*nk.
 * input: the 256-byte scale/bias block. output: meta 256 B | output tile 0 |
 * output tile (T-1) % 64 (2 KiB each). meta[5] = issue ticks (to the last store's
 * barrier), meta[6] = completion ticks (after reading the last tile back), meta[7] = phase.
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

#define BIAS_ALIGNED 0x31001800u
#define BIAS_NET 0x31001c00u
#define POOL_A 0x31002000u
#define POOL_B (POOL_A + 64u * 2048u)
#define POOL_O (POOL_B + 64u * 2048u)
#define TILE(base, i) ((base) + 2048u * ((i) & 63u))

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

static __attribute__((noinline)) void fill(fnpu_u32 base, fnpu_u32 vectors, fnpu_u32 word) {
    volatile HVX_Vector *p = (volatile HVX_Vector *)base;
    HVX_Vector v = Q6_V_vsplat_R(word);
    for (fnpu_u32 i = 0; i < vectors; i++) p[i] = v;
    __asm__ volatile("barrier" ::: "memory");
}

/* Read a tile's first and last vectors back (waits for the HMX writes). */
static __attribute__((noinline)) void sink_tile(fnpu_u32 base) {
    const volatile HVX_Vector *tile = (const volatile HVX_Vector *)base;
    volatile HVX_Vector sink[2];
    sink[0] = tile[0];
    sink[1] = tile[15];
    (void)sink;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 12) return FNPU_UNSUPPORTED;
    fnpu_u32 kind = ctx->params[0], tiles = ctx->params[1], nk = ctx->params[2];
    fnpu_u32 flags = ctx->params[3];
    if (kind < 1 || kind > 2 || !tiles || tiles > 512 || !nk || nk > 16 || flags > 31 ||
        (ctx->flags & (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT)) !=
            (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT) ||
        ctx->input_bytes != 256 || ctx->output_bytes != 256 + 2 * 2048 ||
        (ctx->input_offset & 127) || (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    unsigned char *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)out;
    meta[0] = 0x484d5354u; meta[1] = kind; meta[2] = tiles; meta[3] = nk; meta[7] = 1;
    __asm__ volatile("barrier" ::: "memory");

    const fnpu_u32 bias = flags & 2 ? BIAS_NET : BIAS_ALIGNED;
    copy_vectors((void *)bias, fnpu_input(ctx), 2);
    fnpu_u32 one = kind == 1 ? 0x01010101u : 0x3c003c00u;
    if (flags & 4) one = kind == 1 ? 0x7f03c215u : 0x3a1fb57du;
    fill(POOL_A, 64 * 16, one);
    fill(POOL_B, 64 * 16, one);
    fill(POOL_O, 64 * 16, 0xa5a5a5a5u);
    meta[7] = 2;
    __asm__ volatile("barrier" ::: "memory");

    volatile fnpu_u32 *vword = (volatile fnpu_u32 *)0x31001f80u;
    volatile fnpu_u32 sword[1];
    vword[0] = 0;
    sword[0] = 0;
    __asm__ volatile("barrier" ::: "memory");
    fnpu_u32 i = 0;
    u64 start = ticks();
    if (kind == 1) {
        for (fnpu_u32 t = 0; t < tiles; t++) {
            fnpu_i8_clear();
            for (fnpu_u32 k = 0; k < nk; k++, i++) {
                fnpu_u32 off = (flags & 8 ? vword[0] : 0) + (flags & 16 ? sword[0] : 0);
                fnpu_i8_load((const void *)(TILE(POOL_A, i) + off), (const void *)TILE(POOL_B, i));
            }
            fnpu_i8_store((void *)TILE(POOL_O, t), (const void *)bias, 0);
            if (flags & 1) sink_tile(TILE(POOL_O, t));
        }
    } else {
        for (fnpu_u32 t = 0; t < tiles; t++) {
            fnpu_hmx_clear();
            for (fnpu_u32 k = 0; k < nk; k++, i++) {
                fnpu_u32 off = (flags & 8 ? vword[0] : 0) + (flags & 16 ? sword[0] : 0);
                fnpu_hmx_load((const fnpu_u16 *)(TILE(POOL_A, i) + off), (const fnpu_u16 *)TILE(POOL_B, i));
            }
            fnpu_hmx_store((fnpu_u16 *)TILE(POOL_O, t), (const void *)bias);
            if (flags & 1) sink_tile(TILE(POOL_O, t));
        }
    }
    __asm__ volatile("barrier" ::: "memory");
    u64 end = ticks();
    /* Completion: hmx_store returns before its tile is written, so read the last
     * tile back from VTCM (first and last vector) and stamp after the reads. */
    {
        const volatile HVX_Vector *last = (const volatile HVX_Vector *)TILE(POOL_O, tiles - 1);
        volatile HVX_Vector sink[2];
        sink[0] = last[0];
        sink[1] = last[15];
        (void)sink;
    }
    __asm__ volatile("barrier" ::: "memory");
    u64 done = ticks();
    meta[5] = (fnpu_u32)(end - start);
    meta[6] = (fnpu_u32)(done - start);
    meta[7] = 3;
    copy_vectors(out + 256, (const void *)TILE(POOL_O, 0), 16);
    copy_vectors(out + 256 + 2048, (const void *)TILE(POOL_O, tiles - 1), 16);
    meta[7] = 4;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
