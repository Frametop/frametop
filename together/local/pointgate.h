// The pointers ft-together passes between its links and this Frame's own programs
// (docs/together.md, "Control and moving"; proto/pointwire.h has the bytes).
//
// In, from a link: a datagram is passed on only when it is a pointer, newer than the last one
// that link sent, and within what a link may send in a second, rubbish counted (linkgate.h).
// Whether the panel it names is in the room is for whoever draws it to see: the room may have
// changed by then anyway.
//
// Out, to the links: one this Frame's own program hands over is read the same way and sent
// with ft-together's own count in it.
//
// The rules only: no sockets here. Header only.
#pragma once

#include "../proto/pointwire.h"
#include "../proto/posewire.h"
#include "linkgate.h"

#include <cstdint>
#include <vector>

namespace local {

class PointGate {
public:
    static constexpr double kASecond = 90;   // taken from one link a second, three times what a Frame sends
    static constexpr double kAtOnce = 30;

    bool Take(int link, const uint8_t *data, size_t n, double now) {
        proto::Point point;
        return gate_.Count(link, now) && proto::Read(data, n, point) && gate_.Newer(link, point.count);
    }
    void Forget(int link) { gate_.Forget(link); }

private:
    LinkGate gate_{kASecond, kAtOnce};
};

// This Frame's own pointer, counted for the links.
class PointCount {
public:
    bool Stamp(const uint8_t *data, size_t n, std::vector<uint8_t> &out) {
        proto::Point point;
        if (!proto::Read(data, n, point)) return false;
        out = Stamp(point);
        return true;
    }
    std::vector<uint8_t> Stamp(proto::Point point) {
        point.count = ++count_;
        return proto::Pack(point);
    }

private:
    uint16_t count_ = 0;
};

}  // namespace local
