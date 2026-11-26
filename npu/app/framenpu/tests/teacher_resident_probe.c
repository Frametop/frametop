/* Teacher tail as a resident op sequence (HMX 1x1 + HVX stride-1 depthwise),
 * FP16, on the grid layout of tests/teacher_tail_layout.py.
 *
 * A job runs ops params[1] .. params[2]-1 of an op table in the arena window
 * (128-byte descriptors, tests/teacher_resident_layout.py). Tensors stay in
 * borrowed VTCM between ops of a job at host-planned vector offsets; load and
 * store ops move whole tensors between VTCM and the window. The stride-2
 * depthwise layers run as separate tests/teacher_dw_probe.c jobs unless
 * RS_STRIDE2 is defined for the linked image.
 *
 * Arithmetic per output vector is that of tests/teacher_pw_probe.c (1x1) and
 * tests/teacher_dw_probe.c (stride 1): one shared post adds the bias (here a
 * pre-splatted vector), clips, adds the residual, masks to interior lanes and
 * stores straight into the zeroed output grid.
 *
 * Descriptor words: kind (1 = 1x1, 2 = depthwise, 3 = load, 4 = store), cpi,
 * cpo, nk (1x1) or k (depthwise), clip, nbin, blk0, nj, ws, dpar, npar, vin,
 * vout, vres (0 = none), vpar (VTCM offsets in vectors, never 0). Parameters: 1x1 = A tiles [o][t] | bias
 * vectors (cpo) | mask (nj); depthwise = tap words | bias vectors | mask.
 * Scratch (B tiles or splatted taps) follows the parameters; for 1x1 ops vpar
 * and npar are multiples of 16 vectors so A and B tiles are 2 KiB aligned. Each descriptor
 * is checked before its op runs: every VTCM index the op can form lies in the
 * borrowed span and every window access inside the window.
 *
 * Input payload: [MAGIC, table offset, table entries]. Meta: [0] magic [1] pc
 * [2] ops run; flags vector at words 64..95; ticks after op i at
 * word 96+i. Ops stop between whole ops at the 20 ms budget. */
/* RS_STRIDE2 adds kind 5: full-resolution depthwise into a temporary grid,
 * then the qualified scalar (2*y+1, 2*x+1) decimation into vout. Descriptor
 * words 15..20 are output nbin, blk0, ws, h, w and temporary vector offset.
 * RS_S2_EVEN (Mercury linked image) keeps (2*y, 2*x) instead, the phase of
 * symmetric (1,1) padding; the bounds checks cover both phases.
 * RS_S2_DIRECT computes only the kept outputs instead: per output row and
 * 32-column block it deals the three input rows into even/odd columns and
 * accumulates the same 9 qf16 products in the same order as rs_dw, so each
 * output equals the decimated stride-1 result; the temporary is unused (still
 * validated). Lanes past the row are stored as zero onto pad words or words a
 * later row rewrites; extra checks bound the reads and that spill.
 * RS_POST_BATCH stores two rows per column block; RS_S2_REV runs the blocks
 * last to first so that holds for rows wider than one block (ow > 32).
 * The ordinary 4 KiB reference build does not include this extension. */
/* RS_MHEADS (Mercury heatmap head, stride-2 builds) adds two kinds:
 * kind 7 (regrid) is an exact word copy between grids. Words: 7, pairs per
 * phase np, phases nph (1..9), oh, ow, source nbin, destination nbin, vin,
 * vout (VTCM vector offsets), source row stride, destination row and column
 * steps, flags (bit 0: the source pair advances by np per phase, bit 1: the
 * destination pair does), 0, then (source word, destination word) of each
 * phase in words 14..31. The destination is zeroed first; phase t copies
 * dst[dbase + y*drow + x*dcol] = src[sbase + y*sws + x] for every pair.
 * kind 8 is a 3x3 (padding 1, same grid) convolution on HMX: kind 1 with K =
 * 9 taps x cpi pairs, where B row t*cpi + p is pair p shifted by
 * (t/3 - 1) * ws + t%3 - 1 words (unaligned vector loads). The reads are
 * bounded like the depthwise reach. Same post, parameters and scratch as 1x1. */
#ifdef RS_MHEADS
#if !defined(RS_STRIDE2) || defined(RS_STEM) || defined(RS_HEADS)
#error "RS_MHEADS is a stride-2 Mercury option (no stem prefix, no teacher heads)"
#endif
#define RG_KIND 7u
#define P3_KIND 8u
#endif
#include "landmark_common.h"
#ifdef RS_STEM
#include "teacher_stem_check.h"
#endif
#ifdef RS_HEADS
#include "teacher_heads_check.h"
void th_run(const fnpu_u32 *, const fnpu_context *);
#endif
void fnpu_hmx_clear(void);
void fnpu_hmx_load(const fnpu_u16 *, const fnpu_u16 *);
void fnpu_hmx_store(fnpu_u16 *, const void *);
#ifdef RS_HMX_INLINE
/* The sdk/hmx.S sequences inline: the same instructions in the same order,
 * with exact clobbers instead of a call. A call clobbers every caller-saved
 * register, so the 1x1 loop kept its state in callee-saved ones and spilled
 * the rest to the DDR stack, and each group and HMX-store barrier then waited
 * ~1.9 us for those stores. */
static inline __attribute__((always_inline)) void rs_hmx_clear(void) {
    __asm__ volatile(".word 0xa6e0c013" ::: "memory");
}
static inline __attribute__((always_inline)) void rs_hmx_load(const fnpu_u16 *w, const fnpu_u16 *a) {
    __asm__ volatile("{ r0 = %0 }\n\t{ r2 = %1 }\n\t{ r1 = #2047 }\n\t.word 0x920041e2\n\t.word 0x9202e1ef"
                     :: "r"(w), "r"(a) : "r0", "r1", "r2", "memory");
}
static inline __attribute__((always_inline)) void rs_hmx_store(fnpu_u16 *out, const void *scale) {
    __asm__ volatile("{ r0 = %1 }\n\t.word 0x9200c3fe\n\t{ r0 = #2 }\n\t.word 0xa6e0da10\n\t"
                     "{ r0 = %0 }\n\t{ r1 = #0 }\n\t.word 0xa6e0c118\n\t{ barrier }"
                     :: "r"(out), "r"(scale) : "r0", "r1", "memory");
}
#define fnpu_hmx_clear rs_hmx_clear
#define fnpu_hmx_load rs_hmx_load
#define fnpu_hmx_store rs_hmx_store
#endif

#define RS_MAGIC 0x31535254u
#define RS_OPCODE 111u
#define RS_DDR 0xf0000000u
#define RS_WIN_LO 0x1a40000u
#ifdef RS_WIN_WIDE
/* RS_WIN_WIDE: the window runs up to the image reservation at 0x1ff0000 (inside
 * the canary-soaked arena 0x1a22000..0x2000000; nothing else is placed above
 * 0x1f00000), 5.69 MiB instead of 4.75 */
#define RS_WIN_HI 0x1ff0000u
#else
#define RS_WIN_HI 0x1f00000u
#endif
#define RS_META 1024u
#ifdef RS_REPLAY
#if !defined(RS_STRIDE2) || defined(RS_STEM) || defined(RS_HEADS)
#error "RS_REPLAY is a stride-2 Mercury option (no stem prefix, no heads)"
#endif
/* Request word 3: crops (0 or 1 = one crop, at most RS_MAX_CROPS); word 4: the
 * crop stride in bytes (4 KiB multiple). Crop k runs descriptors a..b again with
 * k * stride added to the window address of its load and store descriptors
 * (kinds 3, 4) before validation; compute ops and parameter images are shared.
 * VTCM carries over from crop to crop, so a plan must leave nothing in it that
 * the next crop reads before writing. */
#define RS_MAX_CROPS 3u
#endif
#ifndef RS_DW_BLOCKS
#define RS_DW_BLOCKS 2
#endif
#if RS_DW_BLOCKS != 2 && RS_DW_BLOCKS != 4
#error "RS_DW_BLOCKS must be 2 or 4"
#endif
#ifdef RS_STRIDE2
#define RS_MAX_KIND 5u
#else
#define RS_MAX_KIND 4u
#endif
#define SPAN ((0x311e0000u - 0x31002000u) / 128u)
/* RS_RESIDENT (Mercury replay + mheads builds): parameter images stay resident in
 * the FeatureNN model-blob region of VTCM, 0x311e0000..0x316eea80, which the
 * firmware refills from DDR before a FeatureNN request whenever the word at
 * 0xf0b12014 is 0. A compute descriptor with word 21 nonzero is resident: its
 * parameters are at FR(vpar) in [RS_RES_LO, RS_RSPAN) and word 21 is its scratch
 * offset in the borrowed span (otherwise scratch follows the parameters).
 * Request word 9 = 1 selects resident mode with the session tag in words 5..8
 * (word 5 nonzero); resident descriptors are refused otherwise. The images are
 * copied (crop 0 only) unless the flag is 0 and FR(SPAN) holds the tag. Before
 * copying, the kernel clears the flag (so FeatureNN reloads its blob before its
 * next use) and the tag; it writes the tag only after a request that ran every
 * descriptor of the whole table. meta[12]: bit 0 resident mode, bit 1 images
 * copied, bit 2 tag written.
 * RS_RES_SLOTS: several models keep their images resident at once, each in its
 * own part of the region (the host plans them apart). Word 9 = 1 | s << 1 picks
 * tag slot s (0..3, words 4s..4s+3 of FR(SPAN)); word 9 = 1 is slot 0, as
 * before. A slot's images are copied unless the flag is 0 and its slot holds
 * the tag. When the flag is set, FeatureNN has refilled the whole region, so
 * every slot is cleared; on a plain tag mismatch only the request's own slot is
 * cleared, and the other models' images and tags stay valid. */
#ifdef RS_RESIDENT
#if !defined(RS_REPLAY) || !defined(RS_MHEADS)
#error "RS_RESIDENT is an option of the Mercury replay image (RS_REPLAY, RS_MHEADS)"
#endif
#define RS_RSPAN ((0x316eea80u - 0x31002000u) / 128u)
#define RS_RES_LO (SPAN + 16u)
#define RS_BLOB_FLAG ((volatile fnpu_u32 *)0xf0b12014u)
#define RS_TAG ((volatile fnpu_u32 *)(0x31002000u + 128u * SPAN))
#ifdef RS_RES_SLOTS
#define RS_NSLOT 4u
#endif
#endif
#ifdef RS_CBIAS
/* RS_CBIAS: a compute op's parameter image holds its biases packed, one FP16
 * pair word per output pair (ceil(cpo / 32) vectors), then its nj mask vectors.
 * Before the op runs, worker 0 splats the words and copies the masks into the
 * last RS_XB_N vectors of the blob region (no resident image may reach them)
 * and points bias there, so every compute path reads the usual layout: bias
 * vector pr, then mask j at cpo + j. Every request must be resident (word 9):
 * only then is the blob region the session's (FeatureNN reloads it after us). */
#ifndef RS_RESIDENT
#error "RS_CBIAS is an option of the resident image"
#endif
#define RS_XB_N 512u
#define RS_XB (RS_RSPAN - RS_XB_N)
#define RS_RES_TOP RS_XB
#elif defined(RS_RESIDENT)
#define RS_RES_TOP RS_RSPAN
#endif
#ifdef RS_S2_EVEN
#define RS_S2_P 0u /* decimation phase: output (y, x) keeps stride-1 (2y+P, 2x+P) */
#else
#define RS_S2_P 1u
#endif
/* RS_PW_PROFILE (timing instrument only): 19.2 MHz tick totals over the request
 * for 1x1 ops -- parameter copy, output zeroing, B gather, HMX clear/load/store,
 * post, barrier after the gather, an extra barrier before the gather (draining
 * the previous block's stores), a barrier after each tile's post, a second
 * barrier after the HMX store -- in meta words 3..11. Outputs are unchanged. */
#ifdef RS_PW_PROFILE
#define RS_PROF ((volatile fnpu_u32 *)LV(63)) /* zeroed with LV(57..63) per request */
#define RS_PT(v) fnpu_u32 v = rs_ticks()
#define RS_PADD(k, v) (RS_PROF[k] += rs_ticks() - (v))
#else
#define RS_PT(v)
#define RS_PADD(k, v)
#endif
#if defined(RS_S2_DIRECT) && !defined(RS_STRIDE2)
#error "RS_S2_DIRECT requires RS_STRIDE2"
#endif
#define LV(i) ((volatile HVX_Vector *)(0x31000000u + 128u * (i)))
#define RS_SCALE LV(56)
#define RS_FLAGS LV(62)
#define RS_ACC(i) LV(24 + (i)) /* 2 accumulators, qf16 zero at 4 */
#define RS_DESC LV(32)
#define FR(i) ((volatile HVX_Vector *)(0x31002000u + 128u * (i)))
#ifdef RS_PW_GROUP
#define RS_PW_SCR(k, no) (RS_PW_GROUP * ((k) + (no)) * 16) /* B tiles + output tiles */
#else
#define RS_PW_SCR(k, no) ((k) * 16)
#endif

typedef long rs_uv __attribute__((__vector_size__(128), __aligned__(4)));

typedef struct {
    fnpu_u32 kind, cpi, cpo, nk, clip, nbin, blk0, nj, ws, dpar, npar, vin, vout, vres, vpar;
    fnpu_u32 bias, scr; /* derived */
#ifdef RS_STRIDE2
    fnpu_u32 onbin, oblk0, ows, oh, ow, tmp;
#endif
#ifdef RS_RESIDENT
    fnpu_u32 rscr; /* word 21: scratch offset of a resident op, else 0 */
#endif
} rs_op;
/* After validation of a compute op, vin, vout, vres, bias and scr hold VTCM
 * addresses (vres stays 0 when absent; plans never place anything at vector
 * 0); vpar stays a vector offset for every kind. */
#define P(x) ((volatile HVX_Vector *)(d->x))

static inline __attribute__((always_inline)) fnpu_u32 rs_ticks(void) {
    unsigned long long t;
    __asm__ volatile("%0 = c31:30" : "=r"(t)::"memory");
    return (fnpu_u32)t;
}

#ifdef RS_POOL
#ifndef RS_DW_FAST
#error "RS_POOL splits the RS_DW_FAST depthwise"
#endif
/* Worker pool (with the RG_POOL loader, which enters workers 1 and 2 in
 * tl_pool_worker for this opcode). Worker 0 posts a job (an operator index
 * and its descriptor, never a code pointer) in a mailbox at LV(40), runs its
 * own share and waits for both workers. Each worker runs a disjoint share of
 * the same per-element operations and ORs its overflow flags into its own
 * vector (LV(50), LV(51)), which worker 0 merges, so outputs are unchanged.
 * Words 0/1 = seq/~seq mark the mailbox open for this request only: after
 * the VTCM restore the page holds firmware data and a worker never takes it
 * for a job. Words: 2 job, 3 operator, 4 descriptor, 5-6 done(w), 8 exit,
 * 7 1x1 HMX count, 9-10 exit ack(w), 11 failure, 12-13 1x1 gather count(w),
 * 14-15 frame_job's return address and
 * result. The loader reads 0, 1, 8, 9 and 10. */
