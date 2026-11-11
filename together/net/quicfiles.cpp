// A file on a link, on a one-way stream of its own (quiclinks.h, File and AwaitFile;
// docs/together.md, "The protocol"): an avatar's model, which may be megabytes. The stream's
// first byte says what it carries (proto::kLookStream), and the file follows to its end.
//
// A file goes out behind everything else on the link (the lowest stream priority), one at a
// time. What comes in is bounded by what the loop asked for: a file is taken only while one is
// awaited, one at a time, and only as many bytes as were awaited. Any other is refused as it
// starts, before its bytes are held.
#include "quicimpl.h"

#include "../proto/messages.h"

#include <algorithm>

namespace net {
namespace {

constexpr int kMostFilesOut = 1;   // on their way out on one link

struct FileOut {
    QuicLinks::Impl *impl = nullptr;
    int link = -1;
    HQUIC stream = nullptr;
    uint8_t kind = proto::kLookStream;
    QUIC_BUFFER buffers[2] = {};
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

struct FileIn {
    QuicLinks::Impl *impl = nullptr;
    int link = -1;
    std::vector<uint8_t> bytes;
    size_t most = 0;
    size_t skip = 1;        // the stream's first byte isn't the file's
    bool taking = false;    // it is the one file its link takes now
    bool refused = false;
};

QUIC_STATUS QUIC_API OnFileOut(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    FileOut &f = *static_cast<FileOut *>(context);
    QuicLinks::Impl &impl = *f.impl;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE: {
        // The pictures, the control stream and the voices go first. Set here, on msquic's
        // thread, before any of the file is in a packet: set from File() it would wait for this
        // thread while holding the lock this thread's callbacks take.
        const uint16_t last = 0;
        if (QUIC_SUCCEEDED(ev->START_COMPLETE.Status)) impl.q->SetParam(stream, QUIC_PARAM_STREAM_PRIORITY, sizeof last, &last);
        break;
    }
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:   // the far end won't take it
        impl.q->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (ev->SHUTDOWN_COMPLETE.AppCloseInProgress) break;   // File() couldn't send it, and closes and frees it itself
        if (Conn *c = impl.Find(f.link)) --c->filesOut;
        impl.q->StreamClose(stream);
        delete &f;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

// With the lock held.
void Refuse(FileIn &f, HQUIC stream) {
    f.refused = true;
    f.bytes.clear(), f.bytes.shrink_to_fit();
    f.impl->q->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
}

void Receive(FileIn &f, HQUIC stream, const QUIC_STREAM_EVENT *ev) {
    if (f.refused) return;
    const Conn *c = f.impl->Find(f.link);
    if (!c || c->closed) return Refuse(f, stream);
    for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
        const size_t skipped = std::min<size_t>(f.skip, b.Length);
        f.skip -= skipped;
        if (f.bytes.size() + (b.Length - skipped) > f.most) return Refuse(f, stream);
        f.bytes.insert(f.bytes.end(), b.Buffer + skipped, b.Buffer + b.Length);
    }
}

QUIC_STATUS QUIC_API OnFileIn(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    FileIn &f = *static_cast<FileIn *>(context);
    QuicLinks::Impl &impl = *f.impl;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        Receive(f, stream, ev);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {   // all of it is here
        Conn *c = impl.Find(f.link);
        if (f.refused || !c || !c->up) break;
        Event e;
        e.kind = Event::kFile, e.bytes = std::move(f.bytes);
        f.bytes.clear();
        impl.Post(*c, std::move(e));
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:   // its sender gave it up
        f.refused = true;
        f.bytes.clear(), f.bytes.shrink_to_fit();
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (Conn *c = f.taking ? impl.Find(f.link) : nullptr) c->fileIn = false;
        impl.q->StreamClose(stream);
        delete &f;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

}  // namespace

void TakeFileStream(QuicLinks::Impl &impl, Conn &c, HQUIC stream, QUIC_STREAM_EVENT *ev) {
    auto *f = new FileIn;
    f->impl = &impl, f->link = c.id;
    impl.q->SetCallbackHandler(stream, reinterpret_cast<void *>(OnFileIn), f);
    if (c.closed || !c.up || c.fileIn || c.fileAwaited == 0) return Refuse(*f, stream);
    f->most = c.fileAwaited, f->taking = true;
    c.fileIn = true, c.fileAwaited = 0;
    Receive(*f, stream, ev);
}

bool QuicLinks::File(int link, std::shared_ptr<const std::vector<uint8_t>> bytes) {
    Impl &impl = *impl_;
    std::unique_lock<std::recursive_mutex> lock(impl.m);
    Conn *c = impl.Find(link);
    if (!c || c->closed || !c->up || !bytes || bytes->empty() || bytes->size() > proto::kMostLook || c->filesOut >= kMostFilesOut) return false;
    auto *f = new FileOut;
    f->impl = &impl, f->link = link, f->bytes = std::move(bytes);
    f->buffers[0].Buffer = &f->kind, f->buffers[0].Length = 1;
    f->buffers[1].Buffer = const_cast<uint8_t *>(f->bytes->data()), f->buffers[1].Length = uint32_t(f->bytes->size());
    if (QUIC_FAILED(impl.q->StreamOpen(c->conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, OnFileOut, f, &f->stream))) {
        delete f;
        return false;
    }
    // Once the send is taken the stream's shutdown always comes (msquic starts it with
    // SHUTDOWN_ON_FAIL), and frees it.
    if (QUIC_SUCCEEDED(impl.q->StreamSend(f->stream, f->buffers, 2, QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, nullptr))) {
        ++c->filesOut;
        return true;
    }
    lock.unlock();   // closing waits for msquic's thread, whose callbacks take the lock
    impl.q->StreamClose(f->stream);
    delete f;
    return false;
}

void QuicLinks::AwaitFile(int link, size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(impl_->m);
    if (Conn *c = impl_->Find(link)) c->fileAwaited = c->closed ? 0 : std::min<size_t>(bytes, proto::kMostLook);
}

}  // namespace net
