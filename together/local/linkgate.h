// What one link may send as datagrams of one kind, and which of them came late: the rule the
// poses and the voice share (posegate.h, voicegate.h).
//
// Each datagram that comes counts against its link before it is read, rubbish too, so a flood
// costs a read each and no more. Of those that read, one is passed on only when its count is
// newer than the last one passed on from that link.
//
// The rules only: no sockets here. Header only.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>

namespace local {

class LinkGate {
public:
    // `aSecond`: what a link may send in a second; `atOnce`: and in one go after a quiet moment.
    LinkGate(double aSecond, double atOnce) : aSecond_(aSecond), atOnce_(atOnce) {}

    // One datagram came on `link` at `now` (seconds). False when the link has sent too many: it
    // isn't read.
    bool Count(int link, double now) {
        Seen &s = seen_[link];
        if (s.known) s.left = std::fmin(atOnce_, s.left + (now - s.at) * aSecond_);
        else s.left = atOnce_;
        s.known = true, s.at = now;
        if (s.left < 1) return false;
        s.left -= 1;
        return true;
    }
    // Whether one counted `count` is newer than the link's last, which it then is. The count
    // wraps; a link's first is always newer.
    bool Newer(int link, uint16_t count) {
        Seen &s = seen_[link];
        if (s.any && int16_t(uint16_t(count - s.count)) <= 0) return false;
        s.any = true, s.count = count;
        return true;
    }
    // The link is gone: one that gets its number later starts afresh.
    void Forget(int link) { seen_.erase(link); }
    size_t links() const { return seen_.size(); }

private:
    struct Seen {
        bool known = false, any = false;   // something came; one was passed on
        uint16_t count = 0;
        double left = 0, at = 0;
    };
    const double aSecond_, atOnce_;
    std::map<int, Seen> seen_;
};

}  // namespace local
