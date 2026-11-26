#ifndef FNPU_RESIDENT_GRAPH_GATHER_H
#define FNPU_RESIDENT_GRAPH_GATHER_H
#include "resident_graph.h"
/* Loads may cross a row/channel only inside the owned input tensor. Both
 * leading padding and trailing/count lanes are masked before arithmetic. */
#ifdef RG_LINKED
/* Copy every in-range kernel row of one unit with aligned loads: at most
 * three blocks cover 32 outputs plus four taps at stride one, four at stride
 * two. rg_gather then reads the same floats from VTCM through RG_SOURCE. */
static void rg_stage(rg_record *r,fnpu_u32 channel,fnpu_u32 y,fnpu_u32 x) {
    int ix=(int)x*(int)r[13]-(int)r[15];
    fnpu_u32 span=r[13]*128u+(r[11]-1u)*4u+127u;
    for(fnpu_u32 ky=0;ky<r[10];ky++) {
        int iy=(int)y*(int)r[12]-(int)r[14]+(int)ky;
        if(iy<0||iy>=(int)r[5]) continue;
        int base=(int)(channel*r[19])+iy*(int)r[6]+ix;
        fnpu_u32 first=(fnpu_u32)((const float *)RG_PTR(r[1])+base);
        const volatile HVX_Vector *from=(const volatile HVX_Vector *)(first&~127u);
        volatile HVX_Vector *to=(volatile HVX_Vector *)(RG_ROWS+ky*512u);
        fnpu_u32 blocks=((first&127u)+span)>>7;
        for(fnpu_u32 b=0;b<blocks;b++) to[b]=from[b];
    }
    __asm__ volatile("" ::: "memory");
}
#endif
static RG_INLINE HVX_Vector rg_gather(rg_record *r,fnpu_u32 channel,
                            fnpu_u32 y,fnpu_u32 x,fnpu_u32 ky,fnpu_u32 kx,fnpu_u32 count RG_STAGED_PARAM) {
    int iy=(int)y*(int)r[12]-(int)r[14]+(int)ky;
    int ix=(int)x*(int)r[13]-(int)r[15]+(int)kx;
    if(iy<0||iy>=(int)r[5]||ix>=(int)r[6]) return Q6_V_vzero();
    fnpu_u32 stride=r[13],shift=stride-1;
    /* The pinned DW/pool loader proves stride is one or two. */
    fnpu_u32 first=ix<0?((fnpu_u32)-ix+shift)>>shift:0;
    fnpu_u32 last=((fnpu_u32)((int)r[6]-ix)+shift)>>shift;
    if(last>count) last=count;
    const float *in=(const float *)RG_PTR(r[1]);
    int base=(int)(channel*r[19])+iy*(int)r[6]+ix;
    if(base>=0&&(fnpu_u32)base+32*stride<=r[4]*r[19]) {
        const float *from=RG_SOURCE(in+base,ky,kx);
        HVX_Vector value=*(const volatile HVX_UVector *)from;
        if(stride==2) value=Q6_V_lo_W(Q6_W_vdeal_VVR(*(const volatile HVX_UVector *)(from+32),value,-4));
        /* vsetq(128) wraps to zero; a full trailing mask is unnecessary. */
        if(last!=32) value=Q6_V_vmux_QVV(Q6_Q_vsetq_R(last*4),value,Q6_V_vzero());
        return Q6_V_vmux_QVV(Q6_Q_vsetq_R(first*4),Q6_V_vzero(),value);
    }
    volatile float *tmp=(volatile float *)0x31001e00u;
    *(volatile HVX_Vector *)tmp=Q6_V_vzero();
    for(fnpu_u32 i=first;i<last;i++) tmp[i]=in[base+(int)(i*stride)];
    return *(volatile HVX_Vector *)tmp;
}
static void rg_next_row(rg_record *r) {
    fnpu_u32 x=RG_STATE[23]+32,y=RG_STATE[22],ch=RG_STATE[21];
    if(x>=r[9]) {x=0;if(++y==r[8]) {y=0;ch++;}}
    RG_STATE[21]=ch;RG_STATE[22]=y;RG_STATE[23]=x;
}
static fnpu_u32 half_bits(fnpu_u32 h) {
    /* The pinned loader proves all depthwise coefficients normal or zero. */
    fnpu_u32 sign=(h&0x8000)<<16,mag=h&0x7fff;
    return sign|(mag?(mag<<13)+0x38000000u:0);
}
#endif
