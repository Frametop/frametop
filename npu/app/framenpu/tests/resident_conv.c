/* Bounded resident 32xKx32 GEMM proof. Build with entry_resident.S.
 * Fixed DDR partition is mirrored and validated by resident_gemm.py.
 * Code remains in the SDK's existing executable slot. Only worker 0 uses HMX.
 */
#include "fnpu_readback.h"
typedef unsigned long long u64;
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);
#define STATE ((volatile fnpu_u32 *)0xf1a23080u)
#define WEIGHTS ((volatile HVX_Vector *)0x88a24000u)
#define INPUT ((const volatile HVX_Vector *)0x88a26000u)
#define OUTPUT ((volatile HVX_Vector *)0x88a28000u)
#define MAGIC 0x5247454du
#ifndef PROBE_TAG
#define PROBE_TAG 2
#endif

static u64 ticks(void) {
    u64 value;
    __asm__ volatile("%0 = c31:30" : "=r"(value) :: "memory");
    return value;
}
static void stamp(volatile fnpu_u32 *out, fnpu_u32 index, u64 value) {
    out[index] = (fnpu_u32)value; out[index+1] = (fnpu_u32)(value >> 32);
}
static __attribute__((noinline)) void publish_code(void) {
    volatile HVX_Vector *stage = (volatile HVX_Vector *)0x31000000u;
    for (fnpu_u32 i=0;i<32;i++) stage[i] = ((const volatile HVX_Vector *)0xf0a92000u)[i];
    for (fnpu_u32 i=0;i<32;i++) {
        ((volatile HVX_Vector *)0x87a92000u)[i] = stage[i];
        __asm__ volatile("barrier" ::: "memory");
        for (fnpu_u32 j=0;j<128;j+=32) {
            fnpu_u32 address = 0x87a92000u + i*128+j;
            __asm__ volatile("dccleaninva(%0)\n\tsyncht" :: "r"(address) : "memory");
        }
    }
    for (fnpu_u32 j=0;j<4096;j+=32) {
        fnpu_u32 address = 0x87a92000u+j;
        __asm__ volatile("icinva(%0)\n\tisync" :: "r"(address) : "memory");
    }
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic != FNPU_MAGIC || ctx->abi != FNPU_ABI || ctx->opcode != 11 ||
        ctx->flags != (FNPU_FLAG_HMX|FNPU_FLAG_DIRECT) || ctx->input_bytes != 128 ||
        ctx->input_offset || ctx->output_offset != 128 || ctx->arena_bytes != 384 ||
        ctx->output_bytes != 256) return FNPU_BAD_ARGUMENT;
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc) :: "memory");
    if (pc >= 0xf0a92000u && pc < 0xf0a93000u) {
        *(volatile fnpu_u32 *)0xf0a93438u = 3;
        __asm__ volatile("barrier" ::: "memory");
        publish_code();
        fnpu_u32 target = (fnpu_u32)frame_job;
        __asm__ volatile("%0 = sub(%0,%1)" : "+r"(target) : "r"(0x69000000u) : "memory");
        return ((fnpu_u32 (*)(const fnpu_context *))target)(ctx);
    }
    const volatile fnpu_u32 *d = fnpu_input(ctx);
    volatile fnpu_u32 *out = fnpu_output(ctx);
    for (fnpu_u32 i=0;i<32;i++) out[i] = d[i];
    out[31] = pc;
    u64 begin=ticks(), copied=0, packed=0, hmx=0;
    stamp(out,32,begin);
    fnpu_u32 op=d[2], depth=d[3];
    if (d[0]!=MAGIC || d[1]!=1 || op>4 || !depth || depth>4 ||
        !(d[4]|d[5]|d[6]|d[7]) || !(d[8]|d[9]) ||
        d[18]!=0x1a24000u || d[19]!=depth*2048 ||
        d[20]!=0x1a26000u || d[21]!=depth*2048 ||
        d[22]!=0x1a28000u || d[23]!=4096 || d[24]!=PROBE_TAG) return FNPU_BAD_ARGUMENT;
    if(d[25]>1) return FNPU_BAD_ARGUMENT;
    for (fnpu_u32 i=26;i<32;i++) if (d[i]) return FNPU_BAD_ARGUMENT;
    if (op==0) { stamp(out,42,ticks()); return FNPU_OK; }
    if (op==1) {
        if (STATE[0]) return FNPU_BAD_ARGUMENT;
        u64 start=ticks();
        for (fnpu_u32 i=0;i<depth*16;i++) WEIGHTS[i] = Q6_Vh_vshuff_Vh(WEIGHTS[i]);
        __asm__ volatile("barrier" ::: "memory");
        stamp(out,36,ticks()-start);
        for (fnpu_u32 i=0;i<18;i++) STATE[i]=d[i];
        STATE[0]=1;
        STATE[18]=d[25];
        stamp(out,42,ticks());
        return FNPU_OK;
    }
    if (STATE[0] != (op==4 ? 2u : 1u) && !(op==4 && STATE[0]==1)) return FNPU_BAD_ARGUMENT;
    for (fnpu_u32 i=3;i<18;i++) {
        if (i==8 || i==9) continue;
        if (STATE[i]!=d[i]) return FNPU_BAD_ARGUMENT;
    }
    if(STATE[18]!=d[25]) return FNPU_BAD_ARGUMENT;
    u64 sequence = ((u64)STATE[9]<<32)|STATE[8];
    if (sequence==~(u64)0 || d[8]!=(fnpu_u32)(sequence+1) || d[9]!=(fnpu_u32)((sequence+1)>>32)) return FNPU_BAD_ARGUMENT;
    STATE[8]=d[8]; STATE[9]=d[9];
    if (op==4) { STATE[0]=0; stamp(out,42,ticks()); return FNPU_OK; }
    volatile HVX_Vector *a=(volatile HVX_Vector *)0x31000000u;
    volatile HVX_Vector *b=(volatile HVX_Vector *)0x31000800u;
    volatile fnpu_u32 *scales=(volatile fnpu_u32 *)0x31001800u;
    for (fnpu_u32 i=0;i<64;i++) scales[i]=i<32?0x3c00u:0;
    u64 t=ticks(); fnpu_hmx_clear(); hmx+=ticks()-t;
    for (fnpu_u32 tile=0;tile<depth;tile++) {
        t=ticks();
        for (fnpu_u32 i=0;i<16;i++) { a[i]=INPUT[tile*16+i]; b[i]=WEIGHTS[tile*16+i]; }
        __asm__ volatile("barrier" ::: "memory");
        copied+=ticks()-t; t=ticks();
        for (fnpu_u32 i=0;i<16;i++) a[i]=Q6_Vh_vshuff_Vh(a[i]);
        __asm__ volatile("barrier" ::: "memory");
        packed+=ticks()-t;
        if (op==3) {
            STATE[0]=2;
            OUTPUT[0]=Q6_V_vsplat_R(0xdecafbad);
            out[30]=1;
            stamp(out,44,ticks());
            __asm__ volatile("barrier\n\ttrap0(#37)" ::: "memory");
            out[30]=2; /* Must remain 1: no abandoned C frame may resume. */
        }
        t=ticks(); fnpu_hmx_load((const fnpu_u16 *)a,(const fnpu_u16 *)b); hmx+=ticks()-t;
    }
    t=ticks(); fnpu_hmx_store((fnpu_u16 *)0x31001000u,(const void *)scales); hmx+=ticks()-t;
    stamp(out,34,copied); stamp(out,36,packed); stamp(out,38,hmx);
    t=ticks();
    for (fnpu_u32 r=0;r<32;r++) OUTPUT[r]=read_row((const volatile fnpu_u16 *)0x31001000u,r);
    volatile float* values=(volatile float*)OUTPUT;
    volatile float* bias=(volatile float*)0x31001900u;
    *(volatile HVX_Vector*)bias=*(const volatile HVX_Vector*)0xf1a23100u;
    for(fnpu_u32 i=0;i<1024;i++) {
        union {float f; fnpu_u32 u;} raw;
        raw.f=values[i];
        if((raw.u&0x7f800000u)==0x7f800000u) out[46]++;
        float value=raw.f+bias[i&31];
        if(d[25]) { if(value<0.f) value=0.f; if(value>6.f) value=6.f; }
        values[i]=value;
    }
    __asm__ volatile("barrier" ::: "memory");
    stamp(out,40,ticks()-t);
    stamp(out,42,ticks());
    return FNPU_OK;
}
