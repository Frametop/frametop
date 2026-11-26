/* Optional profiler. Baseline arithmetic helpers are retained in this image.
 * Coarse mode calls them directly. Detailed single-band mode samples 1/8
 * blocks, with phase 0..7 covering every block across eight invocations.
 * Mode 3 brackets every block in one single-band request as a cross-check.
 * HMX store already has a completion barrier (sdk/hmx.S). Brackets include
 * that clear/load/store transaction, never an estimate from issued MACs.
 * Log FR(14016)..FR(14144) is disjoint from all live prefix buffers. */
#ifndef TEACHER_STEM_PROFILE_H
#define TEACHER_STEM_PROFILE_H
#define TP_LOG ((volatile fnpu_u32 *)FR(14016))
#define TP_BAND(b) (TP_LOG + 64 + 448*(b))
#define TP_STAGE(b,s) (TP_BAND(b) + 80*(s))
#define TP_MAGIC 0x31505354u
static __attribute__((noinline)) void tp_layer(rs_op *d, const fnpu_u32 *r, fnpu_u32 band,
        fnpu_u32 stage, fnpu_u32 off, fnpu_u32 npar, fnpu_u32 cin, fnpu_u32 cout,
        fnpu_u32 kind, fnpu_u32 clip, fnpu_u32 in, fnpu_u32 out) {
    d->kind=kind;d->cpi=cin;d->cpo=cout;d->nk=kind==1?1:3;
    d->clip=clip;d->nbin=89;d->blk0=8;d->nj=72;d->ws=114;
    d->vpar=TS_PAR;d->npar=npar;d->dpar=0;
    d->vin=(fnpu_u32)FR(in);d->vout=(fnpu_u32)FR(out);d->vres=0;
    d->bias=(fnpu_u32)FR(TS_PAR+(kind==1?((cout+15)/16)*16:(cin*9+31)/32));
    d->scr=(fnpu_u32)FR(TS_PAR+npar);
    fnpu_u32 a=rs_ticks();
    rs_copy(FR(TS_PAR),(const volatile HVX_Vector *)(RS_DDR+r[6])+band*TS_BLOCK+off,npar);
    fnpu_u32 b=rs_ticks();
    rs_zero(FR(out),cout*89);
    fnpu_u32 c=rs_ticks();
    TP_STAGE(band,stage)[0]=b-a;TP_STAGE(band,stage)[1]=c-b;
}
static __attribute__((noinline)) void tp_stem(const rs_op *d, fnpu_u32 band, fnpu_u32 phase) {
    volatile fnpu_u32 *p=TP_STAGE(band,0);
#pragma clang loop unroll(disable)
    for (fnpu_u32 j=0;j<72;j++) {
        fnpu_u32 sample=phase==8 || (j&7)==phase,a=0,b=0,c=0,ready=0;
        if (sample) a=rs_ticks();
        ts_rgb_tile(d,band,j);
        if (sample) b=rs_ticks();
        __asm__ volatile("barrier" ::: "memory");
        if (sample) ready=rs_ticks();
        fnpu_hmx_clear();
        fnpu_hmx_load((const fnpu_u16 *)FR(TS_PAR),(const fnpu_u16 *)P(scr));
        fnpu_hmx_store((fnpu_u16 *)LV(0),(const void *)RS_SCALE);
        if (sample) c=rs_ticks();
#pragma clang loop unroll(disable)
        for (fnpu_u32 pr=0;pr<12;pr++) rs_post(d,LV(0)[pr],pr,j);
        if (sample) {
            fnpu_u32 e=rs_ticks();
            p[3]+=b-a;p[4]+=c-ready;p[7]+=e-c;p[8+j]+=e-a;
            TP_BAND(band)[403]+=ready-b;
        }
    }
}
static __attribute__((noinline)) void tp_pw(const rs_op *d, fnpu_u32 band, fnpu_u32 stage, fnpu_u32 phase) {
    volatile fnpu_u32 *p=TP_STAGE(band,stage);
    volatile HVX_Vector *bt=P(scr);
#pragma clang loop unroll(disable)
    for (fnpu_u32 j=0;j<72;j++) {
        fnpu_u32 sample=phase==8 || (j&7)==phase,a=0,b=0,total=0,pack_ticks=0,hmx_ticks=0,post_ticks=0,fence_ticks=0,ready=0;
        if (sample) a=rs_ticks();
        const volatile HVX_Vector *src=P(vin)+8+j;
#pragma clang loop unroll(disable)
        for (fnpu_u32 pair=0;pair<16;pair++) bt[pair]=pair<d->cpi?src[pair*89]:LV(57)[0];
        if (sample) { b=rs_ticks();pack_ticks=b-a;total=b-a; }
        __asm__ volatile("barrier" ::: "memory");
        if (sample) { ready=rs_ticks();fence_ticks=ready-b;total+=fence_ticks; }
#pragma clang loop unroll(disable)
        for (fnpu_u32 o=0;16*o<d->cpo;o++) {
            if (sample) a=o?rs_ticks():ready;
            fnpu_hmx_clear();
            fnpu_hmx_load((const fnpu_u16 *)FR(TS_PAR+o*16),(const fnpu_u16 *)bt);
            fnpu_hmx_store((fnpu_u16 *)LV(0),(const void *)RS_SCALE);
            if (sample) b=rs_ticks();
#pragma clang loop unroll(disable)
            for (fnpu_u32 rr=0;rr<16 && 16*o+rr<d->cpo;rr++) rs_post(d,LV(0)[rr],16*o+rr,j);
            if (sample) {
                fnpu_u32 e=rs_ticks();
                hmx_ticks+=b-a;post_ticks+=e-b;total+=e-a;
            }
        }
        /* Keep profiler VTCM writes outside the measured packing/HMX/post
         * sequence: a mid-tile log write can move a fence's wait time. */
        if (sample) {
            p[3]+=pack_ticks;p[4]+=hmx_ticks;p[7]+=post_ticks;p[8+j]+=total;
            TP_BAND(band)[403+stage]+=fence_ticks;
        }
    }
}
static __attribute__((noinline)) void tp_dw(const rs_op *d, fnpu_u32 band, fnpu_u32 stage, fnpu_u32 phase) {
    volatile fnpu_u32 *p=TP_STAGE(band,stage);
    const volatile fnpu_u32 *coef=(const volatile fnpu_u32 *)FR(TS_PAR);
    fnpu_u32 prep=rs_ticks();
    HVX_Vector qzero=Q6_Vqf32_vadd_VsfVsf(Q6_V_vzero(),Q6_V_vzero());
    RS_ACC(4)[0]=qzero;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i=0;i<d->cpi*9;i++) P(scr)[i]=Q6_V_vsplat_R(coef[i]);
    p[5]=rs_ticks()-prep;
