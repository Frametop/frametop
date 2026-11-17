// Where a follower of Steam's (a popup: the footer, a tooltip, the keyboard; steamwin.h) goes: where
// Steam puts its own overlay for it, moved with the panel it sits on when that panel is a tile of
// ours. Steam places its popups by its own panels (its dashboard window's, its menu's), and those
// are where Steam left them, not where our tiles are.
//
// A Frame is an overlay's pixels in the room: room = o + px·ax + py·ay + pz·az, with px, py the
// window's pixels (origin bottom left, y up, as OpenVR's overlay mouse coordinates are) and az out
// of its front, as long as ax. vrsteamfollow.cpp reads each one with
// GetTransformForOverlayCoordinates at three points (FromPoints): that works on Steam's overlays
// too, whatever kind of transform they have. Then a follower's frame on our tile is
// ourTile · inverse(Steam's tile) · Steam's follower (Remap), and ToOverlay gives the transform that
// draws it there at width 1 m (its size in the matrix, as Steam's own are).
//
// Pure math, no OpenVR: screens/tests/steamplace-test.cpp.
#pragma once

#include <cmath>
#include <vector>

namespace steamplace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }

struct Frame {
    V3 o, ax, ay, az;
};

inline V3 Apply(const Frame &f, V3 p) { return f.o + f.ax * p.x + f.ay * p.y + f.az * p.z; }

// From the room points of a w x h window's pixels (w/2, h/2), (w, h/2) and (w/2, h). False when
// they don't make a panel (a 0 size, the points on a line).
inline bool FromPoints(V3 centre, V3 right, V3 top, double w, double h, Frame *f) {
    if (w <= 0 || h <= 0) return false;
    const V3 ax = (right - centre) * (2 / w), ay = (top - centre) * (2 / h);
    const V3 n = Cross(ax, ay);
    const double ln = Len(n), lx = Len(ax);
    if (ln < 1e-12 || lx < 1e-12) return false;
    f->ax = ax, f->ay = ay, f->az = n * (lx / ln);
    f->o = centre - ax * (w / 2) - ay * (h / 2);
    return true;
}

// a after b: a's pixels of b's.
inline Frame Mul(const Frame &a, const Frame &b) {
    auto lin = [&](V3 v) { return a.ax * v.x + a.ay * v.y + a.az * v.z; };
    return {Apply(a, b.o), lin(b.ax), lin(b.ay), lin(b.az)};
}

inline bool Inverse(const Frame &f, Frame *out) {
    const double det = Dot(f.ax, Cross(f.ay, f.az));
    if (std::fabs(det) < 1e-18) return false;
    // the rows of the inverse of [ax ay az]
    const V3 r0 = Cross(f.ay, f.az) * (1 / det), r1 = Cross(f.az, f.ax) * (1 / det), r2 = Cross(f.ax, f.ay) * (1 / det);
    out->ax = {r0.x, r1.x, r2.x}, out->ay = {r0.y, r1.y, r2.y}, out->az = {r0.z, r1.z, r2.z};
    const V3 t = {Dot(r0, f.o), Dot(r1, f.o), Dot(r2, f.o)};
    out->o = t * -1;
    return true;
}

// A follower's frame moved as `from` (Steam's panel it sits on) is to `to` (our tile of it).
inline bool Remap(const Frame &to, const Frame &from, const Frame &follower, Frame *out) {
    Frame inv;
    if (!Inverse(from, &inv)) return false;
    *out = Mul(to, Mul(inv, follower));
    return true;
}

// The overlay transform (3x4, row major) that draws a w x h window over `f` at width 1 m.
inline void ToOverlay(const Frame &f, double w, double h, float m[3][4]) {
    const V3 c = Apply(f, {w / 2, h / 2, 0}), x = f.ax * w, y = f.ay * w, z = f.az * w;
    const V3 cols[4] = {x, y, z, c};
    for (int col = 0; col < 4; ++col) {
        m[0][col] = float(cols[col].x), m[1][col] = float(cols[col].y), m[2][col] = float(cols[col].z);
    }
}

// A panel a follower may sit on: its frame and size, pixels.
struct Panel {
    Frame f;
    double w = 0, h = 0;
};
constexpr double kNearM = 0.25;   // farther from all of them than this: it sits on none

// How far `p` is from a panel (0 on it), metres.
inline double Distance(const Panel &s, V3 p) {
    Frame inv;
    if (!Inverse(s.f, &inv)) return 1e9;
    const V3 q = Apply(inv, p);
    const double dx = q.x < 0 ? -q.x : q.x > s.w ? q.x - s.w : 0;
    const double dy = q.y < 0 ? -q.y : q.y > s.h ? q.y - s.h : 0;
    return Len(s.f.ax) * std::sqrt(dx * dx + dy * dy + q.z * q.z);
}

// Which one a follower centred at `p` sits on: the nearest within kNearM, or -1.
inline int SitsOn(V3 p, const std::vector<Panel> &panels) {
    int best = -1;
    double bestD = kNearM;
    for (size_t i = 0; i < panels.size(); ++i) {
        const double d = Distance(panels[i], p);
        if (d <= bestD) bestD = d, best = int(i);
    }
    return best;
}

}  // namespace steamplace
