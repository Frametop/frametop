// ft-screens' "shift": the room's panels moved on the floor, because you moved yourself through
// a shared room (together/roompanels/moving.py) and the room stands still. A shift turns
// everything `yaw` radians to the left about the way up through the floor's middle, then moves
// it (x, z) metres. ft-screens keeps the sum of those it made, so "shift back" can put the
// panels where they would stand had you never moved: when you leave the room.
//
// The sums only. M is any type with a float m[3][4] (vr::HmdMatrix34_t in vr.cpp). Header
// only, no OpenVR, so its test runs anywhere (screens/tests/roomshift-test.cpp).
#pragma once

#include <cmath>

namespace roomshift {

struct Shift {
    double x = 0, z = 0, yaw = 0;
};

// `first`, then `next`: the one shift that does both.
inline Shift Then(const Shift &first, const Shift &next) {
    const double c = std::cos(next.yaw), s = std::sin(next.yaw);
    return {c * first.x + s * first.z + next.x, -s * first.x + c * first.z + next.z, std::remainder(first.yaw + next.yaw, 2 * M_PI)};
}

// The shift that undoes `shift`.
inline Shift Back(const Shift &shift) {
    const double c = std::cos(shift.yaw), s = std::sin(shift.yaw);
    return {-(c * shift.x - s * shift.z), -(s * shift.x + c * shift.z), -shift.yaw};
}

// As a pose: in front of a panel's pose, that panel shifted.
template <class M>
M Pose(const Shift &shift) {
    const double c = std::cos(shift.yaw), s = std::sin(shift.yaw);
    M m{};
    m.m[0][0] = float(c), m.m[0][2] = float(s), m.m[0][3] = float(shift.x);
    m.m[1][1] = 1;
    m.m[2][0] = float(-s), m.m[2][2] = float(c), m.m[2][3] = float(shift.z);
    return m;
}

}  // namespace roomshift
