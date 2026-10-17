// Compare every byte with the original keyboard renderer, without a headset or graphics stack.
#include "../rounded.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

void Original(uint8_t *px, int width, int x0, int y0, int x1, int y1, double radius, const uint8_t rgba[4]) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double cx = std::clamp(x + 0.5, x0 + radius, x1 - radius);
            const double cy = std::clamp(y + 0.5, y0 + radius, y1 - radius);
            const double cover = std::clamp(radius - std::hypot(x + 0.5 - cx, y + 0.5 - cy) + 0.5, 0.0, 1.0);
            if (cover <= 0) continue;
            uint8_t *p = &px[(size_t(y) * width + x) * 4];
            const double a = rgba[3] / 255.0 * cover, below = p[3] / 255.0;
            const double out = a + below * (1 - a);
            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t(std::lround(out > 0 ? (rgba[c] * a + p[c] * below * (1 - a)) / out : 0));
            p[3] = uint8_t(std::lround(out * 255));
        }
}

using Fill = decltype(&Original);
constexpr int kW = 1280, kH = 470;

uint64_t Hash(const std::vector<uint8_t> &px) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t b : px) hash = (hash ^ b) * 1099511628211ull;
    return hash;
}

// The real keyboard's rounded shapes, including hovered/pressed/latched colours and grip.
// Text and arrows use unchanged renderers; this fixture isolates the rounded rectangles.
void Keyboard(std::vector<uint8_t> &px, Fill fill, int state) {
    std::fill(px.begin(), px.end(), uint8_t(0));
    const uint8_t back[4] = {20, 22, 26, 225}, bar[4] = {220, 222, 228, uint8_t(state % 2 ? 235 : 120)};
    fill(px.data(), kW, 0, 0, kW, kH, 22, back);
    fill(px.data(), kW, kW / 2 - 130, 15, kW / 2 + 130, 27, 6, bar);
    const std::vector<std::vector<double>> rows = {
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2},
        {1.5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.5},
        {1.75, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.25},
        {1.25, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.75, 1, 1},
        {1.5, 1.5, 7, 2, 1, 1, 1},
    };
    const double unit = double(kW - 24) / 15, row = double(kH - 40 - 12) / 5;
    int index = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
        double x = 12;
        for (double units : rows[r]) {
            uint8_t face[4] = {58, 62, 70, 255};
            if (index == state - 1) face[0] = 26, face[1] = 115, face[2] = 232;
            else if (index == state) for (int c = 0; c < 3; ++c) face[c] = uint8_t(face[c] + 38);
            fill(px.data(), kW, int(std::lround(x + 3.5)), int(std::lround(40 + r * row + 3.5)),
                 int(std::lround(x + units * unit - 3.5)), int(std::lround(40 + (r + 1) * row - 3.5)), 10, face);
            x += units * unit;
            ++index;
        }
    }
}

bool Check() {
    // Every source channel/alpha pair over transparent pixels, including nonzero RGB at A=0.
    for (double radius : {0., 0.5})
      for (int a = 0; a < 256; ++a)
        for (int c = 0; c < 256; ++c) {
            uint8_t old[4] = {43, 129, 255, 0}, got[4];
            std::copy_n(old, 4, got);
            const uint8_t rgba[4] = {uint8_t(c), uint8_t(255 - c), uint8_t(c ^ 85), uint8_t(a)};
            Original(old, 1, 0, 0, 1, 1, radius, rgba);
            rounded::Fill(got, 1, 0, 0, 1, 1, radius, rgba);
            if (std::memcmp(old, got, 4)) return false;
        }
    std::mt19937 rng(42);
    std::vector<uint8_t> old(64 * 48 * 4), got(old.size());
    for (int i = 0; i < 4000; ++i) {
        for (auto &v : old) v = uint8_t(rng());
        if (i % 3 == 0) for (size_t p = 3; p < old.size(); p += 4) old[p] = 0;
        if (i % 3 == 1) for (size_t p = 3; p < old.size(); p += 4) old[p] = 255;
        got = old;
        const uint8_t color[4] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(i % 256)};
        const int x = int(rng() % 40), y = int(rng() % 24), w = 1 + int(rng() % 24), h = 1 + int(rng() % 24);
        const double radius = std::min(w, h) * (rng() % 501) / 1000.;
        Original(old.data(), 64, x, y, x + w, y + h, radius, color);
        rounded::Fill(got.data(), 64, x, y, x + w, y + h, radius, color);
        if (old != got) return false;  // compares untouched pixels outside the rectangle too
    }
    old.resize(kW * kH * 4), got.resize(old.size());
    // Coverage near 1 must keep the original subtraction's rounding at larger coordinates.
    for (auto &v : old) v = uint8_t(rng());
    got = old;
    const uint8_t color[4] = {191, 73, 225, 50};
    Original(old.data(), kW, 31, 267, 51, 298, 2.84, color);
    rounded::Fill(got.data(), kW, 31, 267, 51, 298, 2.84, color);
    if (old != got) return false;
    uint64_t hash = 0;
    for (int state = 0; state <= 62; ++state) {
        Keyboard(old, Original, state), Keyboard(got, rounded::Fill, state);
        if (old != got) return false;
        hash ^= Hash(got);
    }
    std::printf("65536 channel/alpha pairs at two coverages, 4001 varied rectangles and 63 keyboard states identical; hash %016llx\n",
                static_cast<unsigned long long>(hash));
    return true;
}

void Benchmark() {
    std::vector<uint8_t> px(kW * kH * 4);
    // Alternate order across rounds; checksum work stays outside the timed region.
    for (int round = 0; round < 5; ++round)
        for (int order = 0; order < 2; ++order) {
            const bool original = (order + round) % 2 == 0;
            const Fill fill = original ? Original : rounded::Fill;
            Keyboard(px, fill, 0);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; ++i) Keyboard(px, fill, i);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
            std::printf("round %d %s %.6f ms hash %016llx\n", round, original ? "original" : "optimized", ms,
                        static_cast<unsigned long long>(Hash(px)));
        }
}

}  // namespace

int main(int argc, char **argv) {
    if (!Check()) {
        std::fputs("rounded renderer differs from the original\n", stderr);
        return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) Benchmark();
}
