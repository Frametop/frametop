#ifndef FNPU_RESIDENT_DW_MASKS_H
#define FNPU_RESIDENT_DW_MASKS_H
#include "landmark_common.h"
/* One magnitude serves both classifications. Predicates are word-replicated;
 * ORing them preserves every NaN/Inf flag until the unit's final reduction. */
static inline __attribute__((always_inline)) HVX_VectorPred dw_product_masks(
    HVX_Vector product,HVX_VectorPred *bad) {
    HVX_Vector mag=Q6_V_vand_VV(product,Q6_V_vsplat_R(0x7fffffffu));
    *bad=Q6_Q_or_QQ(*bad,Q6_Q_vcmp_gt_VuwVuw(mag,Q6_V_vsplat_R(0x7f7fffffu)));
    return Q6_Q_vcmp_eq_VwVw(mag,Q6_V_vzero());
}
static inline __attribute__((always_inline)) HVX_Vector dw_add_checked(
    HVX_Vector sum,HVX_Vector product,HVX_VectorPred *bad) {
    HVX_VectorPred pzero=dw_product_masks(product,bad);
    HVX_Vector next=Q6_Vsf_equals_Vqf32(Q6_Vqf32_vadd_VsfVsf(sum,product));
    next=Q6_V_vmux_QVV(pzero,sum,next);
    return Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(sum,1),Q6_V_vzero()),product,next);
}
#endif
