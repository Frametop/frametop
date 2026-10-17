// The keyboard's straight-alpha rounded rectangles, without SteamVR or GBM dependencies.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rounded {

inline void Fill(uint8_t *px, int width, int x0, int y0, int x1, int y1, double radius, const uint8_t rgba[4]) {
    for (int y = y0; y < y1; ++y) {
        const double cy = std::clamp(y + 0.5, y0 + radius, y1 - radius);
        const double dy = y + 0.5 - cy;
        for (int x = x0; x < x1; ++x) {
            const double cx = std::clamp(x + 0.5, x0 + radius, x1 - radius);
            const double dx = x + 0.5 - cx;
            // Most pixels lie on a straight strip or in the centre. Keep the coverage
            // arithmetic even there: fractional radii can round just below full coverage.
            const double distance = dx == 0 ? std::abs(dy) : dy == 0 ? std::abs(dx) : std::hypot(dx, dy);
            const double cover = std::clamp(radius - distance + 0.5, 0.0, 1.0);
            if (cover <= 0) continue;
            uint8_t *p = &px[(size_t(y) * width + x) * 4];
            // A fully covered opaque key replaces its background. The empty background
            // likewise takes a nonzero source unchanged, including its straight RGB.
            if (cover == 1 && rgba[3] > 0 && (rgba[3] == 255 || p[3] == 0)) {
                std::copy_n(rgba, 4, p);
                continue;
            }
            const double a = rgba[3] / 255.0 * cover, below = p[3] / 255.0;
            const double out = a + below * (1 - a);
            for (int c = 0; c < 3; ++c)
                p[c] = uint8_t(std::lround(out > 0 ? (rgba[c] * a + p[c] * below * (1 - a)) / out : 0));
            p[3] = uint8_t(std::lround(out * 255));
        }
    }
}

}  // namespace rounded
