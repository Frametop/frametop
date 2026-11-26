/* DDR -> VTCM copy strategies for the Mercury weight copy (rs_copy pays one
 * 0.105 us DDR vector load at a time: 7,312 vectors ~= 750 us per crop).
 *
 * opcode 13. params = (method, vectors n 1..768, unroll u in 1,2,4,8,16, spin, cache).
 *   method 0: rs_copy as in teacher_resident_probe.c (volatile, one vector at a time),
 *             uncached source alias 0xf0a97000.
 *   method 1: u loads in flight, then u stores, uncached source.
 *   method 2: as 1 from the cached alias (0x87a97000), source lines invalidated first
 *             (cold, and coherent with the host's /dev/mem writes).
 *   method 3: as 2, but l2fetch the whole source first (timed together with the copy).
 *   method 4: as 3, but spin `spin` ticks between the l2fetch and the timed copy:
 *             the copy cost when a prefetch was issued early enough (overlap bound).
 * The source is the job input (n * 128 bytes); the destination is FeatureNN worker
 * scratch 0x31002000.. (used within this job only, approved region).
 * output: meta 256 B | XOR fold of the n copied vectors (128 B) | first and last
 * copied vector (256 B). meta[5] = copy ticks, meta[6] = prefetch issue ticks,
 * meta[7] = phase (4 when done).
 */
#include "fnpu_readback.h"

typedef unsigned long long u64;
#define DST 0x31002000u
#define CACHED_DELTA 0x69000000u

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}

static __attribute__((noinline)) void copy_one(volatile HVX_Vector *to, const volatile HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = from[i];
}

#define LOADS(k) HVX_Vector v##k = from[i + k];
#define STORES(k) to[i + k] = v##k;

static __attribute__((noinline)) void copy_u2(HVX_Vector *to, const HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i += 2) { LOADS(0) LOADS(1) STORES(0) STORES(1) }
}
static __attribute__((noinline)) void copy_u4(HVX_Vector *to, const HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i += 4) { LOADS(0) LOADS(1) LOADS(2) LOADS(3) STORES(0) STORES(1) STORES(2) STORES(3) }
}
static __attribute__((noinline)) void copy_u8(HVX_Vector *to, const HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i += 8) {
        LOADS(0) LOADS(1) LOADS(2) LOADS(3) LOADS(4) LOADS(5) LOADS(6) LOADS(7)
        STORES(0) STORES(1) STORES(2) STORES(3) STORES(4) STORES(5) STORES(6) STORES(7)
    }
}
static __attribute__((noinline)) void copy_u16(HVX_Vector *to, const HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i += 16) {
        LOADS(0) LOADS(1) LOADS(2) LOADS(3) LOADS(4) LOADS(5) LOADS(6) LOADS(7)
        LOADS(8) LOADS(9) LOADS(10) LOADS(11) LOADS(12) LOADS(13) LOADS(14) LOADS(15)
        STORES(0) STORES(1) STORES(2) STORES(3) STORES(4) STORES(5) STORES(6) STORES(7)
        STORES(8) STORES(9) STORES(10) STORES(11) STORES(12) STORES(13) STORES(14) STORES(15)
    }
}

static __attribute__((noinline)) void invalidate(fnpu_u32 base, fnpu_u32 bytes) {
    for (fnpu_u32 off = 0; off < bytes; off += 32)
        __asm__ volatile("dcinva(%0)" :: "r"(base + off) : "memory");
    __asm__ volatile("syncht" ::: "memory");
}

/* l2fetch(Rs, Rt): Rt = stride << 16 | width << 8 | height; 128-byte rows. */
static __attribute__((noinline)) void prefetch(fnpu_u32 base, fnpu_u32 vectors) {
    for (fnpu_u32 v = 0; v < vectors; v += 255) {
        fnpu_u32 rows = vectors - v < 255 ? vectors - v : 255;
        fnpu_u32 desc = (128u << 16) | (128u << 8) | rows;
        __asm__ volatile("l2fetch(%0,%1)" :: "r"(base + v * 128u), "r"(desc) : "memory");
    }
}

static __attribute__((noinline)) void copy_vectors(void *dst, const void *src, fnpu_u32 count) {
    for (fnpu_u32 i = 0; i < count; i++)
        ((volatile HVX_Vector *)dst)[i] = ((const volatile HVX_Vector *)src)[i];
    __asm__ volatile("barrier" ::: "memory");
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 13) return FNPU_UNSUPPORTED;
    fnpu_u32 method = ctx->params[0], n = ctx->params[1], u = ctx->params[2], spin = ctx->params[3];
    if (method > 4 || !n || n > 768 || (u != 1 && u != 2 && u != 4 && u != 8 && u != 16) ||
        (n & (u - 1)) || (method == 0 && u != 1) || (method && u == 1) || spin > 192000u ||
        (ctx->flags & FNPU_FLAG_DIRECT) != FNPU_FLAG_DIRECT ||
        ctx->input_bytes != n * 128u || ctx->output_bytes != 256 + 128 + 256 ||
        (ctx->input_offset & 127) || (ctx->output_offset & 127))
        return FNPU_BAD_ARGUMENT;
    unsigned char *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)out;
    meta[0] = 0x44435059u; meta[1] = method; meta[2] = n; meta[3] = u; meta[7] = 1;
    __asm__ volatile("barrier" ::: "memory");

    fnpu_u32 src = (fnpu_u32)fnpu_input(ctx);
    if (method >= 2) {
        src -= CACHED_DELTA;
        invalidate(src, n * 128u);
    }
    meta[7] = 2;
    __asm__ volatile("barrier" ::: "memory");
    u64 p0 = ticks();
    if (method >= 3) prefetch(src, n);
    u64 p1 = ticks();
    if (method == 4) while (ticks() - p1 < spin) {}
    __asm__ volatile("barrier" ::: "memory");
    u64 start = method == 4 ? ticks() : p0;
    HVX_Vector *to = (HVX_Vector *)DST;
    const HVX_Vector *from = (const HVX_Vector *)src;
    if (method == 0) copy_one(to, from, n);
    else if (u == 2) copy_u2(to, from, n);
    else if (u == 4) copy_u4(to, from, n);
    else if (u == 8) copy_u8(to, from, n);
    else copy_u16(to, from, n);
    __asm__ volatile("barrier" ::: "memory");
    /* Completion: read the last copied vector back (orders the VTCM stores). */
    {
        volatile HVX_Vector sink = ((const volatile HVX_Vector *)DST)[n - 1];
        (void)sink;
    }
    u64 end = ticks();
    meta[5] = (fnpu_u32)(end - start);
    meta[6] = (fnpu_u32)(p1 - p0);
    meta[7] = 3;
    HVX_Vector acc = Q6_V_vzero();
    for (fnpu_u32 i = 0; i < n; i++) acc = Q6_V_vxor_VV(acc, ((const volatile HVX_Vector *)DST)[i]);
    *(volatile HVX_Vector *)(out + 256) = acc;
    copy_vectors(out + 384, (const void *)DST, 1);
    copy_vectors(out + 512, (const void *)(DST + (n - 1) * 128u), 1);
    meta[7] = 4;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
