/* Lane-wise FP32 multiply/add depthwise, residual, max/average pooling.
 * Host lowers bounded patches; separate rounding follows strict scalar ncnn.
 * No activation is allowed to hide a nonfinite intermediate. */
#include "landmark_common.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 vectors=c->params[0], taps=c->params[1], clip=c->params[2], op=c->opcode;
    if(!valid(c) || !vectors || vectors>256 || clip>1 ||
       c->output_bytes!=(vectors+1)*128) return FNPU_BAD_ARGUMENT;
    fnpu_u32 expected;
    if(op==21) {
        if(!taps || taps>25 || vectors>16) return FNPU_BAD_ARGUMENT;
        expected=(2*taps+1)*vectors*128;
    } else if(op==22) {
        if(taps || clip) return FNPU_BAD_ARGUMENT;
        expected=2*vectors*128;
    } else if(op==23 || op==24) {
        if(!taps || taps>49 || clip || vectors>64) return FNPU_BAD_ARGUMENT;
        expected=(taps+(op==24))*vectors*128;
    } else return FNPU_UNSUPPORTED;
    if(c->input_bytes!=expected) return FNPU_BAD_ARGUMENT;
    fault_control(c);
    const volatile HVX_Vector *in=fnpu_input(c);
    volatile HVX_Vector *out=fnpu_output(c);
    HVX_Vector bad=Q6_V_vzero();
    for(fnpu_u32 v=0;v<vectors;v++) {
        HVX_Vector sum=op==21?in[2*taps*vectors+v]:Q6_V_vzero();
        if(op==22) sum=addf(in[v],in[vectors+v]);
        else for(fnpu_u32 t=0;t<taps;t++) {
            HVX_Vector value=in[t*vectors+v];
            bad=Q6_V_vor_VV(bad,nonfinite(value));
            if(op==21) {
                value=mulf(value,in[(taps+t)*vectors+v]);
                bad=Q6_V_vor_VV(bad,nonfinite(value));
            }
            if(op==23) sum=t?Q6_Vsf_vmax_VsfVsf(sum,value):value;
            else sum=addf(sum,value);
        }
        if(op==24) sum=mulf(sum,in[taps*vectors+v]);
        bad=Q6_V_vor_VV(bad,nonfinite(sum));
        out[v]=clip?clip6(sum):sum;
    }
    out[vectors]=bad;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