#define RS_MB ((volatile fnpu_u32 *)LV(40))
#define RS_WFLAGS(w) LV(49 + (w))
#define RS_SEQ (*(const volatile fnpu_u32 *)0xf0a93000u)    /* request sequence (uncached) */
#define RS_W0_SAVED (*(const volatile fnpu_u32 *)0xf0a93424u) /* worker 0 status 9: page saved */
#define RS_W0_DONE (*(const volatile fnpu_u32 *)0xf0a93400u)  /* worker 0 completion word */
#define RS_POOL_TICKS 38400u                                  /* 2 ms bound per wait */
#define RS_FN_DW 1u
#define RS_FN_S2 2u
enum { MB_OPEN = 0, MB_OPEN2 = 1, MB_JOB = 2, MB_FN = 3, MB_ARG = 4, MB_DONE = 4, MB_EXIT = 8,
       MB_ACK = 8, MB_FAIL = 11 };
#define RS_PAUSE() __asm__ volatile("pause(#8)" ::: "memory")
#ifndef RS_POOL_IDLE
#define RS_POOL_IDLE 255 /* an idle worker's pause: hardware threads share issue slots */
#endif
#define RS_STR_(x) #x
#define RS_STR(x) RS_STR_(x)
#ifndef RS_POOL_IDLE_N
#define RS_POOL_IDLE_N 8 /* pauses per idle poll */
#endif
#define RS_IDLE()                                                                     \
    do {                                                                              \
        _Pragma("clang loop unroll(disable)") for (fnpu_u32 q_ = 0; q_ < RS_POOL_IDLE_N; q_++) \
            __asm__ volatile("pause(#" RS_STR(RS_POOL_IDLE) ")" ::: "memory");       \
    } while (0)
#define RS_BARRIER() __asm__ volatile("barrier" ::: "memory")
static void rs_pool_slice(fnpu_u32 fn, const rs_op *d, fnpu_u32 w, fnpu_u32 job);
#if defined(RS_STRIDE2) && defined(RS_S2_DIRECT)
static void rs_s2_range(const rs_op *d, fnpu_u32 c0, fnpu_u32 c1, volatile HVX_Vector *fdst);
#endif

static __attribute__((noinline)) void rs_pool_open(void) {
    const fnpu_u32 seq = RS_SEQ;
#ifdef RS_MULTI
    /* segment 1 of a request: the pool stays open (closing it would send the
     * workers back to the loader); the job counter carries on */
    if ((RS_MB[MB_OPEN] == seq) & (RS_MB[MB_OPEN2] == ~seq)) {
        /* the workers are idle between jobs: their flags start clean, as below */
        RS_WFLAGS(1)[0] = Q6_V_vzero();
        RS_WFLAGS(2)[0] = Q6_V_vzero();
        RS_BARRIER();
        return;
    }
#endif
    for (fnpu_u32 k = 2; k < 14; k++) RS_MB[k] = 0; /* 14, 15: frame_job's */
    RS_WFLAGS(1)[0] = Q6_V_vzero();
    RS_WFLAGS(2)[0] = Q6_V_vzero();
#ifdef RS_POOL_PROF
    LV(52)[0] = Q6_V_vzero();
#endif
    RS_BARRIER();
    RS_MB[MB_OPEN2] = ~seq;
    RS_MB[MB_OPEN] = seq;
    RS_BARRIER();
}
/* both workers' word base + w equal to want, within the bound */
static __attribute__((noinline)) fnpu_u32 rs_pool_wait(fnpu_u32 base, fnpu_u32 want) {
    const fnpu_u32 t0 = rs_ticks();
    while (RS_MB[base + 1] != want || RS_MB[base + 2] != want) {
        if (rs_ticks() - t0 > RS_POOL_TICKS) return 0;
        RS_PAUSE();
    }
    RS_BARRIER();
    return 1;
}
/* 1 when the pool is closed cleanly (or was never opened) */
static __attribute__((used, noinline)) fnpu_u32 rs_pool_close(void) {
    const fnpu_u32 seq = RS_SEQ;
    if (RS_MB[MB_OPEN] != seq || RS_MB[MB_OPEN2] != ~seq) return 1;
    RS_BARRIER();
    RS_MB[MB_EXIT] = seq;
    RS_BARRIER();
    return rs_pool_wait(MB_ACK, seq) & !RS_MB[MB_FAIL];
}
#endif

static __attribute__((noinline)) void rs_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = from[i];
}

static __attribute__((noinline)) void rs_zero(volatile HVX_Vector *to, fnpu_u32 n) {
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < n; i++) to[i] = Q6_V_vzero();
}

#ifdef RS_MHEADS
/* Kind 7 bounds: every word read lies in the source tensor's pairs and every
 * word written in the destination's (sizes < 2^14, so products cannot wrap). */
static __attribute__((noinline)) fnpu_u32 rg_bad(const volatile fnpu_u32 *r) {
    fnpu_u32 w[32], big = 0;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < 32; i++) big |= w[i] = r[i];
    fnpu_u32 np = w[1], nph = w[2], oh = w[3], ow = w[4], snb = w[5], dnb = w[6], vin = w[7], vout = w[8];
    fnpu_u32 sws = w[9], drow = w[10], dcol = w[11], fl = w[12];
    if ((big > 0x3fff) | (np == 0) | (nph == 0) | (nph > 9) | (oh == 0) | (ow == 0) | (snb == 0) | (dnb == 0) |
        (vin == 0) | (vout == 0) | (fl > 3) | (w[13] != 0) | (dcol == 0))
        return 1;
    fnpu_u32 sp = (fl & 1) ? nph * np : np, dp = (fl & 2) ? nph * np : np;
    fnpu_u32 bad = (vin + sp * snb > SPAN) | (vout + dp * dnb > SPAN) |
                   ((vin < vout + dp * dnb) & (vout < vin + sp * snb)); /* no overlap */
#pragma clang loop unroll(disable)
    for (fnpu_u32 t = 0; t < nph; t++)
        bad |= (w[14 + 2 * t] + (oh - 1) * sws + ow > 32 * snb) |
               (w[15 + 2 * t] + (oh - 1) * drow + (ow - 1) * dcol >= 32 * dnb);
    return bad;
}

static __attribute__((noinline)) void rg_run(const volatile fnpu_u32 *r) {
    const fnpu_u32 np = r[1], nph = r[2], oh = r[3], ow = r[4], snb = r[5], dnb = r[6], sws = r[9];
    const fnpu_u32 drow = r[10], dcol = r[11], fl = r[12];
    const volatile fnpu_u32 *src = (const volatile fnpu_u32 *)FR(r[7]);
    volatile fnpu_u32 *dst = (volatile fnpu_u32 *)FR(r[8]);
    rs_zero((volatile HVX_Vector *)dst, ((fl & 2) ? nph * np : np) * dnb);
#pragma clang loop unroll(disable)
    for (fnpu_u32 t = 0; t < nph; t++) {
        const fnpu_u32 sb = r[14 + 2 * t], db = r[15 + 2 * t];
        const fnpu_u32 sp0 = (fl & 1) ? t * np : 0, dp0 = (fl & 2) ? t * np : 0;
#pragma clang loop unroll(disable)
        for (fnpu_u32 p = 0; p < np; p++) {
            const volatile fnpu_u32 *s = src + 32 * snb * (sp0 + p) + sb;
            volatile fnpu_u32 *o = dst + 32 * dnb * (dp0 + p) + db;
#pragma clang loop unroll(disable)
            for (fnpu_u32 y = 0; y < oh; y++, s += sws, o += drow) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 x = 0; x < ow; x++) o[x * dcol] = s[x];
            }
        }
    }
}
#endif

/* Fetch descriptor i into d; nonzero if any index it implies leaves bounds. */
#ifdef RS_REPLAY
static __attribute__((noinline)) fnpu_u32 rs_get(fnpu_u32 tab, fnpu_u32 i, rs_op *d, fnpu_u32 off) {
#else
static __attribute__((noinline)) fnpu_u32 rs_get(fnpu_u32 tab, fnpu_u32 i, rs_op *d) {
#endif
    rs_copy(RS_DESC, (const volatile HVX_Vector *)(RS_DDR + tab) + i, 1);
#ifdef RS_STEM
    if (((const volatile fnpu_u32 *)RS_DESC)[0] == TS_KIND) {
        d->kind = TS_KIND;
        return ts_bad((const fnpu_u32 *)RS_DESC);
    }
#endif
#ifdef RS_HEADS
    if (((const volatile fnpu_u32 *)RS_DESC)[0] == TH_KIND) {
        fnpu_u32 *row = (fnpu_u32 *)d;
        /* Kind 6 uses the raw descriptor, never the compute-op pointer form. */
        for (fnpu_u32 w = 0; w < 16; w++) row[w] = ((const volatile fnpu_u32 *)RS_DESC)[w];
        return th_bad((const fnpu_u32 *)RS_DESC);
    }
#endif
#ifdef RS_MHEADS
    if (((const volatile fnpu_u32 *)RS_DESC)[0] == RG_KIND) {
        d->kind = RG_KIND; /* runs from the raw descriptor in RS_DESC */
        return rg_bad((const volatile fnpu_u32 *)RS_DESC);
    }
#endif
    fnpu_u32 *t = (fnpu_u32 *)d, big = 0;
#pragma clang loop unroll(disable)
#ifdef RS_RESIDENT
    /* vpar is bounded below (resident images lie past 2^14); every other word as before */
    for (fnpu_u32 w = 0; w < 15; w++) big |= (t[w] = ((const volatile fnpu_u32 *)RS_DESC)[w]) & -((w != 9) & (w != 14));
    big |= (d->rscr = ((const volatile fnpu_u32 *)RS_DESC)[21]);
#else
    for (fnpu_u32 w = 0; w < 15; w++) big |= (t[w] = ((const volatile fnpu_u32 *)RS_DESC)[w]) & -(w != 9);
#endif
#ifdef RS_REPLAY
    /* off <= the window size (frame_job); a dpar inside the window cannot wrap */
    if (((d->kind == 3) | (d->kind == 4)) & (d->dpar <= RS_WIN_HI)) d->dpar += off;
#endif
#ifdef RS_STRIDE2
    for (fnpu_u32 w = 0; w < 6; w++) big |= ((fnpu_u32 *)&d->onbin)[w] = ((const volatile fnpu_u32 *)RS_DESC)[15 + w];
    fnpu_u32 s2 = d->kind == 5;
#endif
    /* every word but dpar < 2^14: no product or sum below can wrap */
#ifdef RS_MHEADS
    fnpu_u32 p3 = d->kind == P3_KIND;
    fnpu_u32 cpi = d->cpi, cpo = d->cpo, nb = d->nbin, nj = d->nj, k = d->nk, pw = (d->kind == 1) | p3;
#ifdef RS_RESIDENT
    fnpu_u32 res = d->rscr != 0;
    fnpu_u32 bad = (big > 0x3fff) | ((d->kind - 1 >= RS_MAX_KIND) & !p3) | (d->vpar >= RS_RSPAN) |
                   (d->vpar + d->npar > (res ? RS_RES_TOP : SPAN)) | (res & (d->vpar < RS_RES_LO)) |
                   (res & ((d->kind == 3) | (d->kind == 4))) | (res & (d->rscr & 15) & -pw) |
#else
    fnpu_u32 bad = (big > 0x3fff) | ((d->kind - 1 >= RS_MAX_KIND) & !p3) | (d->vpar + d->npar > SPAN) |
#endif
                   ((d->dpar | ((d->vpar | d->npar) << 3 & -pw)) & 127) |
#else
    fnpu_u32 cpi = d->cpi, cpo = d->cpo, nb = d->nbin, nj = d->nj, k = d->nk, pw = d->kind == 1;
    fnpu_u32 bad = (big > 0x3fff) | (d->kind - 1 >= RS_MAX_KIND) | (d->vpar + d->npar > SPAN) |
                   ((d->dpar | ((d->vpar | d->npar) << 3 & -(d->kind == 1))) & 127) |
#endif
                   /* 1x1: tiles 2 KiB aligned */ (d->dpar - RS_WIN_LO + 128 * d->npar > RS_WIN_HI - RS_WIN_LO);
    bad |= d->vpar == 0;
#ifdef RS_STRIDE2
    bad |= (d->dpar < RS_WIN_LO) | (d->dpar > RS_WIN_HI) |
           (d->npar > (RS_WIN_HI - d->dpar) / 128);
    if (d->kind == 3 || d->kind == 4) return bad;
    /* Reject caps before products involving the additional dimensions. */
    if (bad) return bad;
    fnpu_u32 out_nb = s2 ? d->onbin : nb;
    if (s2) {
        bad |= (d->oh == 0) | (d->ow == 0) | ((d->oh | d->ow | d->ows | d->ws) > 255) |
               (d->ows != d->ow + 2) | (d->ws != 2 * d->ow + 2) | (d->onbin == 0) |
               (d->tmp == 0) | (d->tmp + cpo * nb > SPAN) | (d->vres != 0) | (d->clip != 1);
        if (bad) return bad;
        bad |= (2 * d->oh * d->ws > 32 * nj) |
               (32 * d->oblk0 + (d->oh - 1) * d->ows + 2 + d->ow > 32 * d->onbin);
#ifdef RS_S2_DIRECT
        {
            /* last word read (row 2(oh-1)+P+1, column block xl, 66 words from tap 0)
             * and last word stored (a full vector from the last row's block xl) */
            fnpu_u32 xl = 32 * ((d->ow - 1) >> 5);
            bad |= (32 * d->blk0 + 1 + RS_S2_P + (RS_S2_P + 2 * d->oh - 1) * d->ws + 2 * xl + 65 >= 32 * nb) |
                   (32 * d->oblk0 + 2 + (d->oh - 1) * d->ows + xl + 32 > 32 * d->onbin);
        }
#endif
        d->tmp = (fnpu_u32)FR(d->tmp);
    }
#else
    if (d->kind > 2) return bad; /* load/store: frame_job addresses FR(vpar) itself */
    fnpu_u32 out_nb = nb;
#endif
    fnpu_u32 kk = k * k, reach = (k >> 1) * (d->ws + 1), no = (cpo + 15) >> 4;
    d->bias = d->vpar + (pw ? no * k * 16 : (cpi * kk + 31) / 32);
#ifdef RS_RESIDENT
    d->scr = res ? d->rscr : d->vpar + d->npar;
#ifdef RS_CBIAS
    /* packed: ceil(cpo / 32) bias vectors, then the masks; expanded at RS_XB */
    bad |= (cpi == 0) | (nj == 0) | (d->vin == 0) | (d->vout == 0) | (d->clip > 1) |
           (d->blk0 + nj > nb) | (d->vin + cpi * nb > SPAN) | (d->vout + cpo * out_nb > SPAN) |
           (d->vres + cpo * nb > SPAN) | (d->bias + ((cpo + 31) >> 5) + nj > d->vpar + d->npar) |
           (cpo + nj > RS_XB_N) | (d->scr + (pw ? RS_PW_SCR(k, no) : cpi * kk) > SPAN);
#else
    bad |= (cpi == 0) | (nj == 0) | (d->vin == 0) | (d->vout == 0) | (d->clip > 1) |
           (d->blk0 + nj > nb) | (d->vin + cpi * nb > SPAN) | (d->vout + cpo * out_nb > SPAN) |
           (d->vres + cpo * nb > SPAN) | (d->bias + cpo + nj > d->vpar + d->npar) | (d->scr + (pw ? RS_PW_SCR(k, no) : cpi * kk) > SPAN);
#endif
#else
    d->scr = d->vpar + d->npar;
    bad |= (cpi == 0) | (nj == 0) | (d->vin == 0) | (d->vout == 0) | (d->clip > 1) |
           (d->blk0 + nj > nb) | (d->vin + cpi * nb > SPAN) | (d->vout + cpo * out_nb > SPAN) |
           (d->vres + cpo * nb > SPAN) | (d->bias + cpo + nj > d->scr) | (d->scr + (pw ? RS_PW_SCR(k, no) : cpi * kk) > SPAN);
#endif
#ifdef RS_MHEADS
    /* kind 8: K = 9 taps x cpi pairs; tap reads reach ws + 1 words either side */
    bad |= p3 ? (k != (9 * cpi + 15) >> 4) | (32 * d->blk0 < d->ws + 1) | (32 * (d->blk0 + nj) + d->ws + 1 > 32 * nb)
         :
#else
    bad |=
#endif
           pw ? (k != (cpi + 15) >> 4)
              : (cpi != cpo) | ((k != 3) & (k != 5)) | (nj & (RS_DW_BLOCKS - 1)) | (32 * d->blk0 < reach) |
                    (32 * (d->blk0 + nj) + reach > 32 * nb);
#ifdef RS_TILES
#if defined(RS_PW_DEEP) && RS_PW_LG == 3
#define RS_LAY_PW 15u
#else
#define RS_LAY_PW 7u
#endif
    /* word 15 of kinds 1/2 (RS_LAY): tile layout flags. 1 input, 2 output, 4
     * residual: tensor vector (pair p, block j) at j * pairs + p, blocks from
     * blk0, so a 1x1 B tile is the tensor itself (no gather). Tile tensors are
     * 16-vector aligned with a pair count that is a multiple of 16; the extent
     * nj * pairs lies inside the planar pairs * nb the bounds above check. */
    {
        const fnpu_u32 lay = s2 ? 0 : d->onbin, cin = lay & 1 ? cpi | d->vin : 0,
                       cout = lay & 2 ? cpo | d->vout : 0, cres = lay & 4 ? cpo | d->vres | (d->vres == 0) : 0;
        bad |= (lay & ~(d->kind == 1 ? RS_LAY_PW : d->kind == 2 ? 2u : 0u)) | ((cin | cout | cres) & 15);
#if defined(RS_PW_DEEP) && RS_PW_LG == 3
        /* 8: the plan gave this tile-input op (no <= nk) the 32 no tiles of scratch
         * that 8-block groups at depth 2 need (RS_PW_LG 3; else it runs 4-block) */
        if (lay & 8) bad |= ((lay & 1) == 0) | (no > k) | (d->scr + 32u * 16u * no > SPAN);
#endif
    }
#endif
    /* compute ops: offsets -> VTCM addresses, except vpar (kept as an offset) */
#pragma clang loop unroll(disable)
    for (fnpu_u32 w = 11; w < 17; w++)
        if (w != 14) t[w] = t[w] ? (fnpu_u32)FR(t[w]) : 0;
    return bad;
}

/* Shared post for output vector (pair pr, block j): bias, clip, residual,
 * interior mask, finite flags; stores into the output grid. */
static __attribute__((noinline)) void rs_post(const rs_op *d, HVX_Vector v, fnpu_u32 pr, fnpu_u32 j) {
    fnpu_u32 at = pr * d->nbin + d->blk0 + j;
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), flags = RS_FLAGS[0];
    flags = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
#ifdef RS_STEM
    if (!d->dpar) {
        /* Prefix-only: early-layer rounding propagates through the whole
         * graph. Add half bias through QFloat32, keeping FP16 storage. */
        HVX_Vector one = Q6_V_vsplat_R(0x3c003c00);
        HVX_VectorPair x = Q6_Wqf32_vmpy_VhfVhf(v,one);
        v = Q6_Vhf_equals_Wqf32(Q6_W_vcombine_VV(
            Q6_Vqf32_vadd_Vqf32Vsf(Q6_V_hi_W(x),P(bias)[2*pr+1]),
            Q6_Vqf32_vadd_Vqf32Vsf(Q6_V_lo_W(x),P(bias)[2*pr])));
    } else
#endif
    v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, P(bias)[pr]));
    if (d->clip) v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), Q6_V_vsplat_R(0x46004600u));
    if (d->vres) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, P(vres)[at]));
    RS_FLAGS[0] = Q6_V_vor_VV(flags, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
#ifdef RS_STEM
    P(vout)[at] = Q6_V_vand_VV(v, P(bias)[(d->dpar ? d->cpo : 2*d->cpo) + j]);
#else
    P(vout)[at] = Q6_V_vand_VV(v, P(bias)[d->cpo + j]);
#endif
}

