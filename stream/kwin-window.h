// A KDE Plasma PC's app windows, one panel each, from utrack's KWin Window Stream host service
// (github.com/utrack/kwin-window-stream, MIT; issue 81). This receiver is utrack's
// (github.com/utrack/frametop, branch custom-window-streamer, stream/window-stream.h), adapted.
// Included after Viewer in ft-stream.cpp:
//   ft-stream kwin-windows HOST --id NAME     the windows the host shares, as its JSON
//   ft-stream stream HOST --id NAME --fd N --app kwin:{UUID} ...
//
// The host service is not a Moonlight host: it streams one KWin window as HEVC over its own TLS
// connection on TCP 14501. It knows clients by the certificates paired with the PC's Sunshine, so
// a display's client (paired already) is what connects, and the PC presents Sunshine's own
// certificate, whose key is the one pinned when the host was added (hosts/ADDRESS.pin). The pin
// is checked as the PC's certificate comes in, before this client's certificate goes out.
//
// Wire format: a type byte and a big-endian uint32 length, then the payload. J: JSON, V: one HEVC
// Annex-B access unit, C: a command (the ft-screens input messages, "size W H", "attention ...").
// The PC's sound comes on a connection of its own: A, a big-endian uint64 host monotonic time in
// ns and one stereo Opus packet. The PC's window sets the stream's size (follow_window): each new
// size comes as new J metadata and a new HEVC sequence, and ft-screens keeps the panel's density.

#include "audio-clock.h"
#include "kwin-frame.h"

constexpr int kKwinPort = 14501;

// The PC's certificate is self-signed, so its chain never checks out: its key's pin decides.
int KwinVerify(int, X509_STORE_CTX *store) {
    if (X509_STORE_CTX_get_error_depth(store) != 0) return 1;  // (only the PC's own matters)
    SSL *tls = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    const auto *pin = tls ? static_cast<const std::string *>(SSL_get_app_data(tls)) : nullptr;
    X509 *certificate = X509_STORE_CTX_get_current_cert(store);
    return pin && certificate && KwinKeyPin(certificate) == *pin;
}

struct KwinPeer {
    int fd = -1;
    SSL_CTX *context = nullptr;
    SSL *tls = nullptr;
    std::string pin;
    KwinFramer frames;
    std::deque<std::string> outgoing;
    size_t written = 0, queued = 0;
    bool readNeedsWrite = false;

    ~KwinPeer() {
        if (tls) SSL_free(tls);
        if (context) SSL_CTX_free(context);
        if (fd >= 0) close(fd);
    }

