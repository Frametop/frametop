#include "resident_graph_gather.h"
#if defined(RG_LINKED) && (RG_DW_REUSE & 2)
#include "resident_dw_masks.h"
#endif
#ifdef RG_DW_PROFILE
/* Request-local counters in unused, already saved VTCM. One component per build
 * avoids simultaneously instrumenting all boundaries and exhausting registers. */
#define DW_OBSERVE(v) do {if(RG_DW_PROFILE==3) __asm__ volatile(""::"v"(v):"memory");} while(0)
#define DW_DEPEND(k,v) do {if(RG_DW_PROFILE==k) __asm__ volatile(""::"v"(v):"memory");} while(0)
#define DW_FENCE(k,v) do {if(RG_DW_PROFILE==k) __asm__ volatile("":"+v"(v)::"memory");} while(0)
#define DW_START(k) rg_u64 dw_mark##k=0;if(RG_DW_PROFILE==k) dw_mark##k=rg_ticks()
#define DW_END(k) do {if(RG_DW_PROFILE==k) {rg_u64 dw_end=rg_ticks(); \
    *(volatile rg_u64 *)0x31001f20u+=dw_end-dw_mark##k; \
    (*(volatile fnpu_u32 *)0x31001f28u)++;}} while(0)
#else
#define DW_OBSERVE(v)
#define DW_DEPEND(k,v)
#define DW_FENCE(k,v)
#define DW_START(k)
#define DW_END(k)
#endif
#if defined(RG_LINKED) && (RG_DW_REUSE & 1)
/* Expand descending so no FP32 store overwrites an unread FP16 coefficient.
 * At most 25 words fit the existing 128-byte tap page. Reset tag per request. */
static const volatile fnpu_u32 *dw_coefficients(const fnpu_u16 *first,fnpu_u32 n) {
    volatile fnpu_u32 *tag=(volatile fnpu_u32 *)0x31001f30u;
    volatile fnpu_u32 *out=(volatile fnpu_u32 *)RG_TAP_COPY;
    if(*tag!=(fnpu_u32)first) {
        const volatile fnpu_u16 *half=rg_taps(first);
        for(fnpu_u32 i=n;i;) {--i;out[i]=half_bits(half[i]);}
        __asm__ volatile("" ::: "memory");*tag=(fnpu_u32)first;
    }
    return out;
}
#endif
#if defined(RG_LINKED) && (RG_DW_REUSE & 4)
/* Five staged rows end at 0xa00. 25 broadcast vectors fit 0xc00..0x1880,
 * below loadu's page at 0x1a00. Other operator dispatch invalidates the tag. */
#ifdef RG_DW_TILE
/* Two operators use it; a call inside a unit loop would spill vectors. */
#define DW_BROADCAST_INLINE RG_INLINE
#else
#define DW_BROADCAST_INLINE
#endif
static DW_BROADCAST_INLINE const volatile HVX_Vector *dw_broadcasts(const fnpu_u16 *first,fnpu_u32 n) {
    volatile fnpu_u32 *tag=(volatile fnpu_u32 *)0x31001f30u;
    volatile HVX_Vector *out=(volatile HVX_Vector *)0x31000c00u;
    if(*tag!=(fnpu_u32)first) {
        const volatile fnpu_u16 *half=rg_taps(first);
        for(fnpu_u32 i=0;i<n;i++) out[i]=Q6_V_vsplat_R(half_bits(half[i]));
        __asm__ volatile("" ::: "memory");*tag=(fnpu_u32)first;
    }
    return out;
}
#endif
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,3)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
    const fnpu_u16 *weight=(const fnpu_u16 *)RG_PTR(r[16]);
#ifdef RG_LINKED
    if(r[10]>5||r[11]>5) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