#ifdef RS_PW_FAST
#ifdef RS_STEM
#error "RS_PW_FAST has no stem prefix path"
#endif
/* rs_post for one HMX tile (output rows 16o.. of block j), same operations in
 * the same order, with the descriptor fields hoisted and the overflow flags
 * kept in a register: one RS_FLAGS update per tile instead of a
 * read-modify-write per vector (a per-op update would spill the vector).
 * Measured with RS_PW_PROFILE: the per-vector update left about 2.7 us of
 * stores to drain per tile, paid by the next barrier or HMX op. */
#ifdef RS_PW_PIPE
/* RS_PW_PIPE: the same per-vector operations, in the same order, as the loop
 * below, so outputs are bit-identical. Fields are hoisted, the tile, bias,
 * residual and output are read and written through plain pointers inside the
 * loop (the caller's barrier and the HMX store call order them against the
 * HMX), and the clip/residual cases are separate loops, so the compiler can
 * overlap independent vectors instead of one ~14-packet chain per vector. */
/* One output vector. The overflow flags of the original loop OR in, per
 * halfword lane, the mask of (x & 0x7c00) == 0x7c00 for the raw HMX value and
 * for the final value. (x & 0x7c00) <= 0x7c00 with equality exactly then, so
 * the running unsigned maximum m of those masked values gives the same flags
 * after one compare at the end: lane set iff max == 0x7c00. */
static inline __attribute__((always_inline)) HVX_Vector rs_pw_post_one(
    const HVX_Vector *t, const HVX_Vector *bias, const HVX_Vector *res, HVX_Vector *out,
    fnpu_u32 rr, fnpu_u32 nb, fnpu_u32 rn, HVX_Vector mask, HVX_Vector m, const int clip, const int resid) {
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u);
    HVX_Vector v = t[rr];
    m = Q6_Vuh_vmax_VuhVuh(m, Q6_V_vand_VV(v, exp));
    v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias[rr]));
    /* keep the rounded FP16 value: without this the compiler fuses the
     * conversion into the residual add as vadd(qf16, hf), skipping a rounding */
    __asm__("" : "+v"(v));
    if (clip) v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), Q6_V_vsplat_R(0x46004600u));
    if (resid) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, res[rr * rn]));
    __asm__("" : "+v"(v));
    m = Q6_Vuh_vmax_VuhVuh(m, Q6_V_vand_VV(v, exp));
    out[rr * nb] = Q6_V_vand_VV(v, mask);
    return m;
}

/* Four vectors per step, each with its own maximum (max is order-free); the
 * tail takes the rest. Returns this tile's flag mask. nb strides the output,
 * rn the residual (they differ only between an RS_TILES planar and tile tensor). */
static inline __attribute__((always_inline)) HVX_Vector rs_pw_post_run(
    const HVX_Vector *t, const HVX_Vector *bias, const HVX_Vector *res, HVX_Vector *out,
    fnpu_u32 n, fnpu_u32 nb, fnpu_u32 rn, HVX_Vector mask, const int clip, const int resid) {
    HVX_Vector m0 = Q6_V_vzero(), m1 = m0, m2 = m0, m3 = m0;
    fnpu_u32 rr = 0;
#pragma clang loop unroll(disable)
    for (; rr + 4 <= n; rr += 4) {
#ifdef RS_POST_BATCH
        /* RS_POST_BATCH: all four vectors' loads before any store, so the four
         * chains overlap (the compiler cannot move a load above a store through
         * these pointers; the tile, bias, residual and output never overlap) */
        HVX_Vector v0 = t[rr], v1 = t[rr + 1], v2 = t[rr + 2], v3 = t[rr + 3];
        const HVX_Vector b0 = bias[rr], b1 = bias[rr + 1], b2 = bias[rr + 2], b3 = bias[rr + 3];
        HVX_Vector r0 = Q6_V_vzero(), r1 = r0, r2 = r0, r3 = r0;
        if (resid) {
            r0 = res[rr * rn];
            r1 = res[(rr + 1) * rn];
            r2 = res[(rr + 2) * rn];
            r3 = res[(rr + 3) * rn];
        }
        /* rs_pw_post_val's steps, a step at a time over the four: each fence (an
         * inline asm, which the packetizer does not cross) then holds all four */
        const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), six = Q6_V_vsplat_R(0x46004600u);
        m0 = Q6_Vuh_vmax_VuhVuh(m0, Q6_V_vand_VV(v0, exp));
        m1 = Q6_Vuh_vmax_VuhVuh(m1, Q6_V_vand_VV(v1, exp));
        m2 = Q6_Vuh_vmax_VuhVuh(m2, Q6_V_vand_VV(v2, exp));
        m3 = Q6_Vuh_vmax_VuhVuh(m3, Q6_V_vand_VV(v3, exp));
        v0 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v0, b0));
        v1 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v1, b1));
        v2 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v2, b2));
        v3 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v3, b3));
        __asm__("" : "+v"(v0), "+v"(v1), "+v"(v2), "+v"(v3));
        if (clip) {
            v0 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v0, Q6_V_vzero()), six);
            v1 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v1, Q6_V_vzero()), six);
            v2 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v2, Q6_V_vzero()), six);
            v3 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v3, Q6_V_vzero()), six);
        }
        if (resid) {
            v0 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v0, r0));
            v1 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v1, r1));
            v2 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v2, r2));
            v3 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v3, r3));
        }
        __asm__("" : "+v"(v0), "+v"(v1), "+v"(v2), "+v"(v3));
        m0 = Q6_Vuh_vmax_VuhVuh(m0, Q6_V_vand_VV(v0, exp));
        m1 = Q6_Vuh_vmax_VuhVuh(m1, Q6_V_vand_VV(v1, exp));
        m2 = Q6_Vuh_vmax_VuhVuh(m2, Q6_V_vand_VV(v2, exp));
        m3 = Q6_Vuh_vmax_VuhVuh(m3, Q6_V_vand_VV(v3, exp));
        out[rr * nb] = Q6_V_vand_VV(v0, mask);
        out[(rr + 1) * nb] = Q6_V_vand_VV(v1, mask);
        out[(rr + 2) * nb] = Q6_V_vand_VV(v2, mask);
        out[(rr + 3) * nb] = Q6_V_vand_VV(v3, mask);
#else
        m0 = rs_pw_post_one(t, bias, res, out, rr, nb, rn, mask, m0, clip, resid);
        m1 = rs_pw_post_one(t, bias, res, out, rr + 1, nb, rn, mask, m1, clip, resid);
        m2 = rs_pw_post_one(t, bias, res, out, rr + 2, nb, rn, mask, m2, clip, resid);
        m3 = rs_pw_post_one(t, bias, res, out, rr + 3, nb, rn, mask, m3, clip, resid);
#endif
    }
#pragma clang loop unroll(disable)
    for (; rr < n; rr++) m0 = rs_pw_post_one(t, bias, res, out, rr, nb, rn, mask, m0, clip, resid);
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u),
                     m = Q6_Vuh_vmax_VuhVuh(Q6_Vuh_vmax_VuhVuh(m0, m1), Q6_Vuh_vmax_VuhVuh(m2, m3));
    return Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(m, exp), -1);
}

#ifdef RS_POOL
/* pool builds: the overflow flags go to the caller's vector (each worker its own) */
#define RS_POST_FARG , volatile HVX_Vector *fdst
#define RS_POST_FDST fdst
#else
#define RS_POST_FARG
#define RS_POST_FDST RS_FLAGS
#endif
#ifdef RS_POOL
static inline __attribute__((always_inline)) void rs_pw_post_body(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile RS_POST_FARG) {
#else
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile RS_POST_FARG) {
#endif
    const fnpu_u32 at = 16 * o * d->nbin + d->blk0 + j;
#ifdef RS_TILES
    const fnpu_u32 lay = d->onbin, tat = j * d->cpo + 16 * o, nb = lay & 2 ? 1 : d->nbin, rn = lay & 4 ? 1 : d->nbin;
    const HVX_Vector *res = d->vres ? (const HVX_Vector *)P(vres) + (lay & 4 ? tat : at) : 0;
    HVX_Vector *out = (HVX_Vector *)P(vout) + (lay & 2 ? tat : at);
#else
    const fnpu_u32 nb = d->nbin, rn = nb;
    const HVX_Vector *res = d->vres ? (const HVX_Vector *)P(vres) + at : 0;
    HVX_Vector *out = (HVX_Vector *)P(vout) + at;
#endif
    fnpu_u32 n = d->cpo - 16 * o;
    if (n > 16) n = 16;
    const HVX_Vector *t = (const HVX_Vector *)tile, *bias = (const HVX_Vector *)P(bias) + 16 * o;
    const HVX_Vector mask = ((const HVX_Vector *)P(bias))[d->cpo + j];
    HVX_Vector fl;
    if (d->clip)
        fl = res ? rs_pw_post_run(t, bias, res, out, n, nb, rn, mask, 1, 1)
                 : rs_pw_post_run(t, bias, res, out, n, nb, rn, mask, 1, 0);
    else
        fl = res ? rs_pw_post_run(t, bias, res, out, n, nb, rn, mask, 0, 1)
                 : rs_pw_post_run(t, bias, res, out, n, nb, rn, mask, 0, 0);
    RS_POST_FDST[0] = Q6_V_vor_VV(RS_POST_FDST[0], fl);
}
#ifdef RS_POOL
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile, volatile HVX_Vector *fdst) {
    rs_pw_post_body(d, o, j, tile, fdst);
}
/* the workers' post: one specialised leaf per case, arguments precomputed, so
 * it needs no callee-saved registers (no stack stores ahead of the next
 * gather's barrier) */
