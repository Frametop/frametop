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

// The aqua border round a panel the 3D mouse carries (vr.cpp: UpdateCarryGlow; user, 2026-11-01: "a
// glowing aqua border with a small air gap between it and the window"): w x h pixels (RGBA), the panel m
// pixels in from each side, clear over it and `gap` pixels out from it, then a line `line` wide, nearly
// opaque, glowing off over `glow` outside it and over half the gap inside it (pixels throughout). The
// line's edges are anti-aliased: each pixel has the share of it that the pixel covers (a box filter on
// the distance out), so it doesn't step (user, 2026-11-01: "the existing border looks quite aliased").
// rgb: aqua for the carried panel, fuchsia for the others carry-all moves.
struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kAqua{40, 228, 240};      // the carried panel's
constexpr Rgb kFuchsia{255, 40, 200};   // the others' in carry-all (user, 2026-11-01: "a fuchsia border that doesn't pulse")

inline std::vector<uint8_t> CarryTexture(int w, int h, int m, double gap, double line, double glow, Rgb rgb = kAqua) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double ox = std::max({m - (x + 0.5), (x + 0.5) - (w - m), 0.0}), oy = std::max({m - (y + 0.5), (y + 0.5) - (h - m), 0.0});
            const double d = std::hypot(ox, oy);
            const double cover = std::clamp(d - gap + 0.5, 0.0, 1.0) - std::clamp(d - gap - line + 0.5, 0.0, 1.0);
            const double a = d <= 0 ? 0 : std::max({0.9 * cover, Alpha(d - gap - line, glow, 0.45), Alpha(gap - d, gap / 2, 0.45)});
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = rgb.r, p[1] = rgb.g, p[2] = rgb.b, p[3] = uint8_t(std::clamp(a, 0.0, 1.0) * 255 + 0.5);
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
            } else if (g.kind == handinput::Guide::Touch) {   // a soft dot of radius r (a ring round it when pressed)
                const double d = std::hypot(X, Y);
                a = (d <= r ? 0.55 : Alpha(d - r, halo, 0.55)) * (g.selected ? 1.4 : 1);
                if (g.selected) a = std::max(a, Alpha(std::fabs(d - 1.6 * r), 0.35 * r, 0.7));
            }
            Pixel(&px[(size_t(y) * w + x) * 4], a);
        }
    return px;
}

// The carry's movement aids (vr.cpp: UpdateCarryAids; user, 2026-11-01). Each grid is w x h pixels
// (RGBA) round a panel pw x ph pixels in the middle: clear over the panel (anti-aliased at its edge),
// the lines at full strength next to it and fading to nothing `margin` pixels out from it. Only the band
// round the panel is worked out (GridSkip), as a grid is drawn each frame.
constexpr Rgb kGridRgb{150, 236, 246};
constexpr double kGridPeak = 0.18;   // (user, 2026-11-01: "please make the grids fainter"; was 0.35)
inline double GridFade(int w, int h, double pw, double ph, double margin, double x, double y) {
    const double ox = std::fabs(x - w / 2.0) - pw / 2, oy = std::fabs(y - h / 2.0) - ph / 2;
    const double d = ox > 0 || oy > 0 ? std::hypot(std::max(ox, 0.0), std::max(oy, 0.0)) : std::max(ox, oy);
    const double cut = std::clamp(d + 0.5, 0.0, 1.0), t = std::clamp(d / std::max(1.0, margin), 0.0, 1.0);
    return cut * (1 - t * t * (3 - 2 * t));
}
// Row y's pixels [*a, *b) are well inside the panel (clear): none if the row isn't.
inline void GridSkip(int w, int h, double pw, double ph, int y, int *a, int *b) {
    *a = *b = 0;
    if (std::fabs(y + 0.5 - h / 2.0) > ph / 2 - 1) return;
    *a = int(std::ceil(w / 2.0 - pw / 2 + 1)), *b = std::max(*a, int(std::floor(w / 2.0 + pw / 2 - 1)));
}
// A line's share of a pixel d pixels from its middle, for a line `line` pixels wide.
inline double LineCover(double d, double line) { return std::clamp(line / 2 + 0.5 - std::fabs(d), 0.0, 1.0); }
// d's distance to the nearest multiple of step.
inline double ToGrid(double d, double step) { return d - step * std::round(d / step); }
inline void GridPixel(uint8_t *p, double a, Rgb rgb = kGridRgb) {
    p[0] = rgb.r, p[1] = rgb.g, p[2] = rgb.b, p[3] = uint8_t(std::clamp(a, 0.0, 1.0) * 255 + 0.5);
}

