// Whether the dashboard's tiles stay where they were put by hand (Move All Windows; vrlayoutgrab.cpp,
// vrtiles.cpp) as the dashboard opens, instead of coming round you at Steam's bar. They stay while you're
// still by the spot you put them from and still face their way: the Steam menu opened there doesn't take
// back the move. Walk off or turn round, and the next opening brings them to you as ever. A layout reset
// does at once. Header only, no OpenVR (screens/tests/tilekeep-test.cpp).
#pragma once

#include "posemath.h"

#include <cmath>
#include <cstdint>

namespace tilekeep {

constexpr double kNearM = 1.5;       // this far from the spot, level, and you've walked off
constexpr int64_t kHoldMs = 1000;    // an opening that left them holds them this long, whatever the bar does as it settles
constexpr double kFacingDeg = 90;    // their middle further round than this, and you've turned from them

// Put by hand: the head's spot then in the tiles' group frame (level, its -Z the way to their middle),
// so it goes with the group wherever that goes after (a dragged dashboard, a universe's jump).
struct Put {
    bool on = false;
    double x = 0, z = 0;
    int64_t holdTill = 0;

    template <class M>
    void At(const M &head, const M &group) {
        const M h = posemath::Mul(posemath::Inverse(group), head);
        on = true, x = h.m[0][3], z = h.m[2][3], holdTill = 0;
    }
    // They go round you again (a reset, another wall): the next opening too.
    void Clear() { *this = {}; }
    // An opening at `now` left them, and till kHoldMs after they're held there.
    void Kept(int64_t now) { holdTill = now + kHoldMs; }
    bool Settling(int64_t now) const { return on && now < holdTill; }
    // They stay, with the head where it is now and their middle `wallM` out from the group's centre.
    template <class M>
    bool Holds(const M &head, const M &group, double wallM) const {
        const M h = posemath::Mul(posemath::Inverse(group), head);
        if (!on || !(std::hypot(h.m[0][3] - x, h.m[2][3] - z) <= kNearM)) return false;
        // the way faced, round (from the head's right axis: looking up or down changes nothing)
        const double fx = h.m[2][0], fz = -h.m[0][0], f = std::hypot(fx, fz);
        const double tx = -h.m[0][3], tz = -wallM - h.m[2][3], t = std::hypot(tx, tz);
        if (f < 0.1 || t < 1e-6) return true;   // (the head on its side, or in their middle: no telling)
        return fx * tx + fz * tz >= std::cos(kFacingDeg * M_PI / 180) * f * t;
    }
};

}  // namespace tilekeep