    void Connect(const Client &client) {
        std::ifstream kept(KeyDir("hosts") + "/" + g_host + ".pin");
        if (!(kept >> pin) || pin.rfind("sha256//", 0) != 0)
            throw std::runtime_error("sign in to this host in Remote Displays first (no pinned key)");
        context = SSL_CTX_new(TLS_client_method());
        if (!context) throw std::runtime_error("can't make a TLS context");
        SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, KwinVerify);
        if (SSL_CTX_use_certificate_file(context, (client.dir + "/client.pem").c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(context, (client.dir + "/key.pem").c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1)
            throw std::runtime_error("can't load this client's certificate");
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(client.host.c_str(), std::to_string(kKwinPort).c_str(), &hints, &addresses))
            throw std::runtime_error("can't resolve the host");
        for (const addrinfo *a = addresses; a; a = a->ai_next) {
            fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, a->ai_protocol);
            if (fd < 0) continue;
            timeval timeout{3, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
            if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(addresses);
        if (fd < 0) throw std::runtime_error("no KWin Window Stream answers on port 14501");
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        tls = SSL_new(context);
        if (!tls || SSL_set_fd(tls, fd) != 1 || SSL_set_app_data(tls, &pin) != 1)
            throw std::runtime_error("can't start TLS");
        if (SSL_connect(tls) != 1) {
            throw std::runtime_error(SSL_get_verify_result(tls) != X509_V_OK && SSL_get0_peer_certificate(tls)
                                         ? "the host's key isn't the one pinned when it was added"
                                         : "the TLS handshake failed (is this client paired with its Sunshine?)");
        }
        X509 *certificate = SSL_get1_peer_certificate(tls);
        const bool same = certificate && KwinKeyPin(certificate) == pin;
        if (certificate) X509_free(certificate);
        if (!same) throw std::runtime_error("the host's key isn't the one pinned when it was added");
        SSL_set_mode(tls, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_set_options(tls, SSL_OP_NO_RENEGOTIATION);
        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) throw std::runtime_error("can't make it nonblocking");
    }

    void Send(char type, const std::string &payload) {
        std::string message = KwinFrame(type, payload);
        if (queued + message.size() > 65536) throw std::runtime_error("the send queue is full");
        queued += message.size();
        outgoing.push_back(std::move(message));
    }

    void Flush() {
        while (!outgoing.empty()) {
            const std::string &message = outgoing.front();
            const int n = SSL_write(tls, message.data() + written, int(message.size() - written));
            if (n <= 0) {
                const int error = SSL_get_error(tls, n);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return;
                throw std::runtime_error("the connection failed to send");
            }
            written += size_t(n);
            if (written == message.size()) {
                queued -= message.size();
                outgoing.pop_front();
                written = 0;
            }
        }
    }

    bool Read(std::deque<KwinPacket> &packets) {
        bool progressed = false;
        while (frames.Parse(packets)) progressed = true;
        if (packets.size() >= kKwinQueued) return progressed;
        uint8_t bytes[kKwinReadChunk];
        const int n = SSL_read(tls, bytes, sizeof bytes);
        readNeedsWrite = false;
        if (n <= 0) {
            const int error = SSL_get_error(tls, n);
            if (error == SSL_ERROR_WANT_WRITE) readNeedsWrite = true;
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return progressed;
            throw std::runtime_error("the host hung up");
        }
        frames.Append(bytes, size_t(n));
        while (frames.Parse(packets)) {}
        return true;
    }
};

// The PC's sound, on its own connection. One stream per host plays it, the one holding the
// host's audio lock (Audio::Claim), so it plays once across the host's display and window panels.
// This thread never waits on the picture.
struct KwinAudio {
    std::atomic<bool> stopping{false};
    std::thread worker;
    int wake = -1;

    ~KwinAudio() {
        stopping = true;
        if (wake >= 0) {
            uint64_t one = 1;
            (void)!write(wake, &one, sizeof one);
        }
        if (worker.joinable()) worker.join();
        if (wake >= 0) close(wake);
    }

    void Wait(int milliseconds) {
        pollfd p{wake, POLLIN, 0};
        poll(&p, 1, milliseconds);
    }

    void Release() {
        AudioCleanup();
        g_audio.paFailed = false;
        g_audio.on = false;
        if (g_audio.lock >= 0) close(g_audio.lock);
        g_audio.lock = -1;
    }

    void Start(const Client &client) {
        if (worker.joinable()) return;
        wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake < 0) return void(std::fprintf(stderr, "kwin audio: no eventfd\n"));
        g_audio.name = "Remote display: " + g_host;
        worker = std::thread([this, &client] {
            while (!stopping) {
                g_audio.Claim(g_host, MonoNs());
                if (!g_audio.on) {
                    Wait(3000);
                    continue;
                }
                try {
                    Listen(client);
                } catch (const std::exception &error) {
                    std::fprintf(stderr, "kwin audio: %s; trying again\n", error.what());
                }
                Release();
                if (!stopping) Wait(10000);
            }
            Release();
        });
    }

