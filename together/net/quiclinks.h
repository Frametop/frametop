// The links between Frames, on QUIC (msquic): session::Links for real. One connection per link,
// with one stream for the control messages, opened by the caller, datagrams beside it, a
// one-way stream for each picture of a shared panel, and one for a file that was asked for.
//
// msquic calls back on threads of its own. Nothing of that reaches the session: what happens on
// a link is put in a queue, and the program's one loop takes it out (fd, Next) and tells the
// session. Call, Send, Close, Datagram, Frame, File and AwaitFile are for that same loop.
//
// QUIC wants a certificate from whoever listens. Ours is made at start and names nobody, and
// callers don't check it: who a peer is comes from Tailscale, which already encrypts the path
// and vouches for the address at its other end.
#pragma once

#include "../session/session.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

constexpr size_t kMostDatagram = 1187;   // what a QUIC datagram carries over Tailscale (spike T3)

struct Event {
    enum Kind { kUp, kBytes, kDown, kDatagram, kFrame, kFrameLost, kFile };
    Kind kind = kDown;
    int link = -1;
    std::string address;          // kUp: the far end's, as a number
    // kBytes: the control stream's next bytes; kDatagram: one datagram; kFrame: all that one of
    // the far end's Frame()s sent; kFile: the file that was awaited, as far as it came
    std::vector<uint8_t> bytes;
    uint32_t tag = 0;             // kFrameLost: the tag of a Frame() that was given up
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
    // Now and then: cuts a closed link whose far end never took what was sent to it before the
    // close, and gives up the pictures that are late. A second apart will do, but while
    // Sending() it is to be every 20 ms or so: a picture's lateness is found here.
    void Tick();
    bool Quiet();               // no link is left, a closed one that is still going neither

    int Call(const std::string &host, uint16_t port) override;
    void Send(int link, const std::vector<uint8_t> &message) override;
    void Close(int link) override;
    // One datagram to a link's far end. It may not arrive. False when it can't be sent at all:
    // the link isn't up, or it is longer than the link carries.
    bool Datagram(int link, const std::vector<uint8_t> &bytes);
    // A picture to a link's far end, on a stream of its own: there it comes whole (kFrame) or
    // not at all. Lost packets are sent again, but not for long: one that isn't all acknowledged
    // 150 ms after it was sent is given up, and kFrameLost tells so with its `tag`. False when
    // it can't be sent at all: the link isn't up, it is longer than a picture may be
    // (proto/framewire.h), or too many are on their way already.
    bool Frame(int link, std::vector<uint8_t> bytes, uint32_t tag);
    bool Sending();             // a picture is on its way out and not yet acknowledged or given up
    // A file to a link's far end, on a stream of its own, behind everything else the link
    // carries. It is taken there only if that end awaits one. False when it can't be sent: the
    // link isn't up, there is no file or one longer than may be sent (proto::kMostLook), or one
    // is on its way on this link already.
    bool File(int link, std::shared_ptr<const std::vector<uint8_t>> bytes);
    // From now on the link's far end may send one file of `bytes` at most, which comes whole
    // as kFile (0: none is taken). A file nobody awaits is refused as it starts, and so is one
    // that goes on past what was awaited.
    void AwaitFile(int link, size_t bytes);

    struct Impl;

private:
    explicit QuicLinks(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