#endif
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 n=0;RG_MORE(n,d);n++) {
        DW_START(6);
        fnpu_u32 channel=RG_STATE[21],y=RG_STATE[22],x=RG_STATE[23];
        fnpu_u32 count=r[9]-x;if(count>32) count=32;
        HVX_Vector sum=Q6_V_vsplat_R(RG_BIAS(r[17],channel));
#if defined(RG_LINKED) && (RG_DW_REUSE & 2)
        HVX_VectorPred bad_pred=Q6_Q_vsetq_R(0);
#else
        HVX_Vector bad=Q6_V_vzero();
#endif
        DW_START(1);
#if defined(RG_LINKED) && (RG_DW_REUSE & 4)
        const volatile HVX_Vector *broadcasts=dw_broadcasts(weight+channel*r[10]*r[11],r[10]*r[11]);
#elif defined(RG_LINKED) && (RG_DW_REUSE & 1)
        const volatile fnpu_u32 *converted=dw_coefficients(weight+channel*r[10]*r[11],r[10]*r[11]);
#else
        RG_TAPS(weight,channel,r);
#endif
        RG_STAGE(r,channel,y,x);
        DW_END(1);
        for(fnpu_u32 ky=0;ky<r[10];ky++) for(fnpu_u32 kx=0;kx<r[11];kx++) {
            DW_START(2);
            HVX_Vector input=rg_gather(r,channel,y,x,ky,kx,count RG_STAGED);
            DW_FENCE(2,input);
            DW_END(2);
            DW_START(3);
            DW_OBSERVE(input);DW_OBSERVE(sum);
#if !defined(RG_LINKED) || !(RG_DW_REUSE & 2)
            DW_OBSERVE(bad);
#endif
            DW_START(12);DW_END(12);
            DW_START(7);
#if defined(RG_LINKED) && (RG_DW_REUSE & 4)
            HVX_Vector coefficient=broadcasts[ky*r[11]+kx];
#elif defined(RG_LINKED) && (RG_DW_REUSE & 1)
            HVX_Vector coefficient=Q6_V_vsplat_R(converted[ky*r[11]+kx]);
#else
            HVX_Vector coefficient=Q6_V_vsplat_R(half_bits(RG_TAP(weight,channel,ky,kx,r)));
#endif
            DW_DEPEND(7,coefficient);DW_END(7);
            DW_START(8);
            HVX_Vector product=mulf(input,coefficient);
            DW_DEPEND(8,product);DW_END(8);
            DW_START(9);
#if defined(RG_LINKED) && (RG_DW_REUSE & 2)
            sum=dw_add_checked(sum,product,&bad_pred);
#else
            bad=Q6_V_vor_VV(bad,nonfinite(product));
            DW_DEPEND(9,bad);DW_END(9);
#if defined(RG_DW_PROFILE) && RG_DW_PROFILE>=7
            DW_START(10);
            HVX_Vector zero=Q6_V_vzero();
            HVX_Vector next=Q6_Vsf_equals_Vqf32(Q6_Vqf32_vadd_VsfVsf(sum,product));
            DW_DEPEND(10,next);DW_END(10);
            DW_START(11);
            next=Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(product,1),zero),sum,next);
            sum=Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VwVw(Q6_Vw_vasl_VwR(sum,1),zero),product,next);
            DW_DEPEND(11,sum);DW_END(11);
#else
            sum=addf(sum,product);
#endif
            DW_OBSERVE(sum);DW_OBSERVE(bad);
#endif
            DW_END(3);
        }
        DW_START(4);
        DW_FENCE(4,sum);
#if defined(RG_LINKED) && (RG_DW_REUSE & 2)
        HVX_Vector bad=Q6_V_vmux_QVV(bad_pred,Q6_V_vsplat_R(-1),Q6_V_vzero());
#endif
        bad=Q6_V_vor_VV(bad,nonfinite(sum));rg_bad(bad);
        if(r[18]) sum=clip6(sum);
        rg_store((float *)RG_PTR(r[3]+(channel*r[20]+y*r[9]+x)*4),count,sum);
        DW_END(4);
        DW_START(5);
        RG_STATE[20]++;RG_STATE[30]++;rg_next_row(r);
        rg_fault(c);
        DW_END(5);
        DW_END(6);
        if(RG_STATE[20]==r[24]) {rg_done(r);break;}
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
