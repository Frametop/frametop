/* Portable instruction semantics for the offline test of the actual C RGB
 * packer. No DSP execution; only byte predicates, shuffle and rotate. */
#include <stdint.h>
#include <string.h>
typedef uint32_t fnpu_u32;
typedef uint16_t fnpu_u16;
typedef long HVX_Vector __attribute__((vector_size(128), aligned(2)));
typedef struct { unsigned char b[128]; } HVX_VectorPred;
typedef struct { HVX_Vector *scr; } rs_op;
static unsigned char vtcm[15296*128] __attribute__((aligned(128)));
#define FR(i) ((HVX_Vector *)(vtcm+128*(i)))
#define TS_RGB 4
#define P(x) (d->x)
static HVX_Vector Q6_V_vzero(void) { return (HVX_Vector){0}; }
static void rs_zero(HVX_Vector *p, fnpu_u32 n) {
    for (fnpu_u32 i=0;i<n;i++) p[i]=Q6_V_vzero();
}
static HVX_VectorPred Q6_Q_vsetq_R(int n) {
    HVX_VectorPred q;
    for (int i=0;i<128;i++) q.b[i]=i<(n&127);
    return q;
}
static HVX_VectorPred Q6_Q_vsetq2_R(int n) {
    HVX_VectorPred q;
    for (int i=0;i<128;i++) q.b[i]=i<=((n-1)&127);
    return q;
}
static HVX_VectorPred Q6_Q_not_Q(HVX_VectorPred q) {
    for (int i=0;i<128;i++) q.b[i]=!q.b[i];
    return q;
}
static HVX_VectorPred Q6_Q_and_QQ(HVX_VectorPred a, HVX_VectorPred b) {
    for (int i=0;i<128;i++) a.b[i]=a.b[i]&&b.b[i];
    return a;
}
static HVX_Vector Q6_V_vmux_QVV(HVX_VectorPred q, HVX_Vector a, HVX_Vector b) {
    unsigned char out[128],aa[128],bb[128];
    memcpy(aa,&a,128);memcpy(bb,&b,128);
    for (int i=0;i<128;i++) out[i]=q.b[i]?aa[i]:bb[i];
    memcpy(&a,out,128);return a;
}
static HVX_Vector Q6_Vh_vshuffe_VhVh(HVX_Vector b, HVX_Vector a) {
    fnpu_u16 aa[64],bb[64],out[64];
    memcpy(aa,&a,128);memcpy(bb,&b,128);
    for (int i=0;i<32;i++) { out[2*i]=aa[2*i];out[2*i+1]=bb[2*i]; }
    memcpy(&a,out,128);return a;
}
static HVX_Vector Q6_V_vror_VR(HVX_Vector a, int n) {
    unsigned char aa[128],out[128];memcpy(aa,&a,128);
    for (int i=0;i<128;i++) out[i]=aa[(i+n)&127];
    memcpy(&a,out,128);return a;
}
