/* Hardware-only fixture: can C code run with its stack in VTCM?
   Worker 0 reads FRAMELIMIT/FRAMEKEY/SP, fills an 8 KiB VTCM area at 0x31010000
   (inside the borrowed span every Mercury request overwrites) with a canary,
   then calls a recursive function with SP at the top of that area and
   FRAMELIMIT at its bottom. The deepest frame spins on the timer for params[0]
   ticks (at most 15 ms) so interrupts can arrive meanwhile. SP and FRAMELIMIT
   are restored before anything is reported. Afterwards the canary shows where
   anything wrote: our own frames lie between the lowest SP reached and the top;
   a write below that came from somewhere else (an interrupt using the user SP).
   Build with tests/entry_mw.S and handoff-tools/build_mw.py (direct dispatch).

   params: [0] spin ticks 1..288000, [1] depth 1..8, [2] nonce.
   Output words: magic, framelimit, framekey, entry SP, lowest SP, lowest
   changed address (0 none), changed words, words changed below lowest SP,
   spun ticks lo/hi, result check, framelimit after, SP after. */
#include "framenpu.h"

#define MAGIC 0x53544b50u /* 'STKP' */
#define AREA 0x31010000u
#define AREA_BYTES 8192u
#define CANARY 0xa5a5a5a5u

__asm__(".text\n"
        ".balign 16\n"
        "switch_call:\n"           /* r0 arg, r1 fn, r2 new SP, r3 new FRAMELIMIT */
        " { allocframe(#16) }\n"
        " { memd(r29+#0) = r17:16 }\n"
        " { r16 = r29 }\n"
        " { r17 = framelimit }\n"
        " { framelimit = r3 }\n"
        " { r29 = r2 }\n"
        " { callr r1 }\n"
        " { r29 = r16 }\n"
        " { framelimit = r17 }\n"
        " { r17:16 = memd(r29+#0) }\n"
        " { dealloc_return }\n");
fnpu_u32 switch_call(fnpu_u32 arg, fnpu_u32 (*fn)(fnpu_u32), fnpu_u32 sp, fnpu_u32 limit);

/* probe state in VTCM just above the stack area, not in the payload */
#define g_spin (((volatile fnpu_u32 *)(AREA + AREA_BYTES))[0])
#define g_min_sp (((volatile fnpu_u32 *)(AREA + AREA_BYTES))[1])
#define g_spun (((volatile unsigned long long *)(AREA + AREA_BYTES))[1])

static __attribute__((noinline)) fnpu_u32 deep(fnpu_u32 n) {
    volatile fnpu_u32 local[8];
    fnpu_u32 sp;
    __asm__ volatile("%0 = r29" : "=r"(sp));
    if (sp < g_min_sp) g_min_sp = sp;
    for (fnpu_u32 i = 0; i < 8; i++) local[i] = n * 8 + i;
    fnpu_u32 r = 0;
    if (n) {
        r = deep(n - 1);
    } else {
        unsigned long long t0, t;
        __asm__ volatile("%0 = c31:30" : "=r"(t0)::"memory");
        do __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
        while (t - t0 < g_spin);
        g_spun = t - t0;
    }
    for (fnpu_u32 i = 0; i < 8; i++) r += local[i];
    return r;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 12)
        return FNPU_BAD_ARGUMENT;
    if (!ctx->params[0] || ctx->params[0] > 288000u || !ctx->params[1] || ctx->params[1] > 8u ||
        !ctx->params[2] || !(ctx->flags & FNPU_FLAG_DIRECT) || ctx->input_bytes ||
        ctx->output_bytes < 64u)
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 w;
    __asm__ volatile("%0 = htid" : "=r"(w));
    if (w) return FNPU_OK; /* worker 0 only */
    fnpu_u32 limit, key, sp;
    __asm__ volatile("%0 = framelimit" : "=r"(limit));
    __asm__ volatile("%0 = framekey" : "=r"(key));
    __asm__ volatile("%0 = r29" : "=r"(sp));
    volatile fnpu_u32 *area = (volatile fnpu_u32 *)AREA;
    for (fnpu_u32 i = 0; i < AREA_BYTES / 4; i++) area[i] = CANARY;
    __asm__ volatile("barrier" ::: "memory");
    g_spin = ctx->params[0];
    g_min_sp = 0xffffffffu;
    g_spun = 0;
    fnpu_u32 check = switch_call(ctx->params[1], deep, AREA + AREA_BYTES, AREA);
    fnpu_u32 limit_after, sp_after;
    __asm__ volatile("%0 = framelimit" : "=r"(limit_after));
    __asm__ volatile("%0 = r29" : "=r"(sp_after));
    fnpu_u32 lowest = 0, changed = 0, below = 0;
    for (fnpu_u32 i = 0; i < AREA_BYTES / 4; i++) {
        if (area[i] == CANARY) continue;
        fnpu_u32 at = AREA + 4 * i;
        if (!lowest) lowest = at;
        changed++;
        if (at < g_min_sp) below++;
    }
    volatile fnpu_u32 *out = fnpu_output(ctx);
    out[0] = MAGIC; out[1] = limit; out[2] = key; out[3] = sp; out[4] = g_min_sp;
    out[5] = lowest; out[6] = changed; out[7] = below;
    out[8] = (fnpu_u32)g_spun; out[9] = (fnpu_u32)(g_spun >> 32);
    out[10] = check; out[11] = limit_after; out[12] = sp_after;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
