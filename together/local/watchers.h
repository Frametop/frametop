// This Frame's own programs that want the poses of the room's other people (ft-avatars, which
// draws them): each asks `watch` on ft-together's socket about once a second, and from then on
// gets every pose the gate passes (posegate.h) as a datagram of its own,
//
//   0x81, whose it is (their place in the room, a byte), then the pose (proto/posewire.h)
//
// sent to the address it asked from. One that stops asking, or is gone, stops being sent to.
#pragma once

#include "datagrams.h"

#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>

namespace local {

class Watchers {
public:
    static constexpr size_t kMost = 4;
    static constexpr double kKeptS = 5;   // after its last `watch`

    // The program at `from` watches from `now` on. False when kMost others do already, or it
    // asked from a socket with no name (there is nowhere to send to).
    bool Add(const sockaddr_un &from, socklen_t len, double now);
    // A person's pose to everyone who watches, from the socket `sock`. Nobody is waited for:
    // a watcher whose queue is full misses this one.
    void Send(int sock, int person, const uint8_t *pose, size_t n, double now);
    size_t count() const { return watching_.size(); }

private:
    struct Watcher {
        sockaddr_un at;
        socklen_t len;
        double asked;
    };
    std::vector<Watcher> watching_;
};

}  // namespace local
