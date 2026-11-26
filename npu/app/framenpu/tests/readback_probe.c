/* Hardware fixture for the same readback helpers used by runtime.c.
   Opcode 0 returns conversion bits, 1 accumulates with the new helper,
   2 uses the qualified pre-HVX scalar readback as an independent DSP oracle. */
#include "fnpu_readback.h"

static float scalar_half(fnpu_u16 h) {
    union { fnpu_u32 u; float f; } v;
    fnpu_u32 sign=(h&0x8000u)<<16, exponent=(h>>10)&31, fraction=h&1023u;
    if (!exponent) {
        if (!fraction) { v.u=sign; return v.f; }
        int shift=0;
        while (!(fraction&1024u)) { fraction<<=1; shift++; }
        v.u=sign|((113u-shift)<<23)|((fraction&1023u)<<13);
    } else {
        v.u=sign|((exponent==31 ? 255u : exponent+112u)<<23)|(fraction<<13);
    }
    return v.f;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    const fnpu_u16 *src=fnpu_input(ctx);
    unsigned char *output=fnpu_output(ctx);
    if (ctx->opcode==0) {
        if (ctx->input_bytes!=2048 || ctx->output_bytes!=4096 ||
            (ctx->input_offset&127) || (ctx->output_offset&127))
            return FNPU_BAD_ARGUMENT;
        for (fnpu_u32 r=0;r<32;r++) ((HVX_Vector *)output)[r]=read_row(src,r);
        return FNPU_OK;
    }
    fnpu_u32 rows=ctx->params[0], cols=ctx->params[1], stride=ctx->params[2], prefix=ctx->params[3];
    if ((ctx->opcode!=1 && ctx->opcode!=2) || !rows || rows>32 || !cols || cols>32 ||
        stride<cols || stride>64 || prefix>124 || (prefix&3) || (ctx->input_offset&127) ||
        ctx->input_bytes!=2048+rows*stride*4 || ctx->output_bytes!=prefix+rows*stride*4+128 ||
        (ctx->opcode==1 && !(ctx->flags&FNPU_FLAG_HMX)))
        return FNPU_BAD_ARGUMENT;
    for (fnpu_u32 i=0;i<ctx->output_bytes;i++) output[i]=0xa5;
    float *out=(float *)(output+prefix);
    const fnpu_u32 *seed=(const fnpu_u32 *)(src+1024);
    for (fnpu_u32 i=0;i<rows*stride;i++) ((fnpu_u32 *)out)[i]=seed[i];
    if (ctx->opcode==1) {
        read_tile(src,out,rows,cols,stride);
    } else {
        for (fnpu_u32 r=0;r<rows;r++) {
            for (fnpu_u32 c=0;c<cols;c++)
                out[r*stride+c]+=scalar_half(src[(r>>1)*64+c*2+(r&1)]);
        }
    }
    return FNPU_OK;
}
