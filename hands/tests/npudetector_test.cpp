// track/npudetector.cpp: one peak decodes to its two points and box, ties keep one palm, and with a fixture
// (npu-hands: det_fixture.py OUT.bin on det.py) every case's palms and tiles match det.py's decode, merge and
// tile_rois.
//   build/npudetector-test [FIXTURE]
#include "../track/npudetector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

constexpr int G = NpuClient::kDetGrid;

static std::vector<Palm> palms_of(const int8_t *obj, const int8_t *reg, float so, float sr, float thr) {
    std::vector<Palm> p;
    for (const DetPoint &d : decode_det(obj, reg, so, sr, thr)) p.push_back(palm_from(d));
    return merge_palms(p);
}

static void one_peak() {
    std::vector<int8_t> obj(G * G, -100), reg(4 * G * G, 0);
    obj[5 * G + 9] = 100;                    // cell (x 9, y 5): centre (75.5, 43.5)
    reg[0 * G * G + 5 * G + 9] = 0;          // wrist at the cell centre
    reg[1 * G * G + 5 * G + 9] = 0;
    reg[2 * G * G + 5 * G + 9] = 0;          // middle base 2 cells (16 px) straight up
    reg[3 * G * G + 5 * G + 9] = -100;       // -100 * 0.02 = -2 cells = -16 px
    auto p = palms_of(obj.data(), reg.data(), 0.05f, 0.02f, 0.3f);
    CHECK(p.size() == 1);
    if (p.size() == 1) {
        CHECK(std::fabs(p[0].kp[0][0] - 75.5) < 1e-6 && std::fabs(p[0].kp[0][1] - 43.5) < 1e-6);
        CHECK(std::fabs(p[0].kp[2][1] - 27.5) < 1e-6);
        CHECK(std::fabs(p[0].size[0] - 1.02 * 16) < 1e-6);
        CHECK(std::fabs(p[0].center[1] - (43.5 - 0.374 * 16)) < 1e-6);
        CHECK(std::fabs(p[0].score - 1 / (1 + std::exp(-5.0))) < 1e-9);
    }
    obj[5 * G + 10] = 100;                   // a tie beside it, on the same hand: both are local maxima, merge
    reg[0 * G * G + 5 * G + 10] = -50;       // keeps one (its points: one cell left, the same as the first's)
    reg[3 * G * G + 5 * G + 10] = -100;
    CHECK(palms_of(obj.data(), reg.data(), 0.05f, 0.02f, 0.3f).size() == 1);
    CHECK(palms_of(obj.data(), reg.data(), 0.05f, 0.02f, 0.995f).empty());   // sigmoid(5) = 0.9933
}

static void fixture(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    size_t at = 0;
    auto take = [&](void *dst, size_t n) {
        if (at + n > b.size()) throw 0;
        std::memcpy(dst, b.data() + at, n);
        at += n;
    };
    auto u32 = [&] { uint32_t v; take(&v, 4); return v; };
    auto f32 = [&] { float v; take(&v, 4); return v; };
    char magic[4];
    take(magic, 4);
    CHECK(std::memcmp(magic, "DETF", 4) == 0 && u32() == 1);
    const uint32_t cases = u32();
    int palms = 0, worst_case = -1;
    double worst = 0;
    for (uint32_t c = 0; c < cases; ++c) {
        const float so = f32(), sr = f32(), thr = f32();
        std::vector<int8_t> obj(G * G), reg(4 * G * G);
        take(obj.data(), obj.size());
        take(reg.data(), reg.size());
        const uint32_t n = u32();
        const auto got = palms_of(obj.data(), reg.data(), so, sr, thr);
        CHECK(got.size() == n);
        for (uint32_t i = 0; i < n; ++i) {
            const float sc = f32(), wx = f32(), wy = f32(), mx = f32(), my = f32();
            if (i >= got.size()) continue;
            const double e = std::max({std::fabs(got[i].kp[0][0] - wx), std::fabs(got[i].kp[0][1] - wy),
                                       std::fabs(got[i].kp[2][0] - mx), std::fabs(got[i].kp[2][1] - my),
                                       1000 * std::fabs(got[i].score - sc)});
            if (e > worst) worst = e, worst_case = int(c);
            ++palms;
        }
    }
    CHECK(worst < 1e-3);
    const uint32_t tiles = u32();
    for (uint32_t t = 0; t < tiles; ++t) {
        const int w = int(u32()), h = int(u32()), grid = int(u32());
        const double frac = f32();
        const uint32_t n = u32();
        const auto r = det_tiles(w, h, grid, frac);
        CHECK(r.size() == n);
        for (uint32_t i = 0; i < n; ++i) {
            const float cx = f32(), cy = f32(), size = f32();
            if (i < r.size())
                CHECK(std::fabs(r[i].center[0] - cx) < 1e-3 && std::fabs(r[i].center[1] - cy) < 1e-3 &&
                      std::fabs(r[i].size - size) < 1e-3 && r[i].rotation == 0);
        }
    }
    CHECK(at == b.size());
    std::printf("fixture: %u cases, %d palms, worst %.2g (case %d), %u tile layouts\n", cases, palms, worst,
                worst_case, tiles);
}

int main(int argc, char **argv) {
    one_peak();
    if (argc > 1) fixture(argv[1]);
    std::printf(failures ? "npudetector-test: %d FAILED\n" : "npudetector-test: ok\n", failures);
    return failures ? 1 : 0;
}