// (rgb: kGridRgb, or kFuchsia while the head carries the panel.)
// Ctrl's plane (it moves in its own plane): lines every `step` pixels in the plane, fixed there (the
// pixel at the top left's middle is at (x0, y0) in it, y up), so they slide past as the panel moves.
inline void PlaneGrid(std::vector<uint8_t> &px, int w, int h, double pw, double ph, double margin, double x0, double y0, double step,
                      double line, Rgb rgb = kGridRgb) {
    px.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        int sa, sb;
        GridSkip(w, h, pw, ph, y, &sa, &sb);
        for (int x = 0; x < w; ++x) {
            if (x == sa && sb > sa) x = sb;
            const double f = GridFade(w, h, pw, ph, margin, x + 0.5, y + 0.5);
            if (f <= 0) continue;
            const double c = std::max(LineCover(ToGrid(x0 + x, step), line), LineCover(ToGrid(y0 - y, step), line));
            GridPixel(&px[(size_t(y) * w + x) * 4], kGridPeak * f * c, rgb);
        }
    }
}

// The sphere round the eyes it moves on (no modifier): its meridians and parallels every `step` radians,
// fixed in the room (yaw from -z, pitch from level), seen from the eyes. The pixel at the top left's
// middle is at o (metres, from the eyes), each pixel right ax and down ay from the last; mpp metres a
// pixel. The angles are worked out every 16 pixels and blended in between (they change slowly across
// one panel), so a frame's grid is cheap.
inline void SphereGrid(std::vector<uint8_t> &px, int w, int h, double pw, double ph, double margin, const double o[3], const double ax[3],
                       const double ay[3], double mpp, double step, double line, Rgb rgb = kGridRgb) {
    px.assign(size_t(w) * h * 4, 0);
    constexpr int kCell = 16;
    const int gw = w / kCell + 2, gh = h / kCell + 2;
    // per lattice point: yaw (from the middle's, unwrapped), pitch, and metres a radian along each
    std::vector<double> yaw(size_t(gw) * gh), pitch(yaw.size()), mYaw(yaw.size()), mPitch(yaw.size());
    double yaw0 = 0;
    {
        double c[3];
        for (int k = 0; k < 3; ++k) c[k] = o[k] + ax[k] * w / 2 + ay[k] * h / 2;
        yaw0 = std::atan2(c[0], -c[2]);
    }
    for (int j = 0; j < gh; ++j)
        for (int i = 0; i < gw; ++i) {
            double d[3];
            for (int k = 0; k < 3; ++k) d[k] = o[k] + ax[k] * i * kCell + ay[k] * j * kCell;
            const double flat = std::hypot(d[0], d[2]), r = std::sqrt(flat * flat + d[1] * d[1]);
            const size_t n = size_t(j) * gw + i;
            yaw[n] = std::remainder(std::atan2(d[0], -d[2]) - yaw0, 2 * M_PI);
            pitch[n] = std::atan2(d[1], flat);
            mYaw[n] = flat, mPitch[n] = r;
        }
    auto at = [&](const std::vector<double> &v, double fx, double fy) {
        const int i = std::min(int(fx), gw - 2), j = std::min(int(fy), gh - 2);
        const double tx = fx - i, ty = fy - j;
        const size_t n = size_t(j) * gw + i;
        return (v[n] * (1 - tx) + v[n + 1] * tx) * (1 - ty) + (v[n + gw] * (1 - tx) + v[n + gw + 1] * tx) * ty;
    };
    for (int y = 0; y < h; ++y) {
        int sa, sb;
        GridSkip(w, h, pw, ph, y, &sa, &sb);
        for (int x = 0; x < w; ++x) {
            if (x == sa && sb > sa) x = sb;
            const double f = GridFade(w, h, pw, ph, margin, x + 0.5, y + 0.5);
            if (f <= 0) continue;
            const double fx = double(x) / kCell, fy = double(y) / kCell;
            const double dy = ToGrid(at(yaw, fx, fy) + yaw0, step) * at(mYaw, fx, fy) / mpp;
            const double dp = ToGrid(at(pitch, fx, fy), step) * at(mPitch, fx, fy) / mpp;
            GridPixel(&px[(size_t(y) * w + x) * 4], kGridPeak * f * std::max(LineCover(dy, line), LineCover(dp, line)), rgb);
        }
    }
}

// Alt's rings (it turns about its centre): an n x n ring, `line` pixels wide, its outer edge at the
// texture's, in rgb. Nearly clear all round, but for a bright pulse at angle 0 (+x) with a tail behind
// it (clockwise, so the ring turned anticlockwise sweeps it round), a quarter turn long; vr.cpp turns
// the ring to sweep it.
inline std::vector<uint8_t> RingTexture(int n, double line, Rgb rgb) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    const double r = n / 2.0 - line / 2 - 1;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double X = x + 0.5 - n / 2.0, Y = n / 2.0 - y - 0.5;
            const double c = LineCover(std::hypot(X, Y) - r, line);
            if (c <= 0) continue;
            const double back = std::fmod(2 * M_PI - std::fmod(std::atan2(Y, X) + 2 * M_PI, 2 * M_PI), 2 * M_PI) / (M_PI / 2);   // quarter turns behind the head
            const double pulse = back < 1 ? (1 - back) * (1 - back) : 0;
            GridPixel(&px[(size_t(y) * n + x) * 4], c * (0.06 + 0.64 * pulse), rgb);
        }
    return px;
}

}  // namespace softglow
