#include "resident_graph.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,8)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    if(RG_STATE[18]>=r[7]||r[17]<RG_WEIGHTS||r[17]>RG_WEIGHTS+RG_WEIGHT_BYTES||r[7]*4>RG_WEIGHTS+RG_WEIGHT_BYTES-r[17]) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    fnpu_u32 p=RG_STATE[22],y=RG_STATE[23];
    fnpu_u32 rem=r[0]==2?r[9]-p:r[8]*r[9]-p,count=rem<32?rem:32;
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 n=0;RG_MORE(n,d);n++) {
        fnpu_u32 channel=RG_STATE[18];
        float *out=(float *)RG_PTR(r[3]+(channel*r[20]+(r[0]==2?y*r[9]:0)+p)*4);
        HVX_Vector value=rg_load(out,count);
        fnpu_u32 bias=RG_BIAS(r[17],channel);
        value=addf(value,Q6_V_vsplat_R(bias));
        rg_bad(value);
        if(r[18]==1) value=clip6(value);
        rg_store(out,count,value);
        RG_STATE[18]++;RG_STATE[30]++;
        rg_fault(c);
        if(RG_STATE[18]==r[7]) {
            RG_STATE[18]=0;
            if(RG_STATE[20]==r[24]) {
                if(r[18]==2) {RG_STATE[24]=2;RG_STATE[20]=RG_STATE[22]=RG_STATE[23]=0;}
                else rg_done(r);
            } else {
                p+=32;
                if(r[0]==2&&p>=r[9]) {p=0;y++;}
                RG_STATE[22]=p;RG_STATE[23]=y;RG_STATE[24]=0;
            }
            break;
        }
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
