/* Finite FP16 pooling and stride-2 stitching. Each word holds two channels.
 * Unaligned vector accesses stay within the existing source/output planes. */
#ifndef TEACHER_STEM_VECTOR_H
#define TEACHER_STEM_VECTOR_H
static inline __attribute__((always_inline)) HVX_Vector tsv_key(HVX_Vector a) {
    HVX_Vector zero = Q6_V_vzero();
    HVX_Vector mag = Q6_V_vand_VV(a,Q6_V_vsplat_R(0x7fff7fff));
    /* Signed integer order of finite halves; both signed zeros map to zero. */
    return Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VhVh(zero,a),Q6_Vh_vsub_VhVh(zero,mag),mag);
}
static inline __attribute__((always_inline)) HVX_Vector tsv_max(HVX_Vector a, HVX_Vector b) {
    return Q6_V_vmux_QVV(Q6_Q_vcmp_gt_VhVh(tsv_key(a),tsv_key(b)),a,b);
}
static inline __attribute__((always_inline)) HVX_VectorPair tsv_deal(const volatile fnpu_u32 *p) {
    HVX_Vector a = *(const volatile rs_uv *)p;
    HVX_Vector b = *(const volatile rs_uv *)(p+32);
    return Q6_W_vdeal_VVR(b,a,-4);
}
static inline __attribute__((always_inline)) void tsv_store(volatile fnpu_u32 *p,
        HVX_Vector v, fnpu_u32 x) {
    /* The final vector has 24 live words. Preserve the eight following words,
     * including the two-column padding and any already written next row. */
    if (x) {
        fnpu_u32 n=96;
        /* Materialize after the pooling predicates. Otherwise LLVM hoists
         * this predicate across the loop and spills it to the scalar stack. */
        __asm__ volatile("" : "+r"(n));
        v = Q6_V_vmux_QVV(Q6_Q_vsetq_R(n),v,*(const volatile rs_uv *)p);
    }
    *(volatile rs_uv *)p = v;
}
static __attribute__((noinline)) void tsv_skip(fnpu_u32 band) {
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)FR(TS_A);
    volatile fnpu_u32 *dst = (volatile fnpu_u32 *)FR(TS_OUT10);
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp=0;cp<8;cp++)
        for (fnpu_u32 y=0;y<8;y++)
            for (fnpu_u32 x=0;x<56;x+=32) {
                fnpu_u32 i=cp*2848+258+(2*y+1)*114+2*x;
                HVX_VectorPair a=tsv_deal(src+i), b=tsv_deal(src+i+114);
                HVX_Vector top=tsv_max(Q6_V_lo_W(a),Q6_V_hi_W(a));
                HVX_Vector bot=tsv_max(Q6_V_lo_W(b),Q6_V_hi_W(b));
                tsv_store(dst+cp*3616+130+(8*band+y)*58+x,tsv_max(top,bot),x);
            }
}
static __attribute__((noinline)) void tsv_downsample(fnpu_u32 band) {
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)FR(TS_A);
    volatile fnpu_u32 *dst = (volatile fnpu_u32 *)FR(TS_OUT8);
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp=0;cp<32;cp++)
        for (fnpu_u32 y=0;y<8;y++)
            for (fnpu_u32 x=0;x<56;x+=32) {
                fnpu_u32 i=cp*2848+259+(2*y+2)*114+2*x;
                tsv_store(dst+cp*3616+130+(8*band+y)*58+x,Q6_V_lo_W(tsv_deal(src+i)),x);
            }
}
#endif
