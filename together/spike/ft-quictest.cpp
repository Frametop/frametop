// ft-quictest: spike T3 for Frametop Together (docs/together.md). Sends what a session would
// send, panels' video, over one msquic connection in one of two ways, and reports what arrived.
//
//   ft-quictest [--listen | --connect HOST] [--port p] [--panels n] [--bitrate bits] [--fps f]
//               [--seconds s] [--bbr] [--drop] [--streams] [--deadline ms]
//
// With neither --listen nor --connect, both ends run in this process over 127.0.0.1. Each panel
// sends --bitrate as --fps frames a second. By default each frame is cut into datagrams as large
// as the link lets them be, and the receiver counts a frame only when every piece of it arrived;
// --drop has msquic drop a datagram it can't send at once, where it would otherwise queue it.
// With --streams each frame is a QUIC stream of its own: lost packets are sent again, and the
// sender gives a frame up when it hasn't all been acknowledged --deadline ms after it was sent
// (default 150). --bbr uses BBR congestion control in place of CUBIC.
//
// The sender reports the largest datagram the link took (at the start and at the end, after
// msquic has probed the path), the path's MTU and round trip, and datagrams sent, acknowledged and
// lost. The receiver reports whole and broken frames, and the time from a frame's first piece
// being sent to its last arriving (only meaningful when both ends share a clock: one machine).
//
// A link like Tailscale's without a second machine: run it in a network namespace whose loopback
// has Tailscale's MTU, with delay and loss added:
//   unshare -Urn sh -c 'ip link set lo up; ip link set lo mtu 1280;
//     tc qdisc add dev lo root netem delay 20ms loss 1%; exec build/ft-quictest'
// Build: together/build.sh --spikes.
#include "../net/selfcert.h"

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1   // BBR
#include <msquic.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

const QUIC_API_TABLE *Q = nullptr;
const QUIC_BUFFER kAlpn = {sizeof("ft-together-spike") - 1, (uint8_t *)"ft-together-spike"};

struct Args {
    bool listen = false, bbr = false, drop = false, streams = false;
    const char *connect = nullptr;
    int port = 47193, panels = 3, bitrate = 3000000, fps = 30, seconds = 10, deadlineMs = 150;
};

// What every video datagram, and every frame's stream, starts with.
struct Header {
    uint8_t panel;
    uint8_t pad[3];
    uint16_t piece, pieces;
    uint32_t frame;
    int64_t sentUs;   // when the frame's first piece was handed to msquic
};

int64_t NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// One end of the connection: what it counted.
struct End {
    const char *name = "";
    std::atomic<HQUIC> conn{nullptr};
    std::atomic<bool> connected{false}, closed{false};
    std::atomic<int> maxSend{0};
    std::atomic<uint64_t> sent{0}, acked{0}, lost{0}, canceled{0}, refused{0};
    std::atomic<uint64_t> framesSent{0}, framesDelivered{0}, framesGivenUp{0};

    std::mutex m;   // the rest: the receiver's counts
    struct Frame {
        int got = 0, pieces = 0;
    };
    struct Panel {
        std::unordered_map<uint32_t, Frame> open;
        uint32_t last = 0;
        bool any = false;
        int whole = 0;
    };
    Panel panels[16];
    uint64_t datagrams = 0, bytes = 0;
    int64_t latencySum = 0, latencyMax = 0;
};

struct Sending {
    QUIC_BUFFER buffer;
    uint8_t data[1];
};

// A frame on a stream of its own. The sender's is in `g_out` from StreamOpen until its stream
// has shut down; the receiver's only counts what arrives.
struct FrameStream {
    End *end = nullptr;
    HQUIC stream = nullptr;
    int64_t startUs = 0;
    bool givenUp = false, delivered = false;   // the sender's, under g_outLock
    Header header = {};
    size_t got = 0;
    QUIC_BUFFER buffer = {};
    std::vector<uint8_t> data;
};
std::mutex g_outLock;
std::vector<FrameStream *> g_out;

