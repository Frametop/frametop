// The links between Frames, on msquic (quiclinks.h): the connections, the control stream and
// the datagrams. The streams that carry a picture each are quicframes.cpp's, the ones that carry
// a file quicfiles.cpp's.
#include "quicimpl.h"

#include "lookup.h"
#include "selfcert.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

namespace net {
namespace {

const QUIC_BUFFER kAlpn = {sizeof("ft-together") - 1, (uint8_t *)"ft-together"};

constexpr size_t kMostLinks = 32;                   // connections at once, the ones being closed too
constexpr size_t kMostQueued = 1u << 20;            // bytes of one link the loop hasn't taken: more, and the link is cut
constexpr size_t kMostDatagramsQueued = 256u << 10; // ...and with this many waiting, its datagrams are dropped
constexpr size_t kMostWaiting = 64u << 10;          // bytes sent to a caller whose stream isn't there yet
constexpr int kMostDatagramsOut = 128;              // datagrams msquic holds for one link: a late one is worth nothing
constexpr std::chrono::seconds kCloseWait(5);       // a closed link gets this long to take what was sent

std::string Text(const QUIC_ADDR &a) {
    char buf[INET6_ADDRSTRLEN] = "";
    if (a.Ip.sa_family == AF_INET) {
        inet_ntop(AF_INET, &a.Ipv4.sin_addr, buf, sizeof buf);
    } else if (a.Ip.sa_family == AF_INET6) {
        if (IN6_IS_ADDR_V4MAPPED(&a.Ipv6.sin6_addr)) inet_ntop(AF_INET, a.Ipv6.sin6_addr.s6_addr + 12, buf, sizeof buf);
        else inet_ntop(AF_INET6, &a.Ipv6.sin6_addr, buf, sizeof buf);
    }
    return buf;
}

// Bytes for msquic to send, which it hands back when it is done with them.
QUIC_BUFFER *Copy(const std::vector<uint8_t> &bytes) {
    auto *b = static_cast<QUIC_BUFFER *>(std::malloc(sizeof(QUIC_BUFFER) + bytes.size()));
    if (!b) return nullptr;
    b->Length = uint32_t(bytes.size());
    b->Buffer = reinterpret_cast<uint8_t *>(b + 1);
    std::memcpy(b->Buffer, bytes.data(), bytes.size());
    return b;
}

QUIC_STATUS QUIC_API Nothing(HQUIC, void *, QUIC_CONNECTION_EVENT *) { return QUIC_STATUS_SUCCESS; }

// What an event's bytes count against while the loop hasn't taken it: the link's pictures, or
// the rest. A file counts against neither: the loop asked for it, one at a time.
size_t *Queued(Conn &c, Event::Kind kind) { return kind == Event::kFile ? nullptr : kind == Event::kFrame ? &c.framesQueued : &c.queued; }

}  // namespace

QuicLinks::Impl::~Impl() {
    {
        std::lock_guard<std::recursive_mutex> lock(m);
        stopping = true;
    }
    if (q) {
        // Not under the lock: these wait for msquic's threads, whose callbacks take it. Each
        // connection is closed and freed by its last callback.
        if (listener) q->ListenerClose(listener);
        if (registration) q->RegistrationShutdown(registration, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);   // the far ends are told
        if (serverConfig) q->ConfigurationClose(serverConfig);
        if (clientConfig) q->ConfigurationClose(clientConfig);
        if (registration) q->RegistrationClose(registration);
        MsQuicClose(q);
    }
    if (wake >= 0) close(wake);
}

HQUIC QuicLinks::Impl::Configure(const QUIC_CREDENTIAL_CONFIG &cred, uint16_t peerStreams) {
    QUIC_SETTINGS s = {};
    s.IdleTimeoutMs = 10000;
    s.IsSet.IdleTimeoutMs = TRUE;
    s.KeepAliveIntervalMs = 2000;
    s.IsSet.KeepAliveIntervalMs = TRUE;
    s.DatagramReceiveEnabled = TRUE;
    s.IsSet.DatagramReceiveEnabled = TRUE;
    s.PeerBidiStreamCount = peerStreams;   // the control stream, which the caller opens
    s.IsSet.PeerBidiStreamCount = TRUE;
    s.PeerUnidiStreamCount = kMostFrameStreams;   // the pictures on their way in, a stream each
    s.IsSet.PeerUnidiStreamCount = TRUE;
    s.SendBufferingEnabled = FALSE;        // a send is done when the far end has acknowledged it
    s.IsSet.SendBufferingEnabled = TRUE;
    s.CongestionControlAlgorithm = QUIC_CONGESTION_CONTROL_ALGORITHM_BBR;
    s.IsSet.CongestionControlAlgorithm = TRUE;
    HQUIC config = nullptr;
    if (QUIC_FAILED(q->ConfigurationOpen(registration, &kAlpn, 1, &s, sizeof s, nullptr, &config))) return nullptr;
    if (QUIC_FAILED(q->ConfigurationLoadCredential(config, &cred))) {
        q->ConfigurationClose(config);
        return nullptr;
    }
    return config;
}

void QuicLinks::Impl::Post(Conn &c, Event e) {
    if (c.closed || stopping) return;
    e.link = c.id;
    if (size_t *queued = Queued(c, e.kind)) *queued += e.bytes.size() + kEventCost;
    queue.push_back(std::move(e));
    const uint64_t one = 1;
    const ssize_t woke = write(wake, &one, sizeof one);
    (void)woke;   // it fails only with the counter full, and then the fd is readable already
}

void QuicLinks::Impl::Write(Conn &c, const std::vector<uint8_t> &bytes) {
    QUIC_BUFFER *b = Copy(bytes);
    if (b && QUIC_FAILED(q->StreamSend(c.stream, b, 1, QUIC_SEND_FLAG_NONE, b))) std::free(b);
}

void QuicLinks::Impl::Cut(Conn &c) { q->ConnectionShutdown(c.conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0); }

Conn *QuicLinks::Impl::Find(int link) {
    const auto it = conns.find(link);
    return it == conns.end() ? nullptr : it->second;
}

QUIC_STATUS QUIC_API QuicLinks::Impl::OnListener(HQUIC, void *context, QUIC_LISTENER_EVENT *ev) {
    Impl &impl = *static_cast<Impl *>(context);
    if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    if (impl.stopping || impl.conns.size() >= kMostLinks) return QUIC_STATUS_CONNECTION_REFUSED;
    auto *c = new Conn;
    c->impl = &impl, c->id = impl.nextLink++, c->conn = ev->NEW_CONNECTION.Connection;
    c->address = Text(*ev->NEW_CONNECTION.Info->RemoteAddress);
    impl.q->SetCallbackHandler(c->conn, reinterpret_cast<void *>(OnConnection), c);
    const QUIC_STATUS status = impl.q->ConnectionSetConfiguration(c->conn, impl.serverConfig);
    if (QUIC_FAILED(status)) {   // msquic drops it, and nothing of ours is told
        impl.q->SetCallbackHandler(c->conn, reinterpret_cast<void *>(Nothing), nullptr);
        delete c;
        return status;
    }
    impl.conns[c->id] = c;
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicLinks::Impl::OnConnection(HQUIC conn, void *context, QUIC_CONNECTION_EVENT *ev) {
    Conn &c = *static_cast<Conn *>(context);
    Impl &impl = *c.impl;
    std::unique_lock<std::recursive_mutex> lock(impl.m);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        if (c.failed || c.closed) break;
        if (c.caller) {
            QUIC_ADDR remote = {};
            uint32_t len = sizeof remote;
            if (QUIC_SUCCEEDED(impl.q->GetParam(conn, QUIC_PARAM_CONN_REMOTE_ADDRESS, &len, &remote))) c.address = Text(remote);
            HQUIC stream = nullptr;
            if (QUIC_FAILED(impl.q->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE, OnStream, &c, &stream))) {
                impl.Cut(c);
                break;
            }
            c.stream = stream;
            if (QUIC_FAILED(impl.q->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
                impl.q->StreamClose(stream);
                c.stream = nullptr;
                impl.Cut(c);
                break;
            }
        }
        c.up = true;
        Event e;
        e.kind = Event::kUp, e.address = c.address;
        impl.Post(c, std::move(e));
        break;
    }
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        const HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
        if (ev->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) {   // a picture, or a file
            TakeFrameStream(impl, c, stream);
            break;
        }
        if (c.caller || c.hadStream || c.closed) {   // a link has one control stream, and its caller opens it
            impl.q->StreamClose(stream);
            break;
        }
        c.stream = stream, c.hadStream = true;
        impl.q->SetCallbackHandler(stream, reinterpret_cast<void *>(OnStream), &c);
        for (const std::vector<uint8_t> &bytes : c.waiting) impl.Write(c, bytes);
        c.waiting.clear(), c.waitingBytes = 0;
        break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        c.maxDatagram = ev->DATAGRAM_STATE_CHANGED.SendEnabled ? ev->DATAGRAM_STATE_CHANGED.MaxSendLength : 0;
        break;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        const QUIC_BUFFER *b = ev->DATAGRAM_RECEIVED.Buffer;
        if (!c.up || b->Length == 0 || b->Length > kMostDatagram || c.queued > kMostDatagramsQueued) break;
        Event e;
        e.kind = Event::kDatagram, e.bytes.assign(b->Buffer, b->Buffer + b->Length);
        impl.Post(c, std::move(e));
        break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(ev->DATAGRAM_SEND_STATE_CHANGED.State)) {
            std::free(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            --c.datagramsOut;
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        if (c.failed) break;   // Call closes and frees it
        Event e;
        e.kind = Event::kDown;
        impl.Post(c, std::move(e));
        impl.conns.erase(c.id);
        ForgetFrames(c);
        lock.unlock();
        impl.q->ConnectionClose(conn);
        delete &c;
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicLinks::Impl::OnStream(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    Conn &c = *static_cast<Conn *>(context);
    Impl &impl = *c.impl;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        Event e;
        e.kind = Event::kBytes;
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
            e.bytes.insert(e.bytes.end(), b.Buffer, b.Buffer + b.Length);
        }
        if (!e.bytes.empty()) impl.Post(c, std::move(e));
        if (c.queued > kMostQueued) impl.Cut(c);   // the session reads faster than anyone has reason to write
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        std::free(ev->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        impl.Cut(c);   // a link without its control stream is no link
        break;
    case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
        if (c.closed) impl.Cut(c);   // what was sent before Close has gone
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (c.stream == stream) c.stream = nullptr;
        if (!ev->SHUTDOWN_COMPLETE.AppCloseInProgress) impl.q->StreamClose(stream);   // or whoever closes it is doing so now
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QuicLinks::QuicLinks(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
QuicLinks::~QuicLinks() = default;

std::unique_ptr<QuicLinks> QuicLinks::Open(const Config &config, std::string &err) {
    auto impl = std::make_unique<Impl>();
    impl->mayCall = config.mayCall;
    impl->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (impl->wake < 0 || QUIC_FAILED(MsQuicOpen2(&impl->q))) return err = "QUIC didn't start", nullptr;
    const QUIC_REGISTRATION_CONFIG reg = {"ft-together", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(impl->q->RegistrationOpen(&reg, &impl->registration))) return err = "QUIC didn't start", nullptr;

    QUIC_CREDENTIAL_CONFIG client = {};
    client.Type = QUIC_CREDENTIAL_TYPE_NONE;
    client.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    impl->clientConfig = impl->Configure(client, 0);
    if (!impl->clientConfig) return err = "QUIC didn't take its settings", nullptr;
    if (!config.port) return std::unique_ptr<QuicLinks>(new QuicLinks(std::move(impl)));   // it only calls

    // The listener's certificate: made new each time, in a folder of our own, gone once loaded.
    const char *run = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(run && *run ? run : "/tmp") + "/ft-together-XXXXXX";
    if (!mkdtemp(&dir[0])) return err = "couldn't make a folder for the certificate", nullptr;
    const std::string key = dir + "/key.pem", cert = dir + "/cert.pem";
    const bool made = selfcert::Make(key, cert, err);
    if (made) {
        const QUIC_CERTIFICATE_FILE files = {key.c_str(), cert.c_str()};
        QUIC_CREDENTIAL_CONFIG server = {};
        server.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        server.CertificateFile = const_cast<QUIC_CERTIFICATE_FILE *>(&files);
        impl->serverConfig = impl->Configure(server, 1);
    }
    unlink(key.c_str()), unlink(cert.c_str()), rmdir(dir.c_str());
    if (!made) return nullptr;
    if (!impl->serverConfig) return err = "QUIC didn't take its settings", nullptr;

    QUIC_ADDR addr = {};
    if (!QuicAddrFromString(config.listen.c_str(), config.port, &addr)) return err = "'" + config.listen + "' isn't an address", nullptr;
    const std::string taken = "couldn't listen on " + config.listen + " port " + std::to_string(config.port) + " (is it taken?)";
    // msquic shares its port with whoever else asks for it (SO_REUSEPORT), and the two would
    // each get some of the calls. A plain socket doesn't share: it tells whether the port is free.
    const int probe = socket(addr.Ip.sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    const bool isFree = probe >= 0 && bind(probe, &addr.Ip, addr.Ip.sa_family == AF_INET ? sizeof addr.Ipv4 : sizeof addr.Ipv6) == 0;
    if (probe >= 0) close(probe);
    if (!isFree) return err = taken, nullptr;
    if (QUIC_FAILED(impl->q->ListenerOpen(impl->registration, Impl::OnListener, impl.get(), &impl->listener)) ||
        QUIC_FAILED(impl->q->ListenerStart(impl->listener, &kAlpn, 1, &addr)))
        return err = taken, nullptr;
    return std::unique_ptr<QuicLinks>(new QuicLinks(std::move(impl)));
}

int QuicLinks::fd() const { return impl_->wake; }

bool QuicLinks::Next(Event &event) {
    Impl &impl = *impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    if (impl.queue.empty()) {
        uint64_t count = 0;
        const ssize_t slept = read(impl.wake, &count, sizeof count);   // the fd isn't readable any more
        (void)slept;
        return false;
    }
    event = std::move(impl.queue.front());
    impl.queue.pop_front();
    Conn *c = impl.Find(event.link);
    if (size_t *queued = c ? Queued(*c, event.kind) : nullptr) *queued -= std::min(*queued, event.bytes.size() + kEventCost);
    return true;
}

void QuicLinks::Tick() {
    Impl &impl = *impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    const Clock::time_point now = Clock::now();
    for (const auto &[id, c] : impl.conns)
        if (c->closed && now - c->closedAt > kCloseWait) impl.Cut(*c);
    GiveUpLateFrames(impl, now);
}

bool QuicLinks::Quiet() {
    std::lock_guard<std::recursive_mutex> lock(impl_->m);
    return impl_->conns.empty();
}

int QuicLinks::Call(const std::string &host, uint16_t port) {
    Impl &impl = *impl_;
    const std::string address = Lookup(host, impl.mayCall);
    if (address.empty()) return -1;
    auto *c = new Conn;
    c->impl = &impl, c->caller = true, c->address = address;
    {
        std::lock_guard<std::recursive_mutex> lock(impl.m);
        if (impl.conns.size() >= kMostLinks || QUIC_FAILED(impl.q->ConnectionOpen(impl.registration, Impl::OnConnection, c, &c->conn))) {
            delete c;
            return -1;
        }
        c->id = impl.nextLink++;
        if (QUIC_SUCCEEDED(impl.q->ConnectionStart(c->conn, impl.clientConfig, QUIC_ADDRESS_FAMILY_UNSPEC, address.c_str(), port))) {
            impl.conns[c->id] = c;
            return c->id;
        }
        c->failed = true;
    }
    impl.q->ConnectionClose(c->conn);   // not under the lock: it waits for msquic's thread
    delete c;
    return -1;
}

void QuicLinks::Send(int link, const std::vector<uint8_t> &message) {
    Impl &impl = *impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    Conn *c = impl.Find(link);
    if (!c || c->closed || message.empty()) return;
    if (c->stream) return impl.Write(*c, message);
    // Without a stream: a caller's link isn't up yet, or the stream has ended and the link is
    // going down. The called end's stream is the caller's to open, and comes a moment after Up.
    if (c->caller || c->hadStream || c->waitingBytes + message.size() > kMostWaiting) return;
    c->waiting.push_back(message), c->waitingBytes += message.size();
}

void QuicLinks::Close(int link) {
    Impl &impl = *impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    // nothing more comes from it: what already waits in the queue goes too
    impl.queue.erase(std::remove_if(impl.queue.begin(), impl.queue.end(), [link](const Event &e) { return e.link == link; }), impl.queue.end());
    Conn *c = impl.Find(link);
    if (!c || c->closed) return;
    c->closed = true, c->closedAt = Clock::now(), c->queued = c->framesQueued = 0;
    if (c->stream) impl.q->StreamShutdown(c->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    else impl.Cut(*c);
}

bool QuicLinks::Datagram(int link, const std::vector<uint8_t> &bytes) {
    Impl &impl = *impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.m);
    Conn *c = impl.Find(link);
    if (!c || c->closed || !c->up || bytes.empty() || bytes.size() > std::min<size_t>(c->maxDatagram, kMostDatagram) || c->datagramsOut >= kMostDatagramsOut) return false;
    QUIC_BUFFER *b = Copy(bytes);
    if (!b) return false;
    if (QUIC_FAILED(impl.q->DatagramSend(c->conn, b, 1, QUIC_SEND_FLAG_NONE, b))) return std::free(b), false;
    ++c->datagramsOut;
    return true;
}

}  // namespace net
