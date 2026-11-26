/* VTCM canary for the FeatureNN worker-0/1 scratch span 0x31002000-0x311e0000
 * (above the entry's saved 8 KiB). Static map: these are per-request
 * bump-allocator scratch slots (0x31000000 + HTID*0xf0000) of FeatureNN, which
 * the master runs only on workers 0/1/2 through the same CPU-request loop as
 * direct jobs, so it can never run concurrently with this job. Gaze (HTID 3,
 * concurrent) lives at 0x31700000+ and is not touched.
 *
 * mode 1: fill the span with an address/seed keyed pattern, then verify.
 * mode 2: verify only (persistence across jobs and time).
 * Output: per 64 KiB block mismatch word counts plus fill/verify ticks. */
#include "landmark_common.h"
typedef unsigned long long vc_u64;
#define VC_MAGIC 0x43545643u
#define VC_OPCODE 103u
#define VC_LO 0x31002000u
#define VC_HI 0x311e0000u
#define VC_BLOCK 0x10000u
#define VC_BLOCKS ((VC_HI - VC_LO + VC_BLOCK - 1) / VC_BLOCK) /* 30, last is short */
static vc_u64 vc_ticks(void) {
    vc_u64 t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return t;
}
static HVX_Vector vc_pattern(HVX_Vector lane, fnpu_u32 addr, fnpu_u32 seed) {
    HVX_Vector a = Q6_Vw_vadd_VwVw(lane, Q6_V_vsplat_R(addr));
    HVX_Vector x = Q6_V_vxor_VV(a, Q6_Vw_vasl_VwR(a, 13));
    x = Q6_V_vxor_VV(x, Q6_Vuw_vlsr_VuwR(x, 7));
    return Q6_V_vxor_VV(x, Q6_V_vsplat_R(seed));
}
fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d = fnpu_input(c);
    fnpu_u32 mode = c->params[1], seed = c->params[2];
    if (!valid(c) || c->opcode != VC_OPCODE || c->params[0] != 1 || mode < 1 || mode > 2 ||
        c->params[5] || c->input_bytes != 128 || c->output_bytes != 256 ||
        d[0] != VC_MAGIC || d[1] != seed)
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *out = fnpu_output(c);
    /* lane word i = 4*i: add 4 to every word at or above word k, k = 1..31 */
    HVX_Vector lane = Q6_V_vzero(), four = Q6_V_vsplat_R(4);
#pragma clang loop unroll(disable)
    for (fnpu_u32 k = 1; k < 32; k++)
        lane = Q6_Vw_vadd_VwVw(lane, Q6_V_vmux_QVV(Q6_Q_vsetq_R(4 * k), Q6_V_vzero(), four));
    vc_u64 t0 = vc_ticks();
    if (mode == 1) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 a = VC_LO; a < VC_HI; a += 128)
            *(HVX_Vector *)a = vc_pattern(lane, a, seed);
    }
    vc_u64 t1 = vc_ticks();
    HVX_Vector one = Q6_V_vsplat_R(1), zero = Q6_V_vzero();
    fnpu_u32 total = 0;
#pragma clang loop unroll(disable)
    for (fnpu_u32 b = 0; b < VC_BLOCKS; b++) {
        fnpu_u32 lo = VC_LO + b * VC_BLOCK, hi = lo + VC_BLOCK;
        if (hi > VC_HI) hi = VC_HI;
        HVX_Vector bad = zero;
#pragma clang loop unroll(disable)
        for (fnpu_u32 a = lo; a < hi; a += 128) {
            HVX_Vector diff = Q6_V_vxor_VV(*(const HVX_Vector *)a, vc_pattern(lane, a, seed));
            bad = Q6_Vw_vadd_VwVw(bad, Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(diff, zero), zero, one));
        }
        for (int s = 64; s >= 4; s >>= 1) bad = Q6_Vw_vadd_VwVw(bad, Q6_V_vror_VR(bad, s));
        fnpu_u32 n = Q6_R_vextract_VR(bad, 0);
        out[16 + b] = n;
        total += n;
    }
    vc_u64 t2 = vc_ticks();
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    out[0] = VC_MAGIC;
    out[1] = pc;
    out[2] = mode;
    out[3] = seed;
    out[4] = total;
    out[5] = (fnpu_u32)(t1 - t0);
    out[6] = (fnpu_u32)(t2 - t1);
    out[7] = VC_BLOCKS;
    out[8] = Q6_R_vextract_VR(lane, 4); /* expect 0 in lane 0 and 4 in lane 1 */
    out[9] = Q6_R_vextract_VR(lane, 124);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 10; i < 16; i++) out[i] = 0;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 16 + VC_BLOCKS; i < 64; i++) out[i] = 0;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
