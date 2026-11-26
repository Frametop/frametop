/* Single-thread HVX issue-rate microbenchmark (worker 0). Each mode runs
 * N iterations of 8 independent operations and reports core cycles
 * (upcycle) and 19.2 MHz ticks, so the cost of one operation class can be
 * read directly. Mode (params[1]):
 *   0 vadd.w register chains        1 aligned VTCM loads (+ xor)
 *   2 valign register chains        3 QFloat16 multiply + add chains
 *   4 aligned VTCM stores           5 empty loop (overhead)
 * Only the saved 8 KiB and the proven FeatureNN scratch span are touched
 * (loads from 0x31002000.., stores to 0x31002000..+8 KiB). */
#include "landmark_common.h"

#define RT_MAGIC 0x31544852u /* "RHT1" */
#define RT_OPCODE 107u
#define RT_META 1024u
#define FR(i) ((volatile HVX_Vector *)(0x31002000u + 128u * (i)))
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))

static inline __attribute__((always_inline)) fnpu_u32 rt_cycles(void) {
    unsigned long long t;
    __asm__ volatile("%0 = upcycle" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

static __attribute__((noinline)) void rt_run(fnpu_u32 mode, fnpu_u32 n) {
    HVX_Vector a0 = LV(32)[0], a1 = LV(33)[0], a2 = LV(34)[0], a3 = LV(35)[0];
    HVX_Vector a4 = LV(36)[0], a5 = LV(37)[0], a6 = LV(38)[0], a7 = LV(39)[0];
    HVX_Vector k = Q6_V_vsplat_R(0x3c003c00u);
    const HVX_Vector *src = (const HVX_Vector *)FR(0);
    HVX_Vector *dst = (HVX_Vector *)FR(0);
    if (mode == 0) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) {
            a0 = Q6_Vw_vadd_VwVw(a0, k); a1 = Q6_Vw_vadd_VwVw(a1, k);
            a2 = Q6_Vw_vadd_VwVw(a2, k); a3 = Q6_Vw_vadd_VwVw(a3, k);
            a4 = Q6_Vw_vadd_VwVw(a4, k); a5 = Q6_Vw_vadd_VwVw(a5, k);
            a6 = Q6_Vw_vadd_VwVw(a6, k); a7 = Q6_Vw_vadd_VwVw(a7, k);
        }
    } else if (mode == 1) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) {
            const HVX_Vector *p = src + 8 * (i & 63);
            a0 = Q6_V_vxor_VV(a0, p[0]); a1 = Q6_V_vxor_VV(a1, p[1]);
            a2 = Q6_V_vxor_VV(a2, p[2]); a3 = Q6_V_vxor_VV(a3, p[3]);
            a4 = Q6_V_vxor_VV(a4, p[4]); a5 = Q6_V_vxor_VV(a5, p[5]);
            a6 = Q6_V_vxor_VV(a6, p[6]); a7 = Q6_V_vxor_VV(a7, p[7]);
        }
    } else if (mode == 2) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) {
            a0 = Q6_V_valign_VVR(a0, k, 4); a1 = Q6_V_valign_VVR(a1, k, 8);
            a2 = Q6_V_valign_VVR(a2, k, 12); a3 = Q6_V_valign_VVR(a3, k, 16);
            a4 = Q6_V_valign_VVR(a4, k, 20); a5 = Q6_V_valign_VVR(a5, k, 24);
            a6 = Q6_V_valign_VVR(a6, k, 28); a7 = Q6_V_valign_VVR(a7, k, 32);
        }
    } else if (mode == 3) {
        HVX_Vector q0 = Q6_Vqf16_vadd_VhfVhf(k, k), q1 = q0, q2 = q0, q3 = q0;
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) {
            q0 = Q6_Vqf16_vadd_Vqf16Vqf16(q0, Q6_Vqf16_vmpy_VhfVhf(a0, k));
            q1 = Q6_Vqf16_vadd_Vqf16Vqf16(q1, Q6_Vqf16_vmpy_VhfVhf(a1, k));
            q2 = Q6_Vqf16_vadd_Vqf16Vqf16(q2, Q6_Vqf16_vmpy_VhfVhf(a2, k));
            q3 = Q6_Vqf16_vadd_Vqf16Vqf16(q3, Q6_Vqf16_vmpy_VhfVhf(a3, k));
        }
        a0 = q0; a1 = q1; a2 = q2; a3 = q3;
    } else if (mode == 4) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) {
            HVX_Vector *p = dst + 8 * (i & 7);
            p[0] = a0; p[1] = a1; p[2] = a2; p[3] = a3;
            p[4] = a4; p[5] = a5; p[6] = a6; p[7] = a7;
        }
    } else {
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < n; i++) __asm__ volatile("" ::: "memory");
    }
    LV(32)[0] = a0; LV(33)[0] = a1; LV(34)[0] = a2; LV(35)[0] = a3;
    LV(36)[0] = a4; LV(37)[0] = a5; LV(38)[0] = a6; LV(39)[0] = a7;
}

fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 mode = c->params[1], n = c->params[2];
    if (!valid(c) || c->opcode != RT_OPCODE || c->params[0] != 1 || mode > 5 || n - 1 > 65535 ||
        c->input_bytes != 128 || c->output_bytes != RT_META)
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    fault_control(c);
    unsigned long long t0, t1;
    __asm__ volatile("%0 = c31:30" : "=r"(t0)::"memory");
    fnpu_u32 y0 = rt_cycles();
    rt_run(mode, n);
    fnpu_u32 y1 = rt_cycles();
    __asm__ volatile("%0 = c31:30" : "=r"(t1)::"memory");
    meta[0] = RT_MAGIC; meta[1] = mode; meta[2] = n;
    meta[3] = y1 - y0; meta[4] = (fnpu_u32)(t1 - t0);
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
