/* Memory-access cost probe (cache-v1 entry, cached instructions).
 * One request times N repetitions of one primitive; the host fits the slope
 * between two repeat counts. Owned resident DDR is only read: through the
 * uncached alias, and through the cached alias with every touched line
 * discarded before return (no line there is ever written, so none is dirty).
 * Writes go to the borrowed, host-restored job arena or the saved VTCM page. */
#include "landmark_common.h"
typedef unsigned long long ap_u64;
#define AP_DDR 0x1c1b000u
#define AP_WINDOW 0x10000u
#define AP_VTCM 0x31001000u
/* Five images keep each inside the 4 KiB slot: reads (0), stores (1),
 * depthwise-tap components (2), the job stack and VTCM round trips (3) and
 * per-unit bookkeeping (4). */
#ifndef AP_SET
#define AP_SET 0
#endif
#define AP_HAS(k) ((k)==0u||(AP_SET==4?(k)>=34u:AP_SET==3?(k)>=23u&&(k)<34u:AP_SET==2?(k)>=15u&&(k)<23u:AP_SET==1?(k)>=10u&&(k)<15u:(k)<10u))
#if AP_SET==3
/* A leaf and a caller that needs a frame: the return reloads LR:FP. */
static __attribute__((noinline)) fnpu_u32 ap_leaf(fnpu_u32 x) {
    __asm__ volatile("" ::: "memory");return x+1;
}
static __attribute__((noinline)) fnpu_u32 ap_framed(fnpu_u32 x) {
    return ap_leaf(x)+ap_leaf(x+2);
}
#endif
static ap_u64 ap_ticks(void) {
    ap_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;
}
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 kind=c->params[1],n=c->params[2];
    if(c->magic!=FNPU_MAGIC||c->abi!=FNPU_ABI||c->flags!=3||c->opcode!=95||
       c->input_offset||c->input_bytes!=128||c->output_offset!=128||c->output_bytes!=4352||
       c->arena_bytes!=4480||c->params[0]!=1||kind>40||!AP_HAS(kind)||!n||n>16384||
       c->params[3]||c->params[4]||c->params[5]) return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta=(volatile fnpu_u32 *)fnpu_output(c);
    unsigned char *body=(unsigned char *)fnpu_output(c)+256;
    const unsigned char *raw=(const unsigned char *)(0xf0000000u+AP_DDR);
    const unsigned char *hot=(const unsigned char *)(0x87000000u+AP_DDR);
    unsigned char *tcm=(unsigned char *)AP_VTCM;
    for(fnpu_u32 i=0;i<64;i++) meta[i]=0;
    fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    fnpu_u32 sum=0;HVX_Vector acc=Q6_V_vzero();
    HVX_Vector zero=Q6_V_vzero(),one=Q6_V_vsplat_R(0x3f800000u),bad=zero;
    if(AP_SET==0&&(kind==3||kind==7)) for(fnpu_u32 i=0;i<32;i++) ((volatile HVX_Vector *)tcm)[i]=Q6_V_vsplat_R(i+1);
    if(AP_SET==0&&(kind==4||kind==8)) for(fnpu_u32 i=0;i<4096;i+=32) sum+=*(const volatile fnpu_u32 *)(hot+i);
    /* Whole chunks only; a slow primitive ends early instead of nearing the
     * 40 ms completion window (384000 ticks = 20 ms). */
