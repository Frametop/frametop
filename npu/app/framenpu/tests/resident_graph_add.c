#include "resident_graph.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,4)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    fnpu_u32 count=r[7]*r[8]*r[9];
    if(!rg_span(r[2],r[21])) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 i=0;RG_MORE(i,d);i++) {
        fnpu_u32 at=RG_STATE[20]*32,len=count-at;if(len>32) len=32;
        HVX_Vector a=rg_load((const float *)RG_PTR(r[1]+at*4),len);
        HVX_Vector b=rg_load((const float *)RG_PTR(r[2]+at*4),len);
        HVX_Vector v=addf(a,b);rg_bad(v);
        rg_store((float *)RG_PTR(r[3]+at*4),len,v);
        RG_STATE[20]++;RG_STATE[30]++;
        rg_fault(c);
        if(RG_STATE[20]==r[24]) {rg_done(r);break;}
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
