/* Teacher linked loader: admin commands for the operator image in the owned
 * DDR reservation 0x1ff0000..0x1ff4000 (tests/teacher_link.c).
 *
 * The slot entry (resident_graph_entry.S, RG_LINK_ENTRY + TL_LINK_ENTRY)
 * runs uncached, publishes nothing, and enters the image for opcodes 110/111
 * only while the seal word (last vector of the reservation) holds TL_SEAL.
 * Every other request reaches this frame_job, which accepts only opcode 112:
 *
 *   input  [TL_MAGIC, command, offset, length], other words zero
 *   command 7 (seal):   seal word zero, offset/length the whole reservation;
 *                       discard data and instruction lines over the
 *                       reservation through the cached alias, then seal.
 *   command 8 (retire): seal word TL_SEAL; clear it, then discard the lines.
 *   output [TL_MAGIC, pc, seal word after], other words zero.
 *
 * Nothing writes the reservation through the cached alias, so lines are
 * discarded, never written back. The host writes the image (and a zero seal
 * vector) before sealing and restores the reservation only after retiring. */
#include "landmark_common.h"

#define TL_MAGIC 0x544c4144u
#define TL_SEAL 0x544c4e4bu
#define TL_OPCODE 112u
#define TL_CODE 0x1ff0000u
#ifndef TL_CODE_BYTES
#define TL_CODE_BYTES 0x4000u /* 0x5000u: the 20 KiB reservation (--code-kib 20) */
#endif
#define TL_SEAL_WORD (*(volatile fnpu_u32 *)(0xf0000000u + TL_CODE + TL_CODE_BYTES - 128u))

fnpu_u32 frame_job(const fnpu_context *c) {
    const volatile fnpu_u32 *d = fnpu_input(c);
    volatile fnpu_u32 *o = fnpu_output(c);
    if (!valid(c) || c->opcode != TL_OPCODE || c->input_bytes != 128 || c->output_bytes != 256 ||
        c->params[0] > 1 || c->params[1] || c->params[2] || c->params[5])
        return FNPU_BAD_ARGUMENT;
    for (fnpu_u32 i = 4; i < 32; i++)
        if (d[i]) return FNPU_BAD_ARGUMENT;
    fnpu_u32 cmd = d[1], seal = TL_SEAL_WORD;
    if (d[0] != TL_MAGIC || d[2] != TL_CODE || d[3] != TL_CODE_BYTES ||
        (cmd == 7 ? seal != 0 : cmd == 8 ? seal != TL_SEAL : 1))
        return FNPU_BAD_ARGUMENT;
    TL_SEAL_WORD = 0;
    __asm__ volatile("barrier" ::: "memory");
    for (fnpu_u32 addr = TL_CODE; addr < TL_CODE + TL_CODE_BYTES; addr += 32) {
        fnpu_u32 cached = 0x87000000u + addr;
        __asm__ volatile("dcinva(%0)\n\ticinva(%0)\n\tisync" ::"r"(cached) : "memory");
    }
    if (cmd == 7) TL_SEAL_WORD = TL_SEAL;
    __asm__ volatile("barrier" ::: "memory");
    for (fnpu_u32 i = 0; i < 64; i++) o[i] = 0;
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
    o[0] = TL_MAGIC;
    o[1] = pc;
    o[2] = TL_SEAL_WORD;
    __asm__ volatile("barrier" ::: "memory");
    return FNPU_OK;
}
