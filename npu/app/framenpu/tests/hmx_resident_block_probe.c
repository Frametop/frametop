/* Fused HMX spatial->projection block, VTCM-resident version (v2).
 *
 * Same computation, payload and output layout as hmx_block_probe.c (dense
 * 3x3 Cin->Cmid + bias + clip6, then 1x1 Cmid->Cout + bias, packed FP16), so
 * the same oracle and harness validate it. The difference is where operands
 * live: the job borrows the FeatureNN worker-0/1 scratch span
 * 0x31002000-0x311e0000 (docs/VTCM_MAP.md; serialized with direct jobs,
 * write canary passed) in addition to the entry's saved 8 KiB.
 *
 * Prologue, once per job: contiguous DDR->VTCM copies of every stage-1 and
 * stage-2 weight tile, the whole planar source band and the x|position
 * table, plus a per-row-pair gather table (source vector offset, valign
 * shift, column-mask kind) that does not depend on the position block.
 * Per 32-position block: pack all NK1 B tiles from VTCM (two aligned VTCM
 * loads + valign + mask per row pair), one barrier, then for each mid block
 * a back-to-back run of HMX loads straight from the resident weight tiles,
 * store into a resident mid tile, bias/clip in place; one barrier; then the
 * projection loads resident W2 x resident mid tiles. Nothing is spilled.
 * FR_PAIR selects the qualified paired load (range 4095 = two contiguous
 * 2 KiB tiles of both operands, docs/HMX_BATCHING.md) for consecutive tiles.
 *
 * Protections are unchanged: cached entry, worker 0, one HMX owner, 20 ms
 * in-kernel budget, exact header/geometry checks, guarded fault control. The
 * borrowed span is per-request scratch: nothing in it is trusted across jobs. */
#include "landmark_common.h"
typedef unsigned long long fb_u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

#if !defined(FB_GEOM) || !defined(FB_W) || !defined(FB_R) || !defined(FB_CIN) || \
    !defined(FB_NM) || !defined(FB_COUT) || !defined(FB_NBIN) || !defined(FB_CMID)
#error geometry macros are required (tools/build_hmx_block_probe.py)
#endif
#ifndef FB_TIMING
#define FB_TIMING 1
#endif
#ifndef FR_PAIR
#define FR_PAIR 1
#endif

#define FB_NK1 ((9 * FB_CIN + 31) / 32)
#define FB_NOUT (FB_R * FB_W)
#define FB_NJ ((FB_NOUT + 31) / 32)
#define FB_NO ((FB_COUT + 31) / 32)
#define FB_CP (FB_CIN / 2)
#define FB_OCP (FB_COUT / 2)
#define FB_XT 640u
#define FB_SRC (FB_XT + FB_NJ * 128u)
#define FB_W1 (FB_SRC + FB_CP * FB_NBIN * 128u)
#define FB_W2 (FB_W1 + FB_NM * FB_NK1 * 2048u)
#define FB_IN (FB_W2 + FB_NO * FB_NM * 2048u)
#define FB_SPILL (384u + FB_OCP * FB_NJ * 128u)
#define FB_OUT (FB_SPILL + (FB_NM > 1 ? FB_NM * 2048u : 0u))
_Static_assert(FB_NBIN >= FB_NJ + 1 + (2 * FB_W + 33) / 32, "back guard");
_Static_assert(FB_CMID <= FB_NM * 32 && FB_CMID + 32 > FB_NM * 32, "mid blocks");
_Static_assert(FB_NM + FB_NO <= 6 && FB_CIN % 2 == 0 && FB_COUT % 2 == 0, "slots");

/* Saved 8 KiB (vector index): output tile 0-15, scale 56-57, bias 58-60,
 * x|pos of the current block 61, flags 62, timers 63. */
#define FB_V(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define FB_A FB_V(0)
#define FB_SCALE ((volatile fnpu_u32 *)FB_V(56))
#define FB_BIAS ((const volatile fnpu_u32 *)FB_V(58))
#define FB_XP FB_V(61)
#define FB_FLAGS FB_V(62)
#define FB_M ((volatile fnpu_u32 *)FB_V(63))

/* Borrowed span, in vectors from 0x31002000. */
#define FR_BASE 0x31002000u
#define FR_LIMIT 0x311e0000u
#define FR_SRCN (FB_CP * FB_NBIN)
/* HMX operand regions (weights, B tiles, mid tiles) start on 4 KiB
 * boundaries, like every tile the qualified probes used. */
