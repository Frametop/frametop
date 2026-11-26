/* Bounded uncached job-arena I/O only. No resident/cache aliases are used. */
#ifndef FNPU_LANDMARK_COMMON_H
#define FNPU_LANDMARK_COMMON_H
#include "fnpu_readback.h"
static int valid(const fnpu_context *c) {
    return c->magic==FNPU_MAGIC && c->abi==FNPU_ABI &&
        c->flags==(FNPU_FLAG_HMX|FNPU_FLAG_DIRECT) && !c->input_offset &&
        c->arena_bytes<=FNPU_ARENA_SIZE && c->input_bytes<=FNPU_ARENA_SIZE &&
        c->output_offset==((c->input_bytes+127)&~127u) &&
        c->output_offset+c->output_bytes==c->arena_bytes &&
        !c->params[3] && !c->params[4] && c->params[5]<=1;
}
static HVX_Vector addf(HVX_Vector a, HVX_Vector b) {
    HVX_Vector zero=Q6_V_vzero();
    HVX_Vector sum=Q6_Vsf_equals_Vqf32(Q6_Vqf32_vadd_VsfVsf(a,b));
    /* qf32 conversion can move even x+0 or 0+x by one FP32 ULP. Preserve
     * these identities exactly, including bias-only and padded HMX rows. */
    sum=Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(b,1),zero),a,sum);
    return Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(a,1),zero),b,sum);
}
static HVX_Vector mulf(HVX_Vector a, HVX_Vector b) {
    return Q6_Vsf_equals_Vqf32(Q6_Vqf32_vmpy_VsfVsf(a,b));
}
static HVX_Vector nonfinite(HVX_Vector a) {
    HVX_Vector exp=Q6_V_vsplat_R(0x7f800000);
    return Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_V_vand_VV(a,exp),exp),
                         Q6_V_vsplat_R(-1),Q6_V_vzero());
}
static HVX_Vector clip6(HVX_Vector a) {
    return Q6_Vsf_vmin_VsfVsf(Q6_Vsf_vmax_VsfVsf(a,Q6_V_vzero()),Q6_V_vsplat_R(0x40c00000));
}
static void fault_control(const fnpu_context *c) {
    if(c->params[5]) {
        *(volatile HVX_Vector *)fnpu_output(c)=Q6_V_vsplat_R(0xdecafbad);
        __asm__ volatile("barrier\n\ttrap0(#37)" ::: "memory");
        *(volatile HVX_Vector *)fnpu_output(c)=Q6_V_vsplat_R(0xbad);
    }
}
#endif
