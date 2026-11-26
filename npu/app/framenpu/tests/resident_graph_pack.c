#include "resident_graph.h"
#include "resident_fp16.h"
#ifndef RG_STEM
#define RG_STEM 0
#endif
#ifdef RG_CONV_FAST
/* Expanded in place: a call between the two gathers would put the first
 * vector on the job stack. */
#define PACK_GATHER static RG_INLINE
#else
#define PACK_GATHER static
#endif
PACK_GATHER HVX_Vector gather(rg_record *r,fnpu_u32 channel,fnpu_u32 count) {
    if(!RG_STEM) {
        if(channel>=r[4]) return Q6_V_vzero();
        return rg_load((const float *)RG_PTR(r[1]+(channel*r[19]+RG_STATE[22])*4),count);
    }
    if(channel>=27) return Q6_V_vzero();
    fnpu_u32 c=channel/9,ky=(channel%9)/3,kx=channel%3;
    fnpu_u32 y=RG_STATE[23]*2+ky,x=RG_STATE[22]*2+kx;
    const float *in=(const float *)RG_PTR(r[1]);
#ifdef RG_LINKED
    /* Same lanes as the scalar loop below, by two loads and a deal. */
    if(y>=224) return Q6_V_vzero();
    fnpu_u32 lanes=x<224?(225-x)>>1:0;
    if(lanes>count) lanes=count;
    const float *row=in+(c*224+y)*224+x;
    HVX_Vector low=rg_loadu(row);
    HVX_Vector even=Q6_V_lo_W(Q6_W_vdeal_VVR(rg_loadu(row+32),low,-4));
    if(lanes<32) even=Q6_V_vmux_QVV(Q6_Q_vsetq_R(lanes*4),even,Q6_V_vzero());
    return even;
#endif
    volatile float *tmp=(volatile float *)0x31001e00u;
    for(fnpu_u32 i=0;i<32;i++) tmp[i]=i<count&&y<224&&x+i*2<224?in[(c*224+y)*224+x+i*2]:0.f;
    return *(volatile HVX_Vector *)tmp;
}
#undef PACK_GATHER
#ifdef RG_CONV_FAST
/* One unit's vector work. Kept out of the unit loop so that no vector
 * constant is hoisted there and held on the job stack across the two
 * conversion calls; the operands of those calls wait in VTCM. */
static __attribute__((noinline)) void pack_unit(rg_record *r,fnpu_u32 pair,fnpu_u32 count) {
    HVX_Vector low=gather(r,pair*2,count),high=gather(r,pair*2+1,count);
    HVX_Vector v=half_pair(low,high);
    ((volatile HVX_Vector *)RG_PTR(RG_SCRATCH))[pair]=v;
    HVX_Vector exp=Q6_V_vsplat_R(0x7c007c00);
    /* All-ones halfwords where the exponent is all ones; built from the
     * predicate so that no zero vector is kept across the calls above. */
    HVX_Vector mask=Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v,exp),exp),-1);
    mask=Q6_V_vor_VV(mask,Q6_V_vor_VV(Q6_Vw_vasl_VwR(mask,16),Q6_Vuw_vlsr_VuwR(mask,16)));
    rg_flags(mask);
}
#endif
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,RG_STEM?10:9)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    if(r[0]!=(RG_STEM?2u:1u)||r[19]!=r[5]*r[6]||!r[26]||r[26]>352||RG_STATE[18]>=r[26]||
       (RG_STEM&&(r[4]!=3||r[5]!=224||r[6]!=224))) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    fnpu_u32 remaining=RG_STEM?r[9]-RG_STATE[22]:r[8]*r[9]-RG_STATE[22];
    fnpu_u32 count=remaining<32?remaining:32;
    if(!count) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 i=0;RG_MORE(i,d);i++) {
        fnpu_u32 pair=RG_STATE[18];
#ifdef RG_CONV_FAST
        pack_unit(r,pair,count);
#else
        HVX_Vector low=gather(r,pair*2,count),high=gather(r,pair*2+1,count);
        HVX_Vector v=half_pair(low,high);
        ((volatile HVX_Vector *)RG_PTR(RG_SCRATCH))[pair]=v;
        HVX_Vector exp=Q6_V_vsplat_R(0x7c007c00);
        HVX_Vector mask=Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v,exp),exp),Q6_V_vsplat_R(-1),Q6_V_vzero());
        mask=Q6_V_vor_VV(mask,Q6_V_vor_VV(Q6_Vw_vasl_VwR(mask,16),Q6_Vuw_vlsr_VuwR(mask,16)));
        rg_bad(mask);
#endif
        RG_STATE[18]++;RG_STATE[30]++;
        rg_fault(c);
        if(RG_STATE[18]==r[26]) {RG_STATE[24]=1;break;}
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
