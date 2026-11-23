#include "npuheads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

constexpr uint32_t kMagic = 0x484E5446, kVersion = 1, kVersion2V = 2;   // 'FTNH'; 2: a two-view student

float sigmoid(float v) { return 1.f / (1.f + std::exp(-v)); }

// y = W x + b, W row-major [rows][cols] (one output: a dot product)
float dot(const float *w, const float *x, int n) {
    float acc = 0;
    for (int c = 0; c < n; ++c) acc += w[c] * x[c];
    return acc;
}

// y += a * x, four lanes at a time (GCC/Clang vector extensions: -O2's cost model leaves the plain loop scalar);
// n is a multiple of 4 (the strides below are padded)
typedef float v4 __attribute__((vector_size(16)));
void axpy(const float *x, float a, float *y, int n) {
    for (int i = 0; i < n; i += 4) {
        v4 xv, yv;
        std::memcpy(&xv, x + i, 16), std::memcpy(&yv, y + i, 16);
        yv += a * xv;
        std::memcpy(y + i, &yv, 16);
    }
}

int pad4(int n) { return (n + 3) & ~3; }

// y = W x + b with Wt = W transposed, [cols][pad4(rows)]: the inner loop runs over independent outputs.
// y has room for pad4(rows).
void linear_t(const float *Wt, const float *b, const float *x, int rows, int cols, float *y) {
    const int rp = pad4(rows);
    std::fill(y, y + rp, 0.f);
    std::copy(b, b + rows, y);
    for (int c = 0; c < cols; ++c) {
        const float v = x[c];
        if (v == 0.f) continue;   // after a ReLU about half the inputs are 0
        axpy(Wt + size_t(c) * rp, v, y, rp);
    }
}

std::vector<float> transpose(const std::vector<float> &m, int rows, int cols) {
    const int rp = pad4(rows);
    std::vector<float> t(size_t(cols) * rp, 0.f);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) t[size_t(c) * rp + r] = m[size_t(r) * cols + c];
    return t;
}

}  // namespace

bool NpuHeads::load(const std::string &path, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return err = "cannot open " + path, false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(data.data(), data.size(), err);
}

size_t NpuHeads::file_bytes(const uint32_t *h) {
    if (h[0] != kMagic || (h[1] != kVersion && h[1] != kVersion2V)) return 0;
    const bool two = h[1] == kVersion2V;
    const size_t K = h[3], W = h[5], kpd = h[6], g = h[7], hidden = h[8], in0 = g + K * kpd;
    if (K > 64 || W > 64 || kpd > 256 || g > 1024 || hidden > 4096) return 0;   // no size_t wrap on a bad header
    return 48 + 4 * (3 + two) + 4 * (W + 2 * (g + 1) + hidden * in0 + hidden + 3 * K * hidden + 3 * K) +
           (two ? 4 * (hidden * in0 + 2 * g) : 0);
}

bool NpuHeads::parse(const uint8_t *data, size_t size, std::string &err) {
    uint32_t h[12];
    if (size < sizeof h + 12) return err = "heads file too short", false;
    std::memcpy(h, data, sizeof h);
    if (h[0] != kMagic || (h[1] != kVersion && h[1] != kVersion2V))
        return err = "not a version 1 or 2 heads file", false;
    two_view = h[1] == kVersion2V;
    graph_id = h[2], K = int(h[3]), H = int(h[4]), W = int(h[5]), kpd = int(h[6]), g = int(h[7]);
    hidden = int(h[8]), pool_h = int(h[9]), pool_w = int(h[10]);
    crop = h[11] == 0 ? 224 : int(h[11]);
    // the tracker's Landmarks hold 21 points; one grid (W values) serves both heatmap axes, so H == W; the rest
    // bounds the sizes so a bad file cannot overflow them
    if (K != 21 || H != W || H < 1 || H > 64 || kpd < 1 || kpd > 256 || g < 1 || g > 1024 ||
        hidden < 1 || hidden > 4096 || pool_h < 1 || pool_w < 1 || pool_h > 64 || pool_w > 64 ||
        crop < 64 || crop > 224 || crop % 32 != 0)   // ft-npud's crop slots are 224 x 224
        return err = "heads file shapes out of range", false;
    if (size != file_bytes(h)) return err = "heads file size differs from its shapes", false;
    float s[4] = {};
    std::memcpy(s, data + sizeof h, 4 * (3 + two_view));
    s_heat = s[0], s_kp = s[1], s_pool = s[2], s_pool_pre = s[3];
    const size_t in0 = size_t(g) + size_t(K) * kpd;
    const uint8_t *p = data + sizeof h + 4 * (3 + two_view);
    auto take = [&p](std::vector<float> &v, size_t n) {
        v.resize(n);
        std::memcpy(v.data(), p, 4 * n);
        p += 4 * n;
    };
    auto take1 = [&p](float &v) {
        std::memcpy(&v, p, 4);
        p += 4;
    };
    take(grid, W);
    take(pres_w, g), take1(pres_b);
    take(right_w, g), take1(right_b);
    take(w0, hidden * in0), take(b0, hidden);
    take(w1, 3 * K * size_t(hidden)), take(b1, 3 * K);
    wp.clear(), pres_pw.clear(), right_pw.clear(), wpt.clear();
    if (two_view) take(wp, hidden * in0), take(pres_pw, g), take(right_pw, g);
    for (int d = 0; d < 256; ++d) exp_lut[d] = std::exp(float(-d) * s_heat);
    w0t = transpose(w0, hidden, int(in0));
    w1t = transpose(w1, 3 * K, hidden);
    if (two_view) wpt = transpose(wp, hidden, int(in0));
    return true;
}

