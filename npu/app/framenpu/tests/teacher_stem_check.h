/* Fixed geometry for seven 8-row output bands. All writes stay in the
 * previously owned VTCM span and DDR window. Pure C for host bounds tests. */
#ifndef TEACHER_STEM_CHECK_H
#define TEACHER_STEM_CHECK_H
#ifdef RS_STEM_GRAY
#define TS_KIND 8u
#define TS_INPUT_VECTORS 784u
#else
#define TS_KIND 7u
#define TS_INPUT_VECTORS 2352u
#endif
#define TS_RGB 4u
#define TS_OUT8 2356u
#define TS_OUT10 5972u
#define TS_A 6880u
#define TS_B 9728u
#define TS_PAR 12576u
#define TS_BLOCK 672u
#define TS_PARAMS 4704u
static inline fnpu_u32 ts_needed_block(fnpu_u32 j) {
    fnpu_u32 a = (32*j)/114, b = (32*j+31)/114;
    return (a >= 2 && a <= 16 && !(a & 1)) || (b >= 2 && b <= 16 && !(b & 1));
}
static inline fnpu_u32 ts_window(fnpu_u32 at, fnpu_u32 n) {
    return at >= 0x1a40000u && at <= 0x1f00000u && !(at & 127) && n <= (0x1f00000u - at) / 128;
}
#define TS_OVER(a,n,b,m) ((a) < (b) + (m)*128u && (b) < (a) + (n)*128u)
static __attribute__((noinline)) fnpu_u32 ts_bad(const fnpu_u32 *r) {
    fnpu_u32 bad = r[0] != TS_KIND || r[1] >= r[2] || r[2] > 7 || r[3] != 8 || r[4] != 20 ||
                   r[7] != TS_PARAMS || r[8] != TS_BLOCK || r[11] != TS_RGB ||
                   r[12] != TS_OUT8 || r[13] != TS_OUT10 || r[14] != TS_A ||
                   r[15] > 6 || r[16] > 5;
#ifdef RS_STEM_PROFILE
    for (fnpu_u32 i = 21; i < 32; i++) bad |= r[i] != 0;
    if (!r[19]) bad |= r[18] != 0 || r[20] != 0;
    else bad |= r[19] > 3 || r[20] > 7 || (r[19] == 3 && r[20]) || r[16] || r[15] || !ts_window(r[18],128) ||
                (r[19] >= 2 && r[2] != r[1]+1);
#else
    for (fnpu_u32 i = 18; i < 32; i++) bad |= r[i] != 0;
#endif
    bad |= !ts_window(r[5],TS_INPUT_VECTORS) || !ts_window(r[6],TS_PARAMS) ||
           !ts_window(r[9],3616) || !ts_window(r[10],904);
    if (bad) return bad;
    bad |= TS_OVER(r[5],TS_INPUT_VECTORS,r[6],TS_PARAMS) || TS_OVER(r[5],TS_INPUT_VECTORS,r[9],3616) ||
           TS_OVER(r[5],TS_INPUT_VECTORS,r[10],904) || TS_OVER(r[6],TS_PARAMS,r[9],3616) ||
           TS_OVER(r[6],TS_PARAMS,r[10],904) || TS_OVER(r[9],3616,r[10],904);
#ifdef RS_STEM_PROFILE
    if (r[19]) bad |= TS_OVER(r[18],128,r[5],TS_INPUT_VECTORS) || TS_OVER(r[18],128,r[6],TS_PARAMS) ||
                     TS_OVER(r[18],128,r[9],3616) || TS_OVER(r[18],128,r[10],904);
#endif
    if (!r[16]) return bad | (r[17] != 0);
    fnpu_u32 debug_n = r[16] < 3 ? 1068u : r[16] == 3 ? 712u : 2848u;
    if (!ts_window(r[17],debug_n)) return 1;
    return bad | TS_OVER(r[17],debug_n,r[5],TS_INPUT_VECTORS) | TS_OVER(r[17],debug_n,r[6],TS_PARAMS) |
                 TS_OVER(r[17],debug_n,r[9],3616) | TS_OVER(r[17],debug_n,r[10],904);
}
#undef TS_OVER
#endif
