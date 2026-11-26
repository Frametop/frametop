#include "resident_graph.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,7)||RG_STATE[24]!=1) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    const volatile fnpu_u32 *d=fnpu_input(c);
        RG_RECORD(r);
        if(r[23]!=1||r[3]!=r[1]||r[19]!=12544||r[20]!=12544||r[8]!=56||r[9]!=56||r[7]!=64) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);rg_u64 start=rg_ticks();
        for(fnpu_u32 n=0;RG_MORE(n,d);n++) {
            fnpu_u32 channel=RG_STATE[21],vec=RG_STATE[22];
            const volatile HVX_Vector *src=(const volatile HVX_Vector *)RG_PTR(r[3]+channel*50176+vec*128);
            volatile HVX_Vector *dst=(volatile HVX_Vector *)RG_PTR(r[3]+channel*12544+vec*128);
            *dst=*src;
            RG_STATE[20]++;RG_STATE[30]++;
            if(++vec==98) {vec=0;channel++;}
            RG_STATE[21]=channel;RG_STATE[22]=vec;
            rg_fault(c);
            if(RG_STATE[20]==r[25]) {rg_done(r);break;}
            if(RG_EXPIRED(start)) break;
        }

    return RG_RETURN(c,FNPU_OK);
}
