/* The flat H-net's host side in C: what probe/host_fast.py does in numpy, for the Python-or-C question.
 * Built by probe/host_c.py (gcc -O3, aarch64). No device access here: the caller moves the bytes.
 *
 *   hc_stage  crop pixels (u8) -> the window's FP16 halves through the scatter tables of host_fast.Fast
 *   hc_heads  one crop's stored head tensor (FP16 halves) -> 63 screen, presence, right, 63 world floats
 *   hc_gap    v30a's final map (npud/v30a_io.py Final): the per-channel sum over the map, and the stray count
 */
#include <math.h>
#include <stdint.h>

void hc_stage(const uint8_t *img, const int32_t *src, const int32_t *dst, int n, const uint16_t *lut, uint16_t *buf)
{
    for (int i = 0; i < n; i++)
        buf[dst[i]] = lut[img[src[i]]];
}

/* e^x for x <= 0, relative error about 1e-7: 2^k * 2^f with f in [-0.5, 0.5), Taylor to degree 6 */
static inline float fexp(float x)
{
    x = x < -80.0f ? -80.0f : x;
    float t = x * 1.44269504f;
    float k = __builtin_floorf(t + 0.5f);
    float f = t - k;
    float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f
              + f * (0.00133336f + f * 0.00015404f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)((int32_t)k + 127) << 23;
    return p * s.f;
}

/* raw: the compact store as FP16 halves; ridx [c * nn]: where each (channel, cell) sits in it.
 * Channels: 84 heatmap logits (keypoint j, plane a * 2 + b of cell (yc, xc) is heatmap pixel (2 yc + a, 2 xc + b)),
 * kpd keypoint features, gd global features. grid [2 n]: the crop coordinate of each heatmap pixel.
 * w0 [nh, gd + 21 kpd], w2 [63, nh], row-major. o [c * nn] and feat [gd + 21 kpd + nh + nn] are scratch.
 * out [128]: x, y, 0 of 21 keypoints; presence; right; world [63]. */
void hc_heads(const __fp16 *raw, const int32_t *ridx, int c, int n, int kpd, int gd, int nh, const float *grid,
              const float *w0, const float *b0, const float *w2, const float *b2, const float *wp, float bp,
              const float *wr, float br, float *o, float *feat, float *out)
{
    int nn = n * n, nin = gd + 21 * kpd;
    float *hid = feat + nin, *h2 = hid + nh;
    for (int i = 0; i < c * nn; i++)
        o[i] = raw[ridx[i]];
    for (int j = 0; j < 21; j++) {
        const float *hm = o + 4 * j * nn;
        float mx = hm[0];
        for (int i = 1; i < 4 * nn; i++)
            mx = hm[i] > mx ? hm[i] : mx;
        float sum = 0.0f, sx = 0.0f, sy = 0.0f;
        for (int i = 0; i < nn; i++)
            h2[i] = 0.0f;
        for (int p = 0; p < 4; p++) {
            int a = p >> 1, b = p & 1;
            const float *pl = hm + p * nn;
            for (int yc = 0; yc < n; yc++) {
                float row = 0.0f, gy = grid[2 * yc + a];
                for (int xc = 0; xc < n; xc++) {
                    float e = fexp(pl[yc * n + xc] - mx);
                    h2[yc * n + xc] += e;
                    row += e;
                    sx += e * grid[2 * xc + b];
                }
                sum += row;
                sy += row * gy;
            }
        }
        float inv = 1.0f / sum;
        out[3 * j] = sx * inv;
        out[3 * j + 1] = sy * inv;
        out[3 * j + 2] = 0.0f;
        for (int d = 0; d < kpd; d++) {
            const float *f = o + (84 + d) * nn;
            float acc = 0.0f;
            for (int i = 0; i < nn; i++)
                acc += h2[i] * f[i];
            feat[gd + j * kpd + d] = acc * inv;
        }
    }
    float zp = bp, zr = br;
    for (int d = 0; d < gd; d++) {
        const float *f = o + (84 + kpd + d) * nn;
        float acc = 0.0f;
        for (int i = 0; i < nn; i++)
            acc += f[i];
        feat[d] = acc / nn;
        zp += feat[d] * wp[d];
        zr += feat[d] * wr[d];
    }
    out[63] = 1.0f / (1.0f + expf(-zp));
    out[64] = 1.0f / (1.0f + expf(-zr));
    for (int h = 0; h < nh; h++) {
        const float *w = w0 + h * nin;
        float acc = b0[h];
        for (int i = 0; i < nin; i++)
            acc += w[i] * feat[i];
        hid[h] = acc > 0.0f ? acc : 0.0f;
    }
    for (int k = 0; k < 63; k++) {
        const float *w = w2 + k * nh;
        float acc = b2[k];
        for (int h = 0; h < nh; h++)
            acc += w[h] * hid[h];
        out[65 + k] = acc;
    }
}

/* raw [pairs][words][2] FP16 halves; inside [ni] and outside [no]: word indices in and out of the map.
 * sum [2 * pairs] (channel 2i + j is half j of pair i) <- the sum over the inside words. Returns the number of
 * nonzero halves in the outside words (0 on a sound run). */
int hc_gap(const __fp16 *raw, int pairs, int words, const int32_t *inside, int ni, const int32_t *outside, int no,
           double *sum)
{
    int stray = 0;
    for (int i = 0; i < pairs; i++) {
        const __fp16 *v = raw + (long)i * words * 2;
        double a = 0.0, b = 0.0;
        for (int k = 0; k < ni; k++) {
            a += v[2 * inside[k]];
            b += v[2 * inside[k] + 1];
        }
        sum[2 * i] = a;
        sum[2 * i + 1] = b;
        for (int k = 0; k < no; k++)
            stray += (v[2 * outside[k]] != 0) + (v[2 * outside[k] + 1] != 0);
    }
    return stray;
}
