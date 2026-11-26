/* Teacher linked operator image entry, at image offset zero. The slot loader
 * (tests/teacher_link_admin.c) tail-jumps here for opcodes 110 and 111 while
 * the image is sealed, with r0 = context and the uncached entry's return in
 * r31. tests/teacher_resident_probe.c (opcode 111, built as tl_rs) and
 * tests/teacher_dw_probe.c (opcode 110, built as tl_dw) are linked unchanged
 * in source; each still validates its own context, opcode and descriptors. */
#include "landmark_common.h"

fnpu_u32 tl_rs(const fnpu_context *c);
fnpu_u32 tl_dw(const fnpu_context *c);
#ifdef RS_POOL
fnpu_u32 tl_pool_worker(fnpu_u32 w);
#endif

__attribute__((section(".text.rg_entry"))) fnpu_u32 tl_entry(const fnpu_context *c) {
#ifdef RS_POOL
    /* the RG_POOL loader enters workers 1 and 2 here for opcode 111 only */
    fnpu_u32 w;
    __asm__ volatile("%0 = htid" : "=r"(w));
    if (w) return w <= 2u && c->opcode == 111u ? tl_pool_worker(w) : 0u;
#endif
    return c->opcode == 111u ? tl_rs(c) : tl_dw(c);
}

#ifdef TL_DIAG_CODE_PAD
#define RS_STR0(x) #x
#define RS_XSTR(x) RS_STR0(x)
/* DIAGNOSTIC: moves every function after the entry by TL_DIAG_CODE_PAD bytes (never executed) */
__asm__(".section .text.rg_pad,\"ax\",@progbits\n\t.space " RS_XSTR(TL_DIAG_CODE_PAD) "\n\t.text");
#endif
