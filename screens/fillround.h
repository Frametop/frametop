// A filled rounded rectangle, antialiased, blended into an RGBA buffer: the shape ft-screens' small
// textures are drawn from (the bar, the menus, the asks). No OpenVR: the offline renders use it too.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ftvr {

// x0..x1, y0..y1 (end exclusive), corner radius r, colour c (its alpha the blend).
inline void FillRound(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, int r, const uint8_t c[4]) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            const int dx = x < x0 + r ? x0 + r - x : x >= x1 - r ? x - (x1 - r - 1) : 0;
            const int dy = y < y0 + r ? y0 + r - y : y >= y1 - r ? y - (y1 - r - 1) : 0;
            const double out = std::sqrt(double(dx * dx + dy * dy)) - r;
            const double a = std::clamp(0.5 - out, 0.0, 1.0) * c[3] / 255.0;
            if (a <= 0) continue;
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (c[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], 255 * a));
        }
}

// x0..x1, y0..y1 (end exclusive), square corners (FillRound's r 0 blends its inside at half).
inline void FillRect(std::vector<uint8_t> &px, int w, int h, int x0, int y0, int x1, int y1, const uint8_t c[4]) {
    const double a = c[3] / 255.0;
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (c[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], c[3]));
        }
}

}  // namespace ftvr
