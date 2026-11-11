// The poses ft-together passes between its links and this Frame's own programs
// (docs/together.md, "The protocol"; proto/posewire.h has the bytes).
//
// In, from a link: a datagram is passed on only when it is a pose (proto::Read takes it), newer
// than the last one that link sent, and within what a link may send in a second. Everything a
// peer sends counts against that, rubbish too, so a flood costs a read each and no more.
//
// Out, to the links: a pose this Frame's own program hands over is read the same way and sent
// with ft-together's own count in it, so the count goes on when that program starts again
// while the links stay up.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/posewire.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace local {

class PoseGate {
public:
    static constexpr double kASecond = 90;   // poses taken from one link a second, three times what a Frame sends
    static constexpr double kAtOnce = 30;    // ...and this many may come in one go after a quiet moment

    // Whether the datagram that came on `link` at `now` (seconds) is passed on.
    bool Take(int link, const uint8_t *data, size_t n, double now) {
        Seen &s = seen_[link];
        if (s.known) s.left = std::fmin(kAtOnce, s.left + (now - s.at) * kASecond);
        s.known = true, s.at = now;
        if (s.left < 1) return false;
        s.left -= 1;
        proto::Pose pose;
        if (!proto::Read(data, n, pose) || (s.any && !proto::Newer(pose.turn, s.turn))) return false;
        s.any = true, s.turn = pose.turn;
        return true;
    }
    // The link is gone: one that gets its number later starts afresh.
    void Forget(int link) { seen_.erase(link); }
    size_t links() const { return seen_.size(); }

private:
    struct Seen {
        bool known = false, any = false;   // something came; a pose was taken
        uint16_t turn = 0;
        double left = kAtOnce, at = 0;
    };
    std::map<int, Seen> seen_;
};

// This Frame's own poses, counted for the links.
class PoseCount {
public:
    // The pose `data` as it goes to the links. False when it isn't a pose.
    bool Stamp(const uint8_t *data, size_t n, std::vector<uint8_t> &out) {
        proto::Pose pose;
        if (!proto::Read(data, n, pose)) return false;
        pose.turn = ++turn_;
        out = proto::Pack(pose);
        return true;
    }

private:
    uint16_t turn_ = 0;
};

}  // namespace local
