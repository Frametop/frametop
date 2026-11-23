// The centre display's look (dashtiles.h's Board: the tile the strip turns to keep straight ahead):
// its blue glow, the test for a drop onto it, and its size when curved round the tiles' arc.
//
// User, 2026-11-23: "a blue grid glow that indicates which one is in the center. And if we want to
// change ... that center display, we would drag it on top of it and indicate that it will replace it
// by having that app glow blue. And it should only happen if it really is right in the center." And
// for the curve: "Neighbors should ideally be positioned so they won't be blocked by the curve since
// the curve should follow how the display would angle naturally if there were many displays in the
// row": curved round the arc's own centre at its radius, the span seen from there the flat panel's,
// its edges at the depth of the neighbours' corners (tiling.h's CornersOnSphere).
// Pure: screens/tests/centreglow-test.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace centreglow {

constexpr double kDropDeg = 4;   // let go with its middle this near the centre's middle (round you and up): it takes its place

// A dragged tile's middle at (az, el), degrees from the arc's centre, right on the centre display's middle.
inline bool Over(double az, double el, double centreAz, double centreEl, double tolDeg = kDropDeg) {
    const double d = az - centreAz - 360 * std::round((az - centreAz) / 360);
    return std::fabs(d) <= tolDeg && std::fabs(el - centreEl) <= tolDeg;
}

// Curved round the arc at `radius` (its middle there): as wide along the arc as the flat tile
// (`widthM` wide, its middle `distM` from the arc's centre) spans seen from there.
inline double ArcWidth(double widthM, double distM, double radius) {
    return distM > 0 ? radius * 2 * std::atan2(widthM / 2, distM) : widthM;
}

// The glow's band round a panel `dist` metres away (about 1.3 degrees, as the white target glow), metres.
inline double Band(double dist) { return std::clamp(0.022 * dist, 0.02, 0.08); }
constexpr double kPeak = 0.55;      // the centre's alpha at the panel's edge
constexpr double kHotPeak = 0.95;   // a dragged tile's that would take its place

// w x h pixels (RGBA), the panel mx and my pixels in from each side and clear over it: blue at
// `peak` at its edge, falling off smoothly to nothing at the texture's edge.
inline std::vector<uint8_t> Texture(int w, int h, int mx, int my, double peak) {
    std::vector<uint8_t> px(size_t(std::max(0, w)) * size_t(std::max(0, h)) * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = std::max({0.0, double(mx - x), double(x - (w - 1 - mx))}) / std::max(1, mx);
            const double dy = std::max({0.0, double(my - y), double(y - (h - 1 - my))}) / std::max(1, my);
            const double d = std::sqrt(dx * dx + dy * dy);   // (0 at the panel's edge, 1 at the band's outside)
            if (d <= 0 || d >= 1) continue;
            uint8_t *p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = 70, p[1] = 150, p[2] = 255, p[3] = uint8_t(std::clamp(peak * (1 - d) * (1 - d), 0.0, 1.0) * 255 + 0.5);
        }
    return px;
}

}  // namespace centreglow
