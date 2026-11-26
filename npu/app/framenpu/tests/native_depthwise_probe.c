/* Real-weight isolated depthwise tiles: same borrowed-arena interface, three
 * compiled arithmetic modes. Padding/quantization is explicit host preparation.
 * All rows are staged into the entry's saved 8 KiB VTCM. No resident aliases.
 * mode0: original FP32/QFloat/FP32 with zero identities and product checks;
 * mode1: FP32 inputs, QFloat32 reduction, convert only at the end;
 * mode2: FP16 inputs/products to QFloat32 pair, convert only at the end.
 * Each row is a bounded checkpoint; reject partial runs rather than retrying. */
#include "landmark_common.h"
#ifndef ND_MODE
#define ND_MODE 0
#endif
#define ROW_BYTES (ND_MODE==2?512u:1024u)
#define ND_COEF ((volatile fnpu_u32 *)0x31001a80u)
#define ND_OUT ((volatile HVX_Vector *)0x31001800u)
#define ND_TMP ((volatile HVX_Vector *)0x31001b80u)
#define ND_TIME ((volatile fnpu_u32 *)0x31001d00u)
typedef unsigned long long nd_u64;
static nd_u64 nd_ticks(void) {nd_u64 t;__asm__ volatile("%0 = c31:30":"=r"(t)::"memory");return t;}
fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d=fnpu_input(c);
    if(!valid(c)||c->opcode!=99||c->input_bytes<256||c->params[0]!=1||c->params[1]||c->params[2]) return FNPU_BAD_ARGUMENT;
    fnpu_u32 channels=d[1];
#ifdef ND_CASE
#if ND_CASE==0
    enum {h=112,w=112,oh=112,ow=112,k=3,stride=1,pad=1,tail=1};
#elif ND_CASE==1
    enum {h=112,w=112,oh=56,ow=56,k=3,stride=2,pad=0,tail=1};
#elif ND_CASE==2
    enum {h=14,w=14,oh=14,ow=14,k=5,stride=1,pad=2,tail=2};
#elif ND_CASE==3
    enum {h=7,w=7,oh=7,ow=7,k=5,stride=1,pad=2,tail=2};
#else
#error invalid ND_CASE
#endif
    enum {elem=ND_MODE==2?2:4,clip=1,srcrow=((w+pad+tail+128/elem-1)/(128/elem))*128,
          srcchan=(h+pad+tail)*srcrow,dstrow=((ow+63)/64)*256,dstchan=oh*dstrow};
    if(d[2]!=h||d[3]!=w||d[4]!=oh||d[5]!=ow||d[6]!=k||d[7]!=stride||d[8]!=pad||d[9]!=clip||
       d[10]!=srcrow||d[11]!=srcchan||d[12]!=dstrow||d[13]!=dstchan||d[14]!=tail) return FNPU_BAD_ARGUMENT;
#else
    fnpu_u32 h=d[2],w=d[3],oh=d[4],ow=d[5],k=d[6],stride=d[7],pad=d[8],clip=d[9],srcrow=d[10],srcchan=d[11],dstrow=d[12],dstchan=d[13];
    fnpu_u32 elem=ND_MODE==2?2:4,tail=d[14];