    void Listen(const Client &client) {
        KwinPeer peer;
        peer.frames.mediaType = 'A';
        peer.Connect(client);
        peer.Send('J', R"({"version":1,"op":"audio"})");
        std::deque<KwinPacket> packets;
        bool configured = false, readMore = false;
        WindowAudioClock clock;
        int64_t lastPacket = MonoNs();
        while (!stopping) {
            peer.Flush();
            pollfd events[2] = {
                {peer.fd, short(POLLIN | ((!peer.outgoing.empty() || peer.readNeedsWrite) ? POLLOUT : 0)), 0},
                {wake, POLLIN, 0},
            };
            poll(events, 2, (readMore || !packets.empty() || SSL_pending(peer.tls)) ? 0 : 200);
            if (stopping) break;
            if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("its connection ended");
            // Records OpenSSL already holds are read even when the socket isn't readable.
            readMore = peer.Read(packets);
            while (!packets.empty()) {
                auto &p = packets.front();
                lastPacket = MonoNs();
                if (p.type == 'J') {
                    const auto metadata = nlohmann::json::parse(p.data.begin(), p.data.end());
                    if (metadata.contains("error")) throw std::runtime_error(metadata["error"].get<std::string>());
                    if (metadata.value("version", 0) != 1 || metadata.value("codec", "") != "opus" ||
                        metadata.value("sample_rate", 0) != 48000 || metadata.value("channels", 0) != 2 ||
                        metadata.value("frame_samples", 0) != 960 || metadata.value("timestamp", "") != "monotonic_ns")
                        throw std::runtime_error("the host's sound is in a format this doesn't play");
                    AudioCleanup();
                    g_audio.paFailed = false;
                    OPUS_MULTISTREAM_CONFIGURATION format{};
                    format.sampleRate = 48000, format.channelCount = 2, format.samplesPerFrame = 960;
                    format.streams = 1, format.coupledStreams = 1;
                    format.mapping[0] = 0, format.mapping[1] = 1;
                    if (AudioInit(AUDIO_CONFIGURATION_STEREO, &format, nullptr, 0)) throw std::runtime_error("no Opus decoder");
                    configured = true;
                    std::printf("kwin audio: stereo Opus at 48000 Hz\n");
                    std::fflush(stdout);
                } else {
                    if (!configured) throw std::runtime_error("sound came before its format");
                    uint64_t timestamp = 0;
                    for (int i = 0; i < 8; ++i) timestamp = (timestamp << 8) | p.data[i];
                    // A connection that fell behind catches up instead of playing old sound.
                    if (clock.Ready(timestamp, MonoNs())) {
                        AudioPlay(reinterpret_cast<char *>(p.data.data() + 8), int(p.data.size() - 8));
                        if (g_audio.paFailed) throw std::runtime_error("nothing here to play it on");
                    }
                }
                packets.pop_front();
            }
            if (MonoNs() - lastPacket > 5'000'000'000) throw std::runtime_error("the host's sound stopped");
        }
    }
};

int KwinWindows(Client &client) {
    signal(SIGPIPE, SIG_IGN);
    try {
        if (!client.server.paired) throw std::runtime_error("this client isn't paired with the host");
        KwinPeer peer;
        peer.Connect(client);
        peer.Send('J', R"({"version":1,"op":"list"})");
        std::deque<KwinPacket> packets;
        const int64_t deadline = MonoNs() + 10'000'000'000;
        while (MonoNs() < deadline) {
            peer.Flush();
            pollfd event{peer.fd, short(POLLIN | (!peer.outgoing.empty() ? POLLOUT : 0)), 0};
            poll(&event, 1, SSL_pending(peer.tls) ? 0 : 100);
            peer.Read(packets);
            if (packets.empty()) continue;
            const auto reply = nlohmann::json::parse(packets.front().data.begin(), packets.front().data.end());
            if (reply.contains("error")) throw std::runtime_error(reply["error"].get<std::string>());
            if (!reply.contains("windows") || !reply["windows"].is_array()) throw std::runtime_error("no window list");
            std::printf("%s\n", reply.dump().c_str());
            return 0;
        }
        throw std::runtime_error("KWin Window Stream didn't answer");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "kwin windows: %s\n", error.what());
        return 1;
    }
}

