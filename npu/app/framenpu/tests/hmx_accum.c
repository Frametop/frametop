/* Hardware-only HMX accumulator isolation test, built with entry_mw.S.
 * params = (iterations 1..32, worker mask 1..7, mode 0..2).
 * mode 0: serialize complete clear/load/store transactions.
 * mode 1: serialize clear/load phases, then serialize deferred stores.
 * mode 2: release complete transactions together after setup.
 * Uniform FP16 A=1, B=2^HTID gives exactly 32*iterations*2^HTID everywhere.
 * Output: 256 bytes control/metadata, three raw 2 KiB tiles, two 8 KiB
 * worker-1/2 VTCM backup slabs. Worker 0 is saved by the entry wrapper.
 * Shared rendezvous are uncached DDR, single-writer monotonic generations.
 * Any rendezvous expires at 24 ms from frame_job entry and signals peers;
 * the remaining time is reserved for scratch restoration and host completion.
 */
#include "framenpu.h"

typedef unsigned long long u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

#define OUTPUT_BYTES (256u + 3u * 2048u + 2u * 8192u)
#define WAIT_TICKS 460800u

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}

static void stamp(volatile fnpu_u32 *p) {
    u64 value = ticks();
    p[0] = (fnpu_u32)value;
    p[1] = (fnpu_u32)(value >> 32);
}

static int rendezvous(volatile fnpu_u32 *out, fnpu_u32 w, fnpu_u32 mask,
                      fnpu_u32 generation, fnpu_u32 started) {
    __asm__ volatile("barrier" ::: "memory");
    out[w] = generation;
    __asm__ volatile("barrier" ::: "memory");
    for (;;) {
        fnpu_u32 ready = 1;
        for (fnpu_u32 i = 0; i < 3; i++) {
            if (!(mask & (1u << i))) continue;
            if (out[4 + i]) return 0;
            if (out[i] < generation) ready = 0;
        }
        if (ready) {
            __asm__ volatile("barrier" ::: "memory");
            return 1;
        }
        if ((fnpu_u32)ticks() - started >= WAIT_TICKS) {
            out[4 + w] = 1;
            __asm__ volatile("barrier" ::: "memory");
            return 0;
        }
        __asm__ volatile("pause(#64)" ::: "memory");
    }
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI)
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode != 8) return FNPU_UNSUPPORTED;
    fnpu_u32 iters = ctx->params[0], mask = ctx->params[1], mode = ctx->params[2];
    if (!iters || iters > 32 || !mask || mask > 7 || mode > 2 ||
        (ctx->flags & (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT)) !=
            (FNPU_FLAG_HMX | FNPU_FLAG_DIRECT) ||
        ctx->input_bytes || ctx->output_bytes != OUTPUT_BYTES)
        return FNPU_BAD_ARGUMENT;

    fnpu_u32 w;
    __asm__ volatile("%0 = htid" : "=r"(w));
    if (w > 2 || !(mask & (1u << w))) return FNPU_OK;
    fnpu_u32 started = (fnpu_u32)ticks();
    volatile fnpu_u32 *out = fnpu_output(ctx);
    volatile fnpu_u32 *meta = out + 16 + w * 16;
    volatile fnpu_u32 *saved = out + 1600 + (w ? w - 1 : 0) * 2048;
    fnpu_u32 base = 0x31000000u + w * 0xf0000u;
    volatile fnpu_u32 *scratch = (volatile fnpu_u32 *)base;
    volatile fnpu_u16 *ta = (volatile fnpu_u16 *)base;
    volatile fnpu_u16 *tb = (volatile fnpu_u16 *)(base + 0x800u);
    volatile fnpu_u16 *tc = (volatile fnpu_u16 *)(base + 0x1000u);
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)(base + 0x1800u);
    fnpu_u32 ok = 0, generation = 1;
    meta[1] = w; meta[2] = mask; meta[3] = mode; meta[4] = iters;
    if (w) {
        for (fnpu_u32 i = 0; i < 2048; i++) saved[i] = scratch[i];
        __asm__ volatile("barrier" ::: "memory");
    }
    for (fnpu_u32 i = 0; i < 1024; i++) {
        ta[i] = 0x3c00u;
        tb[i] = (fnpu_u16)(0x3c00u + (w << 10));
        tc[i] = 0x7e00u; /* NaN sentinel catches unwritten elements. */
    }
    for (fnpu_u32 i = 0; i < 64; i++) scales[i] = i < 32 ? 0x3c00u : 0;
    stamp(meta + 6);
    if (!rendezvous(out, w, mask, generation++, started)) goto restore;

    if (mode == 2) {
        stamp(meta + 8);
        fnpu_hmx_clear();
        for (fnpu_u32 n = 0; n < iters; n++)
            fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
        stamp(meta + 10);
        stamp(meta + 12);
        fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
        stamp(meta + 14);
    } else {
        for (fnpu_u32 turn = 0; turn < 3; turn++) {
            if (!(mask & (1u << turn))) continue;
            if (w == turn) {
                stamp(meta + 8);
                fnpu_hmx_clear();
                for (fnpu_u32 n = 0; n < iters; n++)
                    fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
                stamp(meta + 10);
                if (mode == 0) {
                    stamp(meta + 12);
                    fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
                    stamp(meta + 14);
                }
            }
            if (!rendezvous(out, w, mask, generation++, started)) goto restore;
        }
        if (mode == 1) {
            for (fnpu_u32 turn = 0; turn < 3; turn++) {
                if (!(mask & (1u << turn))) continue;
                if (w == turn) {
                    stamp(meta + 12);
                    fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
                    stamp(meta + 14);
                }
                if (!rendezvous(out, w, mask, generation++, started)) goto restore;
            }
        }
    }
    if (!rendezvous(out, w, mask, generation++, started)) goto restore;
    volatile fnpu_u16 *tile = (volatile fnpu_u16 *)(out + 64 + w * 512);
    for (fnpu_u32 i = 0; i < 1024; i++) tile[i] = tc[i];
    ok = 1;
restore:
    __asm__ volatile("barrier" ::: "memory");
    if (w) {
        for (fnpu_u32 i = 0; i < 2048; i++) scratch[i] = saved[i];
        __asm__ volatile("barrier" ::: "memory");
    }
    meta[5] = !ok;
    meta[0] = 0x48414343u;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK; /* meta[5] reports rendezvous failure after restoration. */
}
