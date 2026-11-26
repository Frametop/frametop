#include "resident_graph.h"
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
    if(!rg_run(c,6)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);
    if(r[7]!=1||r[8]!=1||r[9]!=1||r[18]!=2||RG_STATE[24]!=2) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    rg_accept(c);
    volatile float *out=(volatile float *)RG_PTR(r[3]);
    union {float f;fnpu_u32 u;} value;value.f=*out;
    if((value.u&0x7f800000)==0x7f800000) RG_STATE[29]=~0u;
    *out=sigmoid(value.f);RG_ONE();
    rg_fault(c);rg_done(r);
    return RG_RETURN(c,FNPU_OK);
}
