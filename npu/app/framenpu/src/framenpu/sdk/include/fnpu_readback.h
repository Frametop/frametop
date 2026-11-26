#ifndef FNPU_READBACK_H
#define FNPU_READBACK_H
#include "framenpu.h"
#include <hexagon_types.h>
#include <hexagon_protos.h>

/* Convert the low halfword of each word lane to exact FP32 bits. Integer
   operations retain signed zeros and NaN payloads without enabling HVX FP
   arithmetic or changing the scalar accumulator's rounding behavior. */
static inline HVX_Vector half_words_to_float(HVX_Vector h) {
    HVX_Vector zero = Q6_V_vzero();
    HVX_Vector fraction = Q6_V_vand_VV(h, Q6_V_vsplat_R(1023));
    HVX_Vector exponent = Q6_V_vand_VV(h, Q6_V_vsplat_R(0x7c00));
    HVX_Vector magnitude = Q6_Vw_vasl_VwR(Q6_V_vand_VV(h, Q6_V_vsplat_R(0x7fff)), 13);
    HVX_Vector normal = Q6_Vw_vadd_VwVw(magnitude, Q6_V_vsplat_R(0x38000000));
    HVX_Vector shift = Q6_Vw_vsub_VwVw(Q6_Vw_vnormamt_Vw(fraction), Q6_V_vsplat_R(20));
    HVX_Vector denormal = Q6_V_vor_VV(
        Q6_Vw_vasl_VwR(Q6_Vw_vsub_VwVw(Q6_V_vsplat_R(113), shift), 23),
        Q6_Vw_vasl_VwR(Q6_V_vand_VV(Q6_Vw_vasl_VwVw(fraction, shift), Q6_V_vsplat_R(1023)), 13));
    denormal = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(fraction, zero), zero, denormal);
    HVX_Vector bits = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(exponent, zero), denormal, normal);
    bits = Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(exponent, Q6_V_vsplat_R(0x7c00)),
                         Q6_V_vor_VV(magnitude, Q6_V_vsplat_R(0x7f800000)), bits);
    return Q6_V_vor_VV(bits, Q6_Vw_vasl_VwR(Q6_V_vand_VV(h, Q6_V_vsplat_R(0x8000)), 16));
}

/* HMX stores each row pair as 32 words: the even row is in low halfwords,
   the odd row in high halfwords. Both conversion and lane order are exact. */
static inline HVX_Vector read_row(const volatile fnpu_u16 *src, fnpu_u32 r) {
    HVX_Vector pair = ((const volatile HVX_Vector *)src)[r/2];
    return half_words_to_float(Q6_Vuw_vlsr_VuwR(pair, (r&1)*16));
}

/* Keep vector constants local to readback, rather than live across HMX calls
   in gemm (which would force large vector spills onto the worker stack). */
static __attribute__((noinline)) void read_tile(const volatile fnpu_u16 *src, float *dst,
                      fnpu_u32 rows, fnpu_u32 cols, fnpu_u32 stride) {
    /* After HMX store/barrier, use 128 bytes immediately after the scale
       table, inside the existing saved/restored 8 KiB VTCM span. */
    volatile float *row = (volatile float *)0x31001900u;
    for (fnpu_u32 r=0;r<rows;r++) {
        *(volatile HVX_Vector *)row = read_row(src, r);
        for (fnpu_u32 c=0;c<cols;c++) dst[r*stride+c] += row[c];
    }
}
#endif
