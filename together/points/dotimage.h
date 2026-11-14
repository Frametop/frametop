// A pointer's mark as a picture: a disc in its owner's colour with a light rim and a dark edge
// outside it, so it shows on a light desktop and on a dark one. While its owner holds a button
// down the disc is whole; otherwise its middle is clear, and what is pointed at shows through.
//
// Where someone looks is another picture: a soft patch of their colour, faint and with no edge,
// since eyes rest on what is read and the mark is never to hide it.
//
// Header only.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace points {

constexpr int kDotPixels = 64;

// kDotPixels x kDotPixels RGBA, straight alpha, of a mark in `colour` (red, green, blue, 0 to 1).
inline std::vector<uint8_t> DotImage(const float colour[3], bool pressed) {
    std::vector<uint8_t> px(size_t(kDotPixels) * kDotPixels * 4, 0);
    const double mid = (kDotPixels - 1) / 2.0, edge = kDotPixels / 2.0 - 1;
    // from the middle, in parts of the radius: clear (or whole when pressed), colour, light rim, dark edge
    const double hole = pressed ? -1 : 0.34, rim = 0.74, dark = 0.90;
    for (int y = 0; y < kDotPixels; ++y)
        for (int x = 0; x < kDotPixels; ++x) {
            const double d = std::hypot(x - mid, y - mid) / edge;
            const double cover = std::clamp((1 - d) * edge, 0.0, 1.0);   // the outside edge, softened over a pixel
            if (cover <= 0 || d < hole) continue;
            double r = colour[0], g = colour[1], b = colour[2], a = 1;
            if (d >= dark) r = g = b = 0.05, a = 0.85;
            else if (d >= rim) r = g = b = 0.96;
            uint8_t *p = &px[(size_t(y) * kDotPixels + size_t(x)) * 4];
            p[0] = uint8_t(std::lround(std::clamp(r, 0.0, 1.0) * 255)), p[1] = uint8_t(std::lround(std::clamp(g, 0.0, 1.0) * 255));
            p[2] = uint8_t(std::lround(std::clamp(b, 0.0, 1.0) * 255)), p[3] = uint8_t(std::lround(a * cover * 255));
        }
    return px;
}

constexpr double kGlanceMost = 0.4;   // how much of what is under its middle a look's mark covers

// kDotPixels x kDotPixels RGBA, straight alpha, of the mark of where someone looks, in `colour`:
// kGlanceMost of it at the middle, fading to nothing at the rim.
inline std::vector<uint8_t> GlanceImage(const float colour[3]) {
    std::vector<uint8_t> px(size_t(kDotPixels) * kDotPixels * 4, 0);
    const double mid = (kDotPixels - 1) / 2.0, edge = kDotPixels / 2.0 - 1;
    for (int y = 0; y < kDotPixels; ++y)
        for (int x = 0; x < kDotPixels; ++x) {
            const double d = std::hypot(x - mid, y - mid) / edge;
            if (d >= 1) continue;
            uint8_t *p = &px[(size_t(y) * kDotPixels + size_t(x)) * 4];
            for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(std::clamp(double(colour[c]), 0.0, 1.0) * 255));
            p[3] = uint8_t(std::lround(kGlanceMost * (1 - d * d) * 255));
        }
    return px;
}

}  // namespace points
