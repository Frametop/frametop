// The tiles' wall with one curve for the centre display, another beside it (DASH_TILE_CENTRE_CURVE_M)
// and another for the rows over and under it (DASH_TILE_HEIGHT_CURVE_M); vrtiles.cpp places the tiles through this.
//
// User, 2026-11-23: "set a different curve for the center justified special display ... and tiles
// above it follow the same curve, but then on the sides of the center display it can use a different
// curve so we can have them either be the same for a smooth curve all around, or a curve of the main
// display that is less aggressive, then more aggressive on displays on the side so they face the user
// better."
//
// The layout (tiling.h, tilefit.h) knows one sphere, the centre's, `rc` its radius. Here its
// azimuth is how far along the wall a tile is, and only where that is in space changes: the centre's
// column (Wall::a0..a1) stays on the sphere, and past each end of it the wall goes on round a
// circle of radius `rs` which leaves the end the way the sphere did (no corner at the join: that
// circle's centre is on the line through the end). As long along the wall as before, so every gap is kept.
//
// Upward there is one slope too, the sphere's, unless the rows over and under have a curve of their
// own (DASH_TILE_HEIGHT_CURVE_M; user, 2026-11-23: "curve options similar for height just like we had
// for left and right"): past the centre display's top and bottom edges the wall leans toward you on
// a circle of radius `rh`, leaving the edge at the slope it had. Rows that lean in have less room round
// you, so the tiles there come nearer and shrink as much (Fit: seen from the wall's centre about the
// same size; never under kMinFit, where it stops leaning in; a flatter curve never grows one).
// A tighter side wall leans over less than the sphere by itself (its rows would be squeezed).
// Everything the same is the sphere again (Place is tiling::Pose).
// Pure: screens/tests/tilecurve-test.cpp.
#pragma once

#include "tiling.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tilecurve {

using tiling::Deg;
using tiling::Rad;

constexpr double kMaxHeadingDeg = 110;   // the wall turns no further round than this: it doesn't close behind you
constexpr double kMaxTileDeg = 150;      // ...and one flat panel takes no more of a turn

constexpr double kMinFit = 0.75;         // a leaning row's tiles no smaller than this
constexpr double kMaxLeanDeg = 80;       // ...and the wall no nearer lying over you

// The wall's slope: a circle of radius `r` round the axis at your eyes' height, its length up the
// sphere's (`rc`) at the same elevation; past a band's edges curved on its own, less long as it leans in.
struct Point {
    double rho = 0;   // how far from the axis
    double y = 0;     // up from the wall's centre
    double fit = 1;   // how big a tile there is, of its size
};
class Profile {
  public:
    // From `e0` to `e1` degrees as the sphere is, then curved `rh` up and down (0: as the sphere all the way).
    void Build(double rc, double r, double e0, double e1, double rh) {
        at_[kMid] = {r, 0, 1};
        for (int way : {1, -1}) {
            for (int i = 1; i <= kMid; ++i) {   // (the sphere's, leaning over as much less as it's tighter)
                const Point &p = at_[size_t(kMid + way * (i - 1))];
                const double rho = r * std::cos(Rad(i * kStepDeg)), ds = rc * Rad(kStepDeg);
                at_[size_t(kMid + way * i)] = {rho, p.y + way * Up(ds, rho - p.rho), 1};
            }
        }
        if (!(rh > 0)) return;
        // (the band's edges first: neither run may start from what the other has bent)
        const int i1 = Edge(e1, true), i0 = std::min(i1, Edge(e0, false));
        Bend(rc, r, rh, i1, 1), Bend(rc, r, rh, i0, -1);
    }
    Point At(double elDeg) const {
        const double t = std::clamp(elDeg / kStepDeg + kMid, 0.0, 2 * kMid - 1e-9);
        const int i = int(t);
        const double f = t - i;
        const Point &a = at_[size_t(i)], &b = at_[size_t(i) + 1];
        return {a.rho + (b.rho - a.rho) * f, a.y + (b.y - a.y) * f, a.fit + (b.fit - a.fit) * f};
    }

