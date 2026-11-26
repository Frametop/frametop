/* Included after the resident helpers. A band spans global 112-wide rows
 * 16*band-1 .. 16*band+18. Only rows 1..17 feed the downsample: DW0 has one
 * real halo row on both sides, and TF-SAME DW1 samples local rows 2,4,..16.
 * Whole RGB and both 56x56 outputs are resident; only band maps use A/B. */
#ifndef TEACHER_STEM_IMPL_H
#define TEACHER_STEM_IMPL_H
static __attribute__((noinline)) void ts_fault(const fnpu_context *c, fnpu_u32 want, fnpu_u32 stage) {
    if (want == stage) {
        *(volatile HVX_Vector *)fnpu_output(c) = Q6_V_vsplat_R(0xdecafbad);
        __asm__ volatile("barrier\n\ttrap0(#37)" ::: "memory");
    }
}
static __attribute__((noinline)) void ts_layer(rs_op *d, const fnpu_u32 *r, fnpu_u32 band,
        fnpu_u32 off, fnpu_u32 npar, fnpu_u32 cin, fnpu_u32 cout, fnpu_u32 kind,
        fnpu_u32 clip, fnpu_u32 in, fnpu_u32 out) {
    d->kind = kind; d->cpi = cin; d->cpo = cout; d->nk = kind == 1 ? 1 : 3;
    d->clip = clip; d->nbin = 89; d->blk0 = 8; d->nj = 72; d->ws = 114;
    d->vpar = TS_PAR; d->npar = npar; d->dpar = 0;
    d->vin = (fnpu_u32)FR(in); d->vout = (fnpu_u32)FR(out); d->vres = 0;
    d->bias = (fnpu_u32)FR(TS_PAR + (kind == 1 ? ((cout + 15) / 16) * 16 : (cin * 9 + 31) / 32));
    d->scr = (fnpu_u32)FR(TS_PAR + npar);
    rs_copy(FR(TS_PAR), (const volatile HVX_Vector *)(RS_DDR + r[6]) + band * TS_BLOCK + off, npar);
    rs_zero(FR(out), cout * 89);
}
/* Stem K order is ncnn's channel, ky, kx (27 real features). Never pad the
 * full image to 32 channels. The two padding channel pairs are tile-local. */
typedef long ts_uv2 __attribute__((__vector_size__(128), __aligned__(2)));
/* Contiguous 64-pixel load; only even halves are later selected. Loads may
 * extend 8 bytes before RGB or 64 bytes after it, both inside owned VTCM.
 * Byte predicates zero everything beyond that input row before HMX sees it. */