void FrameArrived(End &e, const Header &h) {
    if (h.panel >= 16) return;
    std::lock_guard<std::mutex> lock(e.m);
    End::Panel &p = e.panels[h.panel];
    p.last = p.any ? std::max(p.last, h.frame) : h.frame;
    p.any = true;
    ++p.whole;
    const int64_t lat = NowUs() - h.sentUs;
    e.latencySum += lat;
    e.latencyMax = std::max(e.latencyMax, lat);
}

QUIC_STATUS QUIC_API OnStream(HQUIC stream, void *context, QUIC_STREAM_EVENT *ev) {
    FrameStream &f = *static_cast<FrameStream *>(context);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
            for (uint32_t k = 0; k < b.Length && f.got + k < sizeof f.header; ++k)
                reinterpret_cast<uint8_t *>(&f.header)[f.got + k] = b.Buffer[k];
            f.got += b.Length;
            std::lock_guard<std::mutex> lock(f.end->m);
            ++f.end->datagrams;
            f.end->bytes += b.Length;
        }
        if ((ev->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) && f.got >= sizeof f.header) FrameArrived(*f.end, f.header);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED: {   // the sender gave the frame up
        std::lock_guard<std::mutex> lock(f.end->m);
        if (f.got >= sizeof f.header && f.header.panel < 16) {
            End::Panel &p = f.end->panels[f.header.panel];
            p.last = p.any ? std::max(p.last, f.header.frame) : f.header.frame;
            p.any = true;
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:   // with send buffering off: all of it was acknowledged
        if (!ev->SEND_COMPLETE.Canceled) {
            std::lock_guard<std::mutex> lock(g_outLock);
            f.delivered = true;
            ++f.end->framesDelivered;
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        {
            std::lock_guard<std::mutex> lock(g_outLock);
            g_out.erase(std::remove(g_out.begin(), g_out.end(), &f), g_out.end());
        }
        Q->StreamClose(stream);
        delete &f;
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

void Received(End &e, const QUIC_BUFFER *b) {
    Header h;
    if (b->Length < sizeof h) return;
    memcpy(&h, b->Buffer, sizeof h);
    if (h.panel >= 16 || h.pieces == 0) return;
    std::lock_guard<std::mutex> lock(e.m);
    ++e.datagrams;
    e.bytes += b->Length;
    End::Panel &p = e.panels[h.panel];
    p.last = p.any ? std::max(p.last, h.frame) : h.frame;
    p.any = true;
    End::Frame &f = p.open[h.frame];
    f.pieces = h.pieces;
    if (++f.got < f.pieces) return;
    p.open.erase(h.frame);
    ++p.whole;
    const int64_t lat = NowUs() - h.sentUs;
    e.latencySum += lat;
    e.latencyMax = std::max(e.latencyMax, lat);
}

QUIC_STATUS QUIC_API OnConnection(HQUIC conn, void *context, QUIC_CONNECTION_EVENT *ev) {
    End &e = *static_cast<End *>(context);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        e.connected = true;
        break;
    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        e.maxSend = ev->DATAGRAM_STATE_CHANGED.SendEnabled ? ev->DATAGRAM_STATE_CHANGED.MaxSendLength : 0;
        break;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        Received(e, ev->DATAGRAM_RECEIVED.Buffer);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        auto *f = new FrameStream;
        f->end = &e;
        Q->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream, (void *)OnStream, f);
        break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
        const QUIC_DATAGRAM_SEND_STATE s = ev->DATAGRAM_SEND_STATE_CHANGED.State;
        if (s == QUIC_DATAGRAM_SEND_SENT) ++e.sent;
        if (s == QUIC_DATAGRAM_SEND_ACKNOWLEDGED || s == QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS) ++e.acked;
        if (s == QUIC_DATAGRAM_SEND_LOST_DISCARDED) ++e.lost;
        if (s == QUIC_DATAGRAM_SEND_CANCELED) ++e.canceled;
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(s)) free(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        e.closed = true;
        Q->ConnectionClose(conn);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

struct Listening {
    End *end;
    HQUIC config;
};

QUIC_STATUS QUIC_API OnListener(HQUIC, void *context, QUIC_LISTENER_EVENT *ev) {
    Listening &l = *static_cast<Listening *>(context);
    if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;
    if (l.end->conn.load()) return QUIC_STATUS_CONNECTION_REFUSED;   // one peer is the whole test
    const HQUIC conn = ev->NEW_CONNECTION.Connection;
    l.end->conn = conn;
    Q->SetCallbackHandler(conn, (void *)OnConnection, l.end);
    return Q->ConnectionSetConfiguration(conn, l.config);
}

HQUIC Configure(HQUIC registration, const Args &a, const QUIC_CREDENTIAL_CONFIG &cred) {
    QUIC_SETTINGS s = {};
    s.IdleTimeoutMs = 10000;
    s.IsSet.IdleTimeoutMs = TRUE;
    s.KeepAliveIntervalMs = 2000;
    s.IsSet.KeepAliveIntervalMs = TRUE;
    s.DatagramReceiveEnabled = TRUE;
    s.IsSet.DatagramReceiveEnabled = TRUE;
    s.PeerUnidiStreamCount = 1024;   // frames in flight, each on its own stream
    s.IsSet.PeerUnidiStreamCount = TRUE;
    s.SendBufferingEnabled = FALSE;   // a send completes when the peer has acknowledged it
    s.IsSet.SendBufferingEnabled = TRUE;
    if (a.bbr) {
        s.CongestionControlAlgorithm = QUIC_CONGESTION_CONTROL_ALGORITHM_BBR;
        s.IsSet.CongestionControlAlgorithm = TRUE;
    }
    HQUIC config = nullptr;
    if (QUIC_FAILED(Q->ConfigurationOpen(registration, &kAlpn, 1, &s, sizeof s, nullptr, &config))) return nullptr;
    if (QUIC_FAILED(Q->ConfigurationLoadCredential(config, &cred))) {
        Q->ConfigurationClose(config);
        return nullptr;
    }
    return config;
}

// One frame on a new stream.
void SendOnStream(End &e, const Header &h, size_t frameBytes) {
    auto *f = new FrameStream;
    f->end = &e;
    f->startUs = h.sentUs;
    f->data.assign(sizeof h + frameBytes, 0);
    memcpy(f->data.data(), &h, sizeof h);
    f->buffer.Buffer = f->data.data();
    f->buffer.Length = uint32_t(f->data.size());
    if (QUIC_FAILED(Q->StreamOpen(e.conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, OnStream, f, &f->stream))) {
        ++e.refused;
        delete f;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_outLock);
        g_out.push_back(f);
    }
    ++e.framesSent;
    // A stream that fails to start still gets its shutdown event, which frees it.
    if (QUIC_FAILED(Q->StreamSend(f->stream, &f->buffer, 1, QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN, nullptr))) {
        ++e.refused;
        Q->StreamShutdown(f->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
}

// Gives up the frames that are older than the deadline and still not all acknowledged.
void GiveUpLate(End &e, int deadlineMs) {
    const int64_t now = NowUs();
    std::lock_guard<std::mutex> lock(g_outLock);
    for (FrameStream *f : g_out)
        if (!f->givenUp && !f->delivered && now - f->startUs > int64_t(deadlineMs) * 1000) {
            f->givenUp = true;
            ++e.framesGivenUp;
            Q->StreamShutdown(f->stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
        }
}

// Sends every panel's frames at the frame rate until the time is up.
void Send(End &e, const Args &a) {
    const int64_t period = 1000000 / a.fps, end = NowUs() + int64_t(a.seconds) * 1000000;
    const size_t frameBytes = size_t(a.bitrate) / 8 / a.fps;
    const QUIC_SEND_FLAGS flags = a.drop ? QUIC_SEND_FLAG_CANCEL_ON_BLOCKED : QUIC_SEND_FLAG_NONE;
    int64_t next = NowUs();
    for (uint32_t frame = 0; NowUs() < end && !e.closed; ++frame) {
        const int room = e.maxSend.load() - int(sizeof(Header));
        if (a.streams) {
            GiveUpLate(e, a.deadlineMs);
            for (int panel = 0; panel < a.panels; ++panel) {
                Header h = {};
                h.panel = uint8_t(panel);
                h.frame = frame;
                h.pieces = 1;
                h.sentUs = NowUs();
                SendOnStream(e, h, frameBytes);
            }
        } else if (room > 0) {
            for (int panel = 0; panel < a.panels; ++panel) {
                Header h = {};
                h.panel = uint8_t(panel);
                h.frame = frame;
                h.pieces = uint16_t((frameBytes + room - 1) / room);
                h.sentUs = NowUs();
                size_t left = frameBytes;
                for (h.piece = 0; h.piece < h.pieces; ++h.piece) {
                    const size_t body = std::min(left, size_t(room));
                    left -= body;
                    auto *s = static_cast<Sending *>(calloc(1, sizeof(Sending) + sizeof h + body));
                    memcpy(s->data, &h, sizeof h);
                    s->buffer.Buffer = s->data;
                    s->buffer.Length = uint32_t(sizeof h + body);
                    if (QUIC_FAILED(Q->DatagramSend(e.conn, &s->buffer, 1, flags, s))) {
                        ++e.refused;
                        free(s);
                    }
                }
            }
        }
        next += period;
        const int64_t wait = next - NowUs();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait));
    }
}

double CpuSeconds() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

void ReportReceiver(End &e, const Args &a) {
    std::lock_guard<std::mutex> lock(e.m);
    int whole = 0, expected = 0;
    for (const End::Panel &p : e.panels)
        if (p.any) whole += p.whole, expected += int(p.last) + 1;
    std::printf("%s received: %d of %d frames whole (%d broken), %llu pieces, %.0f kbit/s, frame latency avg %.1f ms max %.1f ms\n",
                e.name, whole, expected, expected - whole, (unsigned long long)e.datagrams, e.bytes * 8.0 / 1000 / a.seconds,
                whole ? e.latencySum / 1000.0 / whole : 0.0, e.latencyMax / 1000.0);
}

}  // namespace

int main(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (k == "--listen") a.listen = true;
        else if (k == "--bbr") a.bbr = true;
        else if (k == "--drop") a.drop = true;
        else if (k == "--streams") a.streams = true;
        else if (k == "--deadline") a.deadlineMs = std::atoi(v), ++i;
        else if (k == "--connect") a.connect = v, ++i;
        else if (k == "--port") a.port = std::atoi(v), ++i;
        else if (k == "--panels") a.panels = std::atoi(v), ++i;
        else if (k == "--bitrate") a.bitrate = std::atoi(v), ++i;
        else if (k == "--fps") a.fps = std::atoi(v), ++i;
        else if (k == "--seconds") a.seconds = std::atoi(v), ++i;
        else {
            std::fprintf(stderr, "ft-quictest: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (a.panels < 1 || a.panels > 16 || a.fps < 1 || a.seconds < 1 || a.bitrate < 8 * a.fps) {
        std::fprintf(stderr, "ft-quictest: bad --panels, --fps, --seconds or --bitrate\n");
        return 2;
    }
    const bool server = a.listen || !a.connect, client = !a.listen;
    if (QUIC_FAILED(MsQuicOpen2(&Q))) {
        std::fprintf(stderr, "ft-quictest: msquic did not open\n");
        return 1;
    }
    const QUIC_REGISTRATION_CONFIG reg = {"ft-quictest", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    HQUIC registration = nullptr;
    if (QUIC_FAILED(Q->RegistrationOpen(&reg, &registration))) return 1;

    End receiver, sender;
    receiver.name = "listener";
    sender.name = "caller";
    HQUIC listener = nullptr, serverConfig = nullptr, clientConfig = nullptr;
    Listening listening = {&receiver, nullptr};
    if (server) {
        const std::string dir = "/tmp/ft-quictest-" + std::to_string(getpid());
        const std::string key = dir + ".key", cert = dir + ".crt";
        std::string err;
        if (!selfcert::Make(key, cert, err)) {
            std::fprintf(stderr, "ft-quictest: %s\n", err.c_str());
            return 1;
        }
        QUIC_CERTIFICATE_FILE files = {key.c_str(), cert.c_str()};
        QUIC_CREDENTIAL_CONFIG cred = {};
        cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        cred.CertificateFile = &files;
        serverConfig = Configure(registration, a, cred);
        unlink(key.c_str());
        unlink(cert.c_str());
        listening.config = serverConfig;
        QUIC_ADDR addr = {};
        QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&addr, uint16_t(a.port));
        if (!serverConfig || QUIC_FAILED(Q->ListenerOpen(registration, OnListener, &listening, &listener)) ||
            QUIC_FAILED(Q->ListenerStart(listener, &kAlpn, 1, &addr))) {
            std::fprintf(stderr, "ft-quictest: could not listen on port %d\n", a.port);
            return 1;
        }
    }
    const double cpu0 = CpuSeconds();
    const int64_t t0 = NowUs();
    if (client) {
        // The spike's listener has a certificate that names nobody, so there is nothing to check.
        QUIC_CREDENTIAL_CONFIG cred = {};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        clientConfig = Configure(registration, a, cred);
        HQUIC conn = nullptr;
        const char *host = a.connect ? a.connect : "127.0.0.1";
        if (!clientConfig || QUIC_FAILED(Q->ConnectionOpen(registration, OnConnection, &sender, &conn)) ||
            QUIC_FAILED(Q->ConnectionStart(conn, clientConfig, QUIC_ADDRESS_FAMILY_UNSPEC, host, uint16_t(a.port)))) {
            std::fprintf(stderr, "ft-quictest: could not start a connection to %s\n", host);
            return 1;
        }
        sender.conn = conn;
        for (int i = 0; i < 500 && !(sender.connected && sender.maxSend > 0) && !sender.closed; ++i) Sleep(10);
        if (!sender.connected || sender.closed) {
            std::fprintf(stderr, "ft-quictest: no connection to %s port %d\n", host, a.port);
            return 1;
        }
        const int max0 = sender.maxSend;
        std::printf("%d panel(s) at %d kbit/s and %d fps for %d s, %s, %s; connected in %.0f ms\n", a.panels, a.bitrate / 1000,
                    a.fps, a.seconds, a.bbr ? "BBR" : "CUBIC",
                    a.streams ? "a stream a frame" : a.drop ? "datagrams dropped when blocked" : "datagrams",
                    (NowUs() - t0) / 1000.0);
        Send(sender, a);
        Sleep(300);   // the last frames' acknowledgements
        if (a.streams) GiveUpLate(sender, 0);
        QUIC_STATISTICS_V2 st = {};
        uint32_t len = sizeof st;
        Q->GetParam(conn, QUIC_PARAM_CONN_STATISTICS_V2, &len, &st);
        std::printf("caller sent: largest datagram %d bytes at the start, %d at the end, path MTU %u, round trip %.1f ms\n",
                    max0, sender.maxSend.load(), st.SendPathMtu, st.Rtt / 1000.0);
        if (a.streams)
            std::printf("caller sent: %llu frames, %llu acknowledged, %llu given up, %llu refused, %u congestion events\n",
                        (unsigned long long)sender.framesSent.load(), (unsigned long long)sender.framesDelivered.load(),
                        (unsigned long long)sender.framesGivenUp.load(), (unsigned long long)sender.refused.load(),
                        st.SendCongestionCount);
        else
            std::printf("caller sent: %llu datagrams, %llu acknowledged, %llu lost, %llu cancelled, %llu refused, %u congestion events\n",
                        (unsigned long long)sender.sent.load(), (unsigned long long)sender.acked.load(),
                        (unsigned long long)sender.lost.load(), (unsigned long long)sender.canceled.load(),
                        (unsigned long long)sender.refused.load(), st.SendCongestionCount);
        Q->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        for (int i = 0; i < 300 && !sender.closed; ++i) Sleep(10);
    }
    if (server) {
        if (!client) {
            std::printf("listening on port %d\n", a.port);
            while (!receiver.conn.load()) Sleep(50);
        }
        for (int i = 0; (i < 300 || !client) && !receiver.closed; ++i) Sleep(10);
        ReportReceiver(receiver, a);
    }
    const double wall = (NowUs() - t0) / 1e6;
    std::printf("CPU %.1f%% of one core over %.1f s\n", (CpuSeconds() - cpu0) / wall * 100, wall);
    if (listener) Q->ListenerClose(listener);
    if (serverConfig) Q->ConfigurationClose(serverConfig);
    if (clientConfig) Q->ConfigurationClose(clientConfig);
    Q->RegistrationClose(registration);
    MsQuicClose(Q);
    return 0;
}
