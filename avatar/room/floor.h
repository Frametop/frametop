// Your own floor laid in a shared room (docs/together.md, "Whose room"): everyone tracks
// themselves on their own floor (SteamVR's standing universe), and the room has directions of
// its own, its owner's. A place on your floor, turned by `yaw` about the way up and then moved
// by (x, z), is that place in the room; the floors are level with each other.
//
// It is laid once, when you come in: so that where you stand then is the spot the room gave
// you, facing the way the spot faces. ft-together keeps it (together/local/floor.h).
//
// The sums only. Header only.
#pragma once

#include "../pose/turn.h"

namespace roomfloor {

using avatar::Vec;
using bodypose::Turn;

struct Floor {
    double x = 0, z = 0, yaw = 0;
};

// The way a head faces, level: radians about the way up, 0 faces -z (proto::Spot's yaw).
// `ahead`: the way it looks; straight up or down counts as -z.
inline double Facing(const Vec &ahead) { return std::fabs(ahead.x) + std::fabs(ahead.z) > 1e-6 ? std::atan2(-ahead.x, -ahead.z) : 0; }

// A direction's turn from the floor to the room, and a turn's (it goes in front of a turn made on the floor).
inline Turn Turned(const Floor &f) { return Turn::About({0, 1, 0}, f.yaw); }

inline Vec ToRoom(const Floor &f, const Vec &onFloor) {
    const Vec r = Turned(f) * onFloor;
    return {r.x + f.x, r.y, r.z + f.z};
}

inline Vec FromRoom(const Floor &f, const Vec &inRoom) { return Turned(f).inverse() * Vec{inRoom.x - f.x, inRoom.y, inRoom.z - f.z}; }

// The floor of someone whose spot in the room is at (spotX, spotZ) facing `spotYaw`, and whose
// head was over (headX, headZ) of their own floor facing `headYaw` (Facing) when they came in.
inline Floor Lay(double spotX, double spotZ, double spotYaw, double headX, double headZ, double headYaw) {
    Floor f;
    f.yaw = bodypose::Wrap(spotYaw - headYaw);
    const Vec head = Turned(f) * Vec{headX, 0, headZ};
    f.x = spotX - head.x, f.z = spotZ - head.z;
    return f;
}

// One floor laid on another: a place on `inner` is one on `outer`, which is one in the room.
inline Floor Then(const Floor &outer, const Floor &inner) {
    const Vec at = ToRoom(outer, {inner.x, 0, inner.z});
    return {at.x, at.z, bodypose::Wrap(outer.yaw + inner.yaw)};
}

}  // namespace roomfloor
