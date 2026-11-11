// What a PanelHand says (panelhand.h), sent to ft-together's local socket.
//
// From a socket with no name: ft-together answers only who has one, and nobody reads answers
// here. An act for a panel the wearer wasn't given is refused there and costs nothing.
//
// Nobody is waited for. When ft-together's queue is full, what didn't go waits for the next
// Send, in order, because a release that never went would leave a button down on someone
// else's desktop until its owner's ft-together let it go (room/desk.h). More than kMostWaiting
// of them and the oldest are dropped. When ft-together isn't there, everything is: the program
// that shows the panel ends then anyway.
//
// Header only.
#pragma once

#include "panelhand.h"

#include "../../conf/ftsock.h"

#include <unistd.h>

namespace local {

class HandSender {
public:
    static constexpr size_t kMostWaiting = 256;

    explicit HandSender(std::string together) : to_(std::move(together)), fd_(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {}
    ~HandSender() {
        if (fd_ >= 0) close(fd_);
    }
    HandSender(const HandSender &) = delete;
    HandSender &operator=(const HandSender &) = delete;

    // Sends what is in `out`, oldest first. What is left in it afterwards waits for the next call.
    void Send(PanelHand::Out &out) {
        size_t sent = 0;
        for (; sent < out.size(); ++sent) {
            const int err = ft_sock_send(fd_, to_.c_str(), out[sent].data(), out[sent].size(), MSG_DONTWAIT);
            if (err == EAGAIN || err == EWOULDBLOCK) break;   // its queue is full: later
        }
        out.erase(out.begin(), out.begin() + long(sent));
        if (out.size() > kMostWaiting) out.erase(out.begin(), out.end() - long(kMostWaiting));
    }

private:
    std::string to_;
    int fd_;
};

}  // namespace local
