/* Small-map depthwise rows computed several at a time.
 *
 * A 7- or 14-wide output row fills a quarter or half of a vector, yet every
 * row pays the whole gather and tap loop. This operator computes four 7-wide
 * or two 14-wide output rows of one channel in one pass, each row in its own
 * 32- or 64-byte slot, and then hands the rows out one work unit at a time.
 * A unit is still one output row: it stores only its own row, advances the
 * same state and reaches the same budget and guarded-fault points, so the
 * host's unit replay is unchanged. Rows computed ahead wait in saved VTCM and
 * are dropped when the request ends; the next request recomputes them from
 * the current row. Every lane sees the same taps in the same order as the
 * ordinary operator.
 *
 * The pass and the per-row hand-out are separate functions that exchange the
 * rows only through VTCM, so the unit loop holds no vector across a call and
 * nothing is spilled to the job stack.
 *
 * Input rows are staged at RG_ROWS, 256 bytes each: the row is written at its
 * left padding inside a zeroed pair of vectors, so a tap is one unaligned
 * VTCM load and padding needs no mask. At most (4-1)*2+5 = 11 rows end at
 * 0xb00, below the coefficient broadcasts at 0xc00. Lanes of a slot beyond
 * the row width and rows beyond the tensor carry values no unit consumes. */
#ifndef FNPU_RESIDENT_DW_TILE_H
#define FNPU_RESIDENT_DW_TILE_H
#define DWT_SUM (*(volatile HVX_Vector *)0x31001880u)
#define DWT_BAD (*(volatile HVX_Vector *)0x31001900u)
/* rg_batch selects this operator only for a record this layout can hold. */
static int dwt_fits(rg_record *r) {
    /* Rows are recomputed after a request ends mid-pass, so no output byte
     * may alias an input byte. */
    return (r[9]==7||r[9]==14)&&r[8]==r[9]&&!r[23]&&r[6]+r[15]<=32&&r[13]-1<=1&&r[12]==r[13]&&
        (r[1]+r[21]<=r[3]||r[3]+r[22]<=r[1]);
}
/* All rows of one pass: slots output rows starting at the current row. */
static __attribute__((noinline)) void dwt_pass(rg_record *r,fnpu_u32 slots,fnpu_u32 bytes) {
    fnpu_u32 channel=RG_STATE[21],taps=r[10]*r[11];
    const volatile HVX_Vector *broadcasts=dw_broadcasts((const fnpu_u16 *)RG_PTR(r[16])+channel*taps,taps);
    fnpu_u32 iy=RG_STATE[22]*r[12]-r[14],rows=(slots-1)*r[12]+r[10];
    volatile HVX_Vector *to=(volatile HVX_Vector *)RG_ROWS;
    for(fnpu_u32 j=0;j<rows;j++,iy++,to+=2) {
        to[0]=Q6_V_vzero();to[1]=Q6_V_vzero();
        /* Unsigned: rows above the tensor wrap past its height. */
        if(iy<r[5]) {
            HVX_Vector v=rg_loadu(RG_PTR(r[1]+(channel*r[19]+iy*r[6])*4u));
            *(volatile HVX_UVector *)((fnpu_u32)to+r[15]*4u)=
                Q6_V_vmux_QVV(Q6_Q_vsetq_R(r[6]*4u),v,Q6_V_vzero());
        }
    }
    __asm__ volatile("" ::: "memory");
    HVX_Vector sum=Q6_V_vsplat_R(RG_BIAS(r[17],channel));
    HVX_VectorPred bad_pred=Q6_Q_vsetq_R(0);
    for(fnpu_u32 ky=0;ky<r[10];ky++) for(fnpu_u32 kx=0;kx<r[11];kx++) {
        HVX_Vector input=Q6_V_vzero();
        for(fnpu_u32 row=slots;row--;) {
            HVX_Vector v=*(const volatile HVX_UVector *)(RG_ROWS+(row*r[12]+ky)*256u+kx*4u);
            if(r[13]==2) v=Q6_V_lo_W(Q6_W_vdeal_VVR(Q6_V_vzero(),v,-4));
            input=Q6_V_vmux_QVV(Q6_Q_vsetq_R(bytes),v,Q6_V_vror_VR(input,128u-bytes));
        }
        sum=dw_add_checked(sum,mulf(input,*broadcasts++),&bad_pred);
    }
    DWT_SUM=sum;
    DWT_BAD=Q6_V_vor_VV(Q6_V_vmux_QVV(bad_pred,Q6_V_vsplat_R(-1),Q6_V_vzero()),nonfinite(sum));
}
/* One work unit: the first waiting row is checked, clipped and stored. */
static __attribute__((noinline)) void dwt_row(rg_record *r,fnpu_u32 bytes) {
    HVX_Vector sum=DWT_SUM,bad=DWT_BAD;
    DWT_SUM=Q6_V_vror_VR(sum,bytes);DWT_BAD=Q6_V_vror_VR(bad,bytes);
    rg_bad(Q6_V_vmux_QVV(Q6_Q_vsetq_R(r[9]*4u),bad,Q6_V_vzero()));
    if(r[18]) sum=clip6(sum);
    rg_store((float *)RG_PTR(r[3]+(RG_STATE[21]*r[20]+RG_STATE[22]*r[9])*4u),r[9],sum);
}
fnpu_u32 rg_op_dwt(const fnpu_context *c) {
    if(!rg_run(c,3)) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    RG_RECORD(r);
    if(r[10]>5||r[11]>5) return RG_RETURN(c,FNPU_BAD_ARGUMENT);
    fnpu_u32 slots=r[9]==7?4u:2u,bytes=r[9]==7?32u:64u,ahead=0;
    while(RG_STATE[30]<RG_BATCH_UNITS) {
        if(!ahead) {dwt_pass(r,slots,bytes);ahead=slots;}
        dwt_row(r,bytes);ahead--;
        RG_STATE[20]++;RG_STATE[30]++;rg_next_row(r);
        rg_fault(c);
        if(RG_STATE[20]==r[24]) {rg_done(r);break;}
        if(RG_EXPIRED(0)) break;
        /* Rows computed ahead belong to one channel. */
        if(!RG_STATE[22]) ahead=0;
    }
    return RG_RETURN(c,FNPU_OK);
}
#endif
