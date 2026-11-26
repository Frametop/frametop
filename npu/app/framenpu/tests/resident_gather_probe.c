/* Independent gather controls in the ordinary borrowed job arena. */
#include "resident_graph_gather.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    if(c->magic!=FNPU_MAGIC||c->abi!=FNPU_ABI||c->flags!=3||c->opcode!=92||
       c->input_bytes!=1680||c->output_bytes!=128||c->output_offset!=1792||
       !c->params[0]||c->params[0]>32||c->params[1]-1>1||c->params[2]>70||
       c->params[3]>2||c->params[4]>2||c->params[5]>2) return FNPU_BAD_ARGUMENT;
    fnpu_u32 r[32];
    r[1]=(fnpu_u32)fnpu_input(c)-0xf0000000u;
    r[4]=2;r[5]=3;r[6]=70;r[19]=210;r[12]=r[13]=c->params[1];r[14]=r[15]=1;
    *(volatile HVX_Vector *)fnpu_output(c)=rg_gather(r,1,c->params[3],c->params[2],c->params[4],c->params[5],c->params[0]);
    __asm__ volatile("barrier" ::: "memory");return FNPU_OK;
}
