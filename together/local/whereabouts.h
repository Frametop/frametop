// Where the others in the room are, as their poses last said (proto/posewire.h: the room's own
// directions): each one's eyes and the way they face, level. ft-together keeps it from the
// poses its gate passes, for the program that moves this Frame's person to someone
// (ft-roompanels' `go person N`). Someone whose poses stopped is forgotten.
//
//   where    -> "ok COUNT PLACE:X:Y:Z:YAW ...": each one's place in the room (a number), their
//               eyes in metres, and the way they face in radians about the way up (0 faces -z)
//
// The rules only. Header only.
#pragma once

#include "../proto/messages.h"
#include "../proto/posewire.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace local {

class Whereabouts {
public:
    static constexpr double kKeptS = 2;   // after their last pose

    // The way a head turned `q` (w, x, y, z) faces, level: 0 faces -z, and straight up or down
    // counts as that too (avatar/room/floor.h, Facing).
    static double Facing(const float q[4]) {
        const double w = q[0], x = q[1], y = q[2], z = q[3];
        const double backX = 2 * (x * z + w * y), backZ = 1 - 2 * (x * x + y * y);   // the head's +z, which it faces away from
        return std::fabs(backX) + std::fabs(backZ) > 1e-6 ? std::atan2(backX, backZ) : 0;
    }

    // A pose of the person at `place`, as it came at `now`.
    void Saw(int place, const proto::Pose &pose, double now) {
        if (place < 0 || place >= proto::kPeople) return;
        Seen &s = seen_[place];
        s.at = now, s.known = true;
        for (int i = 0; i < 3; ++i) s.eyes[i] = pose.eyes[i];
        s.yaw = Facing(pose.head);
    }
    // The answer to `where` at `now`.
    std::string Ask(double now) const {
        std::string entries;
        int count = 0;
        for (int place = 0; place < proto::kPeople; ++place) {
            const Seen &s = seen_[place];
            if (!s.known || now - s.at > kKeptS) continue;
            char one[96];
            std::snprintf(one, sizeof one, " %d:%.3f:%.3f:%.3f:%.4f", place, s.eyes[0], s.eyes[1], s.eyes[2], s.yaw);
            entries += one, ++count;
        }
        return "ok " + std::to_string(count) + entries;
    }

private:
    struct Seen {
        bool known = false;
        double at = 0, eyes[3] = {}, yaw = 0;
    };
    Seen seen_[proto::kPeople];
};

}  // namespace local