static inline __attribute__((always_inline)) HVX_Vector ts_feature(int y, int x, fnpu_u32 k) {
    HVX_Vector zero = Q6_V_vzero();
    if (k >= 27 || y < 0 || y >= 112) return zero;
    fnpu_u32 ch = k/9, tap = k%9, iy = 2*y + tap/3;
#ifdef RS_STEM_GRAY
    /* Alias the three logical channels to one physical plane. Keep every
     * original weight and K position, preserving HMX accumulation order. */
    ch = 0;
#endif
    if (iy >= 224) return zero;
    int ix = 2*x + (int)(tap%3);
    const fnpu_u16 *p = (const fnpu_u16 *)FR(TS_RGB) + ch*50176 + iy*224 + ix;
    HVX_Vector v = *(const ts_uv2 *)p;
    int lo = ix < 0 ? -2*ix : 0, hi = 2*(224-ix);
    if (hi > 128) hi = 128;
    /* vsetq(128) wraps to an empty predicate; vsetq2 includes a full vector. */
    HVX_VectorPred keep = Q6_Q_and_QQ(Q6_Q_not_Q(Q6_Q_vsetq_R(lo)), Q6_Q_vsetq2_R(hi));
    return Q6_V_vmux_QVV(keep,v,zero);
}
static inline __attribute__((always_inline)) HVX_Vector ts_pair(int y, int x, fnpu_u32 pair) {
    HVX_Vector a = ts_feature(y,x,2*pair), b = ts_feature(y,x,2*pair+1);
    return Q6_Vh_vshuffe_VhVh(b,a);
}
static __attribute__((noinline, flatten)) void ts_rgb_tile(const rs_op *d, fnpu_u32 band, fnpu_u32 j) {
    fnpu_u32 pos = 32*j, col = pos%114, n = 114-col;
    int y = (int)(16*band)-1+(int)(pos/114), x = (int)col-2;
    rs_zero(P(scr),16);
#pragma clang loop unroll(disable)
    for (fnpu_u32 pair = 0; pair < 14; pair++) {
        HVX_Vector v = ts_pair(y,x,pair);
        if (n < 32) {
            HVX_Vector next = ts_pair(y+1,-2,pair);
            next = Q6_V_vror_VR(next,128-4*n);
            v = Q6_V_vmux_QVV(Q6_Q_vsetq_R(4*n),v,next);
        }
        P(scr)[pair] = v;
    }
}
static __attribute__((noinline)) void ts_stem(const rs_op *d, fnpu_u32 band) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 j = 0; j < 72; j++) {
        ts_rgb_tile(d,band,j);
        __asm__ volatile("barrier" ::: "memory");
        fnpu_hmx_clear();
        fnpu_hmx_load((const fnpu_u16 *)FR(TS_PAR), (const fnpu_u16 *)P(scr));
        fnpu_hmx_store((fnpu_u16 *)LV(0),(const void *)RS_SCALE);
#pragma clang loop unroll(disable)
        for (fnpu_u32 pr = 0; pr < 12; pr++) rs_post(d,LV(0)[pr],pr,j);
    }
}
#ifdef RS_STEM_VECTOR
#include "teacher_stem_vector.h"
#endif
/* Finite half maximum, selecting the second operand on equal signed zeros,
 * matching the contract model's ordered maximum operations. */
static inline fnpu_u32 ts_hmax(fnpu_u32 a, fnpu_u32 b) {
    a &= 65535u; b &= 65535u;
    if (!(a & 32767u) && !(b & 32767u)) return b;
    fnpu_u32 ka = a & 32768u ? (a ^ 65535u) : (a | 32768u);
    fnpu_u32 kb = b & 32768u ? (b ^ 65535u) : (b | 32768u);
    return ka > kb ? a : b;
}
static inline fnpu_u32 ts_maxpair(fnpu_u32 a, fnpu_u32 b) {
    return ts_hmax(a,b) | (ts_hmax(a >> 16,b >> 16) << 16);
}
static __attribute__((noinline)) void ts_skip(fnpu_u32 band) {
#ifdef RS_STEM_VECTOR
    tsv_skip(band);
#else
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)FR(TS_A);
    volatile fnpu_u32 *dst = (volatile fnpu_u32 *)FR(TS_OUT10);
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp = 0; cp < 8; cp++)
        for (fnpu_u32 y = 0; y < 8; y++)
            for (fnpu_u32 x = 0; x < 56; x++) {
                fnpu_u32 i = cp*2848 + 258 + (2*y+1)*114 + 2*x;
                fnpu_u32 v = ts_maxpair(ts_maxpair(src[i],src[i+1]),src[i+114]);
                dst[cp*3616 + 130 + (8*band+y)*58 + x] = ts_maxpair(v,src[i+115]);
            }
#endif
}
static __attribute__((noinline)) void ts_downsample(fnpu_u32 band) {
#ifdef RS_STEM_VECTOR
    tsv_downsample(band);
#else
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)FR(TS_A);
    volatile fnpu_u32 *dst = (volatile fnpu_u32 *)FR(TS_OUT8);
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp = 0; cp < 32; cp++)
        for (fnpu_u32 y = 0; y < 8; y++)
            for (fnpu_u32 x = 0; x < 56; x++)
                dst[cp*3616 + 130 + (8*band+y)*58+x] = src[cp*2848 + 259 + (2*y+2)*114 + 2*x];