#endif
    if(d[0]!=0x4e445031u||!channels||channels>8||!h||h>112||!w||w>112||!oh||!ow||
       (k!=3&&k!=5)||(stride!=1&&stride!=2)||pad>k/2||tail>k/2||clip>1||h+pad+tail<k||w+pad+tail<k||
       oh!=((h+pad+tail-k)>>(stride-1))+1||ow!=((w+pad+tail-k)>>(stride-1))+1||
       srcrow!=((w+pad+tail+128/elem-1)/(128/elem))*128u||srcchan!=(h+pad+tail)*srcrow||
       dstrow!=((ow+63)/64)*256u||dstchan!=oh*dstrow||
       c->input_bytes!=128+channels*(128+srcchan)||c->output_bytes!=256+channels*dstchan) return FNPU_BAD_ARGUMENT;
    for(fnpu_u32 i=15;i<32;i++) if(d[i]) return FNPU_BAD_ARGUMENT;
    const unsigned char *base=fnpu_input(c);
    const unsigned char *input=base+128+channels*128;
    unsigned char *output=(unsigned char *)fnpu_output(c)+256;
    volatile fnpu_u32 *meta=fnpu_output(c);
    for(fnpu_u32 i=0;i<64;i++) meta[i]=0;
    for(fnpu_u32 i=0;i<16;i++) ND_TIME[i]=0;
    *(volatile nd_u64 *)(ND_TIME+4)=nd_ticks();
    for(fnpu_u32 ch=0;ch<channels;ch++) {
        const fnpu_u16 *weights=(const fnpu_u16 *)(base+128+ch*128+4);
        fnpu_u32 bias=*(const volatile fnpu_u32 *)(base+128+ch*128);
        if((bias&0x7f800000u)==0x7f800000u) return FNPU_BAD_ARGUMENT;
        for(fnpu_u32 t=0;t<k*k;t++) {
            fnpu_u32 half=weights[t],mag=half&0x7fff,exp=mag&0x7c00;
            /* Same normal-or-zero coefficient invariant as the pinned graph. */
            if((!exp&&mag)||exp==0x7c00) return FNPU_BAD_ARGUMENT;
            ND_COEF[t]=ND_MODE==2?half:((half&0x8000)<<16)|(mag?(mag<<13)+0x38000000u:0);
        }
        for(fnpu_u32 y=0;y<oh;y++) {
            fnpu_u32 t0=(fnpu_u32)nd_ticks();
            HVX_Vector bad=Q6_V_vzero();
            for(fnpu_u32 i=0;i<dstrow/128;i++) ND_OUT[i]=Q6_V_vzero();
            for(fnpu_u32 ky=0;ky<k;ky++) {
                volatile HVX_Vector *to=(volatile HVX_Vector *)(0x31000000u+ky*ROW_BYTES);
                const volatile HVX_Vector *from=(const volatile HVX_Vector *)(input+ch*srcchan+(y*stride+ky)*srcrow);
                for(fnpu_u32 v=0;v<ROW_BYTES/128;v++) {
                    HVX_Vector data=v<srcrow/128?from[v]:Q6_V_vzero();
                    to[v]=data;
#if ND_MODE==2
                    HVX_Vector exp=Q6_V_vsplat_R(0x7c007c00u);
                    bad=Q6_V_vor_VV(bad,Q6_V_vmux_QVV(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(data,exp),exp),Q6_V_vsplat_R(-1),Q6_V_vzero()));
#else
                    bad=Q6_V_vor_VV(bad,nonfinite(data));
#endif
                }
            }
            fnpu_u32 t1=(fnpu_u32)nd_ticks();ND_TIME[0]+=t1-t0;
            for(fnpu_u32 x=0;x<ow;x+=(ND_MODE==2?64u:32u)) {
                HVX_Vector sum=Q6_V_vsplat_R(bias);
#if ND_MODE!=0
                sum=Q6_Vqf32_vadd_VsfVsf(sum,Q6_V_vzero());
#endif
#if ND_MODE==2
                HVX_Vector other=sum;
#endif
                for(fnpu_u32 ky=0;ky<k;ky++) for(fnpu_u32 kx=0;kx<k;kx++) {
                    const unsigned char *at=(const unsigned char *)(0x31000000u+ky*ROW_BYTES+(x*stride+kx)*elem);
                    HVX_Vector v=*(const volatile HVX_UVector *)at;
                    if(stride==2) {
                        HVX_Vector b=*(const volatile HVX_UVector *)(at+128);
                        v=Q6_V_lo_W(Q6_W_vdeal_VVR(b,v,ND_MODE==2?-2:-4));
                    }
#if ND_MODE==2
                    HVX_VectorPair product=Q6_Wqf32_vmpy_VhfVhf(v,Q6_Vh_vsplat_R(ND_COEF[ky*k+kx]));
                    sum=Q6_Vqf32_vadd_Vqf32Vqf32(sum,Q6_V_lo_W(product));
                    other=Q6_Vqf32_vadd_Vqf32Vqf32(other,Q6_V_hi_W(product));
#elif ND_MODE==1
                    HVX_Vector product=Q6_Vqf32_vmpy_VsfVsf(v,Q6_V_vsplat_R(ND_COEF[ky*k+kx]));
                    sum=Q6_Vqf32_vadd_Vqf32Vqf32(sum,product);
#else
                    HVX_Vector product=mulf(v,Q6_V_vsplat_R(ND_COEF[ky*k+kx]));
                    bad=Q6_V_vor_VV(bad,nonfinite(product));sum=addf(sum,product);
#endif
                }
#if ND_MODE!=0
                sum=Q6_Vsf_equals_Vqf32(sum);
#endif
                bad=Q6_V_vor_VV(bad,nonfinite(sum));if(clip) sum=clip6(sum);
                ND_OUT[x/32]=sum;
#if ND_MODE==2
                other=Q6_Vsf_equals_Vqf32(other);
                bad=Q6_V_vor_VV(bad,nonfinite(other));if(clip) other=clip6(other);
                ND_OUT[x/32+1]=other;
#endif
            }
            fnpu_u32 t2=(fnpu_u32)nd_ticks();ND_TIME[1]+=t2-t1;
            for(fnpu_u32 v=0;v<dstrow/128;v++) ((volatile HVX_Vector *)(output+ch*dstchan+y*dstrow))[v]=ND_OUT[v];
            *ND_TMP=bad;
            fnpu_u32 flags=0;for(fnpu_u32 i=0;i<32;i++) flags|=((const volatile fnpu_u32 *)ND_TMP)[i];ND_TIME[3]|=flags;
            fnpu_u32 t3=(fnpu_u32)nd_ticks();ND_TIME[2]+=t3-t2;ND_TIME[6]++;
            fault_control(c);
            if(t3-ND_TIME[4]>384000u) goto finish;
        }
    }
finish:;
    nd_u64 end=nd_ticks(),begin=*(const volatile nd_u64 *)(ND_TIME+4);fnpu_u32 pc;__asm__ volatile("%0 = pc":"=r"(pc)::"memory");
    meta[0]=0x4e445031u;meta[1]=pc;meta[2]=ND_MODE;meta[3]=ND_TIME[6];meta[4]=ND_TIME[3];
    meta[5]=channels;meta[6]=oh;meta[7]=ow;meta[8]=begin;meta[9]=begin>>32;
    meta[10]=end;meta[11]=end>>32;meta[12]=ND_TIME[0];meta[13]=0;
    meta[14]=ND_TIME[1];meta[15]=0;meta[16]=ND_TIME[2];meta[17]=0;
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
