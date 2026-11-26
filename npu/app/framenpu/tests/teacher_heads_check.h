/* Fixed-shape GAP + FP32 heads descriptor. Pure integer bounds checker,
 * also compiled on the host by the offline tests. Vector offsets are relative
 * to the existing borrowed span; parameters are read only from the window. */
#ifndef TEACHER_HEADS_CHECK_H
#define TEACHER_HEADS_CHECK_H
#define TH_KIND 6u
#define TH_PARAM_VECTORS 2692u /* bias[128], weights[672][128], all FP32 */
static __attribute__((noinline)) fnpu_u32 th_bad(const fnpu_u32 *r) {
    fnpu_u32 bad = r[0] != 6 || r[1] != 336 || r[2] != 4 || r[3] != 672 ||
                   r[4] != 49 || r[5] != 7 || r[6] != 1 || r[7] != 7 || r[8] != 9;
    bad |= r[10] != TH_PARAM_VECTORS;
    bad |= r[9] < 0x1a40000u || r[9] > 0x1f00000u || (r[9] & 127) ||
           TH_PARAM_VECTORS > (0x1f00000u - r[9]) / 128;
    bad |= r[15] > 2;
    for (fnpu_u32 i = 16; i < 32; i++) bad |= r[i] != 0;
    for (fnpu_u32 i = 11; i < 15; i++) bad |= r[i] == 0 || r[i] > 15296u;
    if (bad) return bad;
    fnpu_u32 a = r[11], b = r[12], c = r[13], d = r[14];
    bad |= a + 2352 > 15296u || b + 4 > 15296u || c + 21 > 15296u || d + TH_PARAM_VECTORS > 15296u;
#define TH_OVERLAP(a,n,b,m) ((a) < (b) + (m) && (b) < (a) + (n))
    bad |= TH_OVERLAP(a,2352,b,4) || TH_OVERLAP(a,2352,c,21) || TH_OVERLAP(a,2352,d,TH_PARAM_VECTORS) ||
           TH_OVERLAP(b,4,c,21) || TH_OVERLAP(b,4,d,TH_PARAM_VECTORS) || TH_OVERLAP(c,21,d,TH_PARAM_VECTORS);
#undef TH_OVERLAP
    return bad;
}
#endif