  private:
    static constexpr double kStepDeg = 0.25;
    static constexpr int kMid = 340;   // (85 degrees each way: past tiling::kMaxTopDeg)
    static double Up(double ds, double in) { return std::sqrt(std::max(0.0, ds * ds - in * in)); }
    static int Edge(double el, bool up) {
        const double t = el / kStepDeg + kMid;
        return int(std::clamp(up ? std::ceil(t) : std::floor(t), 0.0, 2.0 * kMid));
    }
    // From node `from` on, up (`way` 1) or down (-1): a circle of radius `rh`, at the slope it has there.
    void Bend(double rc, double r, double rh, int from, int way) {
        double lean = std::asin(r / rc * std::sin(way * Rad((from - kMid) * kStepDeg)));   // (toward you, going that way)
        Point p = at_[size_t(from)];
        for (int i = from + way; i >= 0 && i <= 2 * kMid; i += way) {
            const double round = r * std::cos(Rad((i - kMid) * kStepDeg)), ds = rc * Rad(kStepDeg) * p.fit;
            const double mid = std::min(Rad(kMaxLeanDeg), lean + ds / rh / 2);
            lean = std::min(Rad(kMaxLeanDeg), lean + ds / rh);
            const double rho = std::max(p.rho - std::sin(mid) * ds, kMinFit * round);
            p = {rho, p.y + way * Up(ds, rho - p.rho), std::clamp(rho / round, kMinFit, 1.0)};
            at_[size_t(i)] = p;
        }
    }
    std::array<Point, 2 * kMid + 1> at_{};
};

struct Wall {
    double rc = 0;                // the centre's radius, metres
    double rs = 0;                // the sides' (the centre's: one curve)
    double a0 = 0, a1 = 0;        // the centre's section round the sphere, degrees
    double rh = 0;                // the rows' over and under (0: as the sphere)
    double e0 = 0, e1 = 0;        // ...past these, the centre display's edges
    Profile centre, side;         // (the sides': rho from where its circle of the tighter of the two radii is centred)
    bool Sides() const { return rs > 0 && rs != rc; }
    bool On() const { return Sides() || rh > 0; }
    int SideOf(double az) const { return !Sides() || (az >= a0 && az <= a1) ? 0 : az < a0 ? -1 : 1; }
    double End(int s) const { return s < 0 ? a0 : a1; }
    double Heading(double az) const {   // which way along the wall at `az` faces away from its middle
        const int s = SideOf(az);
        return s ? End(s) + (az - End(s)) * rc / rs : az;
    }
    double Radius(int s) const { return s ? rs : rc; }
    const Profile &Slope(int s) const { return s ? side : centre; }
    // The slope's own middle this far out from the circle's: the part of the radius it doesn't lean over.
    double Straight(int s) const { return s ? rs - std::min(rs, rc) : 0; }
};

// The wall of layout `l` round its `centre` display (its whole column, or itself with no columns;
// a point straight ahead with none): its sides curved `rs`, the rows over and under curved `rh`
// (either 0: as the centre). The sides never so tight that the strip turns past kMaxHeadingDeg
// (then as tight as that allows, but not flatter than the centre for it).
inline Wall For(const tiling::Layout &l, int centre, const tiling::Params &p, double rs, double rh) {
    Wall w;
    w.rc = p.radius;
    if (!(p.radius > 0)) return w;
    const auto own = [&](double r) { return r > 0 && std::fabs(r - p.radius) >= 1e-6; };
    if (!own(rs) && !own(rh)) return w;
    const tiling::Placed *c = nullptr;
    for (const tiling::Placed &t : l.tiles)
        if (t.id == centre) c = &t;
    if (c) {
        w.a0 = c->az0, w.a1 = c->az1;
        for (const tiling::Placed &t : l.tiles)
            if (!p.fit && t.col == c->col) w.a0 = std::min(w.a0, t.az0), w.a1 = std::max(w.a1, t.az1);
        w.a0 -= p.gapDeg / 2, w.a1 += p.gapDeg / 2;
        w.e0 = c->el0 - p.gapDeg / 2, w.e1 = c->el1 + p.gapDeg / 2;
    }
    w.rs = w.rc;
    if (own(rs)) {
        double need = rs;
        const auto cap = [&](double along, double end) {   // (a side `along` degrees long, its join `end` out from the middle)
            if (along > 0) need = std::max(need, end < kMaxHeadingDeg ? along * w.rc / (kMaxHeadingDeg - end) : w.rc);
        };
        for (const tiling::Placed &t : l.tiles) cap(t.az1 - w.a1, w.a1), cap(w.a0 - t.az0, -w.a0);
        w.rs = std::min(need, std::max(rs, w.rc));
    }
    w.rh = own(rh) ? rh : 0;
    if (!w.On()) return w;
    w.centre.Build(w.rc, w.rc, w.e0, w.e1, w.rh);
    w.side.Build(w.rc, std::min(w.rs, w.rc), w.e0, w.e1, w.rh);
    return w;
}

