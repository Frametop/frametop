// The quick reset's rules (ft-layout reset; vrrecentre.cpp; user, 2026-11-25: "when you reset layouts, it
// actually resets the position of all of your displays. We just want reset layout to recenter things based
// off of where you're looking"). Everything in the room moves in one piece, by one level step: turned about
// the way up and moved, nothing of it nearer to or further from the rest. The step takes where your head was
// when the lead (your main screen) was last placed, facing so that the lead is where the layout has it round
// you, to where your head is now: so the lead is straight ahead again (or where its layout has it), as far
// and as high as you had it.
//   M is any type with a float m[3][4] (vr::HmdMatrix34_t in vrrecentre.cpp). Header only, no OpenVR, so
// its test runs anywhere (screens/tests/recentre-test.cpp).
#pragma once

#include <cmath>
#include <vector>

#include "posemath.h"

namespace recentre {

constexpr double kFlatM = 0.2;      // nearer than this on the floor, which way it is from you means nothing
constexpr double kGuessM = 1.5;     // not known where you stood: this far in front of the lead
constexpr double kAgree = 0.3;      // panels all round you have no middle (their mean direction is shorter than this)

// Yaw as ft-screens' "head" has it (radians, left positive, 0 facing -Z): of a pose's back (its +Z), and
// the one you face to look along (dx, dz).
template <class M>
double YawOf(const M &m) { return std::atan2(double(m.m[0][2]), double(m.m[2][2])); }
inline double YawAlong(double dx, double dz) { return std::atan2(-dx, -dz); }

// A level frame at a point, facing `yaw`.
template <class M>
M Level(double x, double y, double z, double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    M m = posemath::Translation<M>(x, y, z);
    m.m[0][0] = float(c), m.m[0][2] = float(s), m.m[2][0] = float(-s), m.m[2][2] = float(c);
    return m;
}
template <class M>
M LevelHead(const M &head) { return Level<M>(head.m[0][3], head.m[1][3], head.m[2][3], YawOf(head)); }

// Where your head was when a panel was last placed or moved (no head pose then: not known).
struct Stood {
    bool known = false;
    float at[3] = {};
};
template <class M>
Stood StoodAt(const M &head) { return {true, {head.m[0][3], head.m[1][3], head.m[2][3]}}; }
// The room moved by `j`, and where you stood with it.
template <class M>
Stood Moved(const M &j, const Stood &s) {
    Stood out = StoodAt(posemath::Mul(j, posemath::Translation<M>(s.at[0], s.at[1], s.at[2])));
    out.known = s.known;
    return out;
}

// The panel the rest go by.
template <class M>
struct Lead {
    M pose{};
    Stood from;             // where your head was when it was last placed
    bool planned = false;   // the layout says where it is round you: `bearing` (radians, left positive)
    double bearing = 0;
};

// Where you stood, as a level frame: `centres` are the panels you see (the lead too), whose middle is
// straight ahead of you when no layout says where the lead is.
template <class M>
M From(const Lead<M> &lead, const M &head, const std::vector<M> &centres) {
    const double own = YawOf(lead.pose);   // (facing the lead's front)
    double p[3] = {lead.pose.m[0][3] + std::sin(own) * kGuessM, head.m[1][3], lead.pose.m[2][3] + std::cos(own) * kGuessM};
    if (lead.from.known)
        for (int k = 0; k < 3; ++k) p[k] = lead.from.at[k];
    double yaw = own;
    const double dx = lead.pose.m[0][3] - p[0], dz = lead.pose.m[2][3] - p[2];
    if (lead.planned) {
        yaw = (std::hypot(dx, dz) < kFlatM ? own : YawAlong(dx, dz)) - lead.bearing;
    } else {
        double x = 0, z = 0;
        int n = 0;
        for (const M &c : centres) {
            const double cx = c.m[0][3] - p[0], cz = c.m[2][3] - p[2], d = std::hypot(cx, cz);
            if (d >= kFlatM) x += cx / d, z += cz / d, ++n;
        }
        if (n && std::hypot(x, z) >= kAgree * n) yaw = YawAlong(x, z);
    }
    return Level<M>(p[0], p[1], p[2], yaw);
}

// The step everything in the room makes.
template <class M>
M Step(const Lead<M> &lead, const M &head, const std::vector<M> &centres) {
    return posemath::Mul(LevelHead(head), posemath::Inverse(From(lead, head, centres)));
}

}  // namespace recentre
