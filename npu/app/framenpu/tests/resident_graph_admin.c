#include "resident_graph.h"
static int program_valid(void) {
    for(fnpu_u32 index=0;index<63;index++) {
        const volatile fnpu_u32 *r=(const volatile fnpu_u32 *)RG_PTR(RG_PROGRAM+index*128);
        if(!r[0]||r[0]>5||r[18]>2||r[23]>2) return 0;
        for(fnpu_u32 j=4;j<10;j++) if(r[j]-1>=(j==4||j==7?704u:224u)) return 0;
        if(r[19]!=r[5]*r[6]||r[20]>50176||r[20]<r[8]*r[9]||
           r[4]*r[19]*4>r[21]||((r[7]-1)*r[20]+r[8]*r[9])*4>r[22]||
           !rg_span(r[1],r[21])||!rg_span(r[3],r[22])) return 0;
        if(r[0]<=3) {
            fnpu_u32 k=r[0]==2?r[4]*9:r[0]==3?r[10]*r[11]:r[4];
            fnpu_u32 rows=r[0]==3?r[7]:(r[7]+1)&~1u;
            if(r[0]<3&&r[26]!=((k+31)>>5)*16) return 0;
            if(!k||k>704||r[16]<RG_WEIGHTS||r[16]>RG_WEIGHTS+RG_WEIGHT_BYTES||
               rows*k*2>RG_WEIGHTS+RG_WEIGHT_BYTES-r[16]||
               r[17]<RG_WEIGHTS||r[17]>RG_WEIGHTS+RG_WEIGHT_BYTES||r[7]*4>RG_WEIGHTS+RG_WEIGHT_BYTES-r[17]) return 0;
        }
        if(r[0]==3&&(r[7]!=r[4]||!r[10]||r[10]>5||!r[11]||r[11]>5||r[12]-1>1||r[13]-1>1)) return 0;
    }
    return 1;
}
fnpu_u32 frame_job(const fnpu_context *c) {
    if(!rg_header(c,0)) return rg_finish(c,FNPU_BAD_ARGUMENT);
    const volatile fnpu_u32 *d=fnpu_input(c);
    fnpu_u32 op=d[2],off=d[21],len=d[22];
    if(op==0) {
        for(fnpu_u32 i=0;i<32;i++) RG_STATE[i]=0;
        for(fnpu_u32 i=4;i<18;i++) RG_STATE[i]=d[i];
        RG_STATE[0]=1;RG_STATE[1]=1;RG_STATE[2]=RG_MAGIC;
        RG_STATE[25]=RG_PROGRAM;
    } else if(op==1) {
        if(RG_STATE[0]!=1||off!=RG_STATE[25]||!len||len>32768||
           (off&127)||(len&127)||off>RG_END||len>RG_END-off) return rg_finish(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);
        for(fnpu_u32 addr=off;addr<off+len;addr+=32) {
            fnpu_u32 cached=0x87000000u+addr;
            __asm__ volatile("dccleaninva(%0)\n\tsyncht"::"r"(cached):"memory");
        }
        RG_STATE[25]=off+len;
    } else if(op==2) {
        if(RG_STATE[0]!=1||RG_STATE[25]!=RG_END||off!=RG_PROGRAM+RG_STATE[26]||
           !len||len>32768||len>RG_ACT-off) return rg_finish(c,FNPU_BAD_ARGUMENT);
        if(off+len==RG_ACT&&!program_valid()) return rg_finish(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);RG_STATE[26]+=len;
        if(off+len==RG_ACT) {RG_STATE[0]=2;RG_STATE[3]=63;RG_STATE[19]=63;}
    } else if(op==3) {
        if(RG_STATE[0]!=2||RG_STATE[19]!=63||off||len) return rg_finish(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);RG_STATE[0]=3;RG_STATE[19]=0;
        RG_STATE[20]=RG_STATE[21]=RG_STATE[22]=RG_STATE[23]=RG_STATE[24]=RG_STATE[27]=0;
        RG_STATE[28]++;
    } else if(op==4) {
        if(RG_STATE[0]!=3||RG_STATE[19]||RG_STATE[20]||off!=RG_ACT+RG_STATE[27]||
           !len||len>RG_INPUT_CHUNK||len>602112-RG_STATE[27]) return rg_finish(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);RG_STATE[27]+=len;
#ifdef RG_LOADER
    } else if(op==7||op==8) {
        /* Seal (7) the uploaded operator image or retire (8) it before the
         * arena is restored. Nothing writes this span through the cached
         * alias, so stale lines are discarded, never written back. */
        if(off!=RG_CODE||len!=RG_CODE_BYTES||RG_STATE[0]==6||
           (op==7?RG_STATE[0]!=2||RG_STATE[19]!=63||RG_STATE[31]:RG_STATE[31]!=RG_SEAL))
            return rg_finish(c,FNPU_BAD_ARGUMENT);
        rg_accept(c);
        RG_STATE[31]=0;
        for(fnpu_u32 addr=off;addr<off+len;addr+=32) {
            fnpu_u32 cached=0x87000000u+addr;
            __asm__ volatile("dcinva(%0)\n\ticinva(%0)\n\tisync"::"r"(cached):"memory");
        }
        if(op==7) RG_STATE[31]=RG_SEAL;
#endif
    } else if(op==6) {
#ifdef RG_LOADER
        if(RG_STATE[31]) return rg_finish(c,FNPU_BAD_ARGUMENT);
#endif
        if(len) {
            if(len>32768||off<RG_PROGRAM||off>RG_STATE[25]||len!=RG_STATE[25]-off)
                return rg_finish(c,FNPU_BAD_ARGUMENT);
            rg_accept(c);RG_STATE[0]=6;RG_STATE[25]=off;
        } else {
            if(off||RG_STATE[25]!=RG_PROGRAM) return rg_finish(c,FNPU_BAD_ARGUMENT);
            rg_accept(c);RG_STATE[0]=5;
        }
    } else return rg_finish(c,FNPU_BAD_ARGUMENT);
    return rg_finish(c,FNPU_OK);
}
