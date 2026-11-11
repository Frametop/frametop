// The links between Frames, on QUIC (msquic): session::Links for real. One connection per link,
// with one stream for the control messages, opened by the caller, and datagrams beside it.
//
// msquic calls back on threads of its own. Nothing of that reaches the session: what happens on
// a link is put in a queue, and the program's one loop takes it out (fd, Next) and tells the
// session. Call, Send, Close and Datagram are for that same loop.
//
// QUIC wants a certificate from whoever listens. Ours is made at start and names nobody, and
// callers don't check it: who a peer is comes from Tailscale, which already encrypts the path
// and vouches for the address at its other end.
#pragma once

#include "../session/session.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

constexpr size_t kMostDatagram = 1187;   // what a QUIC datagram carries over Tailscale (spike T3)

struct Event {
    enum Kind { kUp, kBytes, kDown, kDatagram };
    Kind kind = kDown;
    int link = -1;
    std::string address;          // kUp: the far end's, as a number
    std::vector<uint8_t> bytes;   // kBytes: the control stream's next bytes; kDatagram: one datagram
};

class QuicLinks : public session::Links {
public:
    struct Config {
        // The address calls are taken on, as a number: this Frame's on the tailnet, or a loopback
        // one. (msquic holds the port on every address, and hands over only what was sent to this one.)
        std::string listen;
        uint16_t port = 0;
        // Whether an address may be called. A name is looked up first (lookup.h), and only an
        // address this allows is ever sent a packet. Without it nobody is called.
        std::function<bool(const std::string &address)> mayCall;
    };

    // Listening, or nothing with `err`.
    static std::unique_ptr<QuicLinks> Open(const Config &config, std::string &err);
    ~QuicLinks() override;

    int fd() const;             // readable while something waits in the queue
    bool Next(Event &event);    // takes it out: what happened on each link, in order
    // Now and then (a second apart will do): cuts a closed link whose far end never took what
    // was sent to it before the close.
    void Tick();
    bool Quiet();               // no link is left, a closed one that is still going neither

    int Call(const std::string &host, uint16_t port) override;
    void Send(int link, const std::vector<uint8_t> &message) override;
    void Close(int link) override;
    // One datagram to a link's far end. It may not arrive. False when it can't be sent at all:
    // the link isn't up, or it is longer than the link carries.
    bool Datagram(int link, const std::vector<uint8_t> &bytes);

    struct Impl;

private:
    explicit QuicLinks(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
