// A shared panel's pictures on a link, each on a one-way stream of its own (quiclinks.h, Frame;
// docs/together.md, "The protocol"). QUIC sends a lost packet again, so a lost packet costs one
// picture a round trip. A picture that isn't all acknowledged in time is given up: its stream is
// reset, and the loop is told, so that the next one sent is a keyframe.
//
// What comes in is bounded: so many streams at once (kMostFrameStreams, which msquic holds the
// far end to), so many bytes a stream and on their way in on a link in all. A stream past that
// is refused, which its sender sees as a picture given up.
#include "quicimpl.h"

#include "../proto/framewire.h"

#include <algorithm>

namespace net {

struct FrameOut {
    QuicLinks::Impl *impl = nullptr;
    Conn *conn = nullptr;   // none once the link is gone (ForgetFrames)
    HQUIC stream = nullptr;
    uint32_t tag = 0;
    Clock::time_point sentAt;
    bool settled = false;   // all of it was acknowledged, or the loop was told it is lost
    QUIC_BUFFER buffer = {};
    std::vector<uint8_t> bytes;
};

namespace {

constexpr std::chrono::milliseconds kGiveUpAfter(150);
constexpr size_t kMostStream = proto::kFrameHead + proto::kMostFrame;   // a picture's stream, in bytes
constexpr size_t kMostFramesIn = 4u << 20;   // bytes of the pictures on their way in on one link
constexpr size_t kMostFramesOut = 64;        // pictures on their way out on one link: more aren't sent

struct FrameIn {
    Conn *conn = nullptr;
    std::vector<uint8_t> bytes;
    bool refused = false;
};

// With the lock held.
void Lost(FrameOut &f) {
    if (f.settled) return;
    f.settled = true;
    if (!f.conn) return;
    Event e;
    e.kind = Event::kFrameLost, e.tag = f.tag;
    f.conn->impl->Post(*f.conn, std::move(e));
}

QUIC_STATUS QUIC_API OnFrameOut(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    FrameOut &f = *static_cast<FrameOut *>(context);
    QuicLinks::Impl &impl = *f.impl;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:   // with send buffering off: all of it was acknowledged, or it never will be
        if (ev->SEND_COMPLETE.Canceled) Lost(f);
        f.settled = true;
        break;
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:   // the far end won't take it
        Lost(f);
        impl.q->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;   // Frame() couldn't send it, and closes and frees it itself
        Lost(f);   // unless it was settled: a stream that ends any other way didn't deliver
        if (f.conn) f.conn->framesOut.erase(std::remove(f.conn->framesOut.begin(), f.conn->framesOut.end(), &f), f.conn->framesOut.end());
        impl.q->StreamClose(stream);
        delete &f;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API OnFrameIn(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    FrameIn &f = *static_cast<FrameIn *>(context);
    Conn &c = *f.conn;
    QuicLinks::Impl &impl = *c.impl;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    const auto forget = [&] {
        c.framesIn -= std::min(c.framesIn, f.bytes.size());
        f.bytes.clear(), f.bytes.shrink_to_fit();
    };
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (f.refused) break;
        size_t more = 0;
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) more += ev->RECEIVE.Buffers[i].Length;
        if (c.closed || f.bytes.size() + more > kMostStream || c.framesIn + more > kMostFramesIn) {
            f.refused = true;
            forget();
            impl.q->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
            break;
        }
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
            f.bytes.insert(f.bytes.end(), b.Buffer, b.Buffer + b.Length);
        }
        c.framesIn += more;
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {   // all of it is here
        if (f.refused || f.bytes.empty()) break;
        c.framesIn -= std::min(c.framesIn, f.bytes.size());
        Event e;
        e.kind = Event::kFrame, e.bytes = std::move(f.bytes);
        f.bytes.clear();
        // a loop that isn't taking what waits gets no pictures on top of it: a late one is worth nothing
        if (c.up && c.framesQueued + e.bytes.size() <= kMostFramesIn) impl.Post(c, std::move(e));
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:   // its sender gave it up
        forget();
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        forget();
        impl.q->StreamClose(stream);
        delete &f;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

}  // namespace

void TakeFrameStream(QuicLinks::Impl &impl, Conn &c, HQUIC stream) {
    auto *f = new FrameIn;
    f->conn = &c;
    impl.q->SetCallbackHandler(stream, reinterpret_cast<void *>(OnFrameIn), f);
    if (c.closed || !c.up) {
        f->refused = true;
        impl.q->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
    }
}

void ForgetFrames(Conn &c) {
    for (FrameOut *f : c.framesOut) f->conn = nullptr, f->settled = true;
    c.framesOut.clear();
}

void GiveUpLateFrames(QuicLinks::Impl &impl, Clock::time_point now) {
    for (const auto &[id, c] : impl.conns)
        for (FrameOut *f : c->framesOut) {
            if (f->settled || now - f->sentAt < kGiveUpAfter) continue;
            Lost(*f);
            impl.q->StreamShutdown(f->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
        }
}

bool QuicLinks::Frame(int link, std::vector<uint8_t> bytes, uint32_t tag) {
    Impl &impl = *impl_;
    std::unique_lock<std::recursive_mutex> lock(impl.m);
    Conn *c = impl.Find(link);
    if (!c || c->closed || !c->up || bytes.empty() || bytes.size() > kMostStream || c->framesOut.size() >= kMostFramesOut) return false;
    auto *f = new FrameOut;
    f->impl = &impl, f->conn = c, f->tag = tag, f->sentAt = Clock::now(), f->bytes = std::move(bytes);
    f->buffer.Buffer = f->bytes.data(), f->buffer.Length = uint32_t(f->bytes.size());
    if (QUIC_FAILED(impl.q->StreamOpen(c->conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, OnFrameOut, f, &f->stream))) {
        delete f;
        return false;
    }
    // Once the send is taken the stream's shutdown always comes (msquic starts it with
    // SHUTDOWN_ON_FAIL), and frees it.
    if (QUIC_SUCCEEDED(impl.q->StreamSend(f->stream, &f->buffer, 1, QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, nullptr))) {
        c->framesOut.push_back(f);
        return true;
    }
    lock.unlock();   // closing waits for msquic's thread, whose callbacks take the lock
    impl.q->StreamClose(f->stream);
    delete f;
    return false;
}

bool QuicLinks::Sending() {
    std::lock_guard<std::recursive_mutex> lock(impl_->m);
    for (const auto &[id, c] : impl_->conns)
        for (const FrameOut *f : c->framesOut)
            if (!f->settled) return true;
    return false;
}

}  // namespace net
