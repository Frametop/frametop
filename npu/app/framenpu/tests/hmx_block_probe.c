/* Fused HMX spatial->projection block prototype (not an integrated executor).
 *
 * One block = dense 3x3 stride-1 convolution (Cin->Cmid, bias, clip6) followed
 * by a 1x1 projection (Cmid->Cout, bias), all packed FP16. Cached entry,
 * worker 0 only, one HMX owner, only the entry's saved 8 KiB of VTCM, the
 * bounded job arena and the existing 20 ms in-kernel budget (40 ms SDK
 * timeout unchanged). Guarded fault and wrong-geometry rejection are kept.
 *
 * Per output position block (32 linear positions of the band):
 *   stage 1, for each 32-channel mid block: clear; for each K tile: copy the
 *   weight tile (DDR->VTCM), stage aligned source vectors (DDR->VTCM, four
 *   quarters of 8 vectors), pack them (valign + column masks, VTCM->VTCM),
 *   load; store into B; bias/clip/finite check in place. When Cmid > 32 the
 *   mid tiles are spilled to arena DDR (8 KiB cannot hold them).
 *   stage 2, for each 32-channel output block: clear; for each mid block load
 *   the projection weights and the mid tile; store; bias, tail mask, finite
 *   check and write planar-packed FP16 rows to arena DDR.
 * With FB_TIMING every stamp attributes the elapsed 19.2 MHz ticks since the
 * previous stamp to one phase, so the phases partition the measured body. */
#include "landmark_common.h"
typedef unsigned long long fb_u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

#if !defined(FB_GEOM) || !defined(FB_W) || !defined(FB_R) || !defined(FB_CIN) || \
    !defined(FB_NM) || !defined(FB_COUT) || !defined(FB_NBIN) || !defined(FB_CMID)
#error geometry macros are required (tools/build_hmx_block_probe.py)
#endif
#ifndef FB_BATCH
#define FB_BATCH 8
#endif
#ifndef FB_TIMING
#define FB_TIMING 1
#endif
#ifndef FB_REUSE
#define FB_REUSE 1
#endif
#ifndef FB_NOHMX
#define FB_NOHMX 0
#endif
#if FB_NOHMX
/* Timing-only control: identical data movement and post-processing with no
 * HMX instruction, so VTCM phases are measured without an HMX op in flight.
 * Its outputs are not numerically meaningful and are not validated. */
#define fnpu_hmx_clear() ((void)0)
#define fnpu_hmx_store(dst, scale) ((void)(dst), (void)(scale))
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
_Static_assert(FB_NBIN >= 1 + ((FB_R + 2) * FB_W + 31) / 32, "band blocks");
_Static_assert(FB_CMID <= FB_NM * 32 && FB_CMID + 32 > FB_NM * 32, "mid blocks");
_Static_assert(FB_NM + FB_NO <= 6 && FB_CIN % 2 == 0 && FB_COUT % 2 == 0, "slots");
_Static_assert(FB_BATCH == 1 || FB_BATCH == 8, "batch");

/* VTCM map, 64 vectors: A 0-15, B 16-31, A2 32-47, staging 48-55, scale
 * 56-57, bias 58-60 (six 16-word slots), x|position<<16 of the current block 61, flags 62, timers 63. */
#define FB_V(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define FB_A FB_V(0)
#define FB_B FB_V(16)
#define FB_A2 FB_V(32)
#define FB_S FB_V(48)
#define FB_SCALE ((volatile fnpu_u32 *)FB_V(56))
#define FB_BIAS ((const volatile fnpu_u32 *)FB_V(58))
#define FB_XP FB_V(61)
#define FB_FLAGS FB_V(62)
#define FB_M ((volatile fnpu_u32 *)FB_V(63))
enum { T_WEIGHT, T_STAGE, T_PACK, T_HMX, T_POST, T_SPILL, T_STORE, T_SETUP, M_LAST = 15, M_FAULT = 16, M_A2 = 17 };

static inline __attribute__((always_inline)) fb_u64 fb_ticks(void) {
    fb_u64 t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return t;
}
#if FB_TIMING
/* Out of line to fit the 4 KiB payload; no vector is live at any stamp. */
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

static __attribute__((noinline)) void fb_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from) {
#if FB_BATCH == 8
    for (fnpu_u32 i = 0; i < 16; i += 8) {
        HVX_Vector v0 = from[i], v1 = from[i + 1], v2 = from[i + 2], v3 = from[i + 3];
        HVX_Vector v4 = from[i + 4], v5 = from[i + 5], v6 = from[i + 6], v7 = from[i + 7];
        to[i] = v0; to[i + 1] = v1; to[i + 2] = v2; to[i + 3] = v3;
        to[i + 4] = v4; to[i + 5] = v5; to[i + 6] = v6; to[i + 7] = v7;
    }
#else
    for (fnpu_u32 i = 0; i < 16; i++) to[i] = from[i];
#endif
}

