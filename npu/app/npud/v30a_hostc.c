/* v30a_hostc.c: the two loops of ft-npud's v30a host path that numpy does slowly (v30a_c.py loads them).
 *
 *   v30a_stage  v30a_io.Stem's gather as a scatter: only the image pixels are written (FP16 bits of v / 255
 *               through the same table), into a buffer whose other halves the caller keeps at zero
 *   v30a_gap    v30a_io.Final's gather and sums: each channel's sum over the map in FP64, and the number of
 *               nonzero halves in the padding words (any bit set but the sign, as numpy's count_nonzero)
 *
 * Build (no -ffast-math: the sums are plain FP64 adds in a fixed order):
 *   gcc -O2 -shared -fPIC -o v30a_hostc.so v30a_hostc.c
 */
#include <stdint.h>

void v30a_stage(const uint8_t *img, const int32_t *src, const int32_t *dst, int n, const uint16_t *lut,
                uint16_t *buf)
{
    for (int i = 0; i < n; i++)
        buf[dst[i]] = lut[img[src[i]]];
}

/* raw: pairs x words x 2 FP16 halves (channel 2i + j is half j of pair i) */
int v30a_gap(const uint16_t *raw, int pairs, int words, const int32_t *inside, int ni, const int32_t *outside,
             int no, double *sum)
{
    int stray = 0;
    for (int i = 0; i < pairs; i++) {
        const uint16_t *u = raw + (long)i * words * 2;
        const __fp16 *v = (const __fp16 *)u;
        double a = 0.0, b = 0.0;
        for (int k = 0; k < ni; k++) {
            a += v[2 * inside[k]];
            b += v[2 * inside[k] + 1];
        }
        sum[2 * i] = a;
        sum[2 * i + 1] = b;
        for (int k = 0; k < no; k++)
            stray += ((u[2 * outside[k]] & 0x7fff) != 0) + ((u[2 * outside[k] + 1] & 0x7fff) != 0);
    }
    return stray;
}
