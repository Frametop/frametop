// The tiles' wall with one curve for the centre display and another beside it (DASH_TILE_CENTRE_CURVE_M;
// vrtiles.cpp places the tiles through this).
//
// User, 2026-11-23: "set a different curve for the center justified special display ... and tiles
// above it follow the same curve, but then on the sides of the center display it can use a different
// curve so we can have them either be the same for a smooth curve all around, or a curve of the main
// display that is less aggressive, then more aggressive on displays on the side so they face the user
// better."
//
// The layout (tiling.h, tilefit.h) knows one sphere, the centre's, `rc` its radius. Here its
// azimuth is how far along the wall a tile is, and only where that is in space changes: the centre's
// column (Sides::a0..a1) stays on the sphere, and past each end of it the wall goes on round a
// circle of radius `rs` which leaves the end the way the sphere did (no corner at the join: that
// circle's centre is on the line through the end). As long along the wall as before, so every gap is kept.
//
// Upward the side wall's slope follows its own curve (a tighter one leans over less, or its higher
// rows would be squeezed into each other round you), as long up as the sphere is, so the desk line
// and the rows meet the centre's. With rs == rc all of it is the sphere again (Place is tiling::Pose).
// Pure: screens/tests/tilecurve-test.cpp.
#pragma once

#include "tiling.h"

#include <algorithm>
#include <cmath>

