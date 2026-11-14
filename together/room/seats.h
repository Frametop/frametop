// Who stands where in a room (docs/together.md, "Whose room"): the owner where they stood when
// they opened it, and the guests at the spots beside them, facing the way the owner faced. The
// room's directions are its owner's: a guest's own floor is laid on their spot.
//
// The rules only.
#pragma once

#include "../proto/messages.h"

#include <cmath>

namespace room {

constexpr float kBeside = 0.9f;   // metres between two people's spots

// The spot of the person in place `id` (1 and up; 0 is the owner's own): the first guest to the
// owner's right, the second to their left, the third further right.
inline proto::Spot SpotBeside(const proto::Spot &owner, int id) {
    static const float kSteps[proto::kPeople] = {0, 1, -1, 2};
    const float by = kBeside * kSteps[id > 0 && id < proto::kPeople ? id : 0];
    proto::Spot s = owner;
    s.at[0] += by * std::cos(owner.yaw);   // yaw 0 faces -z: the right is +x
    s.at[2] -= by * std::sin(owner.yaw);
    return s;
}

// A place nobody has, or -1 when the room is full.
inline int FreeId(const proto::Room &room) {
    for (int id = 0; id < proto::kPeople; ++id)
        if (!room.person(id)) return id;
    return -1;
}

}  // namespace room
