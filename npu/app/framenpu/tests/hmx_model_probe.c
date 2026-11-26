/* Shape-cost probe for HMX-centric spatial blocks. Cached entry, worker0 only.
 * mode0 reuses first VTCM pair through K (synthetic arithmetic control);
 * mode1 streams distinct prepacked pairs and writes raw half output;
 * mode2 streams plus existing FP32 conversion/bias/clip output;
 * mode3 streams plus supported QFloat16 bias/clip and packed-half output.
 * Host packing is measured separately; this is not an integrated executor. */
#include "landmark_common.h"
typedef unsigned long long mp_u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *,const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *,const void *);
#define MP_A ((volatile HVX_Vector *)0x31000000u)
#define MP_B ((volatile HVX_Vector *)0x31000800u)
#define MP_C ((volatile HVX_Vector *)0x31001000u)
#define MP_SCALE ((volatile fnpu_u32 *)0x31001800u)
#define MP_BIAS ((volatile HVX_Vector *)0x31001900u)
#define MP_FLAG (*(volatile fnpu_u32 *)0x31001a00u)
#define MP_FAULT (*(volatile fnpu_u32 *)0x31001a04u)
#define MP_START (*(volatile mp_u64 *)0x31001b00u)
#ifndef MODEL_MODE
#error MODEL_MODE required
#endif
static mp_u64 mp_ticks(void) {mp_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;}
static inline __attribute__((always_inline)) void mp_copy(volatile HVX_Vector *to,const volatile HVX_Vector *from,fnpu_u32 n) {
    for(fnpu_u32 i=0;i<n;i++) to[i]=from[i];
    __asm__ volatile("barrier" ::: "memory");
}
static __attribute__((noinline)) void mp_trap(const fnpu_context *c) {fault_control(c);}
static __attribute__((noinline)) void mp_half(volatile HVX_Vector *out,fnpu_u32 biased) {
    HVX_Vector flags=Q6_V_vzero(),exp=Q6_V_vsplat_R(0x7c007c00u);
    for(fnpu_u32 pair=0;pair<16;pair++) {
        HVX_Vector v=MP_C[pair];
        flags=Q6_V_vor_VV(flags,Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v,exp),exp),-1));
        if(biased) {
            v=Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v,Q6_V_vsplat_R(((const volatile fnpu_u32 *)MP_BIAS)[pair])));
            flags=Q6_V_vor_VV(flags,Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v,exp),exp),-1));
            v=Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v,Q6_V_vzero()),Q6_V_vsplat_R(0x46004600u));
        }
        out[pair]=v;
    }
    for(fnpu_u32 bytes=64;bytes>=4;bytes>>=1) flags=Q6_V_vor_VV(flags,Q6_V_vror_VR(flags,bytes));
    MP_BIAS[1]=flags;MP_FLAG|=*(const volatile fnpu_u32 *)(MP_BIAS+1);
}
static __attribute__((noinline)) void mp_float(volatile HVX_Vector *out) {
    HVX_Vector bad=Q6_V_vzero();
    for(fnpu_u32 row=0;row<32;row++) {
        HVX_Vector v=read_row((const volatile fnpu_u16 *)MP_C,row);bad=Q6_V_vor_VV(bad,nonfinite(v));
        v=addf(v,Q6_V_vsplat_R(((const volatile fnpu_u32 *)MP_BIAS)[row]));
        bad=Q6_V_vor_VV(bad,nonfinite(v));out[row]=clip6(v);
    }
    for(fnpu_u32 bytes=64;bytes>=4;bytes>>=1) bad=Q6_V_vor_VV(bad,Q6_V_vror_VR(bad,bytes));
    MP_BIAS[1]=bad;MP_FLAG|=*(const volatile fnpu_u32 *)(MP_BIAS+1);
}
fnpu_u32 frame_job(const fnpu_context *c) {
    enum {mode=MODEL_MODE};fnpu_u32 requested=c->params[2];
    const volatile fnpu_u32 *d=fnpu_input(c);
    if(!valid(c)||c->opcode!=100||c->input_bytes<4480||c->output_bytes!=6400||c->params[0]!=1||c->params[1]!=mode||!requested||requested>64) return FNPU_BAD_ARGUMENT;
    fnpu_u32 depth=d[0];
    if(!depth||depth>25||c->input_bytes!=384+depth*4096) return FNPU_BAD_ARGUMENT;
    for(fnpu_u32 i=1;i<32;i++) if(d[i]) return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta=fnpu_output(c);
    volatile HVX_Vector *out=(volatile HVX_Vector *)((unsigned char *)fnpu_output(c)+256);
    for(fnpu_u32 i=0;i<64;i++) meta[i]=0;
    const volatile HVX_Vector *a=(const volatile HVX_Vector *)((const unsigned char *)fnpu_input(c)+128);
    const volatile HVX_Vector *b=a+depth*16;
    for(fnpu_u32 i=0;i<64;i++) MP_SCALE[i]=i<32?0x3c00u:0;
    mp_copy(MP_BIAS,b+depth*16+(mode==3),1);MP_FLAG=0;MP_FAULT=c->params[5];
    if(!mode) {mp_copy(MP_A,a,16);mp_copy(MP_B,b,16);}
    MP_START=mp_ticks();fnpu_u32 done=0;
    for(;done<requested;) {
        fnpu_hmx_clear();
        for(fnpu_u32 tile=0;tile<depth;tile++) {
            if(mode) {mp_copy(MP_A,a+tile*16,16);mp_copy(MP_B,b+tile*16,16);}
            fnpu_hmx_load((const fnpu_u16 *)MP_A,(const fnpu_u16 *)MP_B);
            if(MP_FAULT) {mp_trap(c);}
        }
        fnpu_hmx_store((fnpu_u16 *)MP_C,(const void *)MP_SCALE);
        if(mode==2) mp_float(out+16);else mp_half(out,mode==3);
        done++;
        if((fnpu_u32)mp_ticks()-(fnpu_u32)MP_START>384000u) break;
    }
    mp_u64 end=mp_ticks(),start=MP_START;fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    meta[0]=0x484d5031u;meta[1]=pc;meta[2]=mode;meta[3]=depth;meta[4]=done;meta[5]=MP_FLAG;
    meta[6]=start;meta[7]=start>>32;meta[8]=end;meta[9]=end>>32;meta[10]=requested;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
