// track/npuheads.cpp: a peaked heatmap lands on its cell's centre, a bad heads file is refused, and with a fixture
// (npu-hands npud/heads_file.py fixture G.npz OUT.bin) every case matches int8_ref.heads: a two-view graph's cases
// (heads file version 2) read with their partner's outputs, or alone when the case's has2 is 0.
//   build/npuheads-test [FIXTURE]
#include "../track/npuheads.h"

#include <algorithm>
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

static std::vector<uint8_t> tiny_file(int H, int W, int kpd, int g, int hidden) {
    std::vector<uint8_t> b;
    auto u = [&b](uint32_t v) { b.insert(b.end(), (uint8_t *)&v, (uint8_t *)&v + 4); };
    auto f = [&b](float v) { b.insert(b.end(), (uint8_t *)&v, (uint8_t *)&v + 4); };
    for (uint32_t v : {0x484E5446u, 1u, 0xabcdu, 21u, uint32_t(H), uint32_t(W), uint32_t(kpd), uint32_t(g),
                       uint32_t(hidden), 2u, 2u, 0u})
        u(v);
    f(0.5f), f(0.1f), f(0.01f);
    for (int i = 0; i < W; ++i) f(8.f * i + 3.5f);
    for (int i = 0; i < 2 * (g + 1) + hidden * (g + 21 * kpd) + hidden + 63 * hidden + 63; ++i) f(0.f);
    return b;
}

static void peaked() {
    NpuHeads h;
    std::string err;
    auto file = tiny_file(4, 4, 1, 1, 1);
    CHECK(h.parse(file.data(), file.size(), err));
    CHECK(h.graph_id == 0xabcd);
    std::vector<uint8_t> out(h.out_bytes(), 0);
    int8_t *heat = reinterpret_cast<int8_t *>(out.data());
    for (int k = 0; k < 21; ++k) {
        std::memset(heat + k * 16, int8_t(-128), 16);
        heat[k * 16 + (k % 4) * 4 + (k / 4) % 4] = 127;   // cell (y = k % 4, x = (k / 4) % 4)
    }
    float screen[42], world[63], p = 0, r = 0;
    h.decode(out.data(), screen, world, p, r);
    for (int k = 0; k < 21; ++k) {
        CHECK(std::fabs(screen[2 * k] - h.grid[(k / 4) % 4]) < 1e-3);
        CHECK(std::fabs(screen[2 * k + 1] - h.grid[k % 4]) < 1e-3);
    }
    CHECK(std::fabs(p - 0.5f) < 1e-6 && std::fabs(r - 0.5f) < 1e-6);   // zero weights: sigmoid(0)
    file.pop_back();
    CHECK(!h.parse(file.data(), file.size(), err));
    auto bad = tiny_file(4, 4, 1, 1, 1);
    bad[12] = 20;   // K = 20
    CHECK(!h.parse(bad.data(), bad.size(), err));
    CHECK(h.crop == 224);   // word 11 = 0: a file from before the crop word
    auto c192 = tiny_file(4, 4, 1, 1, 1);
    c192[44] = 192;
    CHECK(h.parse(c192.data(), c192.size(), err) && h.crop == 192);
    c192[44] = 200;   // not a multiple of 32
    CHECK(!h.parse(c192.data(), c192.size(), err));
    c192[44] = 0, c192[45] = 1;   // 256: bigger than ft-npud's slots
    CHECK(!h.parse(c192.data(), c192.size(), err));
    auto sq5 = tiny_file(5, 5, 1, 1, 1), rect = tiny_file(4, 5, 1, 1, 1);
    CHECK(h.parse(sq5.data(), sq5.size(), err));
    CHECK(!h.parse(rect.data(), rect.size(), err));   // one grid for both axes: H must equal W
    auto v2 = tiny_file(4, 4, 1, 1, 1);
    v2[4] = 2;   // version 2 without its pool_pre scale and partner weights
    CHECK(!h.parse(v2.data(), v2.size(), err));
    v2[4] = 3;
    CHECK(!h.parse(v2.data(), v2.size(), err) && err.find("version") != std::string::npos);
}

static void fixture(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint32_t hdr[12];
    if (d.size() < sizeof hdr) return std::printf("FAIL fixture: too short\n"), void(++failures);
    std::memcpy(hdr, d.data(), sizeof hdr);
    const size_t head = NpuHeads::file_bytes(hdr);
    NpuHeads h;
    std::string err;
    if (!head || head > d.size() || !h.parse(d.data(), head, err))
        return std::printf("FAIL fixture heads: %s\n", err.c_str()), void(++failures);
    // a case: outputs, then (two-view) the partner's outputs and f32 has2, then the expected floats
    const size_t ob = h.out_bytes(), lead = h.two_view ? 2 * ob + 4 : ob, one = lead + 4 * (42 + 2 + 63);
    const size_t n = (d.size() - head) / one;
    CHECK(n > 0 && head + n * one == d.size());
    double worst = 0;
    int paired = 0;
    for (size_t c = 0; c < n; ++c) {
        const uint8_t *p = d.data() + head + c * one;
        float has2 = 0, want[42 + 2 + 63];
        if (h.two_view) std::memcpy(&has2, p + 2 * ob, 4);
        std::memcpy(want, p + lead, sizeof want);
        float screen[42], world[63], pr, rt;
        h.decode(p, screen, world, pr, rt, has2 != 0.f ? p + ob : nullptr);
        paired += has2 != 0.f;
        for (int i = 0; i < 42; ++i) worst = std::max(worst, double(std::fabs(screen[i] - want[i])));
        worst = std::max({worst, double(std::fabs(pr - want[42])), double(std::fabs(rt - want[43]))});
        for (int i = 0; i < 63; ++i) worst = std::max(worst, double(std::fabs(world[i] - want[44 + i])));
    }
    std::printf("fixture: %zu cases (%d with a partner), max diff from int8_ref.heads %.2e\n", n, paired, worst);
    CHECK(worst < 1e-3);
    if (h.two_view) CHECK(paired > 0 && paired < int(n));   // both kinds covered
}

int main(int argc, char **argv) {
    peaked();
    if (argc > 1) fixture(argv[1]);
    std::printf(failures ? "npuheads: %d failures\n" : "npuheads: ok\n", failures);
    return failures != 0;
}
