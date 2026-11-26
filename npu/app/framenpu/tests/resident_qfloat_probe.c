/* Register-only dependent arithmetic slopes. All writes are in the borrowed
 * job arena; the cache entry saves/restores the existing 8 KiB of VTCM. */
#include "landmark_common.h"
typedef unsigned long long qf_u64;
static qf_u64 qf_ticks(void) {
    qf_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;
}
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 kind=c->params[1],n=c->params[2];
    if(!valid(c)||c->opcode!=96||c->input_offset||c->input_bytes!=256||
       c->output_offset!=256||c->output_bytes!=256||c->arena_bytes!=512||
       c->params[0]!=1||kind>4||!n||n>16384||c->params[3]||c->params[4]||c->params[5])
        return FNPU_BAD_ARGUMENT;
    const volatile HVX_Vector *in=fnpu_input(c);
    HVX_Vector a=in[0],b=in[1],acc=a,bad=Q6_V_vzero();
    volatile fnpu_u32 *meta=fnpu_output(c);
    for(fnpu_u32 j=0;j<32;j++) meta[j]=0;
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    fnpu_u32 done=0; qf_u64 start=qf_ticks();
#define QF_LOOP(code) \
    for(fnpu_u32 i=0;i<n;) { \
        fnpu_u32 stop=n-i>256u?i+256u:n; \
        for(;i<stop;i++) {code;} \
        done=i;if(qf_ticks()-start>384000u) break; \
    }
    if(kind==0) QF_LOOP(__asm__ volatile("nop" ::: "memory"))
    if(kind==1) QF_LOOP(acc=mulf(acc,b))
    if(kind==2) QF_LOOP(acc=Q6_Vsf_equals_Vqf32(Q6_Vqf32_vadd_VsfVsf(acc,b)))
    if(kind==3) QF_LOOP(acc=addf(acc,b))
    if(kind==4) QF_LOOP(
        HVX_Vector x=Q6_V_vxor_VV(a,Q6_V_vsplat_R((i&1u)*0x80000000u));
        HVX_Vector product=mulf(x,b);
        bad=Q6_V_vor_VV(bad,nonfinite(product));acc=addf(acc,product))
    /* Pin the final vector dependency before the end timestamp. */
    __asm__ volatile(""::"v"(acc),"v"(bad):"memory");
    qf_u64 end=qf_ticks();
    *(volatile HVX_Vector *)(meta+32)=acc;
    meta[0]=0x51465331u;meta[1]=pc;meta[2]=kind;meta[3]=done;
    meta[4]=start;meta[5]=start>>32;meta[6]=end;meta[7]=end>>32;
    meta[8]=n;__asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
