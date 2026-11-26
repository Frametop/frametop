#include "framenpu.h"
#include <hexagon_types.h>
#include <hexagon_protos.h>
#include "fnpu_readback.h"

void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);

static fnpu_u32 packed(fnpu_u32 row, fnpu_u32 col) {
    return (row >> 1) * 64 + col * 2 + (row & 1);
}

/* Full tiles only: every vector load stays inside complete source rows.
   A stride of 64 contains two depth tiles; select the appropriate half of
   the shuffled row pair. Other strides, alignments and edges use scalar
   zero padding, without reading beyond the logical matrix. */
static void pack_tile(volatile fnpu_u16 *dst, const fnpu_u16 *src,
                      fnpu_u32 rows, fnpu_u32 cols, fnpu_u32 stride, fnpu_u32 column) {
    if (rows >= 32 && cols >= 32 && !((fnpu_u32)src & 127) &&
        (stride == 32 || stride == 64)) {
        const HVX_Vector *p = (const HVX_Vector *)src;
        for (fnpu_u32 pair=0;pair<16;pair++) {
            HVX_Vector v;
            if (stride == 32) {
                v = Q6_Vh_vshuff_Vh(p[pair]);
            } else {
                HVX_VectorPair both = Q6_W_vshuff_VVR(p[pair*2+1], p[pair*2], -2);
                v = column ? Q6_V_hi_W(both) : Q6_V_lo_W(both);
            }
            ((volatile HVX_Vector *)dst)[pair] = v;
        }
        return;
    }
    for (fnpu_u32 r=0;r<32;r++) {
        for (fnpu_u32 c=0;c<32;c++)
            dst[packed(r,c)] = r<rows && c<cols ? src[r*stride+column+c] : 0;
    }
}

static fnpu_u32 gemm(const fnpu_context *ctx) {
    fnpu_u32 m = ctx->params[0], k = ctx->params[1], n = ctx->params[2];
    fnpu_u32 relu = ctx->params[3];
    fnpu_u32 multi = ctx->opcode == 6;
    fnpu_u32 seeded = multi ? ctx->params[4] : 0;
    if (!m || !k || !n || m > 32 || k > (multi ? 64u : 32u) || n > 32 || relu > 1 ||
        seeded > 1 || (multi && !(ctx->flags & FNPU_FLAG_DIRECT)) ||
        !(ctx->flags & FNPU_FLAG_HMX))
        return FNPU_BAD_ARGUMENT;
    fnpu_u32 bias_offset = ((m*k + k*n)*2 + 3) & ~3u;
    if (ctx->input_bytes != bias_offset + n*4 + seeded*m*n*4 || ctx->output_bytes != m*n*4)
        return FNPU_BAD_ARGUMENT;
    const fnpu_u16 *a = fnpu_input(ctx);
    const fnpu_u16 *b = a + m*k;
    const float *bias = (const float *)((const unsigned char *)a + bias_offset);
    const float *seed = bias + n;
    float *out = fnpu_output(ctx);
    volatile fnpu_u16 *ta = (volatile fnpu_u16 *)0x31000000u;
    volatile fnpu_u16 *tb = (volatile fnpu_u16 *)0x31000800u;
    volatile fnpu_u16 *tc = (volatile fnpu_u16 *)0x31001000u;
    volatile fnpu_u32 *scales = (volatile fnpu_u32 *)0x31001800u;
    for (fnpu_u32 i=0;i<32;i++) scales[i]=0x3c00u;
    for (fnpu_u32 i=32;i<64;i++) scales[i]=0;
    for (fnpu_u32 i=0;i<m*n;i++) out[i]=seeded ? seed[i] : 0.0f;
    for (fnpu_u32 mr=0;mr<m;mr+=32) {
        for (fnpu_u32 nc=0;nc<n;nc+=32) {
            for (fnpu_u32 kb=0;kb<k;kb+=32) {
                pack_tile(ta, a+mr*k, m-mr, k-kb, k, kb);
                pack_tile(tb, b+kb*n+nc, k-kb, n-nc, n, 0);
                __asm__ volatile("barrier" ::: "memory");
                fnpu_hmx_clear();
                fnpu_hmx_load((const fnpu_u16 *)ta, (const fnpu_u16 *)tb);
                fnpu_hmx_store((fnpu_u16 *)tc, (const void *)scales);
                /* Readback serializes completion before these VTCM tiles are reused.
                   Each depth-32 product rounds to FP16, then sums into FP32 DDR. */
                read_tile(tc, out+mr*n+nc, m-mr, n-nc, n);
            }
        }
    }
    for (fnpu_u32 r=0;r<m;r++) {
        for (fnpu_u32 c=0;c<n;c++) {
            float value=out[r*n+c]+bias[c];
            out[r*n+c]=relu && value<0.0f ? 0.0f : value;
        }
    }
    return FNPU_OK;
}

