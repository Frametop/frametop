// A hand hanging at the side: its pinches aren't clicks. The pointer helper's side of "Hanging
// hands" in ft-pointer.cpp. Header-only and free of OpenVR, so pointer/tests/hanging-test.cpp can
// drive it with made-up hands.
//
// Hands relaxed down at the sides touch thumb to finger as they sway, and the side cameras see them
// from above, where an index or middle pinch is easy to misread: they clicked, right-clicked and
// started two-hand drags (user, 2026-11-22: "when my hands are down at my side, they are triggering
// taps when I'm not tapping"). A narrated take that day put every one of those pinches 0.62-1.30 m
// under the eyes with the forearm (elbow to knuckles) 24-76 deg below level, and the wearer's own
// taps 0.28-0.38 m under the eyes with it 28-47 deg above level (typing hands 0.40-0.53 m, about
// level). So a pinch is a hanging hand's when both hold:
//   its knuckles more than below (0.58 m, POINTER_PINCH_HANG_BELOW; 0 off) under the eyes, and
//   its forearm more than forearmDeg (20 deg, POINTER_PINCH_HANG_FOREARM) below level.
// Both, so a low tap the arm model gets wrong (the elbow is inferred) still needs the height to be
// dropped, and a tall or seated wearer's height needs the arm's angle too. Without an elbow it's
// not hanging. Heights go by the room's up, not the head's, so looking down doesn't change them.
#pragma once

#include <cmath>

namespace hanging {

struct V3 {
    double x = 0, y = 0, z = 0;   // room metres, +y up
};

struct Params {
    double below = 0.58;       // m under the eyes; 0 off
    double forearmDeg = 20;    // deg below level
};

struct Check {
    bool hanging = false;
    double below = 0;          // the knuckles under the eyes, m
    double forearmDeg = 0;     // elbow to knuckles below level, deg (90 straight down; negative raised)
};

inline Check Test(const Params &p, const V3 &eye, const V3 &knuckle, const V3 &elbow, bool hasElbow) {
    Check c;
    c.below = eye.y - knuckle.y;
    const double drop = elbow.y - knuckle.y, across = std::hypot(knuckle.x - elbow.x, knuckle.z - elbow.z);
    c.forearmDeg = std::atan2(drop, across) * 180 / M_PI;
    c.hanging = p.below > 0 && hasElbow && c.below > p.below && c.forearmDeg > p.forearmDeg;
    return c;
}

}  // namespace hanging