namespace tilecurve {

using tiling::Deg;
using tiling::Rad;

constexpr double kMaxHeadingDeg = 110;   // the wall turns no further round than this: it doesn't close behind you
constexpr double kMaxTileDeg = 150;      // ...and one flat panel takes no more of a turn

struct Sides {
    double rc = 0, rs = 0;   // the centre's radius and the sides', metres (rs 0: one curve, the sphere)
    double a0 = 0, a1 = 0;   // the centre's section round the sphere, degrees
    bool On() const { return rs > 0 && rc > 0; }
    int SideOf(double az) const { return !On() || (az >= a0 && az <= a1) ? 0 : az < a0 ? -1 : 1; }
    double End(int side) const { return side < 0 ? a0 : a1; }
    double Heading(double az) const {   // which way along the wall at `az` faces away from its middle
        const int side = SideOf(az);
        return side ? End(side) + (az - End(side)) * rc / rs : az;
    }
};

// The sides of layout `l` round its `centre` display (its whole column, or itself with no columns;
// a point straight ahead with none), curved `rs`. Never so tight that the strip turns past kMaxHeadingDeg
// (then as tight as that allows, but not flatter than the centre for it).
inline Sides For(const tiling::Layout &l, int centre, const tiling::Params &p, double rs) {
    Sides s;
    s.rc = p.radius;
    if (!(rs > 0) || !(p.radius > 0) || std::fabs(rs - p.radius) < 1e-6) return s;
    const tiling::Placed *c = nullptr;
    for (const tiling::Placed &t : l.tiles)
        if (t.id == centre) c = &t;
    if (c) {
        s.a0 = c->az0, s.a1 = c->az1;
        for (const tiling::Placed &t : l.tiles)
            if (!p.fit && t.col == c->col) s.a0 = std::min(s.a0, t.az0), s.a1 = std::max(s.a1, t.az1);
        s.a0 -= p.gapDeg / 2, s.a1 += p.gapDeg / 2;
    }
    double need = rs;
    const auto cap = [&](double along, double end) {   // (a side `along` degrees long, its join `end` out from the middle)
        if (along > 0) need = std::max(need, end < kMaxHeadingDeg ? along * s.rc / (kMaxHeadingDeg - end) : s.rc);
    };
    for (const tiling::Placed &t : l.tiles) cap(t.az1 - s.a1, s.a1), cap(s.a0 - t.az0, -s.a0);
    s.rs = std::min(need, std::max(rs, s.rc));
    return s;
}

namespace detail {
// The side wall's profile at `elDeg` (the sphere's elevation of the same length up): its height, how
// near its circle's axis it has come, and its lean. `lean` / rc of the sphere's: the tighter of the two curves.
inline double Lean(const Sides &s) { return std::min(s.rs, s.rc); }
inline double Height(const Sides &s, double elDeg) {
    const double e = Rad(elDeg), k = Lean(s) / s.rc;
    const int n = 32;   // (Simpson's rule over the profile's slope: nothing but the sine at k == 1)
    double sum = 0;
    for (int i = 0; i <= n; ++i) {
        const double x = e * i / n;
        sum += (i == 0 || i == n ? 1 : i % 2 ? 4 : 2) * std::sqrt(std::max(0.0, 1 - k * k * std::sin(x) * std::sin(x)));
    }
    return s.rc * sum * e / (3 * n);
}
// The side circle's centre, at the height of the sphere's.
inline void Axis(const Sides &s, int side, double *x, double *z) {
    const double a = Rad(s.End(side)), d = s.rc - s.rs;
    *x = d * std::sin(a), *z = -d * std::cos(a);
}
}  // namespace detail

// The pose of a tile on the wall, its middle (group frame, as tiling::Pose's; `dist` from the sphere's
// centre where the layout put it: Placed::distM, or the radius for the centre curved round the arc).
inline void Place(const Sides &s, const tiling::Placed &p, double dist, double m[3][4]) {
    const int side = s.SideOf(p.azDeg);
    if (!side) return tiling::Pose(p.azDeg, p.elDeg, dist, m);
    const double k = detail::Lean(s) / s.rc, pitch = Deg(std::asin(k * std::sin(Rad(p.elDeg))));
    // a flat panel: its nearest edge just as wide as its share of the side's turn, so corners meet corners
    const double turn = std::clamp((p.az1 - p.az0) * s.rc / s.rs, 1e-6, kMaxTileDeg);
    const double out = p.widthM / 2 / std::tan(Rad(turn) / 2) + p.heightM / 2 * std::fabs(std::sin(Rad(pitch)));
    double x, z;
    detail::Axis(s, side, &x, &z);
    const double heading = s.Heading(p.azDeg), h = Rad(heading);
    tiling::Pose(heading, pitch, 1, m);   // (which way it faces; its place next)
    m[0][3] = x + out * std::sin(h);
    m[1][3] = detail::Height(s, p.elDeg) - (s.rc - dist) * std::sin(Rad(pitch));
    m[2][3] = z - out * std::cos(h);
}

// A point (group frame) as the layout's azimuth and elevation: Place the other way round.
inline void Direction(const Sides &s, const double pt[3], double *azDeg, double *elDeg) {
    tiling::Direction(pt, azDeg, elDeg);
    if (!s.On()) return;
    const double az = tiling::WrapNear(*azDeg, (s.a0 + s.a1) / 2);
    const int side = s.SideOf(az);
    *azDeg = az;
    if (!side) return;
    double x, z;
    detail::Axis(s, side, &x, &z);
    const double dx = pt[0] - x, dz = pt[2] - z;
    const double heading = tiling::WrapNear(Deg(std::atan2(dx, -dz)), s.End(side) + side * 90);
    *azDeg = s.End(side) + (heading - s.End(side)) * s.rs / s.rc;
    // the profile seen from its own centre (a point nearer than the wall counts as the place behind it)
    const double lean = detail::Lean(s), up = std::atan2(pt[1], std::max(1e-6, std::hypot(dx, dz) - (s.rs - lean)));
    double lo = -89, hi = 89;
    for (int i = 0; i < 40; ++i) {
        const double e = (lo + hi) / 2;
        (std::atan2(detail::Height(s, e), lean * std::cos(Rad(e))) < up ? lo : hi) = e;
    }
    *elDeg = (lo + hi) / 2;
}

}  // namespace tilecurve
