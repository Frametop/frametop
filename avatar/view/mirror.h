// The mirror ft-avatars stands in the room, the sums: a sheet of glass on the floor in front of
// its wearer, and what each eye sees in it. A mirror shows the room turned over through its
// plane, as if the glass were a window onto that room: an eye's view is the turned-over room
// (viewmath::Mirror) seen through the glass's rectangle from where the eye really is. Each eye
// gets its own, so what is in the mirror has depth behind the glass.
//
// Header only, no GPU or SteamVR calls (the tests use it).
#pragma once

#include "viewmath.h"

#include <cmath>

namespace mirror {

using avatar::Vec;

struct Glass {
    Vec centre, right, up, out;   // `out` is the way its face looks; the three are square, a right hand's
    double width = 1.1, height = 2.0;   // metres
};

constexpr double kAway = 1.4;       // how far ahead of its wearer a glass stands
constexpr double kTooNear = 0.05;   // an eye this near the glass's plane, or behind it, sees nothing in it
constexpr double kDeep = 12;        // how far behind the glass is drawn, metres

// A glass standing on the floor (at height `floor`) kAway ahead of someone whose eyes are at
// `eyes` and who looks along `ahead` (level is taken of it; straight up or down counts as -z),
// facing them.
inline Glass Stand(const Vec &eyes, const Vec &ahead, double floor) {
    Vec level{ahead.x, 0, ahead.z};
    level = level.norm() > 1e-3 ? level.unit() : Vec{0, 0, -1};
    Glass g;
    g.up = {0, 1, 0};
    g.out = level * -1;
    g.right = g.up.cross(g.out);
    g.centre = Vec{eyes.x, floor + g.height / 2, eyes.z} + level * kAway;
    return g;
}

// How wide an eye's picture of the glass is when it is `h` pixels tall (even: two go side by side).
inline int EyeWidth(const Glass &g, int h) { return int(std::lround(h * g.width / g.height / 2)) * 2; }

// Where the glass is, as an overlay's place is told: row-major 3x4, +x right, +y up, +z out of
// its face.
inline void Pose(const Glass &g, float out[3][4]) {
    const Vec *axes[3] = {&g.right, &g.up, &g.out};
    for (int c = 0; c < 3; ++c) out[0][c] = float(axes[c]->x), out[1][c] = float(axes[c]->y), out[2][c] = float(axes[c]->z);
    out[0][3] = float(g.centre.x), out[1][3] = float(g.centre.y), out[2][3] = float(g.centre.z);
}

// The room seen from `eye` through the glass's rectangle, as through a window: the room -> clip
// space, the rectangle filling the viewport with its top in the first row (as SteamVR reads a
// buffer), depth from `nearM` to `farM` metres from the eye, measured square to the glass. What
// is drawn may be on either side of the glass. The top going first turns the picture over: what
// it shows winds the other way from the room (bodygpu::View::mirrored). False when the eye is
// at the glass's plane or behind it (kTooNear).
inline bool Window(const Glass &g, const Vec &eye, double nearM, double farM, viewmath::Mat4 &clip) {
    const Vec e = eye - g.centre;
    const double ex = e.dot(g.right), ey = e.dot(g.up), ez = e.dot(g.out);
    if (!(ez > kTooNear) || !(nearM > 0 && farM > nearM)) return false;
    // A point at (x, y, z) in the glass's frame, seen from the eye, V = point - eye, meets the
    // glass at (ez Vx - ex Vz, ez Vy - ey Vz) / -Vz: the clip's x and y over its w, with the
    // half sizes divided out. Its z runs from -Vz = nearM to farM.
    const double hw = g.width / 2, hh = g.height / 2;
    const double A = (farM + nearM) / (farM - nearM), B = -2 * farM * nearM / (farM - nearM);
    const double K[4][4] = {{ez / hw, 0, -ex / hw, 0}, {0, -ez / hh, ey / hh, 0}, {0, 0, -A, B}, {0, 0, -1, 0}};
    // the room -> the glass's frame, the eye taken off: rows right, up, out; then K
    const Vec *axes[3] = {&g.right, &g.up, &g.out};
    double L[4][4] = {};
    for (int r = 0; r < 3; ++r) {
        L[r][0] = axes[r]->x, L[r][1] = axes[r]->y, L[r][2] = axes[r]->z;
        L[r][3] = -axes[r]->dot(eye);
    }
    L[3][3] = 1;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += K[r][k] * L[k][c];
            clip.m[c * 4 + r] = float(s);
        }
    return true;
}

// What an eye at `eye` sees in the glass, a mirror: the room turned over through the glass's
// plane, seen through the glass as a window, from the glass's plane to kDeep behind it, so
// nothing on the eye's side of the glass is drawn. The view turns the room over twice (the
// mirror, and the top going first), so what it shows winds the way the room does. False when
// the eye sees nothing in it (kTooNear).
inline bool EyeView(const Glass &g, const Vec &eye, viewmath::Mat4 &clip) {
    const double ez = (eye - g.centre).dot(g.out);
    viewmath::Mat4 window;
    if (!(ez > kTooNear) || !Window(g, eye, ez, ez + kDeep, window)) return false;
    clip = viewmath::Mul(window, viewmath::Mirror(g.centre, g.out));
    return true;
}

// Whether the glass is within `within` radians of where someone at `eyes` looks (`ahead`), both
// taken level: looking down at your own feet in it counts. Yes for someone right at it, or
// looking straight up or down.
inline bool InSight(const Glass &g, const Vec &eyes, const Vec &ahead, double within) {
    const Vec to{g.centre.x - eyes.x, 0, g.centre.z - eyes.z}, level{ahead.x, 0, ahead.z};
    if (to.norm() < 0.3 || level.norm() < 1e-3) return true;
    return to.unit().dot(level.unit()) > std::cos(within);
}

// Toward the light a body in front of the glass is lit by: over the glass, a little to one side
// (the side of a body that faces the glass is the one the glass shows).
inline Vec Light(const Glass &g) { return (g.out * -0.55 + g.up * 0.7 + g.right * 0.3).unit(); }

}  // namespace mirror
