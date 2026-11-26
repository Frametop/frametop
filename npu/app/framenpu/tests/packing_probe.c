/* Hardware-only fixture: exercise the runtime's actual packing helper in DDR.
   It does not add a diagnostic opcode to the shipped runtime. */
#define frame_job runtime_frame_job
#include "../src/framenpu/sdk/runtime.c"
#undef frame_job

fnpu_u32 frame_job(const fnpu_context *ctx) {
    fnpu_u32 rows=ctx->params[0], cols=ctx->params[1], stride=ctx->params[2];
    fnpu_u32 column=ctx->params[3], prefix=ctx->params[4];
    if (ctx->opcode==5) {
        if (rows>124 || (rows&3) || ctx->input_bytes<rows || ctx->output_bytes<rows)
            return FNPU_BAD_ARGUMENT;
        volatile fnpu_context copy;
        for (fnpu_u32 i=0;i<sizeof(copy)/4;i++)
            ((volatile fnpu_u32 *)&copy)[i]=((const fnpu_u32 *)ctx)[i];
        copy.input_offset+=rows;
        copy.output_offset+=rows;
        copy.input_bytes-=rows;
        copy.output_bytes-=rows;
        return runtime_frame_job((const fnpu_context *)&copy);
    }
    if (!rows || rows>32 || !cols || cols>32 || !stride || stride>64 ||
        (column!=0 && column!=32) || column+cols>stride || prefix>124 || (prefix&3) ||
        ctx->input_bytes!=prefix+rows*stride*2 || ctx->output_bytes!=2048)
        return FNPU_BAD_ARGUMENT;
    pack_tile(fnpu_output(ctx), (const fnpu_u16 *)((const unsigned char *)fnpu_input(ctx)+prefix),
              rows, cols, stride, column);
    return FNPU_OK;
}
