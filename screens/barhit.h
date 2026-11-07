// Drag-bar target geometry and its optional preview, with no OpenVR dependency.
// A padded canvas keeps the 256x24 pill the same physical size on a larger overlay.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace barhit {
constexpr double kDefaultPaddingCm = 2, kMaxPaddingCm = 50;
constexpr double kAspect = 24.0 / 256;

inline double Padding(double cm) {
    return std::isfinite(cm) ? std::clamp(cm, 0.0, kMaxPaddingCm) : kDefaultPaddingCm;
}
inline double Height(double width, double cm) { return width * kAspect + 2 * Padding(cm) / 100; }
inline double Width(double width, double cm) { return width + 2 * Padding(cm) / 100; }
inline double Below(double width, double button, double cm) {
    return width * 0.06 + std::max(Height(width, cm) / 2, button / 2);
}

// Constant physical padding needs a different raster for each pill size. Bound the
// canvas on both axes, even for a tiny nearby pill and maximum padding. Equal raster
// keys share the same image; small pose changes need no texture regeneration.
using Raster = std::array<int, 4>;  // canvas width/height, pill width/height
inline Raster Pixels(double width, double xCm, double yCm) {
    width = std::max(0.001, width);
    const double density = std::min(256 / width, 1022 / std::max(Width(width, xCm), Height(width, yCm)));
    const int w = std::max(1, int(std::lround(width * density)));
    const int h = std::max(1, int(std::lround(width * kAspect * density)));
    return {w + 2 * int(std::lround(Padding(xCm) / 100 * density)),
            h + 2 * int(std::lround(Padding(yCm) / 100 * density)), w, h};
}

struct Style {
    bool enabled = false, always = false;
    uint32_t color = 0x66bbff, outlineColor = 0x99ddff;
    double opacity = 0.12, outlineOpacity = 0.7, outlineWidth = 1;
};

inline bool Color(const char *text, uint32_t *out) {
    if (std::strlen(text) != 7 || text[0] != '#') return false;
    uint32_t n = 0;
    for (int i = 1; i < 7; ++i) {
        const char c = text[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        n = n * 16 + unsigned(d);
    }
    *out = n;
    return true;
}

struct Image { int width = 0, height = 0; std::vector<uint8_t> pixels; };

// Straight-alpha source-over, so the visible pill stays intact over a translucent zone.
inline void Over(uint8_t *dst, const uint8_t *src) {
    const double a = src[3] / 255.0, b = dst[3] / 255.0 * (1 - a), total = a + b;
    if (total <= 0) return;
    for (int c = 0; c < 3; ++c) dst[c] = uint8_t(std::lround((src[c] * a + dst[c] * b) / total));
    dst[3] = uint8_t(std::lround(total * 255));
}

inline Image Render(const Raster &raster, const Style &style, const std::vector<uint8_t> &pill) {
    Image im;
    im.width = raster[0], im.height = raster[1];
    im.pixels.resize(size_t(im.width) * im.height * 4, 0);
    if (style.enabled) {
        const int line = style.outlineWidth > 0 ? std::max(1, int(std::lround(std::clamp(style.outlineWidth, 0.0, 8.0) * raster[2] / 256))) : 0;
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) {
                const bool edge = x < line || y < line || x >= im.width - line || y >= im.height - line;
                const uint32_t rgb = edge ? style.outlineColor : style.color;
                auto *p = &im.pixels[(size_t(y) * im.width + x) * 4];
                p[0] = uint8_t(rgb >> 16), p[1] = uint8_t(rgb >> 8), p[2] = uint8_t(rgb);
                p[3] = uint8_t(std::lround(std::clamp(edge ? style.outlineOpacity : style.opacity, 0.0, 1.0) * 255));
            }
    }
    const int dx = (im.width - raster[2]) / 2, dy = (im.height - raster[3]) / 2;
    for (int y = 0; y < raster[3]; ++y)
        for (int x = 0; x < raster[2]; ++x) {
            const int sx = std::min(255, int((x + 0.5) * 256 / raster[2]));
            const int sy = std::min(23, int((y + 0.5) * 24 / raster[3]));
            Over(&im.pixels[(size_t(y + dy) * im.width + x + dx) * 4], &pill[(size_t(sy) * 256 + sx) * 4]);
        }
    return im;
}
}  // namespace barhit
