#include "npuheads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

constexpr uint32_t kMagic = 0x484E5446, kVersion = 1;   // 'FTNH'

float sigmoid(float v) { return 1.f / (1.f + std::exp(-v)); }

// y = W x + b, W row-major [rows][cols]
void linear(const float *W, const float *b, const float *x, int rows, int cols, float *y) {
    for (int r = 0; r < rows; ++r) {
        const float *w = W + size_t(r) * cols;
        float acc = 0;
        for (int c = 0; c < cols; ++c) acc += w[c] * x[c];
        y[r] = acc + b[r];
    }
}

}  // namespace

bool NpuHeads::load(const std::string &path, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return err = "cannot open " + path, false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(data.data(), data.size(), err);
}

bool NpuHeads::parse(const uint8_t *data, size_t size, std::string &err) {
    uint32_t h[12];
    if (size < sizeof h + 12) return err = "heads file too short", false;
    std::memcpy(h, data, sizeof h);
    if (h[0] != kMagic || h[1] != kVersion) return err = "not a version 1 heads file", false;
    graph_id = h[2], K = int(h[3]), H = int(h[4]), W = int(h[5]), kpd = int(h[6]), g = int(h[7]);
    hidden = int(h[8]), pool_h = int(h[9]), pool_w = int(h[10]);
    // the tracker's Landmarks hold 21 points; the rest bounds the sizes so a bad file cannot overflow them
    if (K != 21 || H < 1 || W < 1 || H > 64 || W > 64 || kpd < 1 || kpd > 256 || g < 1 || g > 1024 ||
        hidden < 1 || hidden > 4096 || pool_h < 1 || pool_w < 1 || pool_h > 64 || pool_w > 64)
        return err = "heads file shapes out of range", false;
    float s[3];
    std::memcpy(s, data + sizeof h, sizeof s);
    s_heat = s[0], s_kp = s[1], s_pool = s[2];
    const size_t in0 = size_t(g) + size_t(K) * kpd;
    const size_t want = sizeof h + sizeof s +
                        4 * (size_t(W) + 2 * (size_t(g) + 1) + hidden * in0 + hidden + 3 * K * size_t(hidden) + 3 * K);
    if (size != want) return err = "heads file size differs from its shapes", false;
    const uint8_t *p = data + sizeof h + sizeof s;
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
    return true;
}

void NpuHeads::decode(const uint8_t *out, float *screen, float *world, float &presence, float &right) const {
    const int N = H * W;
    const int8_t *heat = reinterpret_cast<const int8_t *>(out);
    const int8_t *kp = heat + size_t(K) * N;
    const uint8_t *pool = reinterpret_cast<const uint8_t *>(kp + size_t(kpd) * N);

    std::vector<float> x(size_t(g) + size_t(K) * kpd), h(N), hid(hidden);
    const int P = pool_h * pool_w;
    for (int c = 0; c < g; ++c) {
        int acc = 0;
        for (int i = 0; i < P; ++i) acc += pool[size_t(c) * P + i];
        x[c] = float(acc) * s_pool / float(P);
    }
    for (int k = 0; k < K; ++k) {
        const int8_t *hk = heat + size_t(k) * N;
        int mx = hk[0];
        for (int i = 1; i < N; ++i) mx = std::max(mx, int(hk[i]));
        float sum = 0;
        for (int i = 0; i < N; ++i) sum += h[i] = std::exp(float(hk[i] - mx) * s_heat);
        const float inv = 1.f / sum;
        float sx = 0, sy = 0;
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx) {
                const float v = h[yy * W + xx] *= inv;
                sx += v * grid[xx], sy += v * grid[yy];
            }
        screen[2 * k] = sx, screen[2 * k + 1] = sy;
        float *f = x.data() + g + size_t(k) * kpd;   // keypoint-major, as the training einsum lays it out
        for (int c = 0; c < kpd; ++c) {
            const int8_t *kc = kp + size_t(c) * N;
            float acc = 0;
            for (int i = 0; i < N; ++i) acc += h[i] * float(kc[i]);
            f[c] = acc * s_kp;
        }
    }
    linear(w0.data(), b0.data(), x.data(), hidden, int(x.size()), hid.data());
    for (float &v : hid) v = std::max(v, 0.f);
    linear(w1.data(), b1.data(), hid.data(), 3 * K, hidden, world);
    float pv, rv;
    linear(pres_w.data(), &pres_b, x.data(), 1, g, &pv);
    linear(right_w.data(), &right_b, x.data(), 1, g, &rv);
    presence = sigmoid(pv), right = sigmoid(rv);
}
