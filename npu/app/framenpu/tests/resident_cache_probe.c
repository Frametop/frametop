/* Stateless code-alias/replacement/fault control in the borrowed job arena. */
#include "fnpu_readback.h"
#ifndef CACHE_PROBE_TAG
#define CACHE_PROBE_TAG 0
#endif
typedef unsigned long long cache_u64;
static cache_u64 cache_ticks(void) {
    cache_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;
}
fnpu_u32 frame_job(const fnpu_context *c) {
    if(c->magic!=FNPU_MAGIC||c->abi!=FNPU_ABI||c->flags!=3||c->opcode!=94||
       c->input_offset||c->input_bytes!=128||c->output_offset!=128||c->output_bytes!=256||
       c->arena_bytes!=384||c->params[0]>1||c->params[5]>1) return FNPU_BAD_ARGUMENT;
    for(fnpu_u32 i=1;i<5;i++) if(c->params[i]) return FNPU_BAD_ARGUMENT;
    cache_u64 start=cache_ticks();
    volatile fnpu_u32 *meta=(volatile fnpu_u32 *)fnpu_output(c)+32;
    for(fnpu_u32 i=0;i<32;i++) meta[i]=0;
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    meta[0]=0x43414331u;meta[1]=pc;meta[2]=CACHE_PROBE_TAG;
    meta[3]=start;meta[4]=start>>32;
    *(volatile HVX_Vector *)fnpu_output(c)=Q6_V_vxor_VV(
        *(const volatile HVX_Vector *)fnpu_input(c),Q6_V_vsplat_R(0xa5a5a5a5u+CACHE_PROBE_TAG));
    if(c->params[5]) {
        meta[7]=1;
        __asm__ volatile("barrier\n\ttrap0(#37)" ::: "memory");
        meta[7]=2;
    }
    cache_u64 end=cache_ticks();meta[5]=end;meta[6]=end>>32;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
