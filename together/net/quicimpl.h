// What is behind QuicLinks (quiclinks.h): a link's connection, and the state msquic's threads
// and the program's loop share. For net/'s own sources only: quiclinks.cpp has the connections,
// the control stream and the datagrams, quicframes.cpp the streams that carry a picture each.
#pragma once

#include "quiclinks.h"

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1   // BBR, which the video was measured with (spike T3)
#include <msquic.h>

#include <chrono>
#include <deque>
#include <map>
#include <mutex>

namespace net {

using Clock = std::chrono::steady_clock;

constexpr size_t kEventCost = 64;   // what a waiting event counts for beside its bytes, so a thousand tiny ones aren't free
constexpr uint16_t kMostFrameStreams = 32;   // pictures on their way in on one link, each on its stream

struct FrameOut;

struct Conn {
    QuicLinks::Impl *impl = nullptr;
    int id = -1;
    HQUIC conn = nullptr, stream = nullptr;
    bool caller = false;
    bool hadStream = false;   // the far end's control stream came
    bool up = false;
    bool closed = false;      // Close()d here: nothing more is told of it
    bool failed = false;      // Call couldn't start it, and closes it itself
    Clock::time_point closedAt;
    std::string address;
    std::vector<std::vector<uint8_t>> waiting;
    size_t waitingBytes = 0;
    size_t queued = 0, framesQueued = 0;   // bytes of its events the loop hasn't taken: pictures, and the rest
    int datagramsOut = 0;
    uint16_t maxDatagram = 0;
    std::vector<FrameOut *> framesOut;   // pictures sent, until their streams have shut down
    size_t framesIn = 0;                 // bytes of the pictures on their way in
};

struct QuicLinks::Impl {
    const QUIC_API_TABLE *q = nullptr;
    HQUIC registration = nullptr, serverConfig = nullptr, clientConfig = nullptr, listener = nullptr;
    std::function<bool(const std::string &)> mayCall;
    int wake = -1;
    // msquic may call back from inside a call made in one of its callbacks, so the lock is one
    // a thread can take twice.
    std::recursive_mutex m;
    std::map<int, Conn *> conns;
    std::deque<Event> queue;
    int nextLink = 1;
    bool stopping = false;

    ~Impl();
    HQUIC Configure(const QUIC_CREDENTIAL_CONFIG &cred, uint16_t peerStreams);
    // These with the lock held.
    void Post(Conn &c, Event e);
    void Write(Conn &c, const std::vector<uint8_t> &bytes);
    void Cut(Conn &c);
    Conn *Find(int link);

    static QUIC_STATUS QUIC_API OnListener(HQUIC, void *context, QUIC_LISTENER_EVENT *ev);
    static QUIC_STATUS QUIC_API OnConnection(HQUIC conn, void *context, QUIC_CONNECTION_EVENT *ev);
    static QUIC_STATUS QUIC_API OnStream(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev);
};

// quicframes.cpp, with the lock held. The far end opened a one-way stream: a picture is coming.
void TakeFrameStream(QuicLinks::Impl &impl, Conn &c, HQUIC stream);
// The pictures sent too long ago that still aren't all acknowledged are given up.
void GiveUpLateFrames(QuicLinks::Impl &impl, Clock::time_point now);
// The link is gone and `c` with it: what its pictures' streams say after this is nothing of its.
void ForgetFrames(Conn &c);

}  // namespace net
