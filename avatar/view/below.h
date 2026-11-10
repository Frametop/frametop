// Your own body, seen when you look down: the sums. SteamVR shows flat pictures in the room, so
// the body is drawn on one: a square lying level under your eyes, with each eye's view of the
// body drawn through it as through a window (mirror::Window), side by side. The body then has
// its own depth, above and below the square.
//
// The square keeps the room's directions and only its place follows you. SteamVR puts a picture
// in the room afresh for every frame of the displays, however old the picture is, so turning
// your head moves nothing on it: the body can't slide as you look around. What is left is the
// picture's age when you walk or lean: the body trails you by the few hundredths of a second a
// picture takes to come up.
//
// Header only, no GPU or SteamVR calls (the tests use it).
#pragma once

#include "../bodyfile.h"
#include "mirror.h"

#include <cmath>

namespace below {

using avatar::Vec;

constexpr double kUnder = 0.5;     // the square lies this far under the eyes: near enough to fill what you see looking down
constexpr double kSide = 1.6;      // metres a side: within 58 degrees of straight down
constexpr double kNear = 0.06;     // nothing nearer an eye is drawn (metres, down from it): your collar is close
constexpr double kDeep = 3;        // ...and nothing further: the floor is nearer
constexpr double kLooksDown = 0.21;   // the head looks down this far (radians under level, 12 degrees) before the body is drawn

// The square under someone whose eyes are at `eyes`: level, facing up, its picture's top toward -z.
inline mirror::Glass Under(const Vec &eyes) {
    mirror::Glass g;
    g.right = {1, 0, 0}, g.up = {0, 0, -1}, g.out = {0, 1, 0};
    g.width = g.height = kSide;
    g.centre = {eyes.x, eyes.y - kUnder, eyes.z};
    return g;
}

// What an eye sees of the room through the square (mirror::Window: it winds the other way).
inline bool EyeView(const mirror::Glass &g, const Vec &eye, viewmath::Mat4 &clip) { return mirror::Window(g, eye, kNear, kDeep, clip); }

// Whether a head that looks along `ahead` looks down at least `from` radians under level.
inline bool LooksDown(const Vec &ahead, double from = kLooksDown) { return ahead.norm() > 1e-6 && ahead.unit().y <= -std::sin(from); }

// Toward the light the body is lit by: from over your head, a little ahead.
inline Vec Light() { return Vec{0.25, 0.9, -0.35}.unit(); }

// A pose's skinning matrices with the parts you don't see of yourself folded away, each to a
// point at its joint: the head, which your eyes are inside, and a hand that is drawn for you
// already (`handDrawn`, left then right: ft-handview draws the hands the tracker sees).
// `rest`: the body's bones' rest places (bodyfile::Body::at).
inline void Fold(const float rest[bodyfile::kRoles][3], const bool handDrawn[2], float bones[bodyfile::kRoles][12]) {
    const auto fold = [&](int from, int role) {
        // where the joint `from` is posed: everything on `role` goes there
        const float *m = bones[from], *at = rest[from];
        float to[3];
        for (int r = 0; r < 3; ++r) to[r] = m[r * 4] * at[0] + m[r * 4 + 1] * at[1] + m[r * 4 + 2] * at[2] + m[r * 4 + 3];
        for (int r = 0; r < 3; ++r) bones[role][r * 4] = bones[role][r * 4 + 1] = bones[role][r * 4 + 2] = 0, bones[role][r * 4 + 3] = to[r];
    };
    for (int role : {bodyfile::kLeftEye, bodyfile::kRightEye, bodyfile::kJaw, bodyfile::kHead}) fold(bodyfile::kHead, role);
    for (int side = 0; side < 2; ++side) {
        if (!handDrawn[side]) continue;
        const int hand = side ? bodyfile::kRightHand : bodyfile::kLeftHand, fingers = side ? bodyfile::kRightFingers : bodyfile::kLeftFingers;
        for (int f = 0; f < 15; ++f) fold(hand, fingers + f);
        fold(hand, hand);
    }
}

}  // namespace below