void NpuHeads::features(const uint8_t *out, bool pre, float *x, float *screen) const {
    const int N = H * W;
    const int8_t *heat = reinterpret_cast<const int8_t *>(out);
    const int8_t *kp = heat + size_t(K) * N;
    const int P = pool_h * pool_w;
    const uint8_t *pool = reinterpret_cast<const uint8_t *>(kp + size_t(kpd) * N) + (pre ? size_t(g) * P : 0);
    const float sp = pre ? s_pool_pre : s_pool;

    const int kp4 = pad4(kpd);
    std::vector<float> h(N), kpt(size_t(N) * kp4, 0.f), f(kp4);
    for (int c = 0; c < kpd; ++c)   // [kpd][N] -> [N][kp4]: the pooling's inner loop runs over channels
        for (int i = 0; i < N; ++i) kpt[size_t(i) * kp4 + c] = float(kp[size_t(c) * N + i]);
    for (int c = 0; c < g; ++c) {
        int acc = 0;
        for (int i = 0; i < P; ++i) acc += pool[size_t(c) * P + i];
        x[c] = float(acc) * sp / float(P);
    }
    for (int k = 0; k < K; ++k) {
        const int8_t *hk = heat + size_t(k) * N;
        int mx = hk[0];
        for (int i = 1; i < N; ++i) mx = std::max(mx, int(hk[i]));
        float sum = 0;
        for (int i = 0; i < N; ++i) sum += h[i] = exp_lut[mx - hk[i]];
        const float inv = 1.f / sum;
        float sx = 0, sy = 0;
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx) {
                const float v = h[yy * W + xx] *= inv;
                sx += v * grid[xx], sy += v * grid[yy];
            }
        if (screen) screen[2 * k] = sx, screen[2 * k + 1] = sy;
        std::fill(f.begin(), f.end(), 0.f);
        for (int i = 0; i < N; ++i)
            if (h[i] != 0.f) axpy(kpt.data() + size_t(i) * kp4, h[i], f.data(), kp4);
        float *xk = x + g + size_t(k) * kpd;   // keypoint-major, as the training einsum lays it out
        for (int c = 0; c < kpd; ++c) xk[c] = f[c] * s_kp;
    }
}

void NpuHeads::decode(const uint8_t *out, float *screen, float *world, float &presence, float &right,
                      const uint8_t *partner) const {
    const size_t in0 = size_t(g) + size_t(K) * kpd;
    std::vector<float> x(in0), hid(pad4(hidden)), wo(pad4(3 * K));
    features(out, false, x.data(), screen);
    linear_t(w0t.data(), b0.data(), x.data(), hidden, int(in0), hid.data());
    float pres = dot(pres_w.data(), x.data(), g) + pres_b, rt = dot(right_w.data(), x.data(), g) + right_b;
    if (two_view && partner) {   // the partner's pre-fusion pool and keypoint features, int8_ref.heads' tp terms
        std::vector<float> xp(in0), hp(pad4(hidden));
        const std::vector<float> zero(hidden, 0.f);
        features(partner, true, xp.data(), nullptr);
        linear_t(wpt.data(), zero.data(), xp.data(), hidden, int(in0), hp.data());
        for (int i = 0; i < hidden; ++i) hid[i] += hp[i];
        pres += dot(pres_pw.data(), xp.data(), g), rt += dot(right_pw.data(), xp.data(), g);
    }
    for (float &v : hid) v = std::max(v, 0.f);
    linear_t(w1t.data(), b1.data(), hid.data(), 3 * K, hidden, wo.data());
    std::copy(wo.begin(), wo.begin() + 3 * K, world);
    presence = sigmoid(pres);
    right = sigmoid(rt);
}
