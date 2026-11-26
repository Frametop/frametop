/* Linked resident operator image: the ten compute modules as one read-only,
 * position-independent program in owned DDR (RG_CODE). The slot loader jumps
 * to rg_batch at image offset zero through the selected instruction alias.
 * One request may cross phase and layer boundaries. It stops between whole
 * work units at the host's tick budget or unit limit, at the host's
 * (layer, phase) stop point, at frame completion, or on a guarded fault.
 * Arithmetic, tensor layout and per-unit state changes are those of the
 * separately qualified modules; the host replays the unit count through an
 * independent state machine before any resource is reclaimed. */
#define RG_LINKED 1
#include "resident_graph.h"
fnpu_u32 rg_batch(const fnpu_context *c) __attribute__((section(".text.rg_entry")));
#define frame_job rg_op_hmx
#include "resident_graph_hmx.c"
#undef frame_job
#define frame_job rg_op_dw
#include "resident_graph_dw.c"
#undef frame_job
#ifdef RG_DW_TILE
#include "resident_dw_tile.h"
#endif
#define frame_job rg_op_add
#include "resident_graph_add.c"
#undef frame_job
#define frame_job rg_op_pool
#include "resident_graph_pool.c"
#undef frame_job
#define frame_job rg_op_sigmoid
#include "resident_graph_sigmoid.c"
#undef frame_job
#define frame_job rg_op_compact
#include "resident_graph_compact.c"
#undef frame_job
#ifndef RG_CONV_FAST
/* A fused image finishes bias and clip inside rg_op_hmx and never enters
 * phase 3, so it carries no post operator; kind 8 is refused below. */
#define frame_job rg_op_post
#include "resident_graph_post.c"
#undef frame_job
#endif
#define frame_job rg_op_avg
#include "resident_graph_avg.c"
#undef frame_job
#define frame_job rg_op_pack
#define gather rg_pack_gather
#define pack_unit rg_pack_unit
#include "resident_graph_pack.c"
#undef frame_job
#undef gather
#undef RG_STEM
#define RG_STEM 1
#undef pack_unit
#define frame_job rg_op_stem
#define gather rg_stem_gather
#define pack_unit rg_stem_unit
#include "resident_graph_pack.c"
#undef frame_job
#undef gather
#ifdef RG_DW_PROFILE
static fnpu_u32 rg_profile_finish(const fnpu_context *c,fnpu_u32 result) {
    result=rg_finish(c,result);
#ifdef RG_DW_PROFILE
    /* Profiling-only echo words: decoded and restored by the opt-in harness
     * before the ordinary completion validator. Native state is untouched. */
    volatile fnpu_u32 *out=fnpu_output(c);
    rg_u64 ticks=*(volatile rg_u64 *)0x31001f20u;
    out[26]=ticks;out[27]=ticks>>32;
    out[28]=*(volatile fnpu_u32 *)0x31001f28u;out[29]=0x44575031u;
#endif
    return result;
}
#else
#define rg_profile_finish rg_finish
#endif
fnpu_u32 rg_batch(const fnpu_context *c) {
#ifdef RG_DW_PROFILE
    *(volatile rg_u64 *)0x31001f20u=0;
    *(volatile fnpu_u32 *)0x31001f28u=0;
#endif
    rg_open();
    if(!rg_header(c,RG_BATCH_KIND)) return rg_profile_finish(c,FNPU_BAD_ARGUMENT);
    const volatile fnpu_u32 *d=fnpu_input(c);
    if(d[2]!=5||RG_STATE[0]!=3||RG_STATE[27]!=602112||RG_STATE[19]>=63||RG_STATE[31]!=RG_SEAL||
       d[18]!=RG_STATE[19]||!d[19]||d[19]>RG_UNIT_LIMIT||d[21]||d[22]||
       !d[23]||d[23]>RG_BUDGET_MAX||d[24]>63||d[25]>3) return rg_profile_finish(c,FNPU_BAD_ARGUMENT);
    fnpu_u32 kind=rg_kind();
    if(d[20]!=(kind==8||kind==9||kind==10?RG_STATE[18]:RG_STATE[20])) return rg_profile_finish(c,FNPU_BAD_ARGUMENT);
    fnpu_u32 stop_layer=d[24],stop_phase=d[25];
    rg_begin(c);
    RG_BATCH_BUDGET=d[23];RG_BATCH_UNITS=d[19];RG_BATCH_FAULT=d[30];RG_BATCH_START=rg_ticks();
    for(;;) {
        fnpu_u32 result;
        kind=rg_kind();
#if RG_DW_REUSE & 4
        /* Avg pooling may reuse part of the broadcast page. */
        if(kind!=3) *(volatile fnpu_u32 *)0x31001f30u=0;
#endif
        if(kind==1) result=rg_op_hmx(c);
#ifdef RG_DW_TILE
        else if(kind==3) result=dwt_fits(rg_layer())?rg_op_dwt(c):rg_op_dw(c);
#else
        else if(kind==3) result=rg_op_dw(c);
#endif
        else if(kind==4) result=rg_op_add(c);
        else if(kind==5) result=rg_op_pool(c);
        else if(kind==6) result=rg_op_sigmoid(c);
        else if(kind==7) result=rg_op_compact(c);
#ifndef RG_CONV_FAST
        else if(kind==8) result=rg_op_post(c);
#endif
        else if(kind==9) result=rg_op_pack(c);
        else if(kind==10) result=rg_op_stem(c);
        else if(kind==11) result=rg_op_avg(c);
        else result=FNPU_BAD_ARGUMENT;
        if(result) return rg_profile_finish(c,result);
        if(RG_STATE[0]!=3||RG_STATE[30]>=RG_BATCH_UNITS||
           (RG_STATE[19]==stop_layer&&RG_STATE[24]==stop_phase)||
           rg_ticks()-RG_BATCH_START>RG_BATCH_BUDGET) break;
    }
    return rg_profile_finish(c,FNPU_OK);
}