/* six register arguments (no stack-passed ones): nnb = n | nb << 5 (RS_TILES:
 * n | output stride << 5 | residual stride << 18), and the flag vector is the
 * calling worker's, RS_WFLAGS(htid) */
#ifdef RS_TILES
#define RS_NNB_ON(x) (((x) >> 5) & 0x1fffu)
#define RS_NNB_RN(x) ((x) >> 18)
#else
#define RS_NNB_ON(x) ((x) >> 5)
#define RS_NNB_RN(x) ((x) >> 5)
#endif
#define RS_PW_LEAF(name, clip, resid)                                                              \
    static __attribute__((noinline)) void name(const HVX_Vector *t, const HVX_Vector *bias,       \
                                               const HVX_Vector *res, HVX_Vector *out, fnpu_u32 nnb, \
                                               const HVX_Vector *mask) {                          \
        fnpu_u32 w_;                                                                               \
        __asm__ volatile("%0 = htid" : "=r"(w_));                                                  \
        const HVX_Vector fl = rs_pw_post_run(t, bias, res, out, nnb & 31u, RS_NNB_ON(nnb), RS_NNB_RN(nnb), *mask, clip, resid); \
        volatile HVX_Vector *f_ = w_ ? RS_WFLAGS(w_) : RS_FLAGS;                                     \
        f_[0] = Q6_V_vor_VV(f_[0], fl);                                                            \
    }
RS_PW_LEAF(rs_pw_leaf00, 0, 0)
RS_PW_LEAF(rs_pw_leaf01, 0, 1)
RS_PW_LEAF(rs_pw_leaf10, 1, 0)
RS_PW_LEAF(rs_pw_leaf11, 1, 1)
#endif
#else /* !RS_PW_PIPE: the original post */
#ifdef RS_PW_GROUP
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j,
                                                 const volatile HVX_Vector *tile) {
#define RS_TILE(rr) tile[rr]
#else
static __attribute__((noinline)) void rs_pw_post(const rs_op *d, fnpu_u32 o, fnpu_u32 j) {
#define RS_TILE(rr) LV(0)[rr]
#endif
    fnpu_u32 n = d->cpo - 16 * o, nb = d->nbin, at = 16 * o * nb + d->blk0 + j;
    if (n > 16) n = 16;
    const volatile HVX_Vector *bias = P(bias) + 16 * o, *res = d->vres ? P(vres) + at : 0;
    volatile HVX_Vector *out = P(vout) + at;
    HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), mask = P(bias)[d->cpo + j], fl = Q6_V_vzero();
    fnpu_u32 clip = d->clip;
#pragma clang loop unroll(disable)
    for (fnpu_u32 rr = 0; rr < n; rr++) {
        HVX_Vector v = RS_TILE(rr);
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias[rr]));
        if (clip) v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), Q6_V_vsplat_R(0x46004600u));
        if (res) v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, res[rr * nb])); /* check: no qf16+hf fusion */
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
        out[rr * nb] = Q6_V_vand_VV(v, mask);
    }
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], fl);
}
#undef RS_TILE
#endif /* RS_PW_PIPE */
#endif
#ifdef RS_MHEADS
/* B tile rows for block src (= vin + blk0 + j): kind 1 takes pair p at the
 * block itself; kind 8 takes tap t of pair p, shifted (t/3-1)*ws + t%3-1 words. */
static __attribute__((noinline)) void rs_gather(const rs_op *d, volatile HVX_Vector *b, const volatile HVX_Vector *src) {
    fnpu_u32 p = 0;
#ifdef RS_MHEADS
    if (d->kind == P3_KIND) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 t = 0; t < 9; t++) {
            const int off = ((int)(t / 3) - 1) * (int)d->ws + (int)(t % 3) - 1;
            const char *s = (const char *)src + 4 * off;
#pragma clang loop unroll(disable)
            for (fnpu_u32 q = 0; q < d->cpi; q++, p++) b[p] = *(const rs_uv *)(s + 128 * d->nbin * q);
        }
    } else
#endif
#ifdef RS_PW_PIPE
    {   /* the same copies through plain pointers: fields hoisted, loads overlapped */
        const fnpu_u32 cpi = d->cpi, nb = d->nbin, tot = d->nk * 16;
        const HVX_Vector *s = (const HVX_Vector *)src;
        HVX_Vector *bb = (HVX_Vector *)b;
#pragma clang loop unroll_count(4)
        for (; p < cpi; p++) bb[p] = s[p * nb];
        if (p < tot) {
            const HVX_Vector z = LV(57)[0]; /* zero */
#pragma clang loop unroll(disable)
            for (; p < tot; p++) bb[p] = z;
        }
        return;
    }
#endif
#pragma clang loop unroll(disable)
    for (; p < d->cpi; p++) b[p] = src[p * d->nbin];
#pragma clang loop unroll(disable)
    for (; p < d->nk * 16; p++) b[p] = LV(57)[0]; /* zero */
}
#define RS_GATHER(b, src) rs_gather(d, b, src)
#else
#define RS_GATHER(b, src) \
    _Pragma("clang loop unroll(disable)") for (fnpu_u32 p = 0; p < nk * 16; p++) (b)[p] = p < d->cpi ? (src)[p * d->nbin] : LV(57)[0] /* zero */
#endif

#if defined(RS_PW_PIPE) && !(defined(RS_PW_GROUP) && defined(RS_MHEADS)) || defined(RS_PW_PIPE) && defined(RS_PW_PROFILE)
#error "RS_PW_PIPE is an option of the grouped heads build (RS_PW_GROUP, RS_MHEADS), without profiling"
#endif
#if defined(RS_POST_BATCH) && !(defined(RS_POOL) && defined(RS_S2_DIRECT) && defined(RS_DW_FAST))
#error "RS_POST_BATCH is an option of the pooled build"
#endif
#if defined(RS_S2_REV) && !defined(RS_POST_BATCH)
#error "RS_S2_REV is an option of the RS_POST_BATCH build"
#endif
#if defined(RS_MULTI) && !(defined(RS_POOL) && defined(RS_REPLAY) && defined(RS_RES_SLOTS)) || \
    defined(RS_MULTI) && (defined(RS_DIAG_WRAP) || defined(RS_DIAG_TAIL))
#error "RS_MULTI is an option of the pooled --res-slots replay build"
#endif
#if defined(RS_STREAM) && !defined(RS_MULTI)
#error "RS_STREAM is an option of the RS_MULTI build"
#endif
#define RS_STREAM_LATE 0x7du /* segment 1's inputs were not staged in time */
#if defined(RS_TILES) && !(defined(RS_PW_PIPE) && defined(RS_STRIDE2) && defined(RS_DW_FAST) && defined(RS_RESIDENT))
#error "RS_TILES is an option of the pipelined resident heads build"
#endif
#ifdef RS_PW_GROUP
#ifndef RS_PW_FAST
#error "RS_PW_GROUP builds on RS_PW_FAST"
#endif
/* 1x1 over groups of RS_PW_GROUP column blocks: gather every block's B tile,
 * one barrier, every HMX tile of the group into its own output buffer, then
 * every post. Per tile the same HMX loads and the same rs_pw_post as rs_pw
 * below, so outputs are bit-identical. The group pays one post -> barrier/HMX
 * transition instead of one per tile (that wait is fixed per transition, about
 * 0.65 us), and a tile's post no longer waits for its own HMX writes to land.
 * Scratch: RS_PW_GROUP * (nk + no) * 16 vectors (B tiles, then output tiles;
 * each 2 KiB aligned since scr is). */
#ifndef RS_PW_ABLATE
#define RS_PW_ABLATE 0
#elif defined(RS_PW_PROFILE)
#error "RS_PW_ABLATE is a timing diagnostic of the unprofiled grouped build"
#endif
/* RS_PW_ABLATE (diagnostic only; outputs are wrong): skip a phase of every 1x1
 * op to time it by difference. 1: HMX loads (clear + store kept), 2: all HMX
 * work, 4: gather, 8: post, 16: the group barrier after the gather (a race:
 * bounds what that barrier still waits for). Every remaining access keeps its address. */
#ifdef RS_POOL
#if !defined(RS_PW_PIPE) || RS_PW_GROUP != 8 || RS_PW_ABLATE
#error "the pooled 1x1 splits the pipelined 8-tile grouped build (no ablation)"
#endif
#define rs_pw rs_pw_w0 /* worker 0 alone (the RS_POOL_SOLO diagnostic) */
#define RS_PW_POST(d, o, j, t) rs_pw_post(d, o, j, t, RS_FLAGS)
#else
#define RS_PW_POST(d, o, j, t) rs_pw_post(d, o, j, t)
#endif
static __attribute__((noinline)) void rs_pw(const rs_op *d) {
    const fnpu_u32 nk = d->nk, no = (d->cpo + 15) >> 4, nj = d->nj;
    volatile HVX_Vector *bt = P(scr), *ot = bt + RS_PW_GROUP * nk * 16;
#pragma clang loop unroll(disable)
    for (fnpu_u32 j0 = 0; j0 < nj; j0 += RS_PW_GROUP) {
        const fnpu_u32 g = nj - j0 < RS_PW_GROUP ? nj - j0 : RS_PW_GROUP;
#ifdef RS_PW_PROFILE
        {   /* instrument: drain the previous group's post stores separately */
            RS_PT(tq);
            __asm__ volatile("barrier" ::: "memory");
            RS_PADD(6, tq);
        }
#endif
        RS_PT(tg);
#ifdef RS_TILES
        const fnpu_u32 tin = d->onbin & 1;
        volatile HVX_Vector *const bg = tin ? P(vin) + j0 * nk * 16 : bt; /* a tile input is read in place */
#else
        const fnpu_u32 tin = 0;
        volatile HVX_Vector *const bg = bt;
#endif
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g && !tin; jj++) {
            const volatile HVX_Vector *src = P(vin) + d->blk0 + j0 + jj;
            volatile HVX_Vector *b = bt + jj * nk * 16;
            if (!(RS_PW_ABLATE & 4)) RS_GATHER(b, src);
        }
        RS_PADD(2, tg);
        RS_PT(tb);
        if (!(RS_PW_ABLATE & 16)) __asm__ volatile("barrier" ::: "memory");
        RS_PADD(5, tb);
        RS_PT(th);
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g && !(RS_PW_ABLATE & 2); jj++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 o = 0; o < no; o++) {
                fnpu_hmx_clear();
#pragma clang loop unroll(disable)
                for (fnpu_u32 t = 0; t < nk && !(RS_PW_ABLATE & 1); t++)
                    fnpu_hmx_load((const fnpu_u16 *)FR(d->vpar + (o * nk + t) * 16),
                                  (const fnpu_u16 *)(bg + jj * nk * 16 + 16 * t));
                fnpu_hmx_store((fnpu_u16 *)(ot + (jj * no + o) * 16), (const void *)RS_SCALE);
            }
        }
        RS_PADD(3, th);
        RS_PT(tp);
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g && !(RS_PW_ABLATE & 8); jj++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 o = 0; o < no; o++) RS_PW_POST(d, o, j0 + jj, ot + (jj * no + o) * 16);
        }
        RS_PADD(4, tp);
    }
}
#ifdef RS_POOL
#undef rs_pw
/* Pooled 1x1. The 8-tile scratch becomes two halves of RS_PWH tiles (B tiles
 * h*RS_PWH.., output tiles h*RS_PWH.. of the same layout): worker 1 owns half 0
 * and groups 0, 2, 4.., worker 2 half 1 and groups 1, 3, 5... A worker gathers
 * its group into its half, counts it in word 11+w, waits until word 7 (HMX
 * done) reaches the group, posts it, then gathers its next group (so its
 * output tiles are free again when worker 0 sees that count). [Superseded
 * by RS_PWS: 2-tile groups, two slots per worker, so a worker always has its
 * next group gathered while worker 0 runs HMX on the current one. Worker 0
 * posting a share of the groups itself was slower at every share tried: the
 * workers wait on its HMX. Kind 1 ops under RS_PW_POOL_MIN blocks run on
 * worker 0 alone (rs_pw_w0): the job handoff costs more than they gain.] Worker 0 runs
 * every HMX tile, in group order. Counts are (job << 8) + n: a stale value
 * from an earlier job compares below. Per tile the same gather, HMX loads and
 * post as rs_pw_w0, so outputs are bit-identical. */
#define RS_PWS 2u /* tiles per slot: 4 slots in the 8-tile scratch, two per worker */
#define RS_PW_POOL_MIN 8u
/* slot of group k: worker (k & 1) owns slots 2(k & 1) and 2(k & 1) + 1, alternately */
#define RS_PW_SLOT(k) (2u * ((k) & 1u) + (((k) >> 1) & 1u))
/* RS_POOL_PROF (diagnostic): 1x1 tick totals over the request in LV(52),
 * reported in meta[3..8]: 0 worker 0's pooled 1x1 ops, 1 its waits for
 * gathers, 2 worker 1's waits for HMX, 3 worker 1's gathers (+ barrier),
 * 4 worker 1's posts, 5 worker 2's waits for HMX */
#ifdef RS_POOL_PROF
#define RS_PP ((volatile fnpu_u32 *)LV(52))
#define RS_PP_T(v) const fnpu_u32 v = rs_ticks()
#define RS_PP_ADD(i, v) (RS_PP[i] += rs_ticks() - (v))
#else
#define RS_PP_T(v) (void)0
#define RS_PP_ADD(i, v) (void)0
#endif
#define MB_HMXD 7
#define MB_GATH 11 /* + w */
#define RS_FN_PW 3u
/* 1 once word reaches base + n; 0 on a failed request or after the bound (marked failed) */
/* inlined: a call would make the group loops keep their state on the DDR
 * stack, and every later barrier (each HMX store's, each gather's) waits for
 * those stores (~1.9 us each) */
