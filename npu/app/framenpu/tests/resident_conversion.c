/* Diagnostic/qualification only: observe v75 conversion's halfword order. */
#include "resident_fp16.h"
fnpu_u32 frame_job(const fnpu_context *c) {
    fnpu_u32 pairs=c->params[0];
    if(c->magic!=FNPU_MAGIC || c->abi!=FNPU_ABI || c->flags!=3 || c->opcode!=90 ||
       !pairs || pairs>128 || c->input_offset || c->input_bytes!=pairs*256 ||
       c->output_offset!=pairs*256 || c->output_bytes!=pairs*128 ||
       c->arena_bytes!=pairs*384) return FNPU_BAD_ARGUMENT;
    const volatile HVX_Vector *in=fnpu_input(c);
    volatile HVX_Vector *out=fnpu_output(c);
    for(fnpu_u32 i=0;i<pairs;i++) out[i]=half_pair(in[i*2],in[i*2+1]);
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
