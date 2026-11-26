/* Two one-value confidence heads. Range-reduced exp with degree-7 Taylor;
 * bounded |x|<=80, saturation outside, no libc/libm dependency. */
#include "landmark_common.h"
static float sigmoid(float x) {
    if(x==0.f) return 0.5f;
    if(x>80.f) return 1.f;
    if(x< -80.f) return 0.f;
    float y=-x*1.4426950408889634f;
    int n=(int)y;
    if(y<(float)n) n--;
    float r=(-x)-(float)n*0.6931471805599453f;
    float p=1.f+r*(1.f+r*(0.5f+r*(0.1666666666666667f+r*(0.0416666666666667f+
            r*(0.0083333333333333f+r*(0.0013888888888889f+r*0.0001984126984127f))))));
    union {fnpu_u32 u;float f;} power;
    power.u=(fnpu_u32)(n+127)<<23;
    union {fnpu_u32 u;float f;} denominator, reciprocal;
    denominator.f=1.f+p*power.f;
    reciprocal.u=0x7ef311c3u-denominator.u;
    for(int i=0;i<4;i++) reciprocal.f*=2.f-denominator.f*reciprocal.f;
    return reciprocal.f;
}
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!valid(c) || c->opcode!=25 || c->params[0]!=1 || c->params[1] || c->params[2] ||
       c->input_bytes!=128 || c->output_bytes!=256) return FNPU_BAD_ARGUMENT;
    fault_control(c);
    const volatile float *in=fnpu_input(c);
    volatile float *out=fnpu_output(c);
    for(fnpu_u32 i=0;i<32;i++) out[i]=sigmoid(in[i]);
    ((volatile HVX_Vector *)out)[1]=nonfinite(*(const volatile HVX_Vector *)in);
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