static inline __attribute__((always_inline)) fnpu_u32 rs_pool_until(fnpu_u32 word, fnpu_u32 base, fnpu_u32 n) {
    const fnpu_u32 t0 = rs_ticks();
    while ((int)(RS_MB[word] - base) < (int)n) {
        if (RS_MB[MB_FAIL]) return 0;
        if (rs_ticks() - t0 > RS_POOL_TICKS) {
            RS_MB[MB_FAIL] = 1;
            return 0;
        }
        __asm__ volatile("pause(#1)" ::: "memory"); /* longer pauses timed the same */
    }
    return 1;
}
/* RS_PW_DEEP (RS_TILES): a tile-input op has no B tiles, so its whole scratch
 * holds output slots: D per worker (4 if no <= nk, 3 if no <= 2 nk, else 2;
 * 2 D slots of RS_PWS * no tiles fit 8 (nk + no) tiles). Worker 1 + h owns
 * slots h D .. h D + D - 1 and runs its i-th group (k = 2 i + h) in slot
 * h D + i % D, so D groups are in flight per worker; the counts are unchanged
 * (group i's slot is free once the worker's count reaches i + 1). Every other
 * op: D = 2, the B tiles first, slot h * 2 + (i & 1) (RS_PW_SLOT). */
/* Groups are 1 << lg column blocks: RS_PW_LG (1 or 2, default 2) for a
 * tile-input op with no <= nk, whose scratch then holds D = 8 >> lg slots per
 * worker of 1 << lg blocks (2 D (1 << lg) no <= 16 no <= 8 (nk + no)); RS_PWS
 * for every other op. (Keeping ops under 16 blocks at 2 timed +2 us.) Fewer, larger groups halve the per-group handshakes
 * (mailbox polls, signals, leaf calls), which dominate the small 16 -> 16 ops. */
#ifndef RS_PW_LG
#define RS_PW_LG 2u
#endif
static inline __attribute__((always_inline)) fnpu_u32 rs_pw_lg(const rs_op *d) {
#ifdef RS_PW_DEEP
#if RS_PW_LG == 3
    if ((d->onbin & 1) && ((d->cpo + 15) >> 4) <= d->nk) return d->onbin & 8 ? 3u : 2u;
#else
    if ((d->onbin & 1) && ((d->cpo + 15) >> 4) <= d->nk) return RS_PW_LG;
#endif
#endif
    return 1u;
}
static inline __attribute__((always_inline)) fnpu_u32 rs_pw_depth(const rs_op *d) {
#ifdef RS_PW_DEEP
    const fnpu_u32 nk = d->nk, no = (d->cpo + 15) >> 4;
    if (d->onbin & 1) return no <= nk ? (RS_PW_LG == 1 ? 4u : 2u) : no <= 2u * nk ? 3u : 2u;
#endif
    return 2u;
}
/* first output slot: the B tiles come first unless the op has none */
static inline __attribute__((always_inline)) volatile HVX_Vector *rs_pw_obase(const rs_op *d) {
#ifdef RS_PW_DEEP
    if (d->onbin & 1) return P(scr);
#endif
    return P(scr) + RS_PW_GROUP * d->nk * 16;
}
/* gather group k (RS_PWS column blocks from j0) into B slot b; then barrier */
static inline __attribute__((always_inline)) void rs_pw_wgather(const rs_op *d, fnpu_u32 k, fnpu_u32 slot) {
    const fnpu_u32 nk = d->nk, nj = d->nj, j0 = RS_PWS * k, g = nj - j0 < RS_PWS ? nj - j0 : RS_PWS;
    volatile HVX_Vector *bt = P(scr) + slot * RS_PWS * nk * 16;
#ifdef RS_TILES
    /* a tile input is read by HMX in place: nothing to gather, and no barrier. The
     * count only says the slot's output tiles were read (the post's loads are done
     * when it returns; its stores to vout are ordered by the barrier before DONE),
     * and that barrier would wait for every pending post store (~139 us per crop). */
    if (d->onbin & 1) return;
#endif
#pragma clang loop unroll(disable)
    for (fnpu_u32 jj = 0; jj < g; jj++) RS_GATHER(bt + jj * nk * 16, P(vin) + d->blk0 + j0 + jj);
    RS_BARRIER();
}
/* post g column blocks from j0, whose output tiles are at ot */
static inline __attribute__((always_inline)) void rs_pw_wpost_at(const rs_op *d, fnpu_u32 j0, fnpu_u32 g,
                                                                 volatile HVX_Vector *ot) {
    const fnpu_u32 no = (d->cpo + 15) >> 4;
#pragma clang loop unroll(disable)
    for (fnpu_u32 jj = 0; jj < g; jj++) {
#pragma clang loop unroll(disable)
        for (fnpu_u32 o = 0; o < no; o++) {
            /* the operands rs_pw_post computes, here once per tile */
            const fnpu_u32 nb = d->nbin, at = 16 * o * nb + d->blk0 + j0 + jj;
            const fnpu_u32 n = d->cpo - 16 * o > 16 ? 16 : d->cpo - 16 * o;
#ifdef RS_TILES
            const fnpu_u32 lay = d->onbin, tat = (j0 + jj) * d->cpo + 16 * o;
            const fnpu_u32 oat = lay & 2 ? tat : at, rat = lay & 4 ? tat : at,
                           nnb = n | (lay & 2 ? 1 : nb) << 5 | (lay & 4 ? 1 : nb) << 18;
#else
            const fnpu_u32 oat = at, rat = at, nnb = n | nb << 5;
#endif
            const HVX_Vector *t = (const HVX_Vector *)(ot + (jj * no + o) * 16),
                             *bias = (const HVX_Vector *)P(bias) + 16 * o,
                             *mask = (const HVX_Vector *)P(bias) + d->cpo + j0 + jj,
                             *res = d->vres ? (const HVX_Vector *)P(vres) + rat : 0;
            HVX_Vector *out = (HVX_Vector *)P(vout) + oat;
            if (d->clip) {
                if (res) rs_pw_leaf11(t, bias, res, out, nnb, mask);
                else rs_pw_leaf10(t, bias, res, out, nnb, mask);
            } else {
                if (res) rs_pw_leaf01(t, bias, res, out, nnb, mask);
                else rs_pw_leaf00(t, bias, res, out, nnb, mask);
            }
        }
    }
}
static inline __attribute__((always_inline)) void rs_pw_wpost(const rs_op *d, fnpu_u32 k, fnpu_u32 slot) {
    const fnpu_u32 lg = rs_pw_lg(d), gs = 1u << lg, no = (d->cpo + 15) >> 4, nj = d->nj, j0 = k << lg,
                   g = nj - j0 < gs ? nj - j0 : gs;
    rs_pw_wpost_at(d, j0, g, rs_pw_obase(d) + ((slot * no) << lg) * 16);
}
/* no stack stores inside its group loop (checked in the disassembly): each HMX
 * store's barrier would wait for them */
static __attribute__((noinline)) void rs_pw_hmx(const rs_op *d, fnpu_u32 base) {
    const fnpu_u32 nk = d->nk, no = (d->cpo + 15) >> 4, nj = d->nj, lg = rs_pw_lg(d), gs = 1u << lg,
                   ng = (nj + gs - 1) >> lg, dep = rs_pw_depth(d);
    volatile HVX_Vector *bt0 = P(scr), *ot0 = rs_pw_obase(d);
    fnpu_u32 c = 0; /* (k >> 1) % dep */
#pragma clang loop unroll(disable)
    for (fnpu_u32 k = 0; k < ng; k++) {
        /* group k: worker 1 + (k & 1), its (k >> 1)-th group, slot (k & 1) dep + c */
        const fnpu_u32 b = (k & 1) * dep + c, g = nj - (k << lg) < gs ? nj - (k << lg) : gs;
        if (k & 1) c = c + 1 == dep ? 0 : c + 1;
        RS_PP_T(tw);
        if (!rs_pool_until(MB_GATH + 1 + (k & 1), base, (k >> 1) + 1)) return;
        RS_PP_ADD(1, tw);
#ifdef RS_TILES
        /* B tiles: the tile input itself, else the gathered slot (gathered ops: lg = 1) */
        volatile HVX_Vector *bt = d->onbin & 1 ? P(vin) + ((k * nk) << lg) * 16 : bt0 + b * RS_PWS * nk * 16,
                            *ot = ot0 + ((b * no) << lg) * 16;
#else
        volatile HVX_Vector *bt = bt0 + b * RS_PWS * nk * 16, *ot = ot0 + b * RS_PWS * no * 16;
#endif
#pragma clang loop unroll(disable)
        for (fnpu_u32 jj = 0; jj < g; jj++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 o = 0; o < no; o++) {
                fnpu_hmx_clear();
#pragma clang loop unroll(disable)
                for (fnpu_u32 t = 0; t < nk; t++)
                    fnpu_hmx_load((const fnpu_u16 *)FR(d->vpar + (o * nk + t) * 16),
                                  (const fnpu_u16 *)(bt + jj * nk * 16 + 16 * t));
                fnpu_hmx_store((fnpu_u16 *)(ot + (jj * no + o) * 16), (const void *)RS_SCALE);
            }
        }
        RS_MB[MB_HMXD] = base + k + 1; /* the store's barrier ordered the tiles first */
    }
}
/* worker w's groups k = h, h + 2, .. (h = w - 1): dep in flight, one per slot */
static __attribute__((noinline)) void rs_pw_hvx(const rs_op *d, fnpu_u32 w, fnpu_u32 base) {
    const fnpu_u32 lg = rs_pw_lg(d), ng = (d->nj + (1u << lg) - 1) >> lg, h = w - 1, dep = rs_pw_depth(d),
                   s0 = h * dep;
    fnpu_u32 n = 0;
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < dep && 2 * i + h < ng; i++) { /* fill every slot */
        RS_PP_T(tg);
        rs_pw_wgather(d, 2 * i + h, s0 + i);
        if (w == 1) RS_PP_ADD(3, tg);
        RS_MB[MB_GATH + w] = base + ++n;
    }
    fnpu_u32 c = 0; /* i % dep */
#pragma clang loop unroll(disable)
    for (fnpu_u32 k = h; k < ng; k += 2) {
        RS_PP_T(tw);
        if (!rs_pool_until(MB_HMXD, base, k + 1)) return;
        RS_PP_ADD(w == 1 ? 2 : 5, tw);
        RS_PP_T(tp);
        rs_pw_wpost(d, k, s0 + c); /* frees the slot's output tiles; its B tiles were free once HMX(k) ran */
        if (w == 1) RS_PP_ADD(4, tp);
        if (k + 2 * dep < ng) {
            RS_PP_T(tg);
            rs_pw_wgather(d, k + 2 * dep, s0 + c); /* the same slot */
            if (w == 1) RS_PP_ADD(3, tg);
            RS_MB[MB_GATH + w] = base + ++n;
        }
        c = c + 1 == dep ? 0 : c + 1;
    }
}
static __attribute__((noinline)) void rs_pool_run(fnpu_u32 fn, const rs_op *d);
static void rs_pw(const rs_op *d) {
#ifdef RS_POOL_SOLO
    rs_pw_w0(d);
#else
    if (d->kind == 1 && d->nj < RS_PW_POOL_MIN) {
        rs_pw_w0(d);
        return;
    }
    RS_PP_T(t0);
    rs_pool_run(RS_FN_PW, d);
    RS_PP_ADD(0, t0);
#endif
}
#endif
#else
static __attribute__((noinline)) void rs_pw(const rs_op *d) {
    volatile HVX_Vector *bt = P(scr);
    fnpu_u32 nk = d->nk;
#pragma clang loop unroll(disable)
    for (fnpu_u32 j = 0; j < d->nj; j++) {
        const volatile HVX_Vector *src = P(vin) + d->blk0 + j;
#ifdef RS_PW_PROFILE
        {   /* instrument: drain the previous block's post stores separately */
            RS_PT(tq);
            __asm__ volatile("barrier" ::: "memory");
            RS_PADD(6, tq);
        }
#endif
        RS_PT(tg);
        RS_GATHER(bt, src);
        RS_PADD(2, tg);
        RS_PT(tb);
        __asm__ volatile("barrier" ::: "memory");
        RS_PADD(5, tb);
#pragma clang loop unroll(disable)
        for (fnpu_u32 o = 0; 16 * o < d->cpo; o++) {
            RS_PT(th);
            fnpu_hmx_clear();
#pragma clang loop unroll(disable)
            for (fnpu_u32 t = 0; t < nk; t++)
                fnpu_hmx_load((const fnpu_u16 *)FR(d->vpar + (o * nk + t) * 16), (const fnpu_u16 *)(bt + 16 * t));
            fnpu_hmx_store((fnpu_u16 *)LV(0), (const void *)RS_SCALE);
            RS_PADD(3, th);
#ifdef RS_PW_PROFILE
            {   /* instrument: a second barrier after the HMX store */
                RS_PT(ts);
                __asm__ volatile("barrier" ::: "memory");
                RS_PADD(8, ts);
            }
#endif
            RS_PT(tp);
#ifdef RS_PW_FAST
            rs_pw_post(d, o, j);
#else
#pragma clang loop unroll(disable)
            for (fnpu_u32 rr = 0; rr < 16 && 16 * o + rr < d->cpo; rr++) rs_post(d, LV(0)[rr], 16 * o + rr, j);
#endif
            RS_PADD(4, tp);
#ifdef RS_PW_PROFILE
            {   /* instrument: drain this tile's post stores */
                RS_PT(tr);
                __asm__ volatile("barrier" ::: "memory");
                RS_PADD(7, tr);
            }
#endif
        }
    }
}
#endif

#ifdef RS_DW_FAST
#if RS_DW_BLOCKS != 2 || defined(RS_STEM)
#error "RS_DW_FAST is the 2-block, stem-free depthwise"
#endif
/* Keeps v in a register as its own value: stops LLVM fusing an FP16 rounding
 * (hf = qf16) into the following qf16 + hf add, which would skip that rounding. */
#define RS_FENCE(v) __asm__ volatile("" : "+v"(v))
/* rs_post's operations for a depthwise output (no residual: rs_get rejects
 * vres on depthwise), in the same order; flags accumulate in fl. */
#define RS_DW_POST(v, at, bias, mask, fl)                                                    \
    do {                                                                                  \
        HVX_Vector x_ = (v);                                                              \
        RS_FENCE(x_);                                                                     \
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(x_, exp), exp), -1)); \
        x_ = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(x_, bias));                         \
        if (clip) x_ = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(x_, Q6_V_vzero()), six);    \
        fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(x_, exp), exp), -1)); \
        P(vout)[at] = Q6_V_vand_VV(x_, mask);                                             \
    } while (0)
/* rs_dw with the taps unrolled for 3x3, the post inlined and the overflow
 * flags held in a register for the whole op (one RS_FLAGS update). Same
 * products, same accumulation order, same post: bit-identical output. */
