// ft-npud's output gather: n crops of one output, from the graph's guard-column planes ([C4][PIN][4] bytes, a crop
// every `stride` bytes, row y of group g at (g*PIN + y*(w+1))*4 from the crop's first row) into dense [ch][h][w]
// slots `slot` bytes apart. vld4 splits 16 (or 8) pixels' 4 interleaved channels at once. src is the daemon's
// cached copy of the planes, never the DSP's memory (reading that in small pieces, or on several threads, was no
// faster than one copy). ft_npud.py's check_gathers holds this to deguard_index before the DSP is touched.
#include <arm_neon.h>
#include <stdint.h>
#include <string.h>

// one group: rows (w+1)*4 bytes apart from s, channels k < k4 into planes h*w bytes apart from d
static void group(const uint8_t *s, uint8_t *d, int h, int w, int k4) {
    const long plane = (long)h * w;
    for (int y = 0; y < h; ++y, s += (w + 1) * 4, d += w) {
        int x = 0;
        if (k4 == 4) {
            for (; x + 16 <= w; x += 16) {
                const uint8x16x4_t v = vld4q_u8(s + 4 * x);
                vst1q_u8(d + x, v.val[0]);
                vst1q_u8(d + plane + x, v.val[1]);
                vst1q_u8(d + 2 * plane + x, v.val[2]);
                vst1q_u8(d + 3 * plane + x, v.val[3]);
            }
            for (; x + 8 <= w; x += 8) {
                const uint8x8x4_t v = vld4_u8(s + 4 * x);
                vst1_u8(d + x, v.val[0]);
                vst1_u8(d + plane + x, v.val[1]);
                vst1_u8(d + 2 * plane + x, v.val[2]);
                vst1_u8(d + 3 * plane + x, v.val[3]);
            }
        }
        for (; x < w; ++x)
            for (int k = 0; k < k4; ++k) d[k * plane + x] = s[4 * x + k];
    }
}

static int k4_of(int ch, int g) { return ch - 4 * g < 4 ? ch - 4 * g : 4; }

void deguard(const uint8_t *src, uint8_t *dst, int n, long stride, int C4, long pin, int h, int w, int ch, long slot) {
    for (int c = 0; c < n; ++c)
        for (int g = 0; g < C4; ++g)
            group(src + c * stride + g * pin * 4, dst + c * slot + (long)4 * g * h * w, h, w, k4_of(ch, g));
}