#pragma clang loop unroll(disable)
    for (fnpu_u32 cp=0;cp<d->cpi;cp++) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 j=0;j<72;j++) {
            if (d->cpi==32 && !ts_needed_block(j)) continue;
            fnpu_u32 sample=phase==8 || (j&7)==phase,a0=0,b0=0;
            if (sample) a0=rs_ticks();
            HVX_Vector a=RS_ACC(4)[0],b=a;
            const char *s=(const char *)P(vin)+cp*89*128+4*(256+32*j-115);
#pragma clang loop unroll(disable)
            for (fnpu_u32 tap=0;tap<9;tap++) {
                HVX_Vector v=*(const rs_uv *)(s+4*((tap/3)*114+tap%3));
                HVX_VectorPair product=Q6_Wqf32_vmpy_VhfVhf(v,P(scr)[cp*9+tap]);
                a=Q6_Vqf32_vadd_Vqf32Vqf32(a,Q6_V_lo_W(product));
                b=Q6_Vqf32_vadd_Vqf32Vqf32(b,Q6_V_hi_W(product));
            }
            if (sample) b0=rs_ticks();
            rs_post(d,Q6_Vhf_equals_Wqf32(Q6_W_vcombine_VV(b,a)),cp,j);
            if (sample) {
                fnpu_u32 e=rs_ticks();
                p[6]+=b0-a0;p[7]+=e-b0;p[8+j]+=e-a0;
            }
        }
    }
}
static __attribute__((noinline)) fnpu_u32 tp_run(const fnpu_context *c, const fnpu_u32 *r, fnpu_u32 start) {
    (void)c;
    rs_op d;
    rs_zero(FR(14016),128);
    TP_LOG[0]=TP_MAGIC;TP_LOG[1]=2;TP_LOG[2]=r[19];TP_LOG[3]=r[20];
    TP_LOG[4]=r[1];TP_LOG[5]=r[2];TP_LOG[13]=256;TP_LOG[14]=0xffffffffu;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i=0;i<256;i++) {
        fnpu_u32 a=rs_ticks(),b=rs_ticks(),dt=b-a;
        TP_LOG[12]+=dt;
        if (dt<TP_LOG[14]) TP_LOG[14]=dt;
        if (dt>TP_LOG[15]) TP_LOG[15]=dt;
    }
    fnpu_u32 begin=rs_ticks(),a=begin,b;
    rs_copy(FR(TS_RGB),(const volatile HVX_Vector *)(RS_DDR+r[5]),TS_INPUT_VECTORS);
    b=rs_ticks();TP_LOG[7]=b-a;a=b;
    rs_zero(FR(TS_OUT8),3616);rs_zero(FR(TS_OUT10),904);
    b=rs_ticks();TP_LOG[8]=b-a;