#ifdef RS_POOL
/* channels c0..c1-1 only (their weight splats too), flags ORed into fdst */
static __attribute__((noinline)) void rs_dw_range(const rs_op *d, fnpu_u32 c0, fnpu_u32 c1,
                                                  volatile HVX_Vector *fdst) {
#else
static __attribute__((noinline)) void rs_dw(const rs_op *d) {
#endif
    const fnpu_u32 k = d->nk, kk = k * k, step = 4 * (d->ws - k), nb = d->nbin, clip = d->clip;
    const fnpu_u32 base = 4 * (32 * d->blk0 - (k >> 1) * (d->ws + 1));
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->vpar);
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), six = Q6_V_vsplat_R(0x46004600u);
    const HVX_Vector zq = Q6_Vqf16_vadd_VhfVhf(Q6_V_vzero(), Q6_V_vzero());
    HVX_Vector fl = Q6_V_vzero();
#ifdef RS_POST_BATCH
    /* fields hoisted (stores through the output would make the compiler reload
     * them from *d), and the overflow flags as a running maximum per vector:
     * a lane ends set iff some (x & 0x7c00) == 0x7c00, as with the compares */
    const fnpu_u32 cpo = d->cpo, njv = d->nj, ws4 = 4 * d->ws;
    HVX_Vector *const outv = (HVX_Vector *)P(vout);
    const HVX_Vector *const bv = (const HVX_Vector *)P(bias);
#ifdef RS_TILES
    const fnpu_u32 tl = d->onbin & 2, js = tl ? d->cpi : 1u, cs = tl ? 1u : nb, a0b = tl ? 0u : d->blk0;
#else
    const fnpu_u32 js = 1u, cs = nb, a0b = d->blk0;
#endif
    HVX_Vector mx0 = Q6_V_vzero(), mx1 = mx0;
#endif
#ifdef RS_POOL
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = c0 * kk; i < c1 * kk; i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = c0; c < c1; c++) {
#else
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cpi * kk; i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cpi; c++) {
#endif
        const char *s = (const char *)d->vin + 128 * nb * c + base;
        const HVX_Vector *w = (const HVX_Vector *)d->scr + c * kk;
        const HVX_Vector bias = P(bias)[c];
#pragma clang loop unroll(disable)
#ifdef RS_POST_BATCH
        for (fnpu_u32 j = 0; j < njv; j += 2) {
#else
        for (fnpu_u32 j = 0; j < d->nj; j += 2) {
#endif
            HVX_Vector a0 = zq, a1 = zq;
            const char *p = s + 128 * j;
            if (k == 3) {
#ifdef RS_POST_BATCH
                const fnpu_u32 r = ws4;
#else
                const fnpu_u32 r = 4 * d->ws;
#endif
#define RS_TAP(o, t)                                                                          \
    a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + (o)), w[t]));       \
    a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + (o) + 128), w[t]))
                RS_TAP(0, 0); RS_TAP(4, 1); RS_TAP(8, 2);
                RS_TAP(r, 3); RS_TAP(r + 4, 4); RS_TAP(r + 8, 5);
                RS_TAP(2 * r, 6); RS_TAP(2 * r + 4, 7); RS_TAP(2 * r + 8, 8);
#undef RS_TAP
            } else {
                const HVX_Vector *wt = w;
#pragma clang loop unroll(disable)
                for (fnpu_u32 dy = 0; dy < k; dy++, p += step) {
#pragma clang loop unroll(disable)
                    for (fnpu_u32 dx = 0; dx < k; dx++, p += 4, wt++) {
                        a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)p, *wt));
                        a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + 128), *wt));
                    }
                }
            }
#ifdef RS_POST_BATCH
            {   /* RS_DW_POST's steps for both vectors, one fence holding the two */
                const fnpu_u32 at = c * cs + a0b + j * js;
                HVX_Vector x0 = Q6_Vhf_equals_Vqf16(a0), x1 = Q6_Vhf_equals_Vqf16(a1);
                __asm__ volatile("" : "+v"(x0), "+v"(x1));
                mx0 = Q6_Vuh_vmax_VuhVuh(mx0, Q6_V_vand_VV(x0, exp));
                mx1 = Q6_Vuh_vmax_VuhVuh(mx1, Q6_V_vand_VV(x1, exp));
                x0 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(x0, bias));
                x1 = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(x1, bias));
                if (clip) {
                    x0 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(x0, Q6_V_vzero()), six);
                    x1 = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(x1, Q6_V_vzero()), six);
                }
                mx0 = Q6_Vuh_vmax_VuhVuh(mx0, Q6_V_vand_VV(x0, exp));
                mx1 = Q6_Vuh_vmax_VuhVuh(mx1, Q6_V_vand_VV(x1, exp));
                outv[at] = Q6_V_vand_VV(x0, bv[cpo + j]);
                outv[at + js] = Q6_V_vand_VV(x1, bv[cpo + j + 1]);
            }
#else
#ifdef RS_TILES
            /* a tile output: (pair c, block j) at j * cpi + c */
            const fnpu_u32 js = d->onbin & 2 ? d->cpi : 1u,
                           at = d->onbin & 2 ? j * d->cpi + c : c * nb + d->blk0 + j;
#else
            const fnpu_u32 js = 1, at = c * nb + d->blk0 + j;
#endif
            RS_DW_POST(Q6_Vhf_equals_Vqf16(a0), at, bias, P(bias)[d->cpo + j], fl);
            RS_DW_POST(Q6_Vhf_equals_Vqf16(a1), at + js, bias, P(bias)[d->cpo + j + 1], fl);
#endif
        }
    }
#ifdef RS_POST_BATCH
    fl = Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_Vuh_vmax_VuhVuh(mx0, mx1), exp), -1);
#endif
#ifdef RS_POOL
    fdst[0] = Q6_V_vor_VV(fdst[0], fl);
}
static void rs_pool_slice(fnpu_u32 fn, const rs_op *d, fnpu_u32 w, fnpu_u32 job) {
    const fnpu_u32 c0 = d->cpi * w / 3u, c1 = d->cpi * (w + 1u) / 3u;
    volatile HVX_Vector *fdst = w ? RS_WFLAGS(w) : RS_FLAGS;
    if (fn == RS_FN_PW) {
        if (w) rs_pw_hvx(d, w, job << 8);
        else rs_pw_hmx(d, job << 8);
    } else if (fn == RS_FN_DW) rs_dw_range(d, c0, c1, fdst);
#ifdef RS_S2_DIRECT
    else if (fn == RS_FN_S2) rs_s2_range(d, c0, c1, fdst);
#endif
}
/* worker 0: post the job, run share 0, wait for shares 1 and 2, merge flags */
static __attribute__((noinline)) void rs_pool_run(fnpu_u32 fn, const rs_op *d) {
    if (RS_MB[MB_FAIL]) return; /* the request already fails */
    const fnpu_u32 job = RS_MB[MB_JOB] + 1u;
    RS_MB[MB_FN] = fn;
    RS_MB[MB_ARG] = (fnpu_u32)d;
    RS_BARRIER();
    RS_MB[MB_JOB] = job;
    rs_pool_slice(fn, d, 0, job);
#ifdef RS_POOL_SOLO
    /* DIAGNOSTIC: worker 0 runs the other shares itself (workers leave at once) */
    rs_pool_slice(fn, d, 1, job);
    rs_pool_slice(fn, d, 2, job);
    RS_MB[MB_DONE + 1] = job;
    RS_MB[MB_DONE + 2] = job;
#endif
    if (!rs_pool_wait(MB_DONE, job)) {
        RS_MB[MB_FAIL] = 1;
        return;
    }
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], Q6_V_vor_VV(RS_WFLAGS(1)[0], RS_WFLAGS(2)[0]));
}
static void rs_dw(const rs_op *d) { rs_pool_run(RS_FN_DW, d); }
/* Workers 1 and 2 run on a VTCM stack (LV(33..39) below LV(40) for worker
 * 1, LV(41..47) below LV(48) for worker 2; 896 bytes each, inside the saved
 * page): a barrier waits ~1.9 us for any pending store to the DDR stack, and
 * the 1x1 gather's barrier follows the post's register traffic. VTCM stores
 * do not hold a barrier. A fault returns through the loader, which reloads
 * the DDR stack pointer it saved. */
#define RS_WSTACK_TOP(w) (0x31001400u + 0x400u * ((w) - 1u))
static __attribute__((used, noinline)) fnpu_u32 rs_pool_worker_body(fnpu_u32 w);
fnpu_u32 rs_on_vtcm(fnpu_u32 w, fnpu_u32 sp);
__asm__(".text\n"
        ".p2align 4\n"
        "rs_on_vtcm:\n" /* r0 = w, r1 = stack top: rs_pool_worker_body(w) on that stack */
        " { allocframe(#8) }\n"
        " { memw(r29+#0) = r16 }\n"
        " { r16 = r29 }\n"
        " { r29 = r1 }\n"
        " { call rs_pool_worker_body }\n"
        " { r29 = r16 }\n"
        " { r16 = memw(r29+#0) }\n"
        " { dealloc_return }\n");
fnpu_u32 tl_pool_worker(fnpu_u32 w) {
    if (w - 1u > 1u) return 0;
    return rs_on_vtcm(w, RS_WSTACK_TOP(w));
}
static __attribute__((used, noinline)) fnpu_u32 rs_pool_worker_body(fnpu_u32 w) {
    const fnpu_u32 seq = RS_SEQ;
#ifdef RS_POOL_SOLO
    if (seq) { /* DIAGNOSTIC: leave, acknowledging the exit in advance once the pool opens */
        while (RS_W0_SAVED != 1u) {
            if (RS_W0_DONE == seq) return 0;
            __asm__ volatile("pause(#255)" ::: "memory");
        }
        while (RS_MB[MB_OPEN] != seq || RS_MB[MB_OPEN2] != ~seq) {
            if (RS_W0_DONE == seq) return 0;
            RS_IDLE();
        }
        RS_MB[MB_ACK + w] = seq;
        RS_BARRIER();
        return 0;
    }
#endif
    /* uncached polls until worker 0's VTCM page is saved (EVB is ours from then on) */
    while (RS_W0_SAVED != 1u) {
        if (RS_W0_DONE == seq) return 0;
        __asm__ volatile("pause(#255)" ::: "memory");
    }
    fnpu_u32 last = 0;
    for (;;) {
        if (RS_MB[MB_OPEN] != seq || RS_MB[MB_OPEN2] != ~seq) {
            if (RS_W0_DONE == seq) return 0; /* never opened, or the page is restored */
            RS_IDLE();
            continue;
        }
        const fnpu_u32 job = RS_MB[MB_JOB];
        if (job != last) {
            rs_pool_slice(RS_MB[MB_FN], (const rs_op *)RS_MB[MB_ARG], w, job);
            RS_BARRIER();
            RS_MB[MB_DONE + w] = job;
            last = job;
            continue;
        }
        if (RS_MB[MB_EXIT] == seq) {
            RS_BARRIER();
            RS_MB[MB_ACK + w] = seq;
            RS_BARRIER();
            return 0;
        }
        RS_IDLE();
    }
}
#else
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], fl);
}
#endif
#else
static __attribute__((noinline)) void rs_dw(const rs_op *d) {
    const fnpu_u32 k = d->nk, kk = k * k, step = 4 * (d->ws - k);
    const fnpu_u32 base = 4 * (32 * d->blk0 - (k >> 1) * (d->ws + 1));
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->vpar);
    RS_ACC(4)[0] = Q6_Vqf16_vadd_VhfVhf(Q6_V_vzero(), Q6_V_vzero());
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cpi * kk; i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cpi; c++) {
        const char *s = (const char *)d->vin + 128 * d->nbin * c + base;
#pragma clang loop unroll(disable)
        for (fnpu_u32 j = 0; j < d->nj; j += RS_DW_BLOCKS) {
            HVX_Vector a0 = RS_ACC(4)[0], a1 = a0; /* qf16 zero */
#if RS_DW_BLOCKS == 4
            HVX_Vector a2 = a0, a3 = a0;
#endif
            const char *p = s + 128 * j;
            const HVX_Vector *w = (const HVX_Vector *)d->scr + c * kk;
#pragma clang loop unroll(disable)
            for (fnpu_u32 dy = 0; dy < k; dy++, p += step) {
#pragma clang loop unroll(disable)
                for (fnpu_u32 dx = 0; dx < k; dx++, p += 4, w++) {
                    HVX_Vector cw = *w;
                    a0 = Q6_Vqf16_vadd_Vqf16Vqf16(a0, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)p, cw));
                    a1 = Q6_Vqf16_vadd_Vqf16Vqf16(a1, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + 128), cw));
#if RS_DW_BLOCKS == 4
                    a2 = Q6_Vqf16_vadd_Vqf16Vqf16(a2, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + 256), cw));
                    a3 = Q6_Vqf16_vadd_Vqf16Vqf16(a3, Q6_Vqf16_vmpy_VhfVhf(*(const rs_uv *)(p + 384), cw));
#endif
                }
            }
            RS_ACC(0)[0] = a0; RS_ACC(1)[0] = a1;
#if RS_DW_BLOCKS == 4
            RS_ACC(2)[0] = a2; RS_ACC(3)[0] = a3;
#endif
#pragma clang loop unroll(disable)
            for (fnpu_u32 q = 0; q < RS_DW_BLOCKS; q++) rs_post(d, Q6_Vhf_equals_Vqf16(RS_ACC(q)[0]), c, j + q);
        }
    }
}
#endif

#if defined(RS_STRIDE2) && defined(RS_S2_DIRECT)
#ifdef RS_POOL
/* channels c0..c1-1 only: their output planes (zeroed here), weight splats and
 * flags (ORed into fdst). Every store stays inside its channel's plane. */
static __attribute__((noinline)) void rs_s2_range(const rs_op *d, fnpu_u32 c0, fnpu_u32 c1,
                                                  volatile HVX_Vector *fdst) {
    const fnpu_u32 ws = d->ws, ows = d->ows, kk = 9;
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->vpar);
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), top = Q6_V_vsplat_R(0x46004600u);
    rs_zero((volatile HVX_Vector *)d->vout + c0 * d->onbin, (c1 - c0) * d->onbin);
    HVX_Vector fl = Q6_V_vzero();
#ifdef RS_POST_BATCH
    HVX_Vector mx = Q6_V_vzero();