static fnpu_u32 elementwise(const fnpu_context *ctx) {
    fnpu_u32 count=ctx->params[0], clip=ctx->params[1];
    if (count>32768 || clip>1 || ctx->input_bytes!=count*4+8 || ctx->output_bytes!=count*4)
        return FNPU_BAD_ARGUMENT;
    const float *in=fnpu_input(ctx);
    float *out=fnpu_output(ctx);
    float low=clip ? in[count] : 0.0f;
    float high=in[count+1];
    for (fnpu_u32 i=0;i<count;i++) {
        float v=in[i];
        if (v<low) v=low;
        if (clip && v>high) v=high;
        out[i]=v;
    }
    return FNPU_OK;
}

static fnpu_u32 add(const fnpu_context *ctx) {
    fnpu_u32 count=ctx->params[0], bc=ctx->params[1], relu=ctx->params[2];
    if (!bc || count>32768 || bc>count || relu>1 ||
        ctx->input_bytes!=(count+bc)*4 || ctx->output_bytes!=count*4)
        return FNPU_BAD_ARGUMENT;
    const float *a=fnpu_input(ctx);
    const float *b=a+count;
    float *out=fnpu_output(ctx);
    fnpu_u32 cursor=0;
    for (fnpu_u32 i=0;i<count;i++) {
        float v=a[i]+b[cursor];
        cursor++;
        if (cursor==bc) cursor=0;
        out[i]=relu && v<0.0f ? 0.0f : v;
    }
    return FNPU_OK;
}

fnpu_u32 frame_job(const fnpu_context *ctx) {
    if (ctx->magic!=FNPU_MAGIC || ctx->abi!=FNPU_ABI ||
        ctx->arena_bytes>FNPU_ARENA_SIZE ||
        ctx->input_offset>ctx->arena_bytes ||
        ctx->input_bytes>ctx->arena_bytes-ctx->input_offset ||
        ctx->output_offset>ctx->arena_bytes ||
        ctx->output_bytes>ctx->arena_bytes-ctx->output_offset ||
        (ctx->input_offset&3) || (ctx->output_offset&3))
        return FNPU_BAD_ARGUMENT;
    if (ctx->opcode==1 || ctx->opcode==6) return gemm(ctx);
    if (ctx->opcode==2) return elementwise(ctx);
    if (ctx->opcode==3) return add(ctx);
    /* Transport probes: no arithmetic for 4, exact byte copy for 5. */
    if (ctx->opcode==4)
        return ctx->input_bytes || ctx->output_bytes ? FNPU_BAD_ARGUMENT : FNPU_OK;
    if (ctx->opcode==5) {
        if (ctx->input_bytes != ctx->output_bytes) return FNPU_BAD_ARGUMENT;
        const unsigned char *in = fnpu_input(ctx);
        unsigned char *out = fnpu_output(ctx);
        fnpu_u32 i=0;
        if (!((ctx->input_offset | ctx->output_offset) & 127)) {
            for (; i+128<=ctx->input_bytes; i+=128)
                *(HVX_Vector *)(out+i) = *(const HVX_Vector *)(in+i);
        }
        for (; i<ctx->input_bytes; i++) out[i]=in[i];
        return FNPU_OK;
    }
    return FNPU_UNSUPPORTED;
}