#define FR_AL(v) (((v) + 31u) & ~31u)
#define FR_S1 FR_AL(FB_NK1 * 16u) /* per mid block */
#define FR_S2 FR_AL(FB_NM * 16u)  /* per output block */
#define FR_W1 0u
#define FR_W2 (FR_W1 + FB_NM * FR_S1)
#define FR_BT (FR_W2 + FB_NO * FR_S2)
#define FR_MID FR_AL(FR_BT + FB_NK1 * 16u)
#define FR_SRC FR_AL(FR_MID + FB_NM * 16u)
#define FR_XT (FR_SRC + FR_SRCN)
#define FR_TBL (FR_XT + FB_NJ)
#define FR_END (FR_TBL + (FB_NK1 * 16u * 4u + 127u) / 128u)
_Static_assert(FR_BASE + FR_END * 128u <= FR_LIMIT, "borrowed VTCM span");
_Static_assert(FR_BASE % 4096u == 0, "tile alignment");
#define FR(i) ((volatile HVX_Vector *)(FR_BASE + 128u * (i)))
#define FR_TABLE ((volatile fnpu_u32 *)FR(FR_TBL))

enum { T_WEIGHT, T_STAGE, T_PACK, T_HMX, T_POST, T_SPILL, T_STORE, T_SETUP, M_LAST = 15, M_FAULT = 16 };

static inline __attribute__((always_inline)) fb_u64 fb_ticks(void) {
    fb_u64 t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return t;
}
#if FB_TIMING
static __attribute__((noinline)) void fb_stamp(fnpu_u32 slot) {
    fnpu_u32 now = (fnpu_u32)fb_ticks();
    FB_M[slot] += now - FB_M[M_LAST];
    FB_M[M_LAST] = now;
}
#define TS(slot) fb_stamp(slot)
#else
#define TS(slot) do { } while (0)
#endif

static inline __attribute__((always_inline)) HVX_Vector fb_inf(HVX_Vector v) {
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u);
    return Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1);
}

/* n vectors, eight loads in flight per group (contiguous aligned source). */
static __attribute__((noinline)) void fr_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from,
                                              fnpu_u32 n) {
    fnpu_u32 i = 0;
#pragma clang loop unroll(disable)
    for (; i + 8 <= n; i += 8) {
        HVX_Vector v0 = from[i], v1 = from[i + 1], v2 = from[i + 2], v3 = from[i + 3];
        HVX_Vector v4 = from[i + 4], v5 = from[i + 5], v6 = from[i + 6], v7 = from[i + 7];
        to[i] = v0; to[i + 1] = v1; to[i + 2] = v2; to[i + 3] = v3;
        to[i + 4] = v4; to[i + 5] = v5; to[i + 6] = v6; to[i + 7] = v7;
    }
#pragma clang loop unroll(disable)
    for (; i < n; i++) to[i] = from[i];
}

/* One HMX load of a single tile pair, or (n == 2) of two contiguous tiles of
 * both operands in one packet. Operand registers are pinned. */
static __attribute__((noinline)) void fr_load(const volatile HVX_Vector *a,
                                              const volatile HVX_Vector *b, fnpu_u32 n) {
    register const volatile HVX_Vector *ra __asm__("r0") = a;
    register const volatile HVX_Vector *rb __asm__("r2") = b;
    if (n == 2)
        __asm__ volatile("r1 = #4095\n.word 0x920041e2\n.word 0x9202e1ef\n"
                         : : "r"(ra), "r"(rb) : "r1", "memory");
    else
        __asm__ volatile("r1 = #2047\n.word 0x920041e2\n.word 0x9202e1ef\n"
                         : : "r"(ra), "r"(rb) : "r1", "memory");
}

/* n consecutive tiles of A and B accumulated into the open transaction. */
static __attribute__((noinline)) void fr_run(const volatile HVX_Vector *a,
                                             const volatile HVX_Vector *b, fnpu_u32 n) {
    fnpu_u32 t = 0;
#if FR_PAIR
#pragma clang loop unroll(disable)
    for (; t + 2 <= n; t += 2) fr_load(a + 16 * t, b + 16 * t, 2);
#endif
#pragma clang loop unroll(disable)
    for (; t < n; t++) fr_load(a + 16 * t, b + 16 * t, 1);
}

