// Whether the dashboard's tiles stay where they are as the dashboard opens (vrtiles.cpp), instead of
// coming round you at Steam's bar. They stay while you're still by the spot they were put or brought
// round you from, and still face their way: an opening there (the Steam menu, Steam come to the front
// for an app that opened or closed) doesn't move them. Walk off or turn round, and the next opening
// brings them to you; a layout reset does at once. Put by hand (Move All Windows; vrlayoutgrab.cpp)
// they keep to a wider spot than brought round you, since you put them there on purpose.
//
// They come round you only from a head that's steady (Steady): the headset worn a while and the head
// not moving. An opening as the headset comes off or goes on, or with the head's pose jumping, waited
// for that and brought them round wherever the head was then (user, 2026-11-26: "it seems like the
// displays get moved off center almost like it thinks the head position moved and recentered").
// Header only, no OpenVR (screens/tests/tilekeep-test.cpp).
#pragma once

#include "posemath.h"

#include <cmath>
#include <cstdint>

namespace tilekeep {

// How far you may go from the spot (level) and how far their middle may be round from where you face
// before an opening brings them to you.
struct Rule {
    double nearM, facingDeg;
};
constexpr Rule kByHand{1.5, 90};   // put by hand (Move All Windows)
constexpr Rule kRound{1.0, 60};    // brought round you (an opening, a reset)
constexpr int64_t kHoldMs = 1000;  // an opening that left them holds them this long, whatever the bar does as it settles

// The head's spot when they were put in the tiles' group frame (level, its -Z the way to their middle),
// so it goes with the group wherever that goes after (a dragged dashboard, a universe's jump).
struct Put {
    bool on = false, byHand = false;
    Rule rule = kByHand;
    double x = 0, z = 0;
    int64_t holdTill = 0;

    template <class M>
    void At(const M &head, const M &group, bool hand = true) {
        const M h = posemath::Mul(posemath::Inverse(group), head);
        on = true, byHand = hand, rule = hand ? kByHand : kRound, x = h.m[0][3], z = h.m[2][3], holdTill = 0;
    }
    // They go round you again (another wall, a guessed bar): the next opening too.
    void Clear() { *this = {}; }
    // An opening at `now` left them, and till kHoldMs after they're held there.
    void Kept(int64_t now) { holdTill = now + kHoldMs; }
    bool Settling(int64_t now) const { return on && now < holdTill; }
    // They stay, with the head where it is now and their middle `wallM` out from the group's centre.
    template <class M>
    bool Holds(const M &head, const M &group, double wallM) const {
        const M h = posemath::Mul(posemath::Inverse(group), head);
        if (!on || !(std::hypot(h.m[0][3] - x, h.m[2][3] - z) <= rule.nearM)) return false;
        // the way faced, round (from the head's right axis: looking up or down changes nothing)
        const double fx = h.m[2][0], fz = -h.m[0][0], f = std::hypot(fx, fz);
        const double tx = -h.m[0][3], tz = -wallM - h.m[2][3], t = std::hypot(tx, tz);
        if (f < 0.1 || t < 1e-6) return true;   // (the head on its side, or in their middle: no telling)
        return fx * tx + fz * tz >= std::cos(rule.facingDeg * M_PI / 180) * f * t;
    }
};

constexpr int64_t kWornMs = 2000;   // worn this long first: putting it on, the head is still finding its place
constexpr double kStillM = 0.15;    // the head more than this from where it was still is moving (lifting the headset off, a pose's jump)...
constexpr int64_t kStillMs = 300;   // ...and still again only this long after

// Whether the head is steady enough to bring the tiles round it: fed each tick with whether the headset
// is worn (its activity level) and the head's position, if SteamVR has one.
struct Steady {
    int64_t wornSince = -1, stillSince = -1;   // -1: not worn, or no head
    double at[3] = {0, 0, 0};                  // where the head has been still

    void Tick(int64_t now, bool worn, bool valid, const double head[3]) {
        if (!worn || !valid) return (void)(wornSince = stillSince = -1);
        if (wornSince < 0) wornSince = now;
        if (stillSince < 0 || std::hypot(head[0] - at[0], head[1] - at[1], head[2] - at[2]) > kStillM)
            stillSince = now, at[0] = head[0], at[1] = head[1], at[2] = head[2];
    }
    bool Ready(int64_t now) const {
        return wornSince >= 0 && now - wornSince >= kWornMs && stillSince >= 0 && now - stillSince >= kStillMs;
    }
};

}  // namespace tilekeep
