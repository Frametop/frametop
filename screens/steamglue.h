// Steam's menu glued to Steam's page (user, 2026-11-17: the menu and its icon strip glued to the
// page "almost as if they were one display", without the black border round it). Steam's menu
// window is 450 x 1200 pixels, but it draws only the strip at its right edge (54 CSS pixels wide,
// from the middle 60% of its height) and its labels to the left of it while the laser is on it
// (the strip grows leftwards, to about 240 CSS pixels). The rest of the window is black.
//
// So the menu's panel shows only the part Steam draws (its page tells us: steamcdp's AskDrawn),
// flush left of the page's panel at the page's density, its middle level with the page's (the
// crop sliding there: Slide; shut at once when the laser leaves, not when Steam shuts). It moves
// with the page, the tiles count it in the page's width (the width it has with nobody on it, so
// its labels opening don't move the tiles), and it has no controls of its own.
//
// Pure math, no OpenVR: screens/tests/steamglue-test.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace steamglue {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;   // window pixels, from the top left
};
inline bool operator==(const Rect &a, const Rect &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
inline bool operator!=(const Rect &a, const Rect &b) { return !(a == b); }

// With the laser gone a while, whether what Steam's menu draws now is its new rest (what the tiles
// count): not while it's still what it drew, open, as the laser left. Steam shuts it in its own
// time, mostly 0.8 s but sometimes more (2026-11-18: taken as the rest after 1.1 s, the tiles were
// retiled to its open width and it slid open again).
inline bool NewRest(const Rect &drawn, const Rect &rest, const Rect &atLeave) {
    return drawn != rest && !(rest.w > 0 && atLeave.w > rest.w && drawn == atLeave);
}

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

// The crop eased from one rect to the next (user, 2026-11-18: make the menu "expand and shrink
// more responsively", not "on a single frame"): Steam snaps its labels open and shut, about 0.5 s
// after the laser comes and 0.8 s after it goes; the crop slides there in kSlideMs instead, from
// wherever it is (a new target mid-slide turns it round smoothly). The first rect is taken as is.
constexpr int64_t kSlideMs = 180;
class Slide {
public:
    void To(const Rect &target, int64_t nowMs) {
        const double to[4] = {double(target.x), double(target.y), double(target.x + target.w), double(target.y + target.h)};
        if (set_ && Same(to, to_)) return;
        double from[4];
        if (set_) Edges(nowMs, from);
        for (int i = 0; i < 4; ++i) from_[i] = set_ ? from[i] : to[i], to_[i] = to[i];
        at_ = set_ ? nowMs : nowMs - kSlideMs;
        set_ = true;
    }
    bool Set() const { return set_; }
    Rect At(int64_t nowMs) const {
        double e[4];
        Edges(nowMs, e);
        const int l = int(std::lround(e[0])), t = int(std::lround(e[1]));
        return {l, t, int(std::lround(e[2])) - l, int(std::lround(e[3])) - t};
    }

private:
    double from_[4] = {}, to_[4] = {};   // left, top, right, bottom
    int64_t at_ = 0;
    bool set_ = false;

    static bool Same(const double a[4], const double b[4]) {
        for (int i = 0; i < 4; ++i)
            if (a[i] != b[i]) return false;
        return true;
    }
    void Edges(int64_t nowMs, double out[4]) const {
        const double p = std::clamp(double(nowMs - at_) / kSlideMs, 0.0, 1.0), k = 1 - (1 - p) * (1 - p) * (1 - p);
        for (int i = 0; i < 4; ++i) out[i] = from_[i] + (to_[i] - from_[i]) * k;   // (eased out: quick, then settling)
    }
};

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
