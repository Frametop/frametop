/* One complete clear/load-all-depth/store transaction, M=N=32, K<=704.
 * Host supplies padded tile-major FP16 A/B; all reads use uncached DDR.
 * Only the SDK's saved/restored first 8 KiB VTCM is touched. */
#include "landmark_common.h"
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *,const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *,const void *);
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 depth=c->params[0], clip=c->params[1];
    if(!valid(c) || c->opcode!=20 || !depth || depth>22 || clip>1 || c->params[2] ||
       c->input_bytes!=depth*4096+128 || c->output_bytes!=4224) return FNPU_BAD_ARGUMENT;
    fault_control(c);
    const volatile HVX_Vector *in=fnpu_input(c);
    volatile HVX_Vector *a=(volatile HVX_Vector *)0x31000000u;
    volatile HVX_Vector *b=(volatile HVX_Vector *)0x31000800u;
    volatile fnpu_u32 *scales=(volatile fnpu_u32 *)0x31001800u;
    for(fnpu_u32 i=0;i<64;i++) scales[i]=i<32?0x3c00u:0;
    fnpu_hmx_clear();
    for(fnpu_u32 t=0;t<depth;t++) {
        for(fnpu_u32 i=0;i<16;i++) {
            a[i]=Q6_Vh_vshuff_Vh(in[t*16+i]);
            b[i]=Q6_Vh_vshuff_Vh(in[(depth+t)*16+i]);
        }
        __asm__ volatile("barrier" ::: "memory");
        fnpu_hmx_load((const fnpu_u16 *)a,(const fnpu_u16 *)b);
    }
    fnpu_hmx_store((fnpu_u16 *)0x31001000u,(const void *)scales);
    volatile HVX_Vector *out=fnpu_output(c);
    HVX_Vector bad=Q6_V_vzero(), bias=in[depth*32];
    for(fnpu_u32 r=0;r<32;r++) {
        HVX_Vector v=read_row((const volatile fnpu_u16 *)0x31001000u,r);
        bad=Q6_V_vor_VV(bad,nonfinite(v));
        v=addf(v,bias);
        bad=Q6_V_vor_VV(bad,nonfinite(v));
        out[r]=clip?clip6(v):v;
    }
    out[32]=bad;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