/* Half bias via QFloat16, finite flags, tail mask; clip6 for stage-1 tiles. */
static __attribute__((noinline)) void fb_post(volatile HVX_Vector *from, volatile HVX_Vector *to,
                                              fnpu_u32 stride, fnpu_u32 slot, fnpu_u32 rows,
                                              fnpu_u32 clip) {
    HVX_Vector flags = FB_FLAGS[0];
    HVX_Vector pos = Q6_Vuw_vlsr_VuwR(FB_XP[0], 16);
    HVX_Vector tail = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(Q6_V_vsplat_R(FB_NOUT), pos),
                                    Q6_V_vsplat_R(-1), Q6_V_vzero());
    HVX_Vector lo = Q6_V_vsplat_R(clip ? 0 : 0xfc00fc00u), hi = Q6_V_vsplat_R(clip ? 0x46004600u : 0x7c007c00u);
#pragma clang loop unroll(disable)
    for (fnpu_u32 rr = 0; rr < rows; rr++) {
        HVX_Vector v = from[rr];
        flags = Q6_V_vor_VV(flags, fb_inf(v));
        v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, Q6_V_vsplat_R(FB_BIAS[16 * slot + rr])));
        flags = Q6_V_vor_VV(flags, fb_inf(v));
        v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, lo), hi);
        to[rr * stride] = Q6_V_vand_VV(v, tail);
    }
    FB_FLAGS[0] = flags;
}

/* Gather table, one word per B row pair: resident source vector offset
 * (without the block index) | valign byte shift << 16 | kind << 24, where
 * kind 0/1/2 = dx -1/0/+1 column mask and 3 = K padding (zero). */
static __attribute__((noinline)) void fr_table(void) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < FB_NK1 * 16u; r++) {
        fnpu_u32 k = 2 * r, tap = k / FB_CIN, ch = k % FB_CIN, word = 3u << 24;
        if (tap < 9) {
            fnpu_u32 s = (tap / 3) * FB_W + tap % 3 + 31;
            word = ((ch / 2) * FB_NBIN + s / 32) | ((s % 32) * 4) << 16 | (tap % 3) << 24;
        }
        FR_TABLE[r] = word;
    }
}

/* All NK1 B tiles of position block j, from the resident band. */
static __attribute__((noinline)) void fr_pack(fnpu_u32 j) {
    HVX_Vector zero = Q6_V_vzero(), ones = Q6_V_vsplat_R(-1);
    HVX_Vector x = Q6_V_vand_VV(FB_XP[0], Q6_V_vsplat_R(0xffff));
    HVX_Vector left = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, zero), zero, ones);
    HVX_Vector right = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, Q6_V_vsplat_R(FB_W - 1)), zero, ones);
    const volatile HVX_Vector *src = FR(FR_SRC + j);
    volatile HVX_Vector *bt = FR(FR_BT);
#pragma clang loop unroll(disable)
    for (fnpu_u32 r = 0; r < FB_NK1 * 16u; r++) {
        fnpu_u32 word = FR_TABLE[r], kind = word >> 24;
        const volatile HVX_Vector *p = src + (word & 0xffffu);
        HVX_Vector v = Q6_V_valign_VVR(p[1], p[0], (word >> 16) & 0xffu);
        HVX_Vector mask = kind == 0 ? left : kind == 2 ? right : ones;
        bt[r] = kind == 3 ? zero : Q6_V_vand_VV(v, mask);
    }
}

static __attribute__((noinline)) void fb_trap(const fnpu_context *c) { fault_control(c); }

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d = fnpu_input(c);
    fnpu_u32 reps = c->params[2];
    if (!valid(c) || c->opcode != 101 || c->params[0] != 1 || c->params[1] != FB_GEOM || !reps ||
        reps > 8 || c->input_bytes != FB_IN || c->output_bytes != FB_OUT)
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 bad = (d[0] ^ 0x31425346u) | (d[1] ^ FB_GEOM) | (d[2] ^ FB_CIN) | (d[4] ^ FB_COUT) |
                   (d[5] ^ FB_W) | (d[6] ^ FB_R) | (d[7] ^ FB_NBIN) | (d[8] ^ FB_NJ) |
                   (d[9] ^ FB_NM) | (d[10] ^ FB_NK1) | (d[3] ^ FB_CMID);
    for (fnpu_u32 i = 13; i < 32; i++) bad |= d[i];
    if (bad || (d[11] | d[12]) > 1024) return FNPU_BAD_ARGUMENT;
    const unsigned char *in = (const unsigned char *)fnpu_input(c);
    unsigned char *base = (unsigned char *)fnpu_output(c);
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)base;
    volatile HVX_Vector *out = (volatile HVX_Vector *)(base + 384);
    volatile HVX_Vector *spill = (volatile HVX_Vector *)(base + FB_SPILL);
    const volatile HVX_Vector *hdr = (const volatile HVX_Vector *)in;
    ((volatile HVX_Vector *)meta)[0] = Q6_V_vzero();
    ((volatile HVX_Vector *)meta)[1] = Q6_V_vzero();
    ((volatile HVX_Vector *)meta)[2] = Q6_V_vzero();
    FB_V(56)[0] = Q6_V_vsplat_R(0x3c00u);
    FB_V(56)[1] = Q6_V_vzero();
    FB_V(63)[0] = Q6_V_vzero();
    for (fnpu_u32 i = 0; i < 3; i++) FB_V(58)[i] = hdr[2 + i];
    FB_FLAGS[0] = Q6_V_vzero();
    FB_M[M_FAULT] = c->params[5];
    fb_u64 start = fb_ticks();
    FB_M[M_LAST] = (fnpu_u32)start;

    /* Prologue: each mid block's W1 run and each output block's W2 run
     * starts on a 4 KiB boundary. */
