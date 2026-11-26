/* GAP of the resident 672x7x7 FP16 grid, FP32 heads and two sigmoids.
 * No scalar/vector state is kept across HMX calls; the epilogue uses no HMX.
 * VTCM: r[11] input, r[12] 128 output floats, r[13] 672 pooled floats,
 * r[14] FP32 bias + channel-major weights. Scratch LV(48/49) is inside the
 * existing saved 8 KiB and is dead before/after this op. */
#include "landmark_common.h"
#include "teacher_heads_check.h"
#define TH_FR(i) ((volatile HVX_Vector *)(0x31002000u + 128u * (i)))
#define TH_LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define TH_INLINE static inline __attribute__((always_inline))
TH_INLINE HVX_Vector th_add(HVX_Vector a, HVX_Vector b) { return addf(a, b); }
TH_INLINE HVX_Vector th_mul(HVX_Vector a, HVX_Vector b) { return mulf(a, b); }
TH_INLINE void th_flags(HVX_Vector v) {
    TH_LV(62)[0] = Q6_V_vor_VV(TH_LV(62)[0], nonfinite(v));
}
/* The same bounded approximation used by the qualified resident graph. */
static __attribute__((noinline)) float th_sigmoid(float x) {
    if (x == 0.f) return 0.5f;
    if (x > 80.f) return 1.f;
    if (x < -80.f) return 0.f;
    float y = -x * 1.4426950408889634f;
    int n = (int)y;
    if (y < (float)n) n--;
    float r = -x - (float)n * 0.6931471805599453f;
    float p = 1.f + r * (1.f + r * (0.5f + r * (0.1666666666666667f + r *
              (0.0416666666666667f + r * (0.0083333333333333f + r *
              (0.0013888888888889f + r * 0.0001984126984127f))))));
    union { fnpu_u32 u; float f; } power, denominator, reciprocal;
    power.u = (fnpu_u32)(n + 127) << 23;
    denominator.f = 1.f + p * power.f;
    reciprocal.u = 0x7ef311c3u - denominator.u;
    for (int i = 0; i < 4; i++) reciprocal.f *= 2.f - denominator.f * reciprocal.f;
    return reciprocal.f;
}
static __attribute__((noinline)) void th_fault(const fnpu_context *c, fnpu_u32 requested, fnpu_u32 stage) {
    if (requested == stage) {
        *(volatile HVX_Vector *)fnpu_output(c) = Q6_V_vsplat_R(0xdecafbad);
        __asm__ volatile("barrier\n\ttrap0(#37)" ::: "memory");
    }
}
TH_INLINE HVX_Vector th_convert(void) {
    return half_words_to_float(TH_LV(48)[0]);
}
static __attribute__((noinline, flatten)) void th_pool(const fnpu_u32 *r) {
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)TH_FR(r[11]);
    volatile fnpu_u32 *gather = (volatile fnpu_u32 *)TH_LV(48);
#pragma clang loop unroll(disable)
    for (fnpu_u32 group = 0; group < 21; group++) {
        TH_LV(49)[0] = Q6_V_vzero();
#pragma clang loop unroll(disable)
        for (fnpu_u32 y = 0; y < 7; y++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 x = 0; x < 7; x++) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 lane = 0; lane < 32; lane++) {
                    fnpu_u32 ch = group * 32 + lane;
                    gather[lane] = src[(ch >> 1) * 224 + 34 + y * 9 + x] >> ((ch & 1) * 16);
                }
                HVX_Vector v = th_convert();
                th_flags(v);
                TH_LV(49)[0] = th_add(TH_LV(49)[0], v);
            }
        }
        HVX_Vector v = th_mul(TH_LV(49)[0], Q6_V_vsplat_R(0x3ca72f05));
        th_flags(v);
        TH_FR(r[13])[group] = v;
    }
}
static __attribute__((noinline)) void th_linear(const fnpu_u32 *r) {
    const volatile HVX_Vector *par = TH_FR(r[14]);
    const volatile fnpu_u32 *pool = (const volatile fnpu_u32 *)TH_FR(r[13]);
    HVX_Vector a0 = par[0], a1 = par[1], a2 = par[2], a3 = par[3];
#pragma clang loop unroll(disable)
    for (fnpu_u32 ch = 0; ch < 672; ch++) {
        HVX_Vector x = Q6_V_vsplat_R(pool[ch]);
        a0 = th_add(a0, th_mul(par[4 + ch * 4], x));
        a1 = th_add(a1, th_mul(par[5 + ch * 4], x));
        a2 = th_add(a2, th_mul(par[6 + ch * 4], x));
        a3 = th_add(a3, th_mul(par[7 + ch * 4], x));
    }
    th_flags(a0); th_flags(a1); th_flags(a2); th_flags(a3);
    TH_FR(r[12])[0] = a0; TH_FR(r[12])[1] = a1;
    TH_FR(r[12])[2] = a2; TH_FR(r[12])[3] = a3;
}
void th_run(const fnpu_u32 *r, const fnpu_context *c) {
    th_pool(r);
    th_fault(c, r[15], 1);
    th_linear(r);
    volatile float *out = (volatile float *)TH_FR(r[12]);
    out[126] = th_sigmoid(out[126]);
    out[127] = th_sigmoid(out[127]);
    th_fault(c, r[15], 2);
}