/* Four B row pairs (K rows k0..k0+7) of position block j. */
static __attribute__((noinline)) void fb_quarter(const unsigned char *src, fnpu_u32 j, fnpu_u32 k0) {
    const volatile HVX_Vector *p[4];
    fnpu_u32 shift[4], kind[4];
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 4; i++) {
        fnpu_u32 k = k0 + 2 * i, tap = k / FB_CIN, ch = k % FB_CIN;
        if (tap >= 9) { /* K padding: read the zero guard, write zeros */
            p[i] = (const volatile HVX_Vector *)src; shift[i] = 0; kind[i] = 3;
            continue;
        }
        fnpu_u32 s = (tap / 3) * FB_W + tap % 3 + 31;
        p[i] = (const volatile HVX_Vector *)(src + ((ch / 2) * FB_NBIN + j + s / 32) * 128u);
        shift[i] = (s % 32) * 4; kind[i] = tap % 3;
    }
#if FB_BATCH == 8
    HVX_Vector v0 = p[0][0], v1 = p[0][1], v2 = p[1][0], v3 = p[1][1];
    HVX_Vector v4 = p[2][0], v5 = p[2][1], v6 = p[3][0], v7 = p[3][1];
    FB_S[0] = v0; FB_S[1] = v1; FB_S[2] = v2; FB_S[3] = v3;
    FB_S[4] = v4; FB_S[5] = v5; FB_S[6] = v6; FB_S[7] = v7;
#else
    for (fnpu_u32 i = 0; i < 4; i++) { FB_S[2 * i] = p[i][0]; FB_S[2 * i + 1] = p[i][1]; }
#endif
    TS(T_STAGE);
    HVX_Vector zero = Q6_V_vzero(), ones = Q6_V_vsplat_R(-1);
    HVX_Vector x = Q6_V_vand_VV(FB_XP[0], Q6_V_vsplat_R(0xffff));
    HVX_Vector left = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, zero), zero, ones);
    HVX_Vector right = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, Q6_V_vsplat_R(FB_W - 1)), zero, ones);
    volatile HVX_Vector *b = FB_B + (k0 % 32) / 2;
    for (fnpu_u32 i = 0; i < 4; i++) {
        HVX_Vector v = Q6_V_valign_VVR(FB_S[2 * i + 1], FB_S[2 * i], shift[i]);
        if (kind[i] == 0) v = Q6_V_vand_VV(v, left);
        else if (kind[i] == 2) v = Q6_V_vand_VV(v, right);
        else if (kind[i] == 3) v = zero;
        b[i] = v;
    }
    TS(T_PACK);
}

/* Shared output processing (one function to fit the 4 KiB payload): half
 * bias via QFloat16, finite flags, zero columns past the band, then clip6 for
 * the stage-1 tile in B (in place) or plain store for the stage-2 tile in A
 * (planar-packed rows, stride NJ vectors, in arena DDR). */
static __attribute__((noinline)) void fb_post(volatile HVX_Vector *from, volatile HVX_Vector *to,
                                              fnpu_u32 stride, fnpu_u32 slot, fnpu_u32 rows) {
    HVX_Vector flags = FB_FLAGS[0];
    HVX_Vector pos = Q6_Vuw_vlsr_VuwR(FB_XP[0], 16);
    HVX_Vector tail = Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(Q6_V_vsplat_R(FB_NOUT), pos),
                                    Q6_V_vsplat_R(-1), Q6_V_vzero());
    fnpu_u32 clip = from == FB_B;
    HVX_Vector lo = Q6_V_vsplat_R(clip ? 0 : 0xfc00fc00u), hi = Q6_V_vsplat_R(clip ? 0x46004600u : 0x7c007c00u);
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

static __attribute__((noinline)) void fb_trap(const fnpu_context *c) { fault_control(c); }

static inline __attribute__((always_inline)) void fb_load(volatile HVX_Vector *a, volatile HVX_Vector *b) {
    __asm__ volatile("barrier" ::: "memory");
#if !FB_NOHMX
    fnpu_hmx_load((const fnpu_u16 *)a, (const fnpu_u16 *)b);
#else
    (void)a, (void)b;
#endif
}

