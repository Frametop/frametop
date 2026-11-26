#include "resident_graph.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,11)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    if(r[23]!=2||r[19]!=49||r[8]!=1||r[9]!=1) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 n=0;RG_MORE(n,d);n++) {
        fnpu_u32 first=RG_STATE[20]*32,count=r[7]-first;if(count>32) count=32;
        HVX_Vector sum=Q6_V_vzero(),bad=Q6_V_vzero();
        volatile float *tmp=(volatile float *)0x31001e00u;
        const float *in=(const float *)RG_PTR(r[1]);
#ifdef RG_LINKED
        /* Transpose the unit into VTCM: cell[t][i] is channel first+i, tap t.
         * Two vectors cover a channel's 49 floats inside the owned arena. */
        volatile float *cell=(volatile float *)0x31000000u;
        const volatile float *whole=(const volatile float *)RG_PARK;
        for(fnpu_u32 i=0;i<count;i++) {
            RG_PARK[0]=rg_loadu(in+(first+i)*49);
            RG_PARK[1]=rg_loadu(in+(first+i)*49+32);
            for(fnpu_u32 t=0;t<49;t++) cell[t*32+i]=whole[t];
        }
        (void)tmp;
#endif
        for(fnpu_u32 t=0;t<49;t++) {
#ifdef RG_LINKED
            HVX_Vector v=*(const volatile HVX_Vector *)(cell+t*32);
            if(count<32) v=Q6_V_vmux_QVV(Q6_Q_vsetq_R(count*4),v,Q6_V_vzero());
#else
            for(fnpu_u32 i=0;i<32;i++) tmp[i]=i<count?in[(first+i)*49+t]:0.f;
            HVX_Vector v=*(volatile HVX_Vector *)tmp;
#endif
            bad=Q6_V_vor_VV(bad,nonfinite(v));sum=addf(sum,v);
        }
        union {float f;fnpu_u32 u;} scale;scale.f=1.f/49.f;
        sum=mulf(sum,Q6_V_vsplat_R(scale.u));bad=Q6_V_vor_VV(bad,nonfinite(sum));rg_bad(bad);
        rg_store((float *)RG_PTR(r[3]+first*4),count,sum);
        RG_STATE[20]++;RG_STATE[30]++;rg_fault(c);
        if(RG_STATE[20]==r[24]) {rg_done(r);break;}
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
