/* Isolated cached-code HVX arithmetic. Inputs/output stay in the borrowed job
 * arena; only the entry's saved/restored 8 KiB VTCM is used. No resident data.
 * 64 distinct lanes; kinds: QFloat control, IEEE FP32, mixed FP16/FP32, FP16.
 * Raw vector-pair ordering is characterized by the host before model work. */
#include "landmark_common.h"
typedef unsigned long long ip_u64;
static ip_u64 ticks(void) {ip_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;}
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 kind=c->params[1],n=c->params[2];
    if(!valid(c)||c->opcode!=98||c->input_bytes!=1152||c->output_bytes!=512||
       kind>7||!n||n>4096) return FNPU_BAD_ARGUMENT;
    const volatile HVX_Vector *in=fnpu_input(c);
    volatile HVX_Vector *out=fnpu_output(c);
    volatile fnpu_u32 *meta=(volatile fnpu_u32 *)out;
    for(fnpu_u32 i=0;i<4;i++) out[i]=Q6_V_vzero();
    HVX_Vector a=in[0],b=in[1],x=in[2],y=in[3],w=in[4],z=in[5];
    HVX_Vector hx=in[6],hw=in[7],hs=in[8];
    HVX_VectorPair pair=Q6_W_vcombine_VV(b,a);
    HVX_Vector qa=Q6_Vqf32_vadd_VsfVsf(a,Q6_V_vzero());
    HVX_Vector qb=Q6_Vqf32_vadd_VsfVsf(b,Q6_V_vzero());
    HVX_VectorPair qp=Q6_W_vcombine_VV(qb,qa);
    ip_u64 start=ticks();fnpu_u32 done=0;
    for(;done<n;) {
        fnpu_u32 end=n-done>32?done+32:n;
        for(;done<end;done++) {
            /* Prevent invariant multiply hoisting in this arithmetic probe.
             * The observation emits no instruction and leaves operand bits. */
            __asm__ volatile("":"+v"(x),"+v"(y),"+v"(w),"+v"(z),"+v"(hx),"+v"(hw));
            if(kind==0) {a=addf(a,mulf(x,w));b=addf(b,mulf(y,z));}
            if(kind==1) {a=Q6_Vsf_vadd_VsfVsf(a,Q6_Vsf_vmpy_VsfVsf(x,w));b=Q6_Vsf_vadd_VsfVsf(b,Q6_Vsf_vmpy_VsfVsf(y,z));}
            if(kind==2) pair=Q6_Wsf_vmpyacc_WsfVhfVhf(pair,hx,hw);
            if(kind==3) hs=Q6_Vhf_vmpyacc_VhfVhfVhf(hs,hx,hw);
            if(kind==4) {
                qa=Q6_Vqf32_vadd_Vqf32Vqf32(qa,Q6_Vqf32_vmpy_VsfVsf(x,w));
                qb=Q6_Vqf32_vadd_Vqf32Vqf32(qb,Q6_Vqf32_vmpy_VsfVsf(y,z));
            }
            if(kind==5) {
                HVX_VectorPair product=Q6_Wqf32_vmpy_VhfVhf(hx,hw);
                qp=Q6_W_vcombine_VV(
                    Q6_Vqf32_vadd_Vqf32Vqf32(Q6_V_hi_W(qp),Q6_V_hi_W(product)),
                    Q6_Vqf32_vadd_Vqf32Vqf32(Q6_V_lo_W(qp),Q6_V_lo_W(product)));
            }
        }
        if(ticks()-start>384000u) break;
    }
    ip_u64 end=ticks();
    if(kind==7) {out[2]=hx;out[3]=hw;}
    else if(kind==5) {out[2]=Q6_Vsf_equals_Vqf32(Q6_V_lo_W(qp));out[3]=Q6_Vsf_equals_Vqf32(Q6_V_hi_W(qp));}
    else if(kind==4) {out[2]=Q6_Vsf_equals_Vqf32(qa);out[3]=Q6_Vsf_equals_Vqf32(qb);}
    else if(kind==2) {out[2]=Q6_V_lo_W(pair);out[3]=Q6_V_hi_W(pair);}
    else if(kind==3) out[2]=hs;
    else {out[2]=a;out[3]=b;}
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    meta[0]=0x49454531u;meta[1]=pc;meta[2]=kind;meta[3]=done;
    meta[4]=start;meta[5]=start>>32;meta[6]=end;meta[7]=end>>32;
    meta[8]=n;meta[9]=0xdecafbad;
    __asm__ volatile("barrier" ::: "memory");fault_control(c);return FNPU_OK;
}
