/* Correct single-owner resident GEMM baseline. Build with the stock SDK entry.
 * params=(depth tiles 1..512, transactions 1..4), opcode 9, input=two row-major
 * 32x32 FP16 blocks (4096 bytes). Each transaction multiplies repeated copies
 * of those blocks along K, retaining the accumulator across its depth tiles.
 * Every transaction's full packed FP16 output is returned, followed by the
 * last output converted to row-major FP32. No packing/DDR copy is in the HMX
 * timing windows. Input blocks remain resident for all transactions. After
 * their last use, their 4 KiB VTCM span holds converted FP32 output.
 * Only the SDK's saved/restored 8 KiB VTCM scratch is touched.
 */
#include "fnpu_readback.h"

typedef unsigned long long u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}

static void stamp(volatile fnpu_u32 *dst, u64 value) {
    dst[0] = (fnpu_u32)value;
    dst[1] = (fnpu_u32)(value >> 32);
}

static __attribute__((noinline)) void copy_vectors(void *dst, const void *src, fnpu_u32 count) {
    for (fnpu_u32 i = 0; i < count; i++)
        ((volatile HVX_Vector *)dst)[i] = ((const volatile HVX_Vector *)src)[i];
    __asm__ volatile("barrier" ::: "memory");
}

static __attribute__((noinline)) void pack_resident(void) {
    volatile HVX_Vector *tiles = (volatile HVX_Vector *)0x31000000u;
    for (fnpu_u32 i = 0; i < 32; i++) tiles[i] = Q6_Vh_vshuff_Vh(tiles[i]);
    __asm__ volatile("barrier" ::: "memory");
}

static __attribute__((noinline)) void convert_resident(void) {
    const volatile fnpu_u16 *tile = (const volatile fnpu_u16 *)0x31001000u;
    volatile HVX_Vector *converted = (volatile HVX_Vector *)0x31000000u;
    for (fnpu_u32 r = 0; r < 32; r++) converted[r] = read_row(tile, r);
    __asm__ volatile("barrier" ::: "memory");
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 9) return FNPU_UNSUPPORTED;
    fnpu_u32 depth = ctx->params[0], transactions = ctx->params[1];
    if (!depth || depth > 512 || !transactions || transactions > 4 ||
        (ctx->flags & (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT)) !=
            (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT) ||
        ctx->input_bytes != 4096 || ctx->output_bytes != 256 + transactions * 2048 + 4096 ||
        (ctx->input_offset & 127) || (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    unsigned char *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)out;
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)0x31001800u;
    for (fnpu_u32 i = 0; i < 64; i++) scales[i] = i < 32 ? 0x3c00u : 0;
    __asm__ volatile("barrier" ::: "memory");

    u64 start = ticks(), transferred, packed;
    copy_vectors((void *)0x31000000u, fnpu_input(ctx), 32);
    transferred = ticks();
    pack_resident();
    packed = ticks();
    stamp(meta + 8, start);
    stamp(meta + 10, transferred);
    stamp(meta + 12, packed);
    for (fnpu_u32 rep = 0; rep < transactions; rep++) {
        u64 begin = ticks();
        fnpu_hmx_clear();
        for (fnpu_u32 tile = 0; tile < depth; tile++)
            fnpu_hmx_load((const fnpu_u16 *)0x31000000u, (const fnpu_u16 *)0x31000800u);
        fnpu_hmx_store((fnpu_u16 *)0x31001000u, (const void *)scales);
        u64 end = ticks();
        u64 copy_start = ticks();
        copy_vectors(out + 256 + rep * 2048, (const void *)0x31001000u, 16);
        u64 copy_end = ticks();
        volatile fnpu_u32 *record = meta + 32 + rep * 8;
        stamp(record, begin); stamp(record + 2, end);
        stamp(record + 4, copy_start); stamp(record + 6, copy_end);
        /* Yield only after a completed transaction. Never clear an active
         * operation gate or turn off HMX because this local deadline elapsed. */
        if ((fnpu_u32)(copy_end - start) > 384000u) return FNPU_BAD_ARGUMENT;
    }
    u64 convert_start = ticks();
    convert_resident();
    u64 convert_end = ticks();
    copy_vectors(out + 256 + transactions * 2048, (const void *)0x31000000u, 32);
    u64 copied = ticks();
    u64 timer_start = ticks();
    u64 timer_end = ticks();
    stamp(meta + 16, convert_start); stamp(meta + 18, convert_end);
    stamp(meta + 20, copied); stamp(meta + 22, timer_end - timer_start);
    meta[0] = 0x484d5342u; meta[1] = 1; meta[2] = depth; meta[3] = transactions;
    meta[4] = 4096; meta[5] = transactions * 2048; meta[6] = 4096;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
