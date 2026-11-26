/* Integer FP32 -> FP16 round-to-nearest/even. The native sf->hf instruction
 * in current LLVM headers requires a later architecture than v75. */
#ifndef FNPU_RESIDENT_FP16_H
#define FNPU_RESIDENT_FP16_H
#include "fnpu_readback.h"
static __attribute__((noinline)) HVX_Vector half_words(HVX_Vector bits) {
    HVX_Vector zero=Q6_V_vzero(),one=Q6_V_vsplat_R(1);
    HVX_Vector mag=Q6_V_vand_VV(bits,Q6_V_vsplat_R(0x7fffffff));
    HVX_Vector exp=Q6_Vuw_vlsr_VuwR(mag,23);
    HVX_Vector lsb=Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(mag,13),one);
    HVX_Vector normal=Q6_Vw_vsub_VwVw(Q6_Vuw_vlsr_VuwR(
        Q6_Vw_vadd_VwVw(mag,Q6_Vw_vadd_VwVw(Q6_V_vsplat_R(4095),lsb)),13),Q6_V_vsplat_R(0x1c000));
    HVX_Vector sh=Q6_Vw_vsub_VwVw(Q6_V_vsplat_R(126),exp);
    HVX_Vector sig=Q6_V_vor_VV(Q6_V_vand_VV(mag,Q6_V_vsplat_R(0x7fffff)),Q6_V_vsplat_R(0x800000));
    HVX_Vector truncated=Q6_Vw_vasr_VwVw(sig,sh);
    HVX_Vector rounding=Q6_Vw_vadd_VwVw(Q6_Vw_vsub_VwVw(
        Q6_Vw_vasl_VwVw(one,Q6_Vw_vsub_VwVw(sh,one)),one),Q6_V_vand_VV(truncated,one));
    HVX_Vector sub=Q6_Vw_vasr_VwVw(Q6_Vw_vadd_VwVw(sig,rounding),sh);
    sub=Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(exp,Q6_V_vsplat_R(101)),sub,zero);
    HVX_Vector h=Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(exp,Q6_V_vsplat_R(112)),normal,sub);
    h=Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VwVw(exp,Q6_V_vsplat_R(142)),Q6_V_vsplat_R(0x7c00),h);
    HVX_Vector nan=Q6_V_vor_VV(Q6_V_vsplat_R(0x7e00),Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(mag,13),Q6_V_vsplat_R(0x3ff)));
    h=Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VuwVuw(mag,Q6_V_vsplat_R(0x7f800000)),nan,h);
    return Q6_V_vor_VV(h,Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(bits,16),Q6_V_vsplat_R(0x8000)));
}
#ifdef RG_LINKED
/* Same two conversions; the operands wait in VTCM instead of on the stack. */
static HVX_Vector half_pair(HVX_Vector low,HVX_Vector high) {
    RG_PARK[0]=high;
    RG_PARK[1]=half_words(low);
    HVX_Vector b=half_words(RG_PARK[0]);
    return Q6_V_vor_VV(RG_PARK[1],Q6_Vw_vasl_VwR(b,16));
}
#else
static HVX_Vector half_pair(HVX_Vector low,HVX_Vector high) {
    HVX_Vector a=half_words(low),b=half_words(high);
    return Q6_V_vor_VV(a,Q6_Vw_vasl_VwR(b,16));
}
#endif
#endif
