/* Host semantics used to execute the actual vector pooling/stitching C. */
#include "teacher_stem_pack_host.h"
#undef TS_RGB
#include "teacher_stem_check.h"
typedef long rs_uv __attribute__((vector_size(128), aligned(4)));
typedef struct { HVX_Vector lo,hi; } HVX_VectorPair;
static HVX_Vector Q6_V_vsplat_R(int n) {
    fnpu_u32 a[32]; for (int i=0;i<32;i++) a[i]=n;
    HVX_Vector v; memcpy(&v,a,128); return v;
}
static HVX_Vector Q6_V_vand_VV(HVX_Vector a, HVX_Vector b) { return a&b; }
static HVX_Vector Q6_Vh_vsub_VhVh(HVX_Vector a, HVX_Vector b) {
    fnpu_u16 aa[64],bb[64]; memcpy(aa,&a,128); memcpy(bb,&b,128);
    for (int i=0;i<64;i++) aa[i]-=bb[i];
    memcpy(&a,aa,128); return a;
}
static HVX_VectorPred Q6_Q_vcmp_gt_VhVh(HVX_Vector a, HVX_Vector b) {
    int16_t aa[64],bb[64]; HVX_VectorPred q;
    memcpy(aa,&a,128); memcpy(bb,&b,128);
    for (int i=0;i<64;i++) q.b[2*i]=q.b[2*i+1]=aa[i]>bb[i];
    return q;
}
static HVX_VectorPair Q6_W_vdeal_VVR(HVX_Vector b, HVX_Vector a, int n) {
    fnpu_u32 all[64],even[32],odd[32];
    if (n!=-4) __builtin_trap();
    memcpy(all,&a,128); memcpy(all+32,&b,128);
    for (int i=0;i<32;i++) {even[i]=all[2*i]; odd[i]=all[2*i+1];}
    HVX_VectorPair p; memcpy(&p.lo,even,128); memcpy(&p.hi,odd,128); return p;
}
static HVX_Vector Q6_V_lo_W(HVX_VectorPair p) { return p.lo; }
static HVX_Vector Q6_V_hi_W(HVX_VectorPair p) { return p.hi; }
