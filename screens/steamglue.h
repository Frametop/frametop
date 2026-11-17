// Steam's menu glued to Steam's page (user, 2026-11-17: the menu and its icon strip glued to the
// page "almost as if they were one display", without the black border round it). Steam's menu
// window is 450 x 1200 pixels, but it draws only the strip at its right edge (54 CSS pixels wide,
// from the middle 60% of its height) and its labels to the left of it while the laser is on it
// (the strip grows leftwards, to about 240 CSS pixels). The rest of the window is black.
//
// So the menu's panel shows only the part Steam draws (its page tells us: steamcdp's AskDrawn),
// flush left of the page's panel at the page's density, its middle level with the page's. It moves
// with the page, the tiles count it in the page's width (the width it has with nobody on it, so
// its labels opening don't move the tiles), and it has no controls of its own.
//
// Pure math, no OpenVR: screens/tests/steamglue-test.cpp.
#pragma once

#include <algorithm>
#include <cmath>

namespace steamglue {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;   // window pixels, from the top left
};

// The window pixels of a w x h window that CSS bounds l, t, r, b (at pixel ratio dpr) cover,
// rounded outwards and clipped to the window. False when that's nothing.
inline bool FromCss(double l, double t, double r, double b, double dpr, int w, int h, Rect *out) {
    if (!(dpr > 0) || w <= 0 || h <= 0 || !std::isfinite(l + t + r + b)) return false;
    const int x0 = std::max(0, int(std::floor(l * dpr))), y0 = std::max(0, int(std::floor(t * dpr)));
    const int x1 = std::min(w, int(std::ceil(r * dpr))), y1 = std::min(h, int(std::ceil(b * dpr)));
    if (x1 <= x0 || y1 <= y0) return false;
    *out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

// Where the glued panel's middle goes in the page's panel's frame (x right, y up, z out of its
// front; 3x4, row major): its right edge on the page's left edge, `up` metres above the page's
// middle. The page is pageW metres wide, curved round a circle `radius` metres away in front of
// it (0: flat), and the glued one, gluedW wide, goes on round the same circle.
inline void Beside(double pageW, double radius, double gluedW, double up, double m[3][4]) {
    double x = -(pageW + gluedW) / 2, z = 0, turn = 0;
    if (radius > 0) {
        turn = (pageW + gluedW) / (2 * radius);   // its middle's angle round the circle from the page's
        x = -radius * std::sin(turn), z = radius * (1 - std::cos(turn));
    }
    const double c = std::cos(turn), s = std::sin(turn);   // turned about y to face the circle's centre
    const double rows[3][4] = {{c, 0, s, x}, {0, 1, 0, up}, {-s, 0, c, z}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) m[r][k] = rows[r][k];
}

}  // namespace steamglue
