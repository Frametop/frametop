/* Read-only VTCM survey. Worker 0, cached entry; the only stores are to this
 * job's own output in the job arena. For each 4 KiB chunk of the 8 MiB VTCM
 * window (0x31000000-0x317fffff) it returns four folded 32-bit words: lane
 * word sum, rotate-xor hash, OR, and nonzero-word count. Repeated over time,
 * these separate never-touched, constant (e.g. model weights) and live
 * (gaze / FeatureNN scratch) regions without writing any VTCM. */
#include "landmark_common.h"
typedef unsigned long long vs_u64;
#define VS_MAGIC 0x53565456u
#define VS_OPCODE 102u
#define VS_BASE 0x31000000u
#define VS_CHUNKS 2048u
#define VS_CHUNK_VECTORS 32u /* 4 KiB */
#define VS_META 256u
#define VS_DUMP_VECTORS 512u /* 64 KiB raw window, params[1]=1, params[2]=offset */
static vs_u64 vs_ticks(void) {
    vs_u64 t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return t;
}
static fnpu_u32 vs_fold_add(HVX_Vector v) {
    for (int s = 64; s >= 4; s >>= 1) v = Q6_Vw_vadd_VwVw(v, Q6_V_vror_VR(v, s));
    return Q6_R_vextract_VR(v, 0);
}
static fnpu_u32 vs_fold_xor(HVX_Vector v) {
    for (int s = 64; s >= 4; s >>= 1) v = Q6_V_vxor_VV(v, Q6_V_vror_VR(v, s));
    return Q6_R_vextract_VR(v, 0);
}
static fnpu_u32 vs_fold_or(HVX_Vector v) {
    for (int s = 64; s >= 4; s >>= 1) v = Q6_V_vor_VV(v, Q6_V_vror_VR(v, s));
    return Q6_R_vextract_VR(v, 0);
}
fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d = fnpu_input(c);
    fnpu_u32 dump = c->params[1], offset = c->params[2];
    if (!valid(c) || c->opcode != VS_OPCODE || c->params[0] != 1 || dump > 1 ||
        (offset & 0xffffu) || offset > 0x7f0000u || (!dump && offset) || c->params[5] ||
        c->input_bytes != 128 ||
        c->output_bytes != VS_META + (dump ? VS_DUMP_VECTORS * 128u : VS_CHUNKS * 16u) ||
        d[0] != VS_MAGIC)
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
    if (dump) {
        const HVX_Vector *src = (const HVX_Vector *)(VS_BASE + offset);
        volatile HVX_Vector *dst = (volatile HVX_Vector *)(meta + VS_META / 4);
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < VS_DUMP_VECTORS; i++) dst[i] = src[i];
        meta[0] = VS_MAGIC;
        meta[2] = offset;
        __asm__ volatile("barrier" ::: "memory");
        return FNPU_OK;
    }
    volatile fnpu_u32 *out = meta + VS_META / 4;
    HVX_Vector one = Q6_V_vsplat_R(1), zero = Q6_V_vzero();
    vs_u64 start = vs_ticks();
#pragma clang loop unroll(disable)
    for (fnpu_u32 chunk = 0; chunk < VS_CHUNKS; chunk++) {
        const HVX_Vector *v = (const HVX_Vector *)(VS_BASE + chunk * VS_CHUNK_VECTORS * 128u);
        HVX_Vector sum = zero, hash = zero, any = zero, count = zero;
#pragma clang loop unroll(disable)
        for (fnpu_u32 i = 0; i < VS_CHUNK_VECTORS; i++) {
            HVX_Vector x = v[i];
            sum = Q6_Vw_vadd_VwVw(sum, x);
            hash = Q6_V_vxor_VV(Q6_V_vror_VR(hash, 4), Q6_Vw_vadd_VwVw(x, Q6_V_vsplat_R(i)));
            any = Q6_V_vor_VV(any, x);
            count = Q6_Vw_vadd_VwVw(count,
                                    Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(x, zero), zero, one));
        }
        out[4 * chunk] = vs_fold_add(sum);
        out[4 * chunk + 1] = vs_fold_xor(hash);
        out[4 * chunk + 2] = vs_fold_or(any);
        out[4 * chunk + 3] = vs_fold_add(count);
    }
    vs_u64 end = vs_ticks();
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    meta[0] = VS_MAGIC;
    meta[1] = pc;
    meta[2] = VS_CHUNKS;
    meta[3] = (fnpu_u32)start;
    meta[4] = (fnpu_u32)(start >> 32);
    meta[5] = (fnpu_u32)end;
    meta[6] = (fnpu_u32)(end >> 32);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 7; i < VS_META / 4; i++) meta[i] = 0;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