#pragma clang loop unroll(disable)
    for (fnpu_u32 m = 0; m < FB_NM; m++)
        fr_copy(FR(FR_W1 + m * FR_S1), (const volatile HVX_Vector *)(in + FB_W1) + m * FB_NK1 * 16u,
                FB_NK1 * 16u);
#pragma clang loop unroll(disable)
    for (fnpu_u32 o = 0; o < FB_NO; o++)
        fr_copy(FR(FR_W2 + o * FR_S2), (const volatile HVX_Vector *)(in + FB_W2) + o * FB_NM * 16u,
                FB_NM * 16u);
    TS(T_WEIGHT);
    fr_copy(FR(FR_SRC), (const volatile HVX_Vector *)(in + FB_SRC), FR_SRCN);
    fr_copy(FR(FR_XT), (const volatile HVX_Vector *)(in + FB_XT), FB_NJ);
    fr_table();
    TS(T_STAGE);

    fnpu_u32 done = 0;
    for (fnpu_u32 rep = 0; rep < reps; rep++) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 j = 0; j < FB_NJ; j++) {
            FB_XP[0] = FR(FR_XT)[j];
            TS(T_SETUP);
            fr_pack(j);
            __asm__ volatile("barrier" ::: "memory"); /* B tiles visible to HMX */
            TS(T_PACK);
#pragma clang loop unroll(disable)
            for (fnpu_u32 m = 0; m < FB_NM; m++) {
                fnpu_hmx_clear();
                if (FB_M[M_FAULT]) { fr_load(FR(FR_W1 + m * FR_S1), FR(FR_BT), 1); fb_trap(c); }
                fr_run(FR(FR_W1 + m * FR_S1), FR(FR_BT), FB_NK1);
                fnpu_hmx_store((fnpu_u16 *)FR(FR_MID + 16u * m), (const void *)FB_SCALE);
                TS(T_HMX);
                fb_post(FR(FR_MID + 16u * m), FR(FR_MID + 16u * m), 1, m, 16, 1);
                TS(T_POST);
            }
            __asm__ volatile("barrier" ::: "memory"); /* posted mid tiles visible */
#pragma clang loop unroll(disable)
            for (fnpu_u32 o = 0; o < FB_NO; o++) {
                fnpu_hmx_clear();
                fr_run(FR(FR_W2 + o * FR_S2), FR(FR_MID), FB_NM);
                fnpu_hmx_store((fnpu_u16 *)FB_A, (const void *)FB_SCALE);
                TS(T_HMX);
                fb_post(FB_A, out + 16 * o * FB_NJ + j, FB_NJ, FB_NM + o,
                        FB_OCP - 16 * o > 16 ? 16 : FB_OCP - 16 * o, 0);
                TS(T_STORE);
            }
            done++;
            if ((fnpu_u32)fb_ticks() - (fnpu_u32)start > 384000u) goto finish;
        }
    }
finish:;
    fb_u64 end = fb_ticks();
    /* Outside the timed body: expose the last block's mid tiles in the spill
     * area so the harness checks them as before. */
    if (FB_NM > 1) fr_copy(spill, FR(FR_MID), FB_NM * 16u);
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = 0x31425346u; meta[1] = pc; meta[2] = FB_GEOM; meta[3] = reps; meta[4] = done;
    meta[6] = start; meta[7] = start >> 32; meta[8] = end; meta[9] = end >> 32;
    meta[10] = 8 | FB_TIMING << 8 | 1 << 16 | FR_PAIR << 28 | 1u << 30;
    ((volatile HVX_Vector *)meta)[1] = FB_V(63)[0];
    ((volatile HVX_Vector *)meta)[2] = FB_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
