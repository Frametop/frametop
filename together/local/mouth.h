// The wearer's mouth on its way into their poses (docs/together.md, "Avatars"). Two of this
// Frame's programs know the wearer: the one that sees them says where they are (a pose), and
// the one that hears them says how their mouth is (a mouth datagram, datagrams.h), as often as
// it changes. ft-together keeps the mouth it was last told and puts it into each pose it sends.
//
// A mouth that isn't told again shuts after kHeldS: the program that told it may be gone, and
// a mouth left open says someone speaks.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/posewire.h"
#include "datagrams.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace local {

class Mouth {
public:
    static constexpr double kHeldS = 0.5;   // an open mouth is told about fifty times a second

    // A datagram from one of this Frame's own programs at `now` (seconds). False when it
    // isn't a mouth.
    bool Take(const uint8_t *data, size_t n, double now) {
        if (n != kMouthBytes || data[0] != kMouthToTogether) return false;
        std::copy(data + 1, data + n, shape_);
        told_ = now;
        return true;
    }
    // The mouth as it is at `now`, into a pose on its way out. A pose keeps its own while
    // none was told lately.
    void Into(proto::Pose &pose, double now) const {
        if (now >= told_ && now - told_ <= kHeldS) std::copy(shape_, shape_ + proto::kMouthShapes, pose.mouth);
    }

private:
    uint8_t shape_[proto::kMouthShapes] = {};
    double told_ = -1e18;
};

}  // namespace local
