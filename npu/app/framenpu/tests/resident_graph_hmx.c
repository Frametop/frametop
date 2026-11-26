#include "resident_graph.h"
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *,const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *,const void *);
#ifdef RG_CONV_FAST
/* Vector work stays in functions of its own, so the unit loop, which calls
 * the HMX routines, holds no vector constant on the job stack. */
#define HMX_PART static __attribute__((noinline))
#else
#define HMX_PART static
#endif
HMX_PART void weight_tile(rg_record *r,fnpu_u32 k,fnpu_u32 kt,fnpu_u32 nc) {
    volatile HVX_Vector *a=(volatile HVX_Vector *)0x31000000u;
    const unsigned char *w=RG_PTR(r[16]);
    for(fnpu_u32 pair=0;pair<16;pair++) {
        fnpu_u32 row=nc+pair*2;
        HVX_Vector v=row<r[7]?RG_LOADU(w+(row/2*k+kt)*4):Q6_V_vzero();
        if(k-kt<32) v=Q6_V_vmux_QVV(Q6_Q_vsetq_R((k-kt)*4),v,Q6_V_vzero());
        a[pair]=v;
    }
}
#ifdef RG_CONV_FAST
/* Read the 32 result rows of one HMX unit and finish each output channel as
 * the separate post operator did after reloading it: add the channel bias,
 * flag a non-finite sum, clip, store. */
HMX_PART void hmx_rows(rg_record *r,fnpu_u32 nc,fnpu_u32 at,fnpu_u32 count) {
    HVX_Vector bad=Q6_V_vzero();
    for(fnpu_u32 row=0;row<32;row++) {
        HVX_Vector v=read_row((const volatile fnpu_u16 *)0x31001000u,row);
        bad=Q6_V_vor_VV(bad,nonfinite(v));
        if(nc+row<r[7]) {
            v=addf(v,Q6_V_vsplat_R(RG_BIAS(r[17],nc+row)));
            bad=Q6_V_vor_VV(bad,nonfinite(v));
            if(r[18]==1) v=clip6(v);
            rg_store((float *)RG_PTR(r[3]+((nc+row)*r[20]+at)*4),count,v);
        }
    }
    rg_bad(bad);
}
#endif
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_run(c,1)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);const volatile fnpu_u32 *d=fnpu_input(c);
#ifdef RG_CONV_FAST
    /* The bias span check of the separate post operator. */
    if(r[17]<RG_WEIGHTS||r[17]>RG_WEIGHTS+RG_WEIGHT_BYTES||r[7]*4>RG_WEIGHTS+RG_WEIGHT_BYTES-r[17]) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
#endif
    fnpu_u32 k=r[4]*(r[0]==2?9u:1u),depth=(k+31)>>5;
    volatile HVX_Vector *b=(volatile HVX_Vector *)0x31000800u;
    volatile fnpu_u32 *scales=(volatile fnpu_u32 *)0x31001800u;
    for(fnpu_u32 i=0;i<64;i++) scales[i]=i<32?0x3c00u:0;
    rg_accept(c);rg_u64 start=rg_ticks();
    for(fnpu_u32 u=0;RG_MORE(u,d);u++) {
        fnpu_u32 nc=RG_STATE[21],p=RG_STATE[22],y=RG_STATE[23];
        fnpu_u32 rem=r[0]==2?r[9]-p:r[8]*r[9]-p,count=rem<32?rem:32;
        fnpu_hmx_clear();
        for(fnpu_u32 t=0;t<depth;t++) {
            weight_tile(r,k,t*32,nc);
            for(fnpu_u32 v=0;v<16;v++) b[v]=((const volatile HVX_Vector *)RG_PTR(RG_SCRATCH))[t*16+v];
            __asm__ volatile("barrier" ::: "memory");
            fnpu_hmx_load((const fnpu_u16 *)0x31000000u,(const fnpu_u16 *)b);
        }
        fnpu_hmx_store((fnpu_u16 *)0x31001000u,(const void *)scales);
#ifdef RG_CONV_FAST
        hmx_rows(r,nc,(r[0]==2?y*r[9]:0)+p,count);
#else
        HVX_Vector bad=Q6_V_vzero();
        for(fnpu_u32 row=0;row<32;row++) {
            HVX_Vector v=read_row((const volatile fnpu_u16 *)0x31001000u,row);
            bad=Q6_V_vor_VV(bad,nonfinite(v));
            if(nc+row<r[7]) {
                rg_store((float *)RG_PTR(r[3]+((nc+row)*r[20]+(r[0]==2?y*r[9]:0)+p)*4),count,v);
            }
        }
        rg_bad(bad);
#endif
        RG_STATE[20]++;RG_STATE[30]++;
        rg_fault(c);
        nc+=32;
        if(nc<r[7]) RG_STATE[21]=nc;
        else {
            RG_STATE[21]=0;RG_STATE[18]=0;
#ifdef RG_CONV_FAST
            /* The post operator's last step for this group of positions. */
            if(RG_STATE[20]==r[24]) {
                if(r[18]==2) {RG_STATE[24]=2;RG_STATE[20]=RG_STATE[22]=RG_STATE[23]=0;}
                else rg_done(r);
            } else {
                p+=32;
                if(r[0]==2&&p>=r[9]) {p=0;y++;}
                RG_STATE[22]=p;RG_STATE[23]=y;RG_STATE[24]=0;
            }
#else
            RG_STATE[24]=3;
#endif
            break;
        }
        if(RG_EXPIRED(start)) break;
    }
    return RG_RETURN(c,FNPU_OK);
}