#define AP_LOOP(statement) \
    for(fnpu_u32 i=0;i<n;) { \
        fnpu_u32 stop=n-i>256u?i+256u:n; \
        for(;i<stop;i++) {fnpu_u32 at=(i*128u)&4095u;(void)at;statement;} \
        if(ap_ticks()-start>384000u) {done=i;early=1;break;} \
    }
    fnpu_u32 done=n,early=0;
    if(kind==9&&n>AP_WINDOW/128) return FNPU_BAD_ARGUMENT;
    ap_u64 start=ap_ticks();
    if(AP_HAS(0u)&&kind==0) AP_LOOP(__asm__ volatile("nop" ::: "memory"))
    if(AP_HAS(1u)&&kind==1) AP_LOOP(sum+=*(const volatile fnpu_u32 *)(raw+at))
    if(AP_HAS(2u)&&kind==2) AP_LOOP(sum+=*(const volatile fnpu_u32 *)(raw+(i&31u)*4u))
    if(AP_HAS(3u)&&kind==3) AP_LOOP(sum+=*(const volatile fnpu_u32 *)(tcm+at))
    if(AP_HAS(4u)&&kind==4) AP_LOOP(sum+=*(const volatile fnpu_u32 *)(hot+at))
    if(AP_HAS(5u)&&kind==5) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_Vector *)(raw+at)))
    if(AP_HAS(6u)&&kind==6) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(raw+at+4)))
    if(AP_HAS(7u)&&kind==7) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_Vector *)(tcm+at)))
    if(AP_HAS(8u)&&kind==8) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_Vector *)(hot+at)))
    /* Every load touches four cache lines not read since the last discard. */
    if(AP_HAS(9u)&&kind==9) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_Vector *)(hot+i*128u)))
    if(AP_HAS(10u)&&kind==10) AP_LOOP(*(volatile fnpu_u32 *)(body+at)=i)
    if(AP_HAS(11u)&&kind==11) AP_LOOP(*(volatile HVX_Vector *)(body+at)=acc)
    if(AP_HAS(12u)&&kind==12) AP_LOOP(*(volatile HVX_UVector *)(body+(at&2047u)+4)=acc)
    if(AP_HAS(13u)&&kind==13) AP_LOOP(*(volatile HVX_Vector *)(tcm+at)=acc)
    if(AP_HAS(14u)&&kind==14) AP_LOOP(*(volatile fnpu_u32 *)(tcm+at)=i)
    /* The depthwise tap arithmetic on registers only. */
    if(AP_HAS(15u)&&kind==15) AP_LOOP(
        HVX_Vector product=mulf(one,Q6_V_vsplat_R(0x3f000000u+i));
        bad=Q6_V_vor_VV(bad,nonfinite(product));acc=addf(acc,product))
    /* Overlapping unaligned loads four bytes apart, as adjacent kernel taps. */
    if(AP_HAS(16u)&&kind==16) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(raw+((i*4u)&1023u))))
    /* Unaligned loads spread over 3 MiB of the owned activation region. */
    if(AP_HAS(17u)&&kind==17) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(raw+4u+(i&1023u)*3072u)))
    /* Load feeding the arithmetic, as one complete stride-one tap. */
    if(AP_HAS(18u)&&kind==18) AP_LOOP(
        HVX_Vector product=mulf(*(const volatile HVX_UVector *)(raw+((i*4u)&1023u)),one);
        bad=Q6_V_vor_VV(bad,nonfinite(product));acc=addf(acc,product))
    /* The same arithmetic on a vector reloaded from VTCM. */
    if(AP_HAS(19u)&&kind==19) AP_LOOP(
        HVX_Vector product=mulf(*(const volatile HVX_Vector *)tcm,one);
        bad=Q6_V_vor_VV(bad,nonfinite(product));acc=addf(acc,product))
    /* Unaligned stores and VTCM loads at the same 4-byte steps. */
    if(AP_HAS(20u)&&kind==20) AP_LOOP(*(volatile HVX_UVector *)(body+((i*4u)&1023u))=acc)
    if(AP_HAS(21u)&&kind==21) AP_LOOP(acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(tcm+((i*4u)&1023u))))
    /* A vector spilled to and reloaded from the job stack. */
    if(AP_HAS(22u)&&kind==22) {
        volatile HVX_Vector slot;
        AP_LOOP(slot=acc;acc=Q6_V_vxor_VV(acc,slot))
    }
