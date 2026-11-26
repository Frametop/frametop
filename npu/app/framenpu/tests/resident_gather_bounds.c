/* Shape/boundary/bit-preservation and timing probe, borrowed job arena only. */
#include "resident_graph_gather.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!valid(c)||c->opcode!=93||c->input_bytes<128||c->output_bytes<256) return FNPU_BAD_ARGUMENT;
    for(fnpu_u32 i=0;i<6;i++) if(c->params[i]) return FNPU_BAD_ARGUMENT;
    const volatile fnpu_u32 *d=fnpu_input(c);
    for(fnpu_u32 i=5;i<32;i++) if(d[i]) return FNPU_BAD_ARGUMENT;
    fnpu_u32 data=(d[0]*d[1]*d[2]*4+127)&~127u;
    if(d[0]-1>2||d[1]-1>111||d[2]-1>111||d[3]-1>31||d[4]-1>15||d[3]*d[4]>32||
       c->input_bytes!=128+data+d[3]*64||c->output_bytes!=128+d[3]*128) return FNPU_BAD_ARGUMENT;
    const volatile fnpu_u32 *cases=d+32+data/4;
    for(fnpu_u32 n=0;n<d[3];n++) {
        const volatile fnpu_u32 *a=cases+n*16;
        if(a[0]-1>1||a[1]-1>1||a[2]>2||a[3]>2||a[4]>=d[0]||a[5]>112||a[6]>112||
           a[7]>4||a[8]>4||a[9]-1>31) return FNPU_BAD_ARGUMENT;
        for(fnpu_u32 i=10;i<16;i++) if(a[i]) return FNPU_BAD_ARGUMENT;
    }
    fnpu_u32 r[32];
    r[1]=(fnpu_u32)fnpu_input(c)+128-0xf0000000u;
    r[4]=d[0];r[5]=d[1];r[6]=d[2];r[19]=d[1]*d[2];
    rg_u64 start=rg_ticks();
    for(fnpu_u32 n=0;n<d[3];n++) {
        const volatile fnpu_u32 *a=cases+n*16;
        r[12]=a[0];r[13]=a[1];r[14]=a[2];r[15]=a[3];
        for(fnpu_u32 i=0;i<d[4];i++)
            ((volatile HVX_Vector *)fnpu_output(c))[n]=rg_gather(r,a[4],a[5],a[6],a[7],a[8],a[9]);
    }
    rg_u64 end=rg_ticks();
    volatile fnpu_u32 *out=(volatile fnpu_u32 *)fnpu_output(c)+d[3]*32;
    for(fnpu_u32 i=0;i<32;i++) out[i]=d[i];
    out[16]=(fnpu_u32)start;out[17]=(fnpu_u32)(start>>32);
    out[18]=(fnpu_u32)end;out[19]=(fnpu_u32)(end>>32);
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");out[20]=pc;
    out[21]=0x47415432u;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