#pragma clang loop unroll(disable)
    for (fnpu_u32 band=r[1];band<r[2];band++) {
        if (rs_ticks()-start>384000u) return 0;
        fnpu_u32 band_start=rs_ticks();
        tp_layer(&d,r,band,0,0,112,14,12,1,1,TS_RGB,TS_A);
        a=rs_ticks();
        if (r[19]>=2) tp_stem(&d,band,r[19]==3?8:r[20]);else ts_stem(&d,band);
        b=rs_ticks();TP_STAGE(band,0)[2]=b-a;
        tp_layer(&d,r,band,1,112,100,12,12,2,1,TS_A,TS_B);
        a=rs_ticks();
        if (r[19]>=2) tp_dw(&d,band,1,r[19]==3?8:r[20]);else ts_dw(&d);
        b=rs_ticks();TP_STAGE(band,1)[2]=b-a;
        tp_layer(&d,r,band,2,224,112,12,8,1,0,TS_B,TS_A);
        a=rs_ticks();
        if (r[19]>=2) tp_pw(&d,band,2,r[19]==3?8:r[20]);else rs_pw(&d);
        b=rs_ticks();TP_STAGE(band,2)[2]=b-a;
        a=rs_ticks();ts_skip(band);b=rs_ticks();TP_BAND(band)[400]=b-a;
        tp_layer(&d,r,band,3,336,176,8,32,1,1,TS_A,TS_B);
        a=rs_ticks();
        if (r[19]>=2) tp_pw(&d,band,3,r[19]==3?8:r[20]);else rs_pw(&d);
        b=rs_ticks();TP_STAGE(band,3)[2]=b-a;
        tp_layer(&d,r,band,4,512,145,32,32,2,1,TS_B,TS_A);
        a=rs_ticks();
        if (r[19]>=2) tp_dw(&d,band,4,r[19]==3?8:r[20]);else ts_dw(&d);
        b=rs_ticks();TP_STAGE(band,4)[2]=b-a;
        a=rs_ticks();ts_downsample(band);b=rs_ticks();TP_BAND(band)[401]=b-a;
        TP_BAND(band)[402]=rs_ticks()-band_start;
    }
    a=rs_ticks();rs_copy((volatile HVX_Vector *)(RS_DDR+r[9]),FR(TS_OUT8),3616);
    b=rs_ticks();TP_LOG[9]=b-a;a=b;
    rs_copy((volatile HVX_Vector *)(RS_DDR+r[10]),FR(TS_OUT10),904);
    b=rs_ticks();TP_LOG[10]=b-a;TP_LOG[6]=b-begin;
    a=rs_ticks();rs_copy((volatile HVX_Vector *)(RS_DDR+r[18]),FR(14016),128);
    b=rs_ticks();((volatile fnpu_u32 *)(RS_DDR+r[18]))[11]=b-a;
    __asm__ volatile("barrier" ::: "memory");
    return 1;
}
#endif
