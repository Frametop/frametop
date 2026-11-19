// Filled rectangles, square or rounded and antialiased, blended into an RGBA buffer, and a card's
// rounded corners cut out: the shapes ft-screens' small textures are drawn from (the bar, the menus, the
// asks). No OpenVR: the offline renders use them too (screens/tests/menulayout-test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftvr {

// x0..x1, y0..y1 (end exclusive), corner radius r, colour c (its alpha the blend). Only the corners'
// edge pixels are worked out; the rest is a plain store (an opaque colour) or one blend.
inline void FillRound(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, int r, const uint8_t c[4]) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            const int dx = x < x0 + r ? x0 + r - x : x >= x1 - r ? x - (x1 - r - 1) : 0;
            const int dy = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r - 1) : 0;
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            const double out = dx || dy ? std::sqrt(double(dx * dx + dy * dy)) - r : -r;
            const double a = std::clamp(0.5 - out, 0.0, 1.0) * c[3] / 255.0;
            if (a <= 0) continue;
            if (a >= 1) {
                p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 255;
                continue;
            }
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (c[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], 255 * a));
        }
}

// x0..x1, y0..y1 (end exclusive), square corners (FillRound's r 0 blends its inside at half). An
// opaque colour is a plain store.
inline void FillRect(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, const uint8_t c[4]) {
    const double a = c[3] / 255.0;
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            if (c[3] == 255) {
                p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 255;
                continue;
            }
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (c[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], c[3]));
        }
}

// Everything outside a rounded card of radius r goes clear (the design's overflow: hidden). Only the four
// r x r corners can be outside it.
inline void Round(std::vector<uint8_t> &px, int W, int H, int r) {
    r = std::clamp(r, 0, std::min(W, H) / 2);
    const int xs[2] = {0, W - r}, ys[2] = {0, H - r};
    for (int cy0 : ys)
        for (int cx0 : xs)
            for (int y = cy0; y < cy0 + r; ++y)
                for (int x = cx0; x < cx0 + r; ++x) {
                    const double cx = std::clamp(x + 0.5, double(r), double(W - r)), cy = std::clamp(y + 0.5, double(r), double(H - r));
                    const double a = std::clamp(r - std::hypot(x + 0.5 - cx, y + 0.5 - cy) + 0.5, 0.0, 1.0);
                    if (a < 1) px[(size_t(y) * W + x) * 4 + 3] = uint8_t(std::lround(px[(size_t(y) * W + x) * 4 + 3] * a));
                }
}

}  // namespace ftvr
