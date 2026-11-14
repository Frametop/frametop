// Where a voice comes from (docs/together.md, "Sound": each voice comes from its avatar's
// head): how loud someone is in each ear, from where their head and the listener's are in the
// room.
//
// No head model: the nearer ear is louder, someone further away is quieter, and someone behind
// a little duller. The far ear always keeps some, as it does in a room.
//
// The sums only. Header only.
#pragma once

#include <cmath>

namespace voice {

struct Ears {
    float left = 0, right = 0;
};

// Someone whose place isn't known is heard in the middle.
constexpr Ears kMiddle = {0.7071f, 0.7071f};

constexpr double kNear = 1.0;      // metres: as loud as it gets from here in
constexpr double kQuietest = 0.2;  // ...and it gets no quieter than this, however far
constexpr double kAside = 0.8;     // how far to one side a voice goes, of all the way
constexpr double kBehind = 0.8;    // how loud straight behind is, of in front

// The listener's head is at `ear` in the room, facing `yaw` (radians about the way up, 0 faces
// -z, as proto::Spot's); the speaker's head at `mouth`.
inline Ears Hear(const double ear[3], double yaw, const double mouth[3]) {
    const double dx = mouth[0] - ear[0], dy = mouth[1] - ear[1], dz = mouth[2] - ear[2];
    const double far = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(far > 1e-3)) return kMiddle;
    const double aside = dx * std::cos(yaw) - dz * std::sin(yaw);    // to the listener's right
    const double ahead = -dx * std::sin(yaw) - dz * std::cos(yaw);
    const double pan = kAside * aside / far;                          // -1 left .. 1 right; overhead is the middle
    const double behind = ahead < 0 ? -ahead / far : 0;
    const double loud = std::fmax(kQuietest, kNear / std::fmax(far, kNear)) * (1 - (1 - kBehind) * behind);
    const double angle = (pan + 1) * 0.7853981633974483;              // equal power: the two ears' squares add up to one
    return {float(loud * std::cos(angle)), float(loud * std::sin(angle))};
}

}  // namespace voice