int StreamKwinWindow(Client &client, const std::string &window, int fd, int width, int height, int fps, int bitrate) {
    Viewer viewer{fd};
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);
    Decoder dec;
    dec.toSteamVR = false;
    Converter conv;
    conv.toSteamVR = false;
    KwinAudio audio;
    try {
        if (!client.server.paired) throw std::runtime_error("this client isn't paired with the host");
        if (width % 2 || height % 2 || fps > 120 || bitrate < 100 || bitrate > 150000)
            throw std::runtime_error("a window stream needs even sizes, 1 to 120 fps and 100 to 150000 kbit/s");
        for (std::string msg; viewer.Read(&msg, true) && !msg.empty();) {
            if (msg.rfind("modifiers", 0) != 0) continue;
            std::istringstream words(msg.substr(9));
            for (std::string m; words >> m;) conv.modifiers.push_back(std::strtoull(m.c_str(), nullptr, 16));
            break;
        }
        KwinPeer peer;
        peer.Connect(client);
        viewer.Send("state connecting kwin window");
        const nlohmann::json request = {{"version", 1}, {"window", window}, {"width", width}, {"height", height},
                                        {"fps", fps}, {"bitrate", bitrate}, {"follow_window", true}};
        peer.Send('J', request.dump());
        if (!conv.Init()) throw std::runtime_error("no converter");
        bool owned[3] = {}, live = false, hidden = false, described = false;  // described: a J said the stream's size
        std::deque<KwinPacket> packets;
        uint64_t sequence = 0, bytes = 0;
        int shown = 0, received = 0;
        int64_t report = MonoNs(), cpu = CpuNs(), lastShown = 0;
        while (!g_stop) {
            peer.Flush();
            pollfd events[3] = {
                {peer.fd, short((packets.size() < kKwinQueued ? POLLIN : 0) |
                                ((!peer.outgoing.empty() || peer.readNeedsWrite) ? POLLOUT : 0)), 0},
                {dec.fd, short(POLLIN | POLLPRI | (!packets.empty() ? POLLOUT : 0)), 0},
                {viewer.fd, POLLIN, 0},
            };
            poll(events, 3, SSL_pending(peer.tls) ? 0 : 20);
            if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("the host hung up");
            if (packets.size() < kKwinQueued) peer.Read(packets);
            std::string message;
            bool open = true;
            while ((open = viewer.Read(&message, false)) && !message.empty()) {
                int index;
                if (std::sscanf(message.c_str(), "release %d", &index) == 1) {
                    if (index >= 0 && index < 3) owned[index] = false;
                } else if (message == "quit") {
                    g_stop = 1;
                } else if (message.rfind("attention ", 0) == 0) {
                    hidden = message == "attention hidden";
                    peer.Send('C', message);
                } else if (message == "blur" || message.rfind("move ", 0) == 0 || message.rfind("button ", 0) == 0 ||
                           message.rfind("key ", 0) == 0 || message.rfind("scroll ", 0) == 0) {
                    peer.Send('C', message);
                }
            }
            if (!open) break;
            if (dec.Events()) {
                if (!conv.Setup(dec)) throw std::runtime_error("can't convert the decoder's buffers");
                const int fds[3] = {conv.out[0].fd, conv.out[1].fd, conv.out[2].fd};
                char text[160];
                std::snprintf(text, sizeof text, "buffers %u %u %x %llx %u %u", conv.width, conv.height, DRM_FORMAT_ABGR8888,
                              (unsigned long long)conv.out[0].modifier, conv.out[0].offset, conv.out[0].stride);
                if (!viewer.Send(text, fds, 3)) break;
                for (bool &slot : owned) slot = false;
            }
            dec.Reclaim();
            while (!packets.empty()) {
                auto &p = packets.front();
                if (p.type == 'J') {
                    const auto metadata = nlohmann::json::parse(p.data.begin(), p.data.end());
                    if (metadata.contains("error")) throw std::runtime_error(metadata["error"].get<std::string>());
                    if (metadata.value("codec", "") != "hevc" || metadata.value("version", 0) != 1)
                        throw std::runtime_error("the host sent a stream this doesn't play");
                    if (metadata.value("audio", false)) audio.Start(client);
                    width = metadata.at("width").get<int>();
                    height = metadata.at("height").get<int>();
                    if (width < 64 || height < 64 || width > 8192 || height > 8192 || width % 2 || height % 2)
                        throw std::runtime_error("the host's window is a size this can't show");
                    // The decoder ignores a source change once it's set up, so each new sequence (the
                    // window resized) gets a new one; the connection, input and panel stay.
                    conv.Reset();
                    dec.Close();
                    dec = Decoder{};
                    dec.toSteamVR = false;
                    if (!dec.Open("/dev/video-dec0", 6)) throw std::runtime_error("can't open the decoder again");
                    for (bool &slot : owned) slot = false;
                    live = false, described = true;
                    std::printf("kwin window %s: HEVC %dx%d\n", window.c_str(), width, height);
                } else {
                    // With no decoder open yet, the picture would wait there for good and reading with it.
                    if (!described) throw std::runtime_error("the host sent a picture before saying its size");
                    if (!dec.Feed(p.data.data(), p.data.size(), sequence)) break;
                    ++sequence, ++received;
                    bytes += p.data.size();
                }
                packets.pop_front();
            }
            uint64_t frame = 0;
            bool last = false;
            int decoded;
            while ((decoded = dec.Decoded(&frame, &last)) >= 0) {
                const int64_t now = MonoNs();
                int slot = -1;
                if (!hidden || now - lastShown >= 1'000'000'000 / kHiddenFps)
                    for (int k = 0; k < 3 && slot < 0; ++k)
                        if (!owned[(conv.next + k) % 3]) slot = (conv.next + k) % 3;
                if (slot >= 0) {
                    conv.ConvertInto(decoded, slot);
                    conv.next = (slot + 1) % 3;
                    owned[slot] = true;
                    if (!viewer.Send("frame " + std::to_string(slot))) g_stop = 1;
                    lastShown = now;
                    ++shown;
                    if (!live) {
                        live = true;
                        peer.Send('C', "size " + std::to_string(conv.width) + " " + std::to_string(conv.height));
                        viewer.Send("state live kwin window");
                    }
                }
                dec.Requeue(decoded);
            }
            const int64_t now = MonoNs();
            if (now - report >= 10'000'000'000) {
                const double seconds = (now - report) / 1e9;
                std::printf("kwin window: in %.1f fps, shown %.1f fps, %.2f Mbit/s, convert %.2f ms, CPU %.1f%% of a core\n",
                            received / seconds, shown / seconds, bytes * 8 / seconds / 1e6, shown ? conv.msSum / shown : 0.0,
                            100 * (CpuNs() - cpu) / 1e9 / seconds);
                report = now, cpu = CpuNs(), bytes = 0, received = shown = 0, conv.msSum = 0;
            }
        }
        // As in Stream: the way out is bounded even with ft-screens gone (the audio thread joins
        // on return, and it or the decoder can hang).
        alarm(kExitWatchdogS);
        viewer.Send("state lost the window's stream ended");
        peer.Send('C', "blur");
        peer.Flush();
        dec.Close();
        return 0;
    } catch (const std::exception &error) {
        alarm(kExitWatchdogS);
        std::fprintf(stderr, "kwin window: %s\n", error.what());
        viewer.Send(std::string("state lost ") + error.what());
        dec.Close();
        return 1;
    }
}
