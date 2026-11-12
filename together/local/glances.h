// Where the others in the room look, for this Frame's programs (docs/together.md, "Gaze"). A
// pose carries where its person looks only while they share that (gaze.h, on their own Frame).
// From each such pose ft-together works out which of the room's panels they look at and where
// on it (glancesums.h), and tells the programs that asked for the others' pointers (`points`):
//
//   kGlanceToWatcher, whose it is (their place in the room, a byte), then a pointer datagram
//   (proto/pointwire.h): the panel and where on it, or no panel. Its count and flags are 0.
//
// One is told ten times a second at most while they look at a panel, and once that they look at
// none when they look away, stop sharing it, or their pose says nothing of it.
//
// Nothing of this goes on a link: it is worked out here, from what the pose said. So your own
// look is never drawn for you, and someone who doesn't share theirs has no mark anywhere.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/pointwire.h"
#include "glancesums.h"

#include <cmath>
#include <vector>

namespace local {

class Glances {
public:
    static constexpr double kEveryS = 0.1;

    // A pose of the person at `place`, as it came at `now` (seconds), among the room's `panels`.
    // True when the programs are told something for it: `out` is then the pointer datagram.
    bool Saw(int place, const proto::Pose &pose, const std::vector<proto::Panel> &panels, double now, std::vector<uint8_t> &out) {
        if (place < 0 || place >= proto::kPeople) return false;
        Told &told = told_[place];
        if (now >= told.at && now - told.at < kEveryS) return false;
        proto::Point point;
        Spot spot;
        const bool on = pose.looks && LooksAt(panels, pose, point.panel, spot);
        if (!on && !told.on) return false;   // they looked at no panel, and still don't
        point.x = Part(spot.x), point.y = Part(spot.y);   // 0 and 0 with no panel
        told.on = on, told.at = now;
        out = proto::Pack(point);
        return true;
    }

private:
    static uint16_t Part(double of) { return uint16_t(std::lround(of * 65535)); }   // of a picture's side: 0 to 1 (glancesums.h)

    struct Told {
        bool on = false;     // the programs were last told a panel
        double at = -1e18;   // ...or anything, then
    };
    Told told_[proto::kPeople];
};

}  // namespace local
