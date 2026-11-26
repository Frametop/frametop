#ifndef FRAMENPU_H
#define FRAMENPU_H
typedef unsigned int fnpu_u32;
typedef unsigned short fnpu_u16;
typedef struct {
    fnpu_u32 sequence, magic, abi, flags;
    fnpu_u32 arena_bytes, input_offset, input_bytes, output_offset, output_bytes;
    fnpu_u32 opcode, params[6];
} fnpu_context;
#define FNPU_MAGIC 0x55504e46u
#define FNPU_ABI 1u
#define FNPU_FLAG_HMX 1u
#define FNPU_FLAG_DIRECT 2u
#define FNPU_OK 0u
#define FNPU_BAD_ARGUMENT 1u
#define FNPU_EXCEPTION 2u
#define FNPU_UNSUPPORTED 3u
#define FNPU_ARENA ((unsigned char *)0xf0a97000u)
#define FNPU_ARENA_SIZE 0x20000u
static inline const void *fnpu_input(const fnpu_context *ctx) {
    return FNPU_ARENA + ctx->input_offset;
}
static inline void *fnpu_output(const fnpu_context *ctx) {
    return FNPU_ARENA + ctx->output_offset;
}
/* Implement this function. The SDK calls frame_job only on worker 0. On
 * direct requests, context 4 runs only the scheduling/join wrapper; no HMX/HVX
 * instructions execute there. Raw reference start/end counters use status
 * words 10..13. Native jobs built with older entry wrappers need rebuilding
 * before direct dispatch; FNPU model files retain their existing format.
 * Return an error code; do not publish completion or change shared control.
 * All offsets are relative to the bounded arena. Trusted native code has
 * supervisor access: this ABI is not a sandbox for untrusted code.
 */
fnpu_u32 frame_job(const fnpu_context *ctx);
#endif
