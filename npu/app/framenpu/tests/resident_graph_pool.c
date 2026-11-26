#include "resident_graph_gather.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,5)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    if(r[23]||r[10]!=2||r[11]!=2||r[12]!=2||r[13]!=2) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 n=0;RG_MORE(n,d);n++) {
        fnpu_u32 channel=RG_STATE[21],y=RG_STATE[22],x=RG_STATE[23];
        fnpu_u32 count=r[9]-x;if(count>32) count=32;
        HVX_Vector value=rg_gather(r,channel,y,x,0,0,count RG_UNSTAGED),bad=nonfinite(value);
        for(fnpu_u32 ky=0;ky<2;ky++) for(fnpu_u32 kx=0;kx<2;kx++) {
            HVX_Vector v=rg_gather(r,channel,y,x,ky,kx,count RG_UNSTAGED);
            bad=Q6_V_vor_VV(bad,nonfinite(v));value=Q6_Vsf_vmax_VsfVsf(value,v);
        }
        rg_bad(bad);rg_store((float *)RG_PTR(r[3]+(channel*r[20]+y*r[9]+x)*4),count,value);
        RG_STATE[20]++;RG_STATE[30]++;rg_next_row(r);
        rg_fault(c);
        if(RG_STATE[20]==r[24]) {rg_done(r);break;}
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
