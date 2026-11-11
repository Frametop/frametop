// The poses ft-together passes between its links and this Frame's own programs
// (docs/together.md, "The protocol"; proto/posewire.h has the bytes).
//
// In, from a link: a datagram is passed on only when it is a pose (proto::Read takes it), newer
// than the last one that link sent, and within what a link may send in a second. Everything a
// peer sends counts against that, rubbish too, so a flood costs a read each and no more
// (linkgate.h).
//
// Out, to the links: a pose this Frame's own program hands over is read the same way and sent
// with ft-together's own count in it, so the count goes on when that program starts again
// while the links stay up, and with the wearer's mouth as the program that hears them last
// told it (mouth.h).
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/posewire.h"
#include "linkgate.h"
#include "mouth.h"

#include <cstdint>
#include <vector>

namespace local {

class PoseGate {
public:
    static constexpr double kASecond = 90;   // poses taken from one link a second, three times what a Frame sends
    static constexpr double kAtOnce = 30;    // ...and this many may come in one go after a quiet moment

    // Whether the datagram that came on `link` at `now` (seconds) is passed on. `pose`: what it
    // says, when it is.
    bool Take(int link, const uint8_t *data, size_t n, double now, proto::Pose &pose) {
        return gate_.Count(link, now) && proto::Read(data, n, pose) && gate_.Newer(link, pose.turn);
    }
    bool Take(int link, const uint8_t *data, size_t n, double now) {
        proto::Pose pose;
        return Take(link, data, n, now, pose);
    }
    // The link is gone: one that gets its number later starts afresh.
    void Forget(int link) { gate_.Forget(link); }
    size_t links() const { return gate_.links(); }

private:
    LinkGate gate_{kASecond, kAtOnce};
};

// This Frame's own poses, counted for the links.
class PoseCount {
public:
    // The pose `data` as it goes to the links at `now`, the wearer's mouth in it. False when
    // it isn't a pose.
    bool Stamp(const uint8_t *data, size_t n, const Mouth &mouth, double now, std::vector<uint8_t> &out) {
        proto::Pose pose;
        if (!proto::Read(data, n, pose)) return false;
        mouth.Into(pose, now);
        pose.turn = ++turn_;
        out = proto::Pack(pose);
        return true;
    }

private:
    uint16_t turn_ = 0;
};

}  // namespace local