#endif
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = c0 * kk; i < c1 * kk; i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
    const fnpu_u32 off0 = 32 * d->blk0 + 1 + RS_S2_P + RS_S2_P * ws - ws;
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = c0; c < c1; c++) {
#else
static __attribute__((noinline)) void rs_s2(rs_op *d) {
    const fnpu_u32 ws = d->ws, ows = d->ows, kk = 9;
    const volatile fnpu_u32 *coef = (const volatile fnpu_u32 *)FR(d->vpar);
    const HVX_Vector exp = Q6_V_vsplat_R(0x7c007c00u), top = Q6_V_vsplat_R(0x46004600u);
    rs_zero((volatile HVX_Vector *)d->vout, d->cpo * d->onbin);
    RS_ACC(4)[0] = Q6_Vqf16_vadd_VhfVhf(Q6_V_vzero(), Q6_V_vzero());
#ifdef RS_DW_FAST
    HVX_Vector fl = Q6_V_vzero();
#endif
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cpi * kk; i++) P(scr)[i] = Q6_V_vsplat_R(coef[i]);
    /* word of tap (dy = 0, dx = 0) for output (0, 0): row P-1, column P-1 */
    const fnpu_u32 off0 = 32 * d->blk0 + 1 + RS_S2_P + RS_S2_P * ws - ws;
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cpi; c++) {
#endif
        const char *in = (const char *)d->vin + 128 * d->nbin * c + 4 * off0;
        volatile char *out = (volatile char *)d->vout + 128 * d->onbin * c + 4 * (32 * d->oblk0 + 2);
        const HVX_Vector *w = (const HVX_Vector *)d->scr + c * kk;
        const HVX_Vector bias = P(bias)[c];
#ifdef RS_POST_BATCH
        /* two output rows per step (an odd last row pairs with itself: the same
         * values stored twice), the same per-vector operations in the same order;
         * fields hoisted, flags as a running maximum (see rs_dw_range) */
        const fnpu_u32 oh = d->oh, ow = d->ow;
        const HVX_Vector qz = RS_ACC(4)[0];
#pragma clang loop unroll(disable)
        for (fnpu_u32 y = 0; y < oh; y += 2) {
            const fnpu_u32 y1 = y + 1 < oh ? y + 1 : y;
#ifdef RS_S2_REV
            /* column blocks last to first: row y's last-block spill (zero lanes past
             * the row) lands on row y1's first columns before row y1's own block 0
             * rewrites them. In the ascending order below, a row of more than one
             * block (ow > 32, n = ow % 32 > 0) loses row y1's first 32 - n - (ows - ow)
             * columns to it (the detector's 80 -> 40 layer: columns 0..21). */
#pragma clang loop unroll(disable)
            for (fnpu_u32 xb = (ow + 31) >> 5; xb-- > 0;) {
                const fnpu_u32 x0 = 32 * xb;
#else
#pragma clang loop unroll(disable)
            for (fnpu_u32 x0 = 0; x0 < ow; x0 += 32) {
#endif
                HVX_Vector a = qz, b = qz;
                const char *p = in + 4 * (2 * y * ws + 2 * x0), *q = in + 4 * (2 * y1 * ws + 2 * x0);
#pragma clang loop unroll(disable)
                for (fnpu_u32 dy = 0; dy < 3; dy++, p += 4 * ws, q += 4 * ws) {
                    HVX_VectorPair e = Q6_W_vdeal_VVR(*(const rs_uv *)(p + 128), *(const rs_uv *)p, -4);
                    HVX_VectorPair f = Q6_W_vdeal_VVR(*(const rs_uv *)(p + 136), *(const rs_uv *)(p + 8), -4);
                    HVX_VectorPair g = Q6_W_vdeal_VVR(*(const rs_uv *)(q + 128), *(const rs_uv *)q, -4);
                    HVX_VectorPair h = Q6_W_vdeal_VVR(*(const rs_uv *)(q + 136), *(const rs_uv *)(q + 8), -4);
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(e), w[3 * dy]));
                    b = Q6_Vqf16_vadd_Vqf16Vqf16(b, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(g), w[3 * dy]));
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_hi_W(e), w[3 * dy + 1]));
                    b = Q6_Vqf16_vadd_Vqf16Vqf16(b, Q6_Vqf16_vmpy_VhfVhf(Q6_V_hi_W(g), w[3 * dy + 1]));
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(f), w[3 * dy + 2]));
                    b = Q6_Vqf16_vadd_Vqf16Vqf16(b, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(h), w[3 * dy + 2]));
                }
                HVX_Vector v = Q6_Vhf_equals_Vqf16(a), u = Q6_Vhf_equals_Vqf16(b);
                __asm__ volatile("" : "+v"(v), "+v"(u));
                v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias));
                u = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(u, bias));
                v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), top);
                u = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(u, Q6_V_vzero()), top);
                const fnpu_u32 n = ow - x0;
                if (n < 32) {
                    const HVX_VectorPred lim = Q6_Q_vsetq_R(4 * n);
                    v = Q6_V_vmux_QVV(lim, v, Q6_V_vzero());
                    u = Q6_V_vmux_QVV(lim, u, Q6_V_vzero());
                }
                mx = Q6_Vuh_vmax_VuhVuh(mx, Q6_Vuh_vmax_VuhVuh(Q6_V_vand_VV(v, exp), Q6_V_vand_VV(u, exp)));
                *(volatile rs_uv *)(out + 4 * (y * ows + x0)) = v;
                *(volatile rs_uv *)(out + 4 * (y1 * ows + x0)) = u;
            }
        }
        continue;
#endif
#pragma clang loop unroll(disable)
        for (fnpu_u32 y = 0; y < d->oh; y++) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 x0 = 0; x0 < d->ow; x0 += 32) {
                HVX_Vector a = RS_ACC(4)[0]; /* qf16 zero */
                const char *p = in + 4 * (2 * y * ws + 2 * x0);
#pragma clang loop unroll(disable)
                for (fnpu_u32 dy = 0; dy < 3; dy++, p += 4 * ws) {
                    HVX_VectorPair e = Q6_W_vdeal_VVR(*(const rs_uv *)(p + 128), *(const rs_uv *)p, -4);
                    HVX_VectorPair f = Q6_W_vdeal_VVR(*(const rs_uv *)(p + 136), *(const rs_uv *)(p + 8), -4);
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(e), w[3 * dy]));
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_hi_W(e), w[3 * dy + 1]));
                    a = Q6_Vqf16_vadd_Vqf16Vqf16(a, Q6_Vqf16_vmpy_VhfVhf(Q6_V_lo_W(f), w[3 * dy + 2]));
                }
#ifdef RS_DW_FAST
                HVX_Vector v = Q6_Vhf_equals_Vqf16(a);
                RS_FENCE(v); /* the FP16 rounding stays its own step, as below */
                v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(v, bias));
#else
                /* Round the accumulator to FP16 before the bias, as rs_dw/rs_post do:
                 * the volatile slot stops LLVM fusing it into a qf16+hf add. */
                RS_ACC(0)[0] = Q6_Vhf_equals_Vqf16(a);
                HVX_Vector v = Q6_Vhf_equals_Vqf16(Q6_Vqf16_vadd_VhfVhf(RS_ACC(0)[0], bias));
#endif
                v = Q6_Vhf_vmin_VhfVhf(Q6_Vhf_vmax_VhfVhf(v, Q6_V_vzero()), top);
                fnpu_u32 n = d->ow - x0;
                if (n < 32) v = Q6_V_vmux_QVV(Q6_Q_vsetq_R(4 * n), v, Q6_V_vzero());
#ifdef RS_DW_FAST
                fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
#else
                RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(Q6_V_vand_VV(v, exp), exp), -1));
#endif
                *(volatile rs_uv *)(out + 4 * (y * ows + x0)) = v;
            }
        }
    }
#ifdef RS_POST_BATCH
    fl = Q6_V_vor_VV(fl, Q6_V_vand_QR(Q6_Q_vcmp_eq_VhVh(mx, exp), -1));
#endif
#ifdef RS_POOL
    fdst[0] = Q6_V_vor_VV(fdst[0], fl);
}
static __attribute__((noinline)) void rs_s2(rs_op *d) {
    RS_ACC(4)[0] = Q6_Vqf16_vadd_VhfVhf(Q6_V_vzero(), Q6_V_vzero()); /* read by every share */
    rs_pool_run(RS_FN_S2, d);
}
#else
#ifdef RS_DW_FAST
    RS_FLAGS[0] = Q6_V_vor_VV(RS_FLAGS[0], fl);
#endif
}
#endif
#elif defined(RS_STRIDE2)
static __attribute__((noinline)) void rs_s2(rs_op *d) {
    fnpu_u32 dst = d->vout;
    rs_zero((volatile HVX_Vector *)dst, d->cpo * d->onbin);
    d->vout = d->tmp;
    rs_dw(d);
    d->vout = dst;
#ifdef RS_S2_EVEN
    /* Symmetric (1,1) padding (PyTorch/Mercury): keep stride-1 word (0, 0). */
    const volatile fnpu_u32 *s0 = (const volatile fnpu_u32 *)d->tmp + 32 * d->blk0 + 2;
#else
    const volatile fnpu_u32 *s0 = (const volatile fnpu_u32 *)d->tmp + 32 * d->blk0 + d->ws + 3;
#endif
    volatile fnpu_u32 *t0 = (volatile fnpu_u32 *)dst + 32 * d->oblk0 + 2;
#pragma clang loop unroll(disable)
    for (fnpu_u32 c = 0; c < d->cpo; c++, s0 += 32 * d->nbin, t0 += 32 * d->onbin) {
        const volatile fnpu_u32 *s = s0;
        volatile fnpu_u32 *t = t0;
#pragma clang loop unroll(disable)
        for (fnpu_u32 y = 0; y < d->oh; y++, s += 2 * d->ws, t += d->ows) {
#pragma clang loop unroll(disable)
            for (fnpu_u32 x = 0; x < d->ow; x++) t[x] = s[2 * x];
        }
    }
}
#endif

#ifdef RS_STEM
#include "teacher_stem_impl.h"
#endif
#ifdef RS_STEM_PROFILE
#include "teacher_stem_profile.h"
#endif

#ifdef RS_CBIAS
/* Expand a validated compute op's packed biases and masks at RS_XB (rs_get
 * bounded cpo + nj by RS_XB_N and the packed words by the image) */
static __attribute__((noinline)) void rs_cbias(rs_op *d) {
    const volatile fnpu_u32 *w = (const volatile fnpu_u32 *)d->bias;
    const volatile HVX_Vector *mk = (const volatile HVX_Vector *)d->bias + ((d->cpo + 31) >> 5);
    volatile HVX_Vector *x = FR(RS_XB);
#pragma clang loop unroll(disable)
    for (fnpu_u32 i = 0; i < d->cpo; i++) x[i] = Q6_V_vsplat_R(w[i]);
#pragma clang loop unroll(disable)
    for (fnpu_u32 j = 0; j < d->nj; j++) x[d->cpo + j] = mk[j];
    d->bias = (fnpu_u32)x;
}
#endif