namespace detail {
// The circle's centre on side `s`, at the height of the sphere's.
inline void Axis(const Wall &w, int s, double *x, double *z) {
    const double a = Rad(w.End(s)), d = s ? w.rc - w.rs : 0;
    *x = d * std::sin(a), *z = -d * std::cos(a);
}
// On the sphere as it was: in the centre's section, in its rows.
inline bool Plain(const Wall &w, const tiling::Placed &p) {
    return !w.SideOf(p.azDeg) && (!(w.rh > 0) || (p.el0 >= w.e0 && p.el1 <= w.e1));
}
}  // namespace detail

// How big a tile is on the wall, of the size the layout gave it: less in a row that leans in.
inline double Fit(const Wall &w, const tiling::Placed &p) {
    if (detail::Plain(w, p) || !(w.rh > 0)) return 1;
    const Profile &f = w.Slope(w.SideOf(p.azDeg));
    const Point b = f.At(p.el0), t = f.At(p.el1);
    const double on = 2 * w.rc * std::sin(Rad(p.el1 - p.el0) / 2);   // (the sphere's own chord, where it just fits)
    return on > 0 ? std::min(f.At(p.elDeg).fit, std::hypot(t.rho - b.rho, t.y - b.y) / on) : 1;
}

// The pose of a tile on the wall, its middle (group frame, as tiling::Pose's; `dist` from the sphere's
// centre where the layout put it: Placed::distM, or the radius for the centre curved round the arc).
// Its size there is Fit's.
inline void Place(const Wall &w, const tiling::Placed &p, double dist, double m[3][4]) {
    if (detail::Plain(w, p)) return tiling::Pose(p.azDeg, p.elDeg, dist, m);
    const int s = w.SideOf(p.azDeg);
    const Profile &f = w.Slope(s);
    const Point b = f.At(p.el0), t = f.At(p.el1);
    const double pitch = std::atan2(b.rho - t.rho, t.y - b.y), fit = Fit(w, p);   // (flat from its bottom on the slope to its top)
    // round you: its nearest edge just as wide as its share of the turn, so corners meet corners
    const double turn = std::clamp((p.az1 - p.az0) * w.rc / w.Radius(s), 1e-6, kMaxTileDeg);
    const double out = p.widthM * fit / 2 / std::tan(Rad(turn) / 2) + p.heightM * fit / 2 * std::fabs(std::sin(pitch));
    const double sag = w.rc * std::cos(Rad(p.el1 - p.el0) / 2) - dist;   // (nearer than its edges' chord, by its width)
    double x, z;
    detail::Axis(w, s, &x, &z);
    const double heading = w.Heading(p.azDeg), h = Rad(heading);
    tiling::Pose(heading, Deg(pitch), 1, m);   // (which way it faces; its place next)
    m[0][3] = x + out * std::sin(h);
    m[1][3] = (b.y + t.y) / 2 - sag * std::sin(pitch);
    m[2][3] = z - out * std::cos(h);
}

// A point (group frame) as the layout's azimuth and elevation: Place the other way round.
inline void Direction(const Wall &w, const double pt[3], double *azDeg, double *elDeg) {
    tiling::Direction(pt, azDeg, elDeg);
    if (!w.On()) return;
    const double az = tiling::WrapNear(*azDeg, (w.a0 + w.a1) / 2);
    const int s = w.SideOf(az);
    *azDeg = az;
    if (!s && !(w.rh > 0)) return;
    double x, z;
    detail::Axis(w, s, &x, &z);
    const double dx = pt[0] - x, dz = pt[2] - z;
    if (s) {
        const double heading = tiling::WrapNear(Deg(std::atan2(dx, -dz)), w.End(s) + s * 90);
        *azDeg = w.End(s) + (heading - w.End(s)) * w.rs / w.rc;
    }
    // the slope seen from its own middle (a point nearer than the wall counts as the place behind it)
    const Profile &f = w.Slope(s);
    const double up = std::atan2(pt[1], std::max(1e-6, std::hypot(dx, dz) - w.Straight(s)));
    double lo = -85, hi = 85;
    for (int i = 0; i < 40; ++i) {
        const double e = (lo + hi) / 2;
        const Point q = f.At(e);
        (std::atan2(q.y, q.rho) < up ? lo : hi) = e;
    }
    *elDeg = (lo + hi) / 2;
}

}  // namespace tilecurve
