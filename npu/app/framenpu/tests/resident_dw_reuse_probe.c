/* Same arithmetic in the original and reused-mask helpers; raw classifications
 * also exercise every IEEE category without executing FP arithmetic on NaNs. */
#include "resident_dw_masks.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!valid(c)||c->opcode!=97||c->input_bytes!=384||c->output_bytes!=512||
       c->arena_bytes!=896||c->params[0]!=1||c->params[1]||c->params[2]||c->params[5])
        return FNPU_BAD_ARGUMENT;
    const volatile HVX_Vector *in=fnpu_input(c);
    HVX_Vector a=in[0],b=in[1],raw=in[2];
    HVX_VectorPred bad=Q6_Q_vsetq_R(0);
    HVX_Vector candidate=dw_add_checked(a,b,&bad),reference=addf(a,b);
    bad=Q6_Q_vsetq_R(0);
    HVX_VectorPred z=dw_product_masks(raw,&bad);
    HVX_Vector other=Q6_V_vxor_VV(raw,Q6_V_vsplat_R(-1));
    (void)dw_product_masks(other,&bad);
    HVX_Vector yes=Q6_V_vsplat_R(-1),zero=Q6_V_vzero();
    HVX_Vector nf=Q6_V_vmux_QVV(bad,yes,zero),zm=Q6_V_vmux_QVV(z,yes,zero);
    HVX_Vector difference=Q6_V_vor_VV(Q6_V_vxor_VV(candidate,reference),
        Q6_V_vor_VV(Q6_V_vxor_VV(nf,Q6_V_vor_VV(nonfinite(raw),nonfinite(other))),
        Q6_V_vxor_VV(zm,Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(raw,1),zero),yes,zero))));
    volatile fnpu_u32 *tmp=(volatile fnpu_u32 *)0x31001000u;
    *(volatile HVX_Vector *)tmp=difference;
    fnpu_u32 flags=0;for(fnpu_u32 i=0;i<32;i++) flags|=tmp[i];
    volatile fnpu_u32 *meta=fnpu_output(c);
    for(fnpu_u32 i=0;i<32;i++) meta[i]=0;
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    meta[0]=0x44575231u;meta[1]=pc;meta[2]=flags;
    *(volatile HVX_Vector *)(meta+32)=candidate;
    *(volatile HVX_Vector *)(meta+64)=nf;
    *(volatile HVX_Vector *)(meta+96)=zm;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