#ifdef RS_DIAG_TAIL
/* DIAGNOSTIC: the wrapper's code layout, by a tail call (no frame) */
static fnpu_u32 rs_job(const fnpu_context *c);
fnpu_u32 frame_job(const fnpu_context *c) { return rs_job(c); }
static __attribute__((noinline)) fnpu_u32 rs_job(const fnpu_context *c) {
#elif defined(RS_POOL) && !defined(RS_DIAG_WRAP)
/* Every return of the request closes the pool before the loader restores
 * VTCM. frame_job keeps its return address and rs_job's result in mailbox
 * words 14 and 15 instead of a stack frame, so rs_job's callees run at the
 * stack depth of the plain build (RS_POOL_SP_PAD makes up rs_job's own frame
 * difference). The 1x1 post is ~60 us/crop slower with the stack 8 or more
 * bytes deeper than the plain build's; the cause is open. */
#ifdef RS_MULTI
/* RS_MULTI: a request of 256 input bytes carries two segments (two models'
 * tables, crop counts and residency slots); rs_job(c, seg) runs one, with its
 * request words at input word 32 * seg and its metadata in output block seg.
 * Segment 1 runs only after segment 0 returned FNPU_OK; the pool is closed
 * once, after the last. Mailbox word 16 keeps the context across the calls. */
static __attribute__((used, noinline)) fnpu_u32 rs_job(const fnpu_context *c, fnpu_u32 seg);
__asm__(".text\n"
        ".globl " RS_STR(frame_job) "\n"
        ".type " RS_STR(frame_job) ",@function\n"
        ".p2align 4\n" RS_STR(frame_job) ":\n"
        " { r1 = ##0x31001438 }\n"
        " { memw(r1+#0) = r31 }\n"
        " { memw(r1+#8) = r0 }\n"
        " { r1 = #0 }\n"
        " { call rs_job }\n"
        " { r1 = ##0x31001438 }\n"
        " { memw(r1+#4) = r0 }\n"
        " { p0 = cmp.eq(r0,#0) }\n"
        " { if (!p0) jump 1f }\n"
        " { r0 = memw(r1+#8) }\n"
        " { r2 = memw(r0+#24) }\n" /* input_bytes */
        " { p0 = cmp.eq(r2,#256) }\n"
        " { if (!p0) jump 1f }\n"
        " { r1 = #1 }\n"
        " { call rs_job }\n"
        " { r1 = ##0x31001438 }\n"
        " { memw(r1+#4) = r0 }\n"
        "1:\n"
        " { call rs_pool_close }\n"
        " { r1 = ##0x31001438 }\n"
        " { r31 = memw(r1+#0) }\n"
        " { r2 = memw(r1+#4) }\n"
        " { p0 = cmp.eq(r0,#0) }\n"
        " { if (p0) r2 = #0x7e }\n"
        " { r0 = r2; jumpr r31 }\n"
        ".size " RS_STR(frame_job) ", .-" RS_STR(frame_job) "\n");
static __attribute__((used, noinline)) fnpu_u32 rs_job(const fnpu_context *c, fnpu_u32 seg) {
#else
static __attribute__((used, noinline)) fnpu_u32 rs_job(const fnpu_context *c);
__asm__(".text\n"
        ".globl " RS_STR(frame_job) "\n"
        ".type " RS_STR(frame_job) ",@function\n"
        ".p2align 4\n" RS_STR(frame_job) ":\n"
        " { r1 = ##0x31001438 }\n"
        " { memw(r1+#0) = r31 }\n"
        " { call rs_job }\n"
        " { r1 = ##0x31001438 }\n"
        " { memw(r1+#4) = r0 }\n"
        " { call rs_pool_close }\n"
        " { r1 = ##0x31001438 }\n"
        " { r31 = memw(r1+#0) }\n"
        " { r2 = memw(r1+#4) }\n"
        " { p0 = cmp.eq(r0,#0) }\n"
        " { if (p0) r2 = #0x7e }\n"
        " { r0 = r2; jumpr r31 }\n"
        ".size " RS_STR(frame_job) ", .-" RS_STR(frame_job) "\n");
static __attribute__((used, noinline)) fnpu_u32 rs_job(const fnpu_context *c) {
#endif
#ifdef RS_POOL_SP_PAD
    char sp_pad_[RS_POOL_SP_PAD]; /* never stored to */
    __asm__ volatile("" :: "r"(sp_pad_) : "memory");
#endif
#elif defined(RS_POOL) || defined(RS_DIAG_WRAP)
static fnpu_u32 rs_job(const fnpu_context *c);
/* every return of the request closes the pool before the loader restores VTCM */
fnpu_u32 frame_job(const fnpu_context *c) {
#ifdef RS_DIAG_PAD
    char pad_[RS_DIAG_PAD]; /* DIAGNOSTIC: shift the stack (never stored to) */
    __asm__ volatile("" :: "r"(pad_) : "memory");
#endif
    const fnpu_u32 r = rs_job(c);
#ifdef RS_POOL
    return rs_pool_close() ? r : 0x7eu;
#else
    __asm__ volatile("" ::: "memory"); /* DIAGNOSTIC: the wrapper frame alone */
    return r;
#endif
}
static __attribute__((noinline)) fnpu_u32 rs_job(const fnpu_context *c) {
#else
fnpu_u32 frame_job(const fnpu_context *c) {
#endif
#ifdef RS_MULTI
    /* segment seg's request words; segment 1 always runs its whole table */
    const volatile fnpu_u32 *in = (const volatile fnpu_u32 *)fnpu_input(c) + 32u * seg;
    const fnpu_u32 nseg = c->input_bytes >> 7;
    fnpu_u32 tab = in[1], n = in[2], a = seg ? 0u : c->params[1], b = seg ? n : c->params[2], i;
#else
    const volatile fnpu_u32 *in = fnpu_input(c);
    fnpu_u32 tab = in[1], n = in[2], a = c->params[1], b = c->params[2], i;
#endif
#ifdef RS_REPLAY
    fnpu_u32 nc = in[3] ? in[3] : 1, cs = in[4], steps = 0, stop = 0, t = 0;
    if ((nc > RS_MAX_CROPS) | (cs & 4095) | ((nc == 1) & (cs != 0)) | (cs > (RS_WIN_HI - RS_WIN_LO) / 2))
        return FNPU_BAD_ARGUMENT;
#endif
    rs_op ds[1];
    rs_op *d = ds;
#ifdef RS_STRIDE2
    if (tab < RS_WIN_LO || tab > RS_WIN_HI || n > (RS_WIN_HI - tab) / 128)
        return FNPU_BAD_ARGUMENT;
#endif
#ifdef RS_MULTI
    if (!valid(c) || c->opcode != RS_OPCODE || c->params[0] != 1 || ((c->input_bytes != 128) & (c->input_bytes != 256)) ||
        (seg >= nseg) | (c->output_bytes != RS_META * nseg) || in[0] != RS_MAGIC || (tab & 127) | (n > 160) | (b > n) |
        (tab - RS_WIN_LO + 128 * n > RS_WIN_HI - RS_WIN_LO))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = (volatile fnpu_u32 *)fnpu_output(c) + (RS_META / 4u) * seg;
#else
    if (!valid(c) || c->opcode != RS_OPCODE || c->params[0] != 1 || c->input_bytes != 128 ||
        c->output_bytes != RS_META || in[0] != RS_MAGIC || (tab & 127) | (n > 160) | (b > n) | /* a >= b runs no op */
        (tab - RS_WIN_LO + 128 * n > RS_WIN_HI - RS_WIN_LO))
        return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *meta = fnpu_output(c);
#endif
    rs_zero((volatile HVX_Vector *)meta, RS_META / 128);
#ifdef RS_CLOCK_PROBE
    /* diagnostic: core cycles, packets and 19.2 MHz ticks over the request, meta[3..8] */
    unsigned long long cy0, pk0, tk0;
    __asm__ volatile("%0 = c15:14" : "=r"(cy0)::"memory");
    __asm__ volatile("%0 = c19:18" : "=r"(pk0)::"memory");
    __asm__ volatile("%0 = c31:30" : "=r"(tk0)::"memory");
#endif
    rs_zero(LV(57), 7); /* zero vector, flags */
    RS_SCALE[0] = Q6_V_vsplat_R(0x3c00u);
#ifdef RS_POOL
    rs_pool_open();
#endif
    fault_control(c);
#ifdef RS_RESIDENT
    /* rload: copy the resident images (crop 0) because the region is not ours */
#ifdef RS_RES_SLOTS
    fnpu_u32 rmode = in[9] & 1u, rslot = in[9] >> 1, rload = 0;
    if ((rslot >= RS_NSLOT) | (rslot & !rmode) | (rmode & (in[5] == 0))) return FNPU_BAD_ARGUMENT;
    volatile fnpu_u32 *rtag = RS_TAG + 4u * rslot;
    if (rmode) {
        fnpu_u32 evicted = *RS_BLOB_FLAG != 0;
        rload = evicted | (rtag[0] != in[5]) | (rtag[1] != in[6]) |
                (rtag[2] != in[7]) | (rtag[3] != in[8]);
        if (rload) {
            /* FeatureNN must reload its blob before using it again: clear the
             * flag before the first store into the region, and confirm it */
            *RS_BLOB_FLAG = 0;
            __asm__ volatile("barrier" ::: "memory");
            if (*RS_BLOB_FLAG != 0) return FNPU_BAD_ARGUMENT;
            /* a refilled region invalidates every slot; a mismatch only ours */
            fnpu_u32 w0 = evicted ? 0u : 4u * rslot, w1 = evicted ? 4u * RS_NSLOT : 4u * rslot + 4u;
            for (fnpu_u32 w = w0; w < w1; w++) RS_TAG[w] = 0;
            __asm__ volatile("barrier" ::: "memory");
        }
    }
#else
    fnpu_u32 rmode = in[9], rload = 0;
    if ((rmode > 1) | (rmode & (in[5] == 0))) return FNPU_BAD_ARGUMENT;
    if (rmode) {
        rload = (*RS_BLOB_FLAG != 0) | (RS_TAG[0] != in[5]) | (RS_TAG[1] != in[6]) |
                (RS_TAG[2] != in[7]) | (RS_TAG[3] != in[8]);
        if (rload) {
            /* FeatureNN must reload its blob before using it again: clear the
             * flag before the first store into the region, and confirm it */
            *RS_BLOB_FLAG = 0;
            __asm__ volatile("barrier" ::: "memory");
            if (*RS_BLOB_FLAG != 0) return FNPU_BAD_ARGUMENT;
            for (fnpu_u32 w = 0; w < 4; w++) RS_TAG[w] = 0;
            __asm__ volatile("barrier" ::: "memory");
        }
    }
#endif
#endif
#ifdef RS_CBIAS
    if (!rmode) return FNPU_BAD_ARGUMENT; /* RS_XB lies in the blob region */
#endif
#ifdef RS_STREAM
    /* A streamed request (word 10 = 1, word 11 = its nonce): segment 1 starts once the
     * host has staged its inputs while segment 0 ran; the host writes word 12 = nonce
     * after them (its seq-cst store orders the staging first, as for publish). Bounded
     * by 5 ms and the request's budget. Each segment then publishes meta word 13 =
     * nonce after its metadata, so the host reads its outputs while the next one runs. */
    const fnpu_u32 strm = in[10];
    if ((strm > 1) | (strm & (in[11] == 0))) return FNPU_BAD_ARGUMENT;
    if (strm & (seg != 0)) {
        const fnpu_u32 w0 = rs_ticks();
        while (in[12] != in[11]) {
            const fnpu_u32 now = rs_ticks();
            if ((now - w0 > 96000u) | (now - RS_MB[17] > 384000u)) return RS_STREAM_LATE;
            RS_PAUSE();
        }
        __asm__ volatile("barrier" ::: "memory");
    }
#endif
    fnpu_u32 t0 = rs_ticks();
#ifdef RS_MULTI
    /* one 20 ms budget for the whole request (the gate hold): segment 0's start
     * in mailbox word 17; segment 1 gets what segment 0 left, none -> no op runs */
    if (!seg) RS_MB[17] = t0;
    const fnpu_u32 used = t0 - RS_MB[17], lim = used < 384000u ? 384000u - used : 0u;
    stop = !lim;
#endif
#ifdef RS_REPLAY
#pragma clang loop unroll(disable)
    for (fnpu_u32 k = 0; k < nc && !stop; k++) {
#pragma clang loop unroll(disable)
    for (i = a; i < b; i++) {
        if (rs_get(tab, i, d, k * cs)) return FNPU_BAD_ARGUMENT;
#else
#pragma clang loop unroll(disable)
    for (i = a; i < b; i++) {
        if (rs_get(tab, i, d)) return FNPU_BAD_ARGUMENT;
#endif
#ifdef RS_STEM
        if (d->kind == TS_KIND) {
#ifdef RS_STEM_PROFILE
            if (((const volatile fnpu_u32 *)RS_DESC)[19]) {
                if (!tp_run(c,(const fnpu_u32 *)RS_DESC,t0)) break;
            } else
#endif
            if (!ts_run(c, (const fnpu_u32 *)RS_DESC, t0)) break;
        } else {
#endif
#ifdef RS_MHEADS
        if (d->kind == RG_KIND) {
            rg_run((const volatile fnpu_u32 *)RS_DESC);
        } else {
#endif
        volatile HVX_Vector *v = FR(d->vpar), *m = (volatile HVX_Vector *)(RS_DDR + d->dpar);
        RS_PT(tc);
#ifdef RS_RESIDENT
        if (d->rscr && !rmode) return FNPU_BAD_ARGUMENT;
        if (d->kind == 4) rs_copy(m, v, d->npar);
        else if (!d->rscr || (rload & (k == 0))) rs_copy(v, m, d->npar);
#else
        if (d->kind == 4) rs_copy(m, v, d->npar);
        else rs_copy(v, m, d->npar);
#endif
#ifdef RS_PW_PROFILE
        if (d->kind == 1) RS_PADD(0, tc);
#endif
#ifdef RS_CBIAS
        if ((d->kind == 1) | (d->kind == 2) | (d->kind == 5) | (d->kind == P3_KIND)) rs_cbias(d);
#endif
        RS_PT(tz);
#ifdef RS_MHEADS
#ifdef RS_TILES
        /* a tile output has no halo: the op writes every vector of it */
        if (((d->kind < 3) | (d->kind == P3_KIND)) & !(d->onbin & 2)) rs_zero(P(vout), d->cpo * d->nbin);
#else
        if ((d->kind < 3) | (d->kind == P3_KIND)) rs_zero(P(vout), d->cpo * d->nbin);
#endif
#else
        if (d->kind < 3) rs_zero(P(vout), d->cpo * d->nbin);
#endif
#ifdef RS_PW_PROFILE
        if (d->kind == 1) RS_PADD(1, tz);
#endif
#ifdef RS_MHEADS
        if ((d->kind == 1) | (d->kind == P3_KIND)) rs_pw(d);
#else
        if (d->kind == 1) rs_pw(d);
#endif
        if (d->kind == 2) rs_dw(d);
#ifdef RS_STRIDE2
        if (d->kind == 5) rs_s2(d);
#endif
#ifdef RS_HEADS
        if (d->kind == TH_KIND) th_run((const fnpu_u32 *)RS_DESC, c);
#endif
#ifdef RS_MHEADS
        }
#endif
#ifdef RS_STEM
        }
#endif
#ifdef RS_REPLAY
        t = rs_ticks() - t0;
        meta[96 + i - a] = t; /* the last crop's ticks remain */
        steps++;
#ifdef RS_MULTI
        if (t > lim) { stop = 1; break; }
#else
        if (t > 384000u) { stop = 1; break; }
#endif
    }
    if (nc > 1) meta[48 + k] = t; /* crop k's end */
    }
#else
        fnpu_u32 t = rs_ticks() - t0;
        meta[96 + i - a] = t;
        if (t > 384000u) { i++; break; }
    }
#endif
    fnpu_u32 pc;
    __asm__ volatile("%0 = pc" : "=r"(pc)::"memory");
#ifdef RS_RESIDENT
    fnpu_u32 tagged = rload & !stop & (a == 0) & (b == n); /* crop 0 copied every image */
    if (tagged) {
        __asm__ volatile("barrier" ::: "memory");
#ifdef RS_RES_SLOTS
        for (fnpu_u32 w = 0; w < 4; w++) rtag[w] = in[5 + w];
#else
        for (fnpu_u32 w = 0; w < 4; w++) RS_TAG[w] = in[5 + w];
#endif
        __asm__ volatile("barrier" ::: "memory");
    }
    meta[12] = rmode | rload << 1 | tagged << 2;
#endif
#ifdef RS_REPLAY
    meta[0] = RS_MAGIC; meta[1] = pc; meta[2] = steps; /* descriptors run, all crops */
#else
    meta[0] = RS_MAGIC; meta[1] = pc; meta[2] = i - a;
#endif
#ifdef RS_PW_PROFILE
    for (fnpu_u32 k = 0; k < 9; k++) meta[3 + k] = RS_PROF[k];
#endif
#ifdef RS_POOL_PROF
#if defined(RS_CLOCK_PROBE) || defined(RS_PW_PROFILE)
#error "RS_POOL_PROF uses meta[3..8]"
#endif
    for (fnpu_u32 k = 0; k < 6; k++) meta[3 + k] = ((volatile fnpu_u32 *)LV(52))[k];
#endif
#ifdef RS_CLOCK_PROBE
#ifdef RS_PW_PROFILE
#error "RS_CLOCK_PROBE and RS_PW_PROFILE share meta[3..]"
#endif
    {
        unsigned long long cy1, pk1, tk1;
        __asm__ volatile("barrier" ::: "memory");
        __asm__ volatile("%0 = c15:14" : "=r"(cy1)::"memory");
        __asm__ volatile("%0 = c19:18" : "=r"(pk1)::"memory");
        __asm__ volatile("%0 = c31:30" : "=r"(tk1)::"memory");
        cy1 -= cy0; pk1 -= pk0; tk1 -= tk0;
        meta[3] = (fnpu_u32)cy1; meta[4] = (fnpu_u32)(cy1 >> 32);
        meta[5] = (fnpu_u32)pk1; meta[6] = (fnpu_u32)(pk1 >> 32);
        meta[7] = (fnpu_u32)tk1; meta[8] = (fnpu_u32)cy0 | 1; /* nonzero: probe ran */
    }
#endif
    ((volatile HVX_Vector *)meta)[2] = RS_FLAGS[0];
    __asm__ volatile("barrier" ::: "memory");
#ifdef RS_STREAM
    if (strm) { /* last: every output and metadata store above has completed */
        meta[13] = in[11];
        __asm__ volatile("barrier" ::: "memory");
    }
#endif
    return FNPU_OK;
}

#if defined(RS_DIAG_LOADER) && !defined(RS_POOL)
/* DIAGNOSTIC: the RG_POOL loader's worker entry, returning at once */
fnpu_u32 tl_pool_worker(fnpu_u32 w) {
    (void)w;
    return 0;
}
#endif
