// This Frame's own programs that want what the room's other people send as datagrams: their
// poses (ft-avatars, which draws them) or their voices (ft-roomvoice, which plays them). Each
// asks on ft-together's socket about once a second (`watch` for poses, `listen` for voices),
// and from then on gets every one the gate passes (posegate.h, voicegate.h) as a datagram of
// its own,
//
//   the kind (datagrams.h: 0x81 a pose, 0x82 a voice), whose it is (their place in the room,
//   a byte), then the datagram as it came (proto/posewire.h, proto/voicewire.h)
//
// sent to the address it asked from. One that stops asking, or is gone, stops being sent to.
// One Watchers is those who asked for one kind.
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
    static constexpr double kKeptS = 5;   // after it last asked

    // `kind`: the first byte of what they are sent.
    explicit Watchers(uint8_t kind = kPoseToWatcher) : kind_(kind) {}
    // The program at `from` watches from `now` on. False when kMost others do already, or it
    // asked from a socket with no name (there is nowhere to send to).
    bool Add(const sockaddr_un &from, socklen_t len, double now);
    // A person's datagram to everyone who watches, from the socket `sock`. Nobody is waited
    // for: a watcher whose queue is full misses this one.
    void Send(int sock, int person, const uint8_t *datagram, size_t n, double now);
    size_t count() const { return watching_.size(); }

private:
    struct Watcher {
        sockaddr_un at;
        socklen_t len;
        double asked;
    };
    const uint8_t kind_;
    std::vector<Watcher> watching_;
};

}  // namespace local