#endif
}
static void ts_observe(const fnpu_u32 *r, fnpu_u32 stage, fnpu_u32 src, fnpu_u32 n) {
    if (r[16] == stage) rs_copy((volatile HVX_Vector *)(RS_DDR+r[17]),FR(src),n);
}
/* Keep the early depthwise reductions in QFloat32. The old QFloat16 path
 * remains unchanged for the qualified tail. Round to FP16 before bias. */
static __attribute__((noinline)) void ts_dw(const rs_op *d) {
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(TS_PAR);
    HVX_Vector qzero = Q6_Vqf32_vadd_VsfVsf(Q6_V_vzero(),Q6_V_vzero());
    RS_ACC(4)[0] = qzero;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i=0;i<d->cpi*9;i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp=0;cp<d->cpi;cp++) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 j=0;j<72;j++) {
            if (d->cpi == 32 && !ts_needed_block(j)) continue;
            HVX_Vector a=RS_ACC(4)[0], b=a;
            const char *s=(const char *)P(vin)+cp*89*128+4*(256+32*j-115);
#pragma clang loop unroll(disable)
            for (fnpu_u32 tap=0;tap<9;tap++) {
                HVX_Vector v=*(const rs_uv *)(s+4*((tap/3)*114+tap%3));
                HVX_VectorPair p=Q6_Wqf32_vmpy_VhfVhf(v,P(scr)[cp*9+tap]);
                a=Q6_Vqf32_vadd_Vqf32Vqf32(a,Q6_V_lo_W(p));
                b=Q6_Vqf32_vadd_Vqf32Vqf32(b,Q6_V_hi_W(p));
            }
            rs_post(d,Q6_Vhf_equals_Wqf32(Q6_W_vcombine_VV(b,a)),cp,j);
        }
    }
}
static __attribute__((noinline)) fnpu_u32 ts_run(const fnpu_context *c, const fnpu_u32 *r, fnpu_u32 start) {
    rs_op d;
    rs_copy(FR(TS_RGB),(const volatile HVX_Vector *)(RS_DDR+r[5]),TS_INPUT_VECTORS);
    rs_zero(FR(TS_OUT8),3616); rs_zero(FR(TS_OUT10),904);
#pragma clang loop unroll(disable)
    for (fnpu_u32 band = r[1]; band < r[2]; band++) {
        if (rs_ticks()-start > 384000u) return 0;
        ts_layer(&d,r,band,0,112,14,12,1,1,TS_RGB,TS_A);
        ts_stem(&d,band); ts_observe(r,1,TS_A,1068); ts_fault(c,r[15],1);
        ts_layer(&d,r,band,112,100,12,12,2,1,TS_A,TS_B);
        ts_dw(&d); ts_observe(r,2,TS_B,1068); ts_fault(c,r[15],2);
        ts_layer(&d,r,band,224,112,12,8,1,0,TS_B,TS_A);
        rs_pw(&d); ts_observe(r,3,TS_A,712); ts_fault(c,r[15],3);
        ts_skip(band); ts_fault(c,r[15],6);
        ts_layer(&d,r,band,336,176,8,32,1,1,TS_A,TS_B);
        rs_pw(&d); ts_observe(r,4,TS_B,2848); ts_fault(c,r[15],4);
        ts_layer(&d,r,band,512,145,32,32,2,1,TS_B,TS_A);
        ts_dw(&d); ts_observe(r,5,TS_A,2848); ts_fault(c,r[15],5);
        ts_downsample(band);
    }
    rs_copy((volatile HVX_Vector *)(RS_DDR+r[9]),FR(TS_OUT8),3616);
    rs_copy((volatile HVX_Vector *)(RS_DDR+r[10]),FR(TS_OUT10),904);
    return 1;
}
#endif