#if AP_SET==3
    /* Scalars on the job stack, and a call through a framed function. */
    volatile fnpu_u32 word=1;
    if(kind==23) AP_LOOP(sum+=word)
    if(kind==24) AP_LOOP(word=i)
    if(kind==25) AP_LOOP(sum+=ap_framed(i))
    meta[12]=(fnpu_u32)&word;
    /* A VTCM vector store followed by a read of the stored bytes. */
    volatile HVX_Vector *slot=(volatile HVX_Vector *)tcm;
    if(kind==26) AP_LOOP(slot[0]=acc;acc=Q6_V_vxor_VV(acc,slot[0]))
    if(kind==27) AP_LOOP(slot[0]=acc;slot[1]=acc;acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(tcm+4)))
    if(kind==28) AP_LOOP(slot[0]=acc;sum+=*(const volatile fnpu_u32 *)tcm)
    /* The same store followed by a load of other VTCM bytes. */
    if(kind==29) AP_LOOP(slot[0]=acc;acc=Q6_V_vxor_VV(acc,slot[8]))
    /* One lane to a scalar register without memory. */
    if(kind==30) AP_LOOP(sum+=(fnpu_u32)Q6_R_vextract_VR(acc,0))
    /* An unaligned DDR span as two aligned loads joined in registers. */
    if(kind==31) AP_LOOP(
        const volatile HVX_Vector *from=(const volatile HVX_Vector *)(raw+at);
        acc=Q6_V_vxor_VV(acc,Q6_V_valign_VVR(from[1],from[0],4)))
    /* The current helper: the same two loads through a VTCM pair. */
    if(kind==32) AP_LOOP(
        const volatile HVX_Vector *from=(const volatile HVX_Vector *)(raw+at);
        slot[0]=from[0];slot[1]=from[1];
        acc=Q6_V_vxor_VV(acc,*(const volatile HVX_UVector *)(tcm+4)))
    /* A VTCM store, then a load of it after unrelated register work. */
    if(kind==33) AP_LOOP(
        slot[0]=acc;
        for(fnpu_u32 j=0;j<8;j++) {
            HVX_Vector product=mulf(one,Q6_V_vsplat_R(0x3f000000u+j));
            bad=Q6_V_vor_VV(bad,nonfinite(product));
        }
        acc=Q6_V_vxor_VV(acc,slot[0]))
#endif
#if AP_SET==4
    volatile HVX_Vector *slot=(volatile HVX_Vector *)tcm;
    /* The reference-tick read every work unit makes for its budget. */
    if(kind==34) AP_LOOP(sum+=(fnpu_u32)ap_ticks())
    /* Count-masked store as the operators do it: VTCM vector, 17 scalar copies. */
    if(kind==35) AP_LOOP(
        slot[0]=acc;
        for(fnpu_u32 j=0;j<17;j++) ((volatile fnpu_u32 *)(body+at))[j]=((const volatile fnpu_u32 *)tcm)[j])
    /* The same lanes as one masked vector store. */
    if(kind==36) AP_LOOP(Q6_vmem_QRIV(Q6_Q_vsetq_R(68),(HVX_Vector *)(body+at),acc))
    /* A scalar VTCM write followed by a vector load of that vector, as the
     * per-unit state flush does; then with the DDR store; then scalar only. */
    if(kind==37) AP_LOOP(*(volatile fnpu_u32 *)(tcm+8)=i;acc=Q6_V_vxor_VV(acc,slot[0]))
    if(kind==38) AP_LOOP(*(volatile fnpu_u32 *)(tcm+8)=i;*(volatile HVX_Vector *)body=slot[0])
    if(kind==39) AP_LOOP(*(volatile fnpu_u32 *)(tcm+8)=i;sum+=*(const volatile fnpu_u32 *)(tcm+8))
    /* The flush from registers alone: no VTCM copy in between. */
    if(kind==40) AP_LOOP(*(volatile HVX_Vector *)body=Q6_V_vsplat_R(i))
#endif
    acc=Q6_V_vor_VV(acc,bad);
    ap_u64 end=ap_ticks();
    if(AP_SET==0&&(kind==4||kind==8||kind==9)) {
        for(fnpu_u32 off=0;off<AP_WINDOW;off+=32) {
            fnpu_u32 line=0x87000000u+AP_DDR+off;
            __asm__ volatile("dcinva(%0)"::"r"(line):"memory");
        }
        meta[10]=1;
    }
    *(volatile HVX_Vector *)(meta+32)=acc;
    meta[0]=0x41435053u;meta[1]=pc;meta[2]=kind;meta[3]=done;meta[9]=early;meta[11]=AP_SET;
    meta[4]=start;meta[5]=start>>32;meta[6]=end;meta[7]=end>>32;meta[8]=sum;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
