// The soft white glow round Frametop's panels (vr.cpp: UpdateTargetGlow, UpdateHandGuides), as textures:
// pure code, so tests (screens/tests/handinput_test.cpp) check them without OpenVR.
//
// User, after the fourth live test (2026-10-10): "The glow is too powerful. It should be a white glow and
// far more subtle. It should only be around the outside of the window." And for resizing: "almost like a
// cutout of the soft glow we had for highlighting, just around the edges and corners when you're within
// the interaction zone". So: white, at most half opaque at the panel's edge (0.85 while held), falling
// off over a thin band (Width: about 1.3 degrees seen from the eyes) and clear over the panel itself;
// whole round a panel ("grab that"'s target), or only an L round a corner, a short stretch of an edge,
// or a ring round a button (a hand's guide, handinput's Guide).
#pragma once

#include "handinput.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace softglow {

// The halo's alpha d outside an edge, over a halo `width` wide: `peak` at the edge, falling off smoothly
// to nothing (the same units for both). Inside (d < 0): nothing.
inline double Alpha(double d, double width, double peak) {
    if (d < 0 || width <= 0) return 0;
    const double x = d / width;
    return x >= 1 ? 0 : peak * (1 - x) * (1 - x);
}
// Its width for a panel `dist` metres from the eyes: about 1.3 degrees (1 cm at arm's reach), 8 mm to 5 cm.
inline double Width(double dist) { return std::clamp(0.022 * dist, 0.008, 0.05); }
// The white (a touch cool), at alpha a.
inline void Pixel(uint8_t *p, double a) {
    p[0] = 246, p[1] = 249, p[2] = 255, p[3] = uint8_t(std::clamp(a, 0.0, 1.0) * 255 + 0.5);
}
constexpr double kTargetPeak = 0.5;   // the target's alpha at the edge, before its fade

// The target's texture: w x h pixels (RGBA), the panel m pixels in from each side, clear over it.
inline std::vector<uint8_t> TargetTexture(int w, int h, int m) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double ox = std::max({m - (x + 0.5), (x + 0.5) - (w - m), 0.0}), oy = std::max({m - (y + 0.5), (y + 0.5) - (h - m), 0.0});
            const double d = std::hypot(ox, oy);
            Pixel(&px[(size_t(y) * w + x) * 4], d > 0 ? Alpha(d, m, kTargetPeak) : 0);
        }
    return px;
}

// A hand's guide's texture, w x h pixels (RGBA) for `size` metres across (w), its point in the middle:
// a corner (the panel lies away from sx, sy; the L runs `arm` along each edge, fading over its last
// two thirds), an edge's point (the panel on the inner side; the segment `arm` long, fading over its
// outer halves), a button's centre (a ring outside radius r; masked off above `edge`, the panel's bottom
// edge over a button under it). halo: the halo's width. Metres throughout. Brighter when selected.
inline std::vector<uint8_t> GuideTexture(const handinput::Guide &g, int w, int h, double size, double halo, double arm, double r,
                                         double edge) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    const double perM = w / size, peak = g.selected ? 0.85 : g.kind == handinput::Guide::Edge ? 0.45 : 0.55;
    auto smooth = [](double a, double b, double x) {
        const double t = std::clamp((x - a) / std::max(1e-9, b - a), 0.0, 1.0);
        return t * t * (3 - 2 * t);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double X = (x + 0.5 - w / 2.0) / perM, Y = (h / 2.0 - y - 0.5) / perM;
            double a = 0;
            if (g.kind == handinput::Guide::Corner) {
                const double ax = g.sx * X, by = g.sy * Y;   // out past each edge
                const double d = std::hypot(std::max(ax, 0.0), std::max(by, 0.0));
                if (ax >= 0 || by >= 0) a = Alpha(d, halo, peak) * (1 - smooth(0.35 * arm, arm, std::max(-ax, -by)));
            } else if (g.kind == handinput::Guide::Edge) {
                const double out = g.sy ? g.sy * Y : g.sx * X, along = g.sy ? X : Y;
                if (out >= 0) a = Alpha(out, halo, peak) * (1 - smooth(0.25 * arm, 0.5 * arm, std::fabs(along)));
            } else if (g.kind == handinput::Guide::Button) {
                const double d = std::hypot(X, Y) - r;
                if (d >= 0 && Y < edge) a = Alpha(d, halo, peak);
            }
            Pixel(&px[(size_t(y) * w + x) * 4], a);
        }
    return px;
}

}  // namespace softglow