/* Stage 2: 1x1 projection of the mid tiles of block j for every output block. */
static __attribute__((noinline)) void fb_project(fnpu_u32 j, volatile HVX_Vector *out,
                                                 volatile HVX_Vector *spill,
                                                 const volatile HVX_Vector *w2) {
    #pragma clang loop unroll(disable)
    for (fnpu_u32 o = 0; o < FB_NO; o++) {
        fnpu_hmx_clear();
        TS(T_HMX);
        #pragma clang loop unroll(disable)
        for (fnpu_u32 m = 0; m < FB_NM; m++) {
            if (FB_NM > 1) { fb_copy(FB_B, spill + 16 * m); TS(T_SPILL); }
            if (!(FB_REUSE && FB_NM * FB_NO == 1 && FB_M[M_A2])) {
                fb_copy(FB_A2, w2 + (o * FB_NM + m) * 16);
                FB_M[M_A2] = 1;
                TS(T_WEIGHT);
            }
            fb_load(FB_A2, FB_B);
            TS(T_HMX);
        }
        fnpu_hmx_store((fnpu_u16 *)FB_A, (const void *)FB_SCALE);
        TS(T_HMX);
        fb_post(FB_A, out + 16 * o * FB_NJ + j, FB_NJ, FB_NM + o,
                FB_OCP - 16 * o > 16 ? 16 : FB_OCP - 16 * o);
        TS(T_STORE);
    }
}

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d = fnpu_input(c);
    fnpu_u32 reps = c->params[2];
    if (!valid(c) || c->opcode != 101 || c->params[0] != 1 || c->params[1] != FB_GEOM || !reps ||
        reps > 8 || c->input_bytes != FB_IN || c->output_bytes != FB_OUT)
        return FNPU_BAD_ARGUMENT;
    /* One combined comparison keeps the header check inside the 4 KiB payload. */
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
    const volatile HVX_Vector *w1 = (const volatile HVX_Vector *)(in + FB_W1);
    const volatile HVX_Vector *w2 = (const volatile HVX_Vector *)(in + FB_W2);
    const volatile HVX_Vector *hdr = (const volatile HVX_Vector *)in;
    const volatile HVX_Vector *xt = (const volatile HVX_Vector *)(in + FB_XT);
    const unsigned char *src = in + FB_SRC;
    ((volatile HVX_Vector *)meta)[0] = Q6_V_vzero();
    ((volatile HVX_Vector *)meta)[1] = Q6_V_vzero();
    ((volatile HVX_Vector *)meta)[2] = Q6_V_vzero();
    FB_V(56)[0] = Q6_V_vsplat_R(0x3c00u); /* scale words 0..31 */
    FB_V(56)[1] = Q6_V_vzero();
    FB_V(63)[0] = Q6_V_vzero();
    for (fnpu_u32 i = 0; i < 3; i++) FB_V(58)[i] = hdr[2 + i];
    FB_FLAGS[0] = Q6_V_vzero();
    FB_M[M_FAULT] = c->params[5];
    fb_u64 start = fb_ticks();
    FB_M[M_LAST] = (fnpu_u32)start;
    fnpu_u32 done = 0;
    for (fnpu_u32 rep = 0; rep < reps; rep++) {
        #pragma clang loop unroll(disable)
        for (fnpu_u32 j = 0; j < FB_NJ; j++) {
            FB_XP[0] = xt[j]; /* host table: x | position << 16 per lane */
            TS(T_SETUP);
            #pragma clang loop unroll(disable)
            for (fnpu_u32 m = 0; m < FB_NM; m++) {
                fnpu_hmx_clear();
                TS(T_HMX);
                #pragma clang loop unroll(disable)
                for (fnpu_u32 t = 0; t < FB_NK1; t++) {
                    fb_copy(FB_A, w1 + (m * FB_NK1 + t) * 16);
                    TS(T_WEIGHT);
                    for (fnpu_u32 q = 0; q < 4; q++) fb_quarter(src, j, 32 * t + 8 * q);
                    fb_load(FB_A, FB_B);
                    TS(T_HMX);
                    if (FB_M[M_FAULT]) fb_trap(c); /* dirty accumulator */
                }
                fnpu_hmx_store((fnpu_u16 *)FB_B, (const void *)FB_SCALE);
                TS(T_HMX);
                fb_post(FB_B, FB_B, 1, m, 16);
                TS(T_POST);
                if (FB_NM > 1) { fb_copy(spill + 16 * m, FB_B); TS(T_SPILL); }
            }
            fb_project(j, out, spill, w2);
            done++;
            if ((fnpu_u32)fb_ticks() - (fnpu_u32)start > 384000u) goto finish;
        }
    }
finish:;
    fb_u64 end = fb_ticks();
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = 0x31425346u; meta[1] = pc; meta[2] = FB_GEOM; meta[3] = reps; meta[4] = done;
    meta[6] = start; meta[7] = start >> 32; meta[8] = end; meta[9] = end >> 32;
    meta[10] = FB_BATCH | FB_TIMING << 8 | FB_REUSE << 16 | FB_NOHMX << 24;
    ((volatile HVX_Vector *)meta)[1] = FB_V(63)[0]; /* phase ticks at words 32..39 */
    ((volatile HVX_Vector *)meta)[2] = FB_FLAGS[0]; /* nonzero lane = nonfinite */
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
