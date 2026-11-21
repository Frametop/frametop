// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// Sockets, the RFB handshake and the live session client that ties the other parts together.
#pragma once

#include "apple_hp_auth.h"
#include "apple_hp_control.h"
#include "apple_hp_creds.h"
#include "apple_hp_h264.h"
#include "apple_hp_input.h"
#include "apple_hp_rtp.h"

namespace ft_apple_hp {

struct SessionOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 5900;
    std::string bind_ip;         // Optional local IPv4 bind for TCP & UDP
    uint16_t udp_ctrl_port = 0;  // 0 => port (5900)
    uint16_t udp_video_port = 0; // 0 => port + 1 (5901)
    std::string username;
    std::string password;
    AuthMode auth_mode = AuthMode::kAuto;
    bool virtual_display = true; // true = send 0x1d virtual display; false = mirror physical screen
    uint32_t logical_w = 1920;
    uint32_t logical_h = 1080;
    double hidpi_scale = 2.0;
    int fps = 60;
    bool audio_enabled = false;
    bool hdr = false;
    bool bake_cursor = true;
    bool warmup_tcp = true;
    int warmup_ms = 1400;
    int settle_ms = 100;
    int toggle_settle_ms = 200;
    int64_t suspend_gap_ms = 3000;
    int64_t wake_grace_ms = 4000;
    int64_t rx_probe_ms = 6000;
    int64_t rx_timeout_ms = 15000;
    KeyboardMappingConfig keyboard{};
};

// Maps a Frametop remote app token ("apple:physical", "apple:virtual", "apple:virtual@1",
// "apple:virtual@2") and requested stream size into Apple Silicon H.264 virtual-display limits
// (<= 4096x2304 backing pixels), preserving aspect ratio when clamping wider ultra-wide sizes
// (e.g. 5120x1440 -> 4096x1152) and keeping 1x scaling when 2x would shrink height below 900 pts.
inline void ResolveAppleDisplayMode(const std::string &app, int req_w, int req_h, SessionOptions *opt) {
    if (app == "apple:physical") {
        opt->virtual_display = false;
        return;
    }
    opt->virtual_display = true;
    double w = std::max(640, req_w);
    double h = std::max(360, req_h);
    if (w > 4096.0 || h > 2304.0) {
        const double s = std::min(4096.0 / w, 2304.0 / h);
        w *= s;
        h *= s;
    }
    const int cw = std::clamp(int(std::lround(w)), 1280, 4096) & ~1;
    const int ch = std::clamp(int(std::lround(h)), 720, 2304) & ~1;
    if (app == "apple:virtual@1") {
        opt->hidpi_scale = 1.0;
        opt->logical_w = uint32_t(cw);
        opt->logical_h = uint32_t(ch);
        return;
    }
    if (app == "apple:virtual@2") {
        opt->hidpi_scale = 2.0;
        if (req_w <= 2048 && req_h <= 1152) {
            opt->logical_w = uint32_t(std::clamp(req_w, 640, 2048) & ~1);
            opt->logical_h = uint32_t(std::clamp(req_h, 360, 1152) & ~1);
        } else {
            opt->logical_w = uint32_t(std::max(640, (cw / 2) & ~1));
            opt->logical_h = uint32_t(std::max(360, (ch / 2) & ~1));
        }
        return;
    }
    // Default "apple:virtual": 4K-tall modes (>= 2560x1800, e.g. 3840x2160 -> 1920x1080@2x) use
    // 2x Retina; shorter or ultra-wide modes (e.g. 4096x1152 32:9, 3440x1440, 2560x1440, 1920x1080)
    // use 1x so macOS keeps >= 1080 vertical points rather than shrinking UI height to 576-720p.
    if (cw >= 2560 && ch >= 1800) {
        opt->hidpi_scale = 2.0;
        opt->logical_w = uint32_t((cw / 2) & ~1);
        opt->logical_h = uint32_t((ch / 2) & ~1);
    } else {
        opt->hidpi_scale = 1.0;
        opt->logical_w = uint32_t(cw);
        opt->logical_h = uint32_t(ch);
    }
}

struct ProbeInfo {
    std::string banner;
    std::vector<uint8_t> security_types;
    int rsa_key_bits = 0;
};

inline bool ResolveIpv4(const std::string &host, in_addr *out_addr, std::string *out_ip = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    const auto *sin = reinterpret_cast<const sockaddr_in *>(res->ai_addr);
    *out_addr = sin->sin_addr;
    if (out_ip) {
        char buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
        *out_ip = buf;
    }
    freeaddrinfo(res);
    return true;
}

inline int OpenTcpIpv4(const in_addr &dest, uint16_t port, const std::string &bind_ip, int timeout_ms = 10000) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    // Keepalive + user timeout detect half-open TCP sockets after host/client sleep or link loss
    // even when no client input is sent (e.g. view_only mode).
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof keepalive);
#ifdef TCP_KEEPIDLE
    int keepidle = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof keepidle);
#elif defined(TCP_KEEPALIVE)
    int keepidle = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &keepidle, sizeof keepidle);
#endif
#ifdef TCP_KEEPINTVL
    int keepintvl = 2;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof keepintvl);
#endif
#ifdef TCP_KEEPCNT
    int keepcnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof keepcnt);
#endif
#ifdef TCP_USER_TIMEOUT
    unsigned int user_timeout_ms = 8000;
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof user_timeout_ms);
#endif
    if (!bind_ip.empty()) {
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        if (inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) != 1 ||
            bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof local) < 0) {
            close(fd);
            return -1;
        }
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    peer.sin_addr = dest;
    int r = connect(fd, reinterpret_cast<const sockaddr *>(&peer), sizeof peer);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

inline int BindUdpIpv4(const std::string &bind_ip, uint16_t port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (bind_ip.empty()) {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof local) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

inline bool SendAll(int fd, const uint8_t *buf, size_t len, int timeout_ms = 10000) {
    size_t sent = 0;
    while (sent < len) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, buf + sent, len - sent, 0);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += size_t(n);
    }
    return true;
}

inline bool SendAll(int fd, const std::vector<uint8_t> &v, int timeout_ms = 10000) {
    return SendAll(fd, v.data(), v.size(), timeout_ms);
}

inline bool RecvExact(int fd, uint8_t *buf, size_t len, int timeout_ms = 10000) {
    size_t got = 0;
    while (got < len) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        const ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        got += size_t(n);
    }
    return true;
}

inline bool DoRfbBannerAndTypes(int fd, std::string *banner_out, std::vector<uint8_t> *types_out) {
    uint8_t banner[12];
    if (!RecvExact(fd, banner, 12, 5000)) return false;
    *banner_out = std::string(reinterpret_cast<const char *>(banner), 12);
    static constexpr uint8_t kVer[12] = {'R', 'F', 'B', ' ', '0', '0', '3', '.', '8', '8', '9', '\n'};
    if (!SendAll(fd, kVer, 12, 5000)) return false;
    uint8_t count = 0;
    if (!RecvExact(fd, &count, 1, 5000) || count == 0) return false;
    types_out->resize(count);
    return RecvExact(fd, types_out->data(), count, 5000);
}

inline UniquePkey Rsa1InitFetchKey(int fd) {
    static constexpr uint8_t kInit[15] = {
        0x21, 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 'R', 'S', 'A', '1', 0x00, 0x00, 0x00, 0x00,
    };
    if (!SendAll(fd, kInit, sizeof kInit, 5000)) return nullptr;
    uint8_t len_b[4];
    if (!RecvExact(fd, len_b, 4, 5000)) return nullptr;
    const uint32_t pkt_len = ReadBe32(len_b);
    if (pkt_len < 16 || pkt_len > 16384) return nullptr;
    std::vector<uint8_t> pkt(pkt_len);
    if (!RecvExact(fd, pkt.data(), pkt_len, 5000)) return nullptr;
    const uint32_t key_len = ReadBe32(pkt.data() + 2);
    if (size_t(6) + key_len > pkt.size()) return nullptr;
    return LoadDerPublicKey(pkt.data() + 6, key_len);
}

inline bool ProbeServer(const std::string &host, uint16_t port, const std::string &bind_ip,
                        ProbeInfo *out, std::string *err_out) {
    in_addr dest{};
    if (!ResolveIpv4(host, &dest)) {
        if (err_out) *err_out = "failed to resolve host IPv4";
        return false;
    }
    const int fd = OpenTcpIpv4(dest, port, bind_ip, 5000);
    if (fd < 0) {
        if (err_out) *err_out = "failed to connect TCP";
        return false;
    }
    if (!DoRfbBannerAndTypes(fd, &out->banner, &out->security_types)) {
        close(fd);
        if (err_out) *err_out = "RFB version/security-types handshake failed";
        return false;
    }
    const bool has_33 = std::find(out->security_types.begin(), out->security_types.end(), 33) !=
                        out->security_types.end();
    if (has_33) {
        UniquePkey pk = Rsa1InitFetchKey(fd);
        if (pk) out->rsa_key_bits = EVP_PKEY_get_bits(pk.get());
    }
    close(fd);
    return out->rsa_key_bits > 0 || !has_33;
}

class SessionClient {
public:
    ~SessionClient() { Close(); }

    const NegotiatedCanvas &canvas() const { return canvas_; }
    const DisplayLayout0x451 &layout() const { return layout_; }
    uint16_t server_init_w() const { return server_w_; }
    uint16_t server_init_h() const { return server_h_; }
    const std::string &auth_used() const { return auth_used_; }
    // The last sign-in failed because the server answered SRP's first packet with a non-SRP reply.
    bool srp_not_offered() const { return srp_not_offered_; }
    H264RtpAssembler &assembler() { return assembler_; }
    uint64_t video_rtp_packets() const { return video_rtp_pkts_; }
    uint64_t video_rtp_bytes() const { return video_rtp_bytes_; }
    uint64_t audio_rtp_packets() const { return audio_rtp_pkts_; }

    // ScreensharingAgent's PostMouseEvent calls hidpi_ConvertActualLocationsToVirtual, which
    // expects backing ("Actual") pixel coordinates (e.g. 3840x2160 on a 1920x1080@2x Retina
    // display) and divides by the HiDPI scale factor on the Mac host.
    uint16_t backing_pointer_w() const {
        if (layout_.backing_w) return layout_.backing_w;
        if (canvas_.width) return uint16_t(std::min<uint32_t>(canvas_.width, 32767));
        return server_w_;
    }
    uint16_t backing_pointer_h() const {
        if (layout_.backing_h) return layout_.backing_h;
        if (canvas_.height) return uint16_t(std::min<uint32_t>(canvas_.height, 32767));
        return server_h_;
    }

    // Verifies RSA1/SRP-6a (or non-SRP) credentials against screensharingd without mounting
    // a virtual display or starting UDP media streaming.
    bool VerifyCredentials(const SessionOptions &opt, std::string *err_out) {
        Close();
        opt_ = opt;
        if (!ResolveIpv4(opt.host, &dest_addr_, &dest_ip_)) {
            if (err_out) *err_out = "cannot resolve host IPv4: " + opt.host;
            return false;
        }
        std::array<uint8_t, 16> wrap_key{};
        const bool ok = RunAuthWithFallback(opt, &wrap_key, err_out);
        OPENSSL_cleanse(wrap_key.data(), wrap_key.size());
        Close();
        return ok;
    }

    void Close() {
        if (tcp_fd_ >= 0) close(tcp_fd_), tcp_fd_ = -1;
        if (udp_ctrl_fd_ >= 0) close(udp_ctrl_fd_), udp_ctrl_fd_ = -1;
        if (udp_video_fd_ >= 0) close(udp_video_fd_), udp_video_fd_ = -1;
        buttons_mask_ = 0;
    }

    bool Connect(const SessionOptions &opt, std::string *err_out) {
        Close();
        opt_ = opt;
        key_mapper_ = EvdevKeyMapper(opt.keyboard);
        if (!ResolveIpv4(opt.host, &dest_addr_, &dest_ip_)) {
            if (err_out) *err_out = "cannot resolve host IPv4: " + opt.host;
            return false;
        }

        const uint16_t ctrl_port = opt.udp_ctrl_port ? opt.udp_ctrl_port : opt.port;
        const uint16_t video_port = opt.udp_video_port ? opt.udp_video_port : uint16_t(opt.port + 1);
        dest_ctrl_sin_.sin_family = AF_INET;
        dest_ctrl_sin_.sin_port = htons(ctrl_port);
        dest_ctrl_sin_.sin_addr = dest_addr_;
        dest_video_sin_.sin_family = AF_INET;
        dest_video_sin_.sin_port = htons(video_port);
        dest_video_sin_.sin_addr = dest_addr_;

        // Bind UDP media sockets before negotiating 0x1c so the initial burst isn't dropped.
        udp_ctrl_fd_ = BindUdpIpv4(opt.bind_ip, ctrl_port);
        udp_video_fd_ = BindUdpIpv4(opt.bind_ip, video_port);
        if (udp_ctrl_fd_ < 0 || udp_video_fd_ < 0) {
            if (err_out) {
                *err_out = "failed to bind local UDP ports " + std::to_string(ctrl_port) + "/" +
                           std::to_string(video_port) +
                           " (if testing on the same Mac, pass --bind-ip with a secondary loopback/LAN IP)";
            }
            Close();
            return false;
        }

        if (opt.warmup_tcp) {
            const int wfd = OpenTcpIpv4(dest_addr_, opt.port, opt.bind_ip, 5000);
            if (wfd >= 0) {
                std::string b;
                std::vector<uint8_t> t;
                DoRfbBannerAndTypes(wfd, &b, &t);
                shutdown(wfd, SHUT_RDWR);
                close(wfd);
                if (opt.warmup_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.warmup_ms));
            }
        }

        std::array<uint8_t, 16> wrap_key{};
        if (!RunAuthWithFallback(opt, &wrap_key, err_out)) {
            Close();
            return false;
        }

        // ClientInit (0xC1) + ServerInit (24B + name).
        const uint8_t client_init = 0xc1;
        uint8_t sinit[24];
        if (!SendAll(tcp_fd_, &client_init, 1) || !RecvExact(tcp_fd_, sinit, 24)) {
            if (err_out) *err_out = "ClientInit/ServerInit failed";
            Close();
            return false;
        }
        server_w_ = ReadBe16(sinit);
        server_h_ = ReadBe16(sinit + 2);
        const uint32_t name_len = ReadBe32(sinit + 20);
        if (name_len > 0) {
            std::vector<uint8_t> name_buf(std::min<uint32_t>(name_len, 4096));
            if (name_len > 4096 || !RecvExact(tcp_fd_, name_buf.data(), name_len)) {
                if (err_out) *err_out = "ServerInit name read failed";
                Close();
                return false;
            }
        }

        // Check optional SessionSelect prompt (00 4a 00 01) when auth user != console user.
        if (!HandleOptionalSessionSelect(opt, wrap_key.data(), err_out)) {
            Close();
            return false;
        }

        // Plaintext Apple prelude + optional virtual display (0x1d) + SetEncodings (0x02).
        auto prelude = BuildViewerInfo();
        const auto f12 = Build0x12Followup();
        prelude.insert(prelude.end(), f12.begin(), f12.end());
        if (!SendAll(tcp_fd_, prelude)) {
            if (err_out) *err_out = "ViewerInfo send failed";
            Close();
            return false;
        }
        if (opt.settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.settle_ms));

        if (opt.virtual_display &&
            !SendAll(tcp_fd_, BuildVirtualDisplay0x1d(opt.logical_w, opt.logical_h, opt.hidpi_scale, opt.hdr,
                                                      "Frametop Virtual Display", double(opt.fps)))) {
            if (err_out) *err_out = "SetDisplayConfiguration (0x1d) send failed";
            Close();
            return false;
        }
        if (!SendAll(tcp_fd_, BuildSetEncodings())) {
            if (err_out) *err_out = "SetEncodings send failed";
            Close();
            return false;
        }

        // Drain cleartext server FramebufferUpdate rectangles until 0x44f (1103) rekey arrives.
        if (!ReadUntilEnc1103(wrap_key.data(), err_out)) {
            Close();
            return false;
        }
        OPENSSL_cleanse(wrap_key.data(), wrap_key.size());

        if (!SendAll(tcp_fd_, BuildPostEncryptionToggle())) {
            if (err_out) *err_out = "PostEncryptionToggle send failed";
            Close();
            return false;
        }
        if (opt.toggle_settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.toggle_settle_ms));
        DrainEncryptedControl(250);

        // Re-assert 0x1d virtual display over the encrypted channel once ScreensharingAgent's
        // Mach port is attached so cold-booted sessions mount SLVirtualDisplay before 0x1c.
        if (opt.virtual_display) {
            if (!SendEncrypted(BuildVirtualDisplay0x1d(opt.logical_w, opt.logical_h, opt.hidpi_scale, opt.hdr,
                                                       "Frametop Virtual Display", double(opt.fps)))) {
                if (err_out) *err_out = "encrypted SetDisplayConfiguration (0x1d) send failed";
                Close();
                return false;
            }
            DrainEncryptedControl(opt.toggle_settle_ms > 50 ? 450 : 20);
        }

        // Build & send encrypted SetEncodings + 0x1c MediaStreamOptions offer.
        OfferConfig ocfg;
        ocfg.codec = VideoCodec::kAvc;
        ocfg.tiles_per_frame = 1;
        ocfg.ltrp = false;
        ocfg.audio_enabled = opt.audio_enabled;
        if (!CreateNegotiatorOffers(ocfg, &offers_) || !NegotiationKeys::Generate(&keys_)) {
            if (err_out) *err_out = "failed to generate 0x1c media offers";
            Close();
            return false;
        }
        if (!video_rx_srtp_.InitFromBlob(keys_.video_key_s.data()) ||
            !video_tx_srtp_.InitFromBlob(keys_.video_key_v.data()) ||
            !audio_rx_srtp_.InitFromBlob(keys_.audio_key_s.data()) ||
            !audio_tx_srtp_.InitFromBlob(keys_.audio_key_v.data())) {
            if (err_out) *err_out = "SRTP context initialization failed";
            Close();
            return false;
        }

        if (!SendEncrypted(BuildSetEncodings())) {
            if (err_out) *err_out = "encrypted SetEncodings send failed";
            Close();
            return false;
        }
        const auto msg_1c = Build0x1cMessage(offers_.audio_bplist, offers_.video_bplist, keys_, opt.bake_cursor,
                                             opt.fps >= 45);
        if (!SendEncrypted(msg_1c)) {
            if (err_out) *err_out = "encrypted 0x1c send failed";
            Close();
            return false;
        }
        if (!opt.bake_cursor) SendEncrypted(BuildFbuRequest(false));

        // Punch UDP paths early and await 0x1c non-zero canvas answer (with degenerate retries).
        SendRtcpKeepalive(true);
        if (!WaitVideoAnswer(msg_1c, err_out)) {
            Close();
            return false;
        }

        fcntl(tcp_fd_, F_SETFL, fcntl(tcp_fd_, F_GETFL, 0) | O_NONBLOCK);
        const int64_t mono_now = NowNs();
        const int64_t boot_now = BootNs();
        last_tx_pulse_ns_ = mono_now;
        liveness_.Reset(mono_now, boot_now, opt_.suspend_gap_ms, opt_.wake_grace_ms,
                        opt_.rx_probe_ms, opt_.rx_timeout_ms);
        SendRtcpKeepalive(true);
        return true;
    }

    // Non-blocking poll cycle: drains UDP video/ctrl, sends periodic SRTCP RR/SR + NACKs, drains TCP control.
    // Optional extra_poll_fd (e.g. V4L2 decoder fd) wakes poll() immediately when hardware decode completes.
    bool PollStep(int timeout_ms, std::vector<H264RtpAssembler::AccessUnit> *out_aus,
                  std::string *err_out = nullptr, int extra_poll_fd = -1,
                  std::vector<std::vector<uint8_t>> *out_audio_aus = nullptr) {
        if (tcp_fd_ < 0 || udp_video_fd_ < 0) return false;
        pollfd pfds[4] = {
            {udp_video_fd_, POLLIN, 0},
            {udp_ctrl_fd_, POLLIN, 0},
            {tcp_fd_, POLLIN, 0},
            {extra_poll_fd, short(extra_poll_fd >= 0 ? (POLLIN | POLLPRI) : 0), 0},
        };
        poll(pfds, extra_poll_fd >= 0 ? 4 : 3, timeout_ms);

        bool got_rx = false;

        // 1. Drain UDP video + muxed video RTCP.
        uint8_t udp_buf[2048];
        for (int n = 0; n < 256; ++n) {
            const ssize_t r = recv(udp_video_fd_, udp_buf, sizeof udp_buf, 0);
            if (r <= 0) break;
            if (r < 12) continue;
            const uint8_t pt_raw = udp_buf[1];
            if (pt_raw >= 192 && pt_raw <= 223) {  // SRTCP from server
                std::vector<uint8_t> rtcp;
                if (video_rx_srtp_.DecryptRtcp(udp_buf, size_t(r), &rtcp)) got_rx = true;
                continue;
            }
            SrtpContext::RtpHeader hdr{};
            std::vector<uint8_t> pay;
            if (video_rx_srtp_.DecryptRtp(udp_buf, size_t(r), &hdr, &pay)) {
                got_rx = true;
                ++video_rtp_pkts_;
                video_rtp_bytes_ += size_t(r);
                last_media_ssrc_ = hdr.ssrc;
                assembler_.PushPacket(hdr, pay, out_aus);
            }
        }

        // 2. Drain UDP audio/ctrl socket (decrypting AAC-ELD packets when audio is active).
        for (int n = 0; n < 256; ++n) {
            const ssize_t r = recv(udp_ctrl_fd_, udp_buf, sizeof udp_buf, 0);
            if (r <= 0) break;
            if (r < 12) continue;
            const uint8_t pt_raw = udp_buf[1];
            if (pt_raw >= 192 && pt_raw <= 223) {  // SRTCP from server
                std::vector<uint8_t> rtcp;
                if (audio_rx_srtp_.DecryptRtcp(udp_buf, size_t(r), &rtcp)) got_rx = true;
                continue;
            }
            if (!opt_.audio_enabled || !out_audio_aus) continue;
            SrtpContext::RtpHeader hdr{};
            std::vector<uint8_t> pay;
            if (audio_rx_srtp_.DecryptRtp(udp_buf, size_t(r), &hdr, &pay) && !pay.empty()) {
                got_rx = true;
                ++audio_rtp_pkts_;
                ParseRfc3640AuPayloads(pay.data(), pay.size(), out_audio_aus);
            }
        }

        // 3. Promptly emit SRTCP NACKs when packet gaps occur.
        uint32_t nack_ssrc = 0;
        const auto lost = assembler_.TakePendingNacks(&nack_ssrc);
        if (!lost.empty()) {
            const auto nack = BuildNack(offers_.video_ssrc, nack_ssrc, lost);
            SendVideoSrtcp(nack);
        }

        // 4. Drain non-blocking TCP control frames.
        if (pfds[2].revents & (POLLHUP | POLLERR)) {
            if (err_out) *err_out = "server closed TCP control socket";
            return false;
        }
        if (pfds[2].revents & POLLIN) {
            bool tcp_rx = false;
            if (!DrainNonBlockingTcp(err_out, &tcp_rx)) return false;
            if (tcp_rx) got_rx = true;
        }

        // 5. Evaluate suspend-wake and peer-liveness timers.
        const int64_t now = NowNs();
        const int64_t boot = BootNs();
        const auto act = liveness_.Step(now, boot, got_rx);
        if (act == SessionLiveness::Action::kTimedOutWake) {
            if (err_out) *err_out = "no response from host after system wake";
            return false;
        }
        if (act == SessionLiveness::Action::kTimedOutIdle) {
            if (err_out) *err_out = "no media from host (connection timed out)";
            return false;
        }
        if (act == SessionLiveness::Action::kProbeWake || act == SessionLiveness::Action::kProbeIdle) {
            last_tx_pulse_ns_ = now;
            ++pulse_tick_;
            SendRtcpKeepalive(true);
            RequestKeyframe();
        } else if (now - last_tx_pulse_ns_ >= 500'000'000LL) {
            // 6. Periodic 500ms SRTCP RR/SR keepalive.
            last_tx_pulse_ns_ = now;
            ++pulse_tick_;
            SendRtcpKeepalive((pulse_tick_ % 10) == 0 || liveness_.wake_probe_pending());
            if (liveness_.wake_probe_pending()) RequestKeyframe();
        }
        return true;
    }

    void RequestKeyframe() {
        if (last_media_ssrc_ != 0) {
            SendVideoSrtcp(BuildFirLegacy(last_media_ssrc_));
            SendVideoSrtcp(BuildPli(offers_.video_ssrc, last_media_ssrc_));
        }
    }

    bool SendPointerMove(double stream_x, double stream_y, uint32_t stream_w, uint32_t stream_h) {
        if (tcp_fd_ < 0) return false;
        last_ptr_x_ = ScalePointerCoord(stream_x, stream_w, backing_pointer_w());
        last_ptr_y_ = ScalePointerCoord(stream_y, stream_h, backing_pointer_h());
        return SendEncrypted(BuildPointerEvent(buttons_mask_, last_ptr_x_, last_ptr_y_));
    }

    bool SendMouseButton(uint32_t linux_btn, bool down) {
        if (tcp_fd_ < 0) return false;
        const uint8_t bit = MouseButtonBit(linux_btn);
        if (bit == 0) return true;
        if (down) buttons_mask_ |= bit;
        else buttons_mask_ = uint8_t(buttons_mask_ & ~bit);
        return SendEncrypted(BuildPointerEvent(buttons_mask_, last_ptr_x_, last_ptr_y_));
    }

    // Wayland/ft-screens scroll convention: dy < 0 is scroll up, dy > 0 is scroll down.
    // In screensharingd's HandlePointerEventMessage, exact button_mask == 0x08 posts scroll up (+1)
    // and exact button_mask == 0x10 posts scroll down (-1) via SSAgent_PostScrollWheelEvent_rpc
    // without touching mouse button state; OR-ing buttons_mask_ or sending 0x20/0x40 falls through
    // into PostMouseEventIntoSession as extra mouse button clicks.
    bool SendMouseScroll(double dx, double dy) {
        (void)dx;
        if (tcp_fd_ < 0 || dy == 0.0) return tcp_fd_ >= 0;
        scroll_accum_y_ = 0.0;
        const int ticks = std::clamp(int(std::lround(std::fabs(dy))), 1, 12);
        const uint8_t bit = (dy < 0.0) ? 0x08 : 0x10;
        bool ok = true;
        for (int t = 0; t < ticks; ++t) {
            ok = SendEncrypted(BuildPointerEvent(bit, last_ptr_x_, last_ptr_y_)) && ok;
        }
        return ok;
    }

    const KeyboardMappingConfig &keyboard_mapping() const { return key_mapper_.config(); }

    bool SendMappedKey(bool down, uint32_t ks) {
        if (tcp_fd_ < 0) return false;
        uint16_t mac_vk = 0;
        if (IsAppleMacKeycodeKeysym(ks, &mac_vk)) {
            std::vector<uint8_t> pkt;
            return BuildAppleKeycodeEvent0x10(cipher_.cbc_key().data(), down, mac_vk, &pkt) &&
                   SendEncrypted(pkt);
        }
        return SendEncrypted(BuildKeyEvent(down, ks));
    }

    bool SetKeyboardMapping(const KeyboardMappingConfig &cfg) {
        if (cfg == key_mapper_.config()) return false;
        opt_.keyboard = cfg;
        key_mapper_.SetConfig(cfg, [&](bool d, uint32_t ks) {
            if (tcp_fd_ >= 0) SendMappedKey(d, ks);
        });
        return true;
    }

    bool SendEvdevKey(uint32_t evdev_code, bool down) {
        if (tcp_fd_ < 0) return false;
        bool ok = true;
        key_mapper_.Process(evdev_code, down, [&](bool d, uint32_t ks) {
            ok = SendMappedKey(d, ks) && ok;
        });
        return ok;
    }

    bool SendInputBlur() {
        if (tcp_fd_ < 0) return false;
        bool ok = true;
        key_mapper_.Blur([&](bool d, uint32_t ks) {
            ok = SendMappedKey(d, ks) && ok;
        });
        if (buttons_mask_ != 0) {
            buttons_mask_ = 0;
            ok = SendEncrypted(BuildPointerEvent(0, last_ptr_x_, last_ptr_y_)) && ok;
        }
        return ok;
    }

private:
    bool RunSingleAuth(bool use_srp, std::array<uint8_t, 16> *wrap_key, std::string *err_out) {
        tcp_fd_ = OpenTcpIpv4(dest_addr_, opt_.port, opt_.bind_ip, 10000);
        if (tcp_fd_ < 0) {
            if (err_out) *err_out = "TCP connect failed to " + dest_ip_;
            return false;
        }
        std::string banner;
        std::vector<uint8_t> sec_types;
        if (!DoRfbBannerAndTypes(tcp_fd_, &banner, &sec_types)) {
            if (err_out) *err_out = "RFB banner/security-types exchange failed";
            return false;
        }
        if (std::find(sec_types.begin(), sec_types.end(), 33) == sec_types.end()) {
            if (err_out) *err_out = "server did not offer Apple RSA1/SRP security type 33";
            return false;
        }
        UniquePkey pub = Rsa1InitFetchKey(tcp_fd_);
        if (!pub) {
            if (err_out) *err_out = "RSA1 server public key exchange failed";
            return false;
        }

        if (use_srp) {
            std::vector<uint8_t> pkt1;
            if (!BuildSrpPacket1(pub.get(), opt_.username, &pkt1) || !SendAll(tcp_fd_, pkt1)) {
                if (err_out) *err_out = "SRP packet-1 send failed";
                return false;
            }
            uint8_t len_b[4];
            if (!RecvExact(tcp_fd_, len_b, 4)) {
                if (err_out) *err_out = "SRP challenge length read failed";
                return false;
            }
            const uint32_t s2c1_len = ReadBe32(len_b);
            if (s2c1_len < 1000 || s2c1_len > 65536) {
                if (err_out) *err_out = "SRP challenge rejected or server fell back to non-SRP";
                srp_not_offered_ = s2c1_len < 1000;
                return false;
            }
            std::vector<uint8_t> s2c1(s2c1_len);
            SrpChallenge ch{};
            SrpClientProof proof{};
            if (!RecvExact(tcp_fd_, s2c1.data(), s2c1_len) || !ParseSrpChallenge(s2c1.data(), s2c1.size(), &ch) ||
                !SolveSrp(ch, opt_.password, &proof)) {
                if (err_out) *err_out = "SRP challenge parse/solve failed";
                return false;
            }
            if (!SendAll(tcp_fd_, BuildSrpPacket2(ch, proof))) {
                if (err_out) *err_out = "SRP packet-2 send failed";
                return false;
            }
            uint8_t m2_hdr[4], res_b[4];
            if (!RecvExact(tcp_fd_, m2_hdr, 4)) {
                if (err_out) *err_out = "SRP server proof read failed";
                return false;
            }
            const uint32_t m2_len = ReadBe32(m2_hdr);
            if (m2_len > 4096) return false;
            std::vector<uint8_t> m2_buf(m2_len);
            if ((m2_len > 0 && !RecvExact(tcp_fd_, m2_buf.data(), m2_len)) || !RecvExact(tcp_fd_, res_b, 4)) {
                if (err_out) *err_out = "SRP SecurityResult read failed";
                return false;
            }
            if (ReadBe32(res_b) != 0) {
                if (err_out) *err_out = "SRP authentication rejected (bad username or password)";
                return false;
            }
            *wrap_key = proof.wrap_key;
            auth_used_ = "srp";
            return true;
        }

        if (!RandomBytes(wrap_key->data(), 16)) return false;
        std::vector<uint8_t> pkt;
        if (!BuildNonSrpPacket(pub.get(), opt_.username, opt_.password, wrap_key->data(), &pkt) ||
            !SendAll(tcp_fd_, pkt)) {
            if (err_out) *err_out = "non-SRP credential send failed";
            return false;
        }
        uint8_t reply[8];
        if (!RecvExact(tcp_fd_, reply, 8) || ReadBe32(reply + 4) != 0) {
            if (err_out) *err_out = "non-SRP authentication rejected (bad username or password)";
            return false;
        }
        auth_used_ = "nonsrp";
        return true;
    }

    bool RunAuthWithFallback(const SessionOptions &opt, std::array<uint8_t, 16> *wrap_key, std::string *err_out) {
        if (opt.auth_mode == AuthMode::kSrp) return RunSingleAuth(true, wrap_key, err_out);
        if (opt.auth_mode == AuthMode::kNonSrp) return RunSingleAuth(false, wrap_key, err_out);
        // kAuto is for the first sign-in only (ft-appless auth). Try RSA only when the
        // server answered SRP's first packet with a short non-SRP reply; a rejected
        // password or a broken challenge never sends it again over the RSA path.
        std::string srp_err, nonsrp_err;
        srp_not_offered_ = false;
        if (RunSingleAuth(true, wrap_key, &srp_err)) return true;
        if (!srp_not_offered_) {
            if (err_out) *err_out = srp_err;
            return false;
        }
        if (tcp_fd_ >= 0) close(tcp_fd_), tcp_fd_ = -1;
        if (RunSingleAuth(false, wrap_key, &nonsrp_err)) return true;
        if (err_out) *err_out = srp_err + "; fallback " + nonsrp_err;
        return false;
    }

    bool HandleOptionalSessionSelect(const SessionOptions &opt, const uint8_t ecb_key[16], std::string *err_out) {
        pollfd pfd{tcp_fd_, POLLIN, 0};
        if (poll(&pfd, 1, 150) <= 0) return true;
        uint8_t peek[4]{};
        if (recv(tcp_fd_, peek, 4, MSG_PEEK) != 4) return true;
        static constexpr uint8_t kSsMagic[4] = {0x00, 0x4a, 0x00, 0x01};
        if (std::memcmp(peek, kSsMagic, 4) != 0) return true;

        uint8_t ssi[76];
        if (!RecvExact(tcp_fd_, ssi, 76)) {
            if (err_out) *err_out = "SessionSelect prompt read failed";
            return false;
        }
        // Auto-login into fresh virtual session via cmd=2 with AES-128-ECB credentials.
        std::vector<uint8_t> body(200, 0);
        WriteBe16(&body[0], 2);
        body[6] = 2;
        uint8_t u64[64]{}, p64[64]{};
        std::memcpy(u64, opt.username.data(), std::min<size_t>(63, opt.username.size()));
        std::memcpy(p64, opt.password.data(), std::min<size_t>(63, opt.password.size()));
        Aes128EcbCrypt(true, ecb_key, u64, 64, &body[0x48]);
        Aes128EcbCrypt(true, ecb_key, p64, 64, &body[0x88]);
        OPENSSL_cleanse(p64, sizeof p64);

        std::vector<uint8_t> msg;
        AppendBe16(msg, 200);
        msg.insert(msg.end(), body.begin(), body.end());
        if (!SendAll(tcp_fd_, msg)) return false;

        pollfd apfd{tcp_fd_, POLLIN, 0};
        if (poll(&apfd, 1, 15000) <= 0) {
            if (err_out) *err_out = "SessionSelect ack timed out";
            return false;
        }
        uint8_t ack[4096];
        return recv(tcp_fd_, ack, sizeof ack, 0) > 0;
    }

    // Reads and drops n bytes 64 KiB at a time, so a peer-sent length can't
    // make us allocate gigabytes.
    bool SkipBytes(size_t n) {
        uint8_t tmp[65536];
        while (n > 0) {
            const size_t chunk = std::min(n, sizeof tmp);
            if (!RecvExact(tcp_fd_, tmp, chunk)) return false;
            n -= chunk;
        }
        return true;
    }

    bool SkipCleartextRectPayload(int32_t enc, uint16_t w, uint16_t h) {
        if (enc == 1103) return false;
        if (enc == 0) {  // Raw
            const size_t bytes = size_t(w) * size_t(h) * 4;
            std::vector<uint8_t> tmp(std::min<size_t>(bytes, 65536));
            size_t left = bytes;
            while (left > 0) {
                const size_t chunk = std::min(left, tmp.size());
                if (!RecvExact(tcp_fd_, tmp.data(), chunk)) return false;
                left -= chunk;
            }
            return true;
        }
        if (enc == 6 || enc == 16) {  // zlib / ZRLE u32 length
            uint8_t lb[4];
            if (!RecvExact(tcp_fd_, lb, 4)) return false;
            return SkipBytes(ReadBe32(lb));
        }
        if (enc == 1105) {  // 0x451 AppleDisplayLayout: u16 payload_len
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> pay(n);
            if (n > 0 && !RecvExact(tcp_fd_, pay.data(), n)) return false;
            ParseDisplayLayout0x451(pay.data(), pay.size(), &layout_);
            return true;
        }
        if (enc == 1107) {  // 0x453 VendorKeysym: fixed 22 bytes
            uint8_t b[22];
            return RecvExact(tcp_fd_, b, 22);
        }
        if (enc == 1109 || enc == 1010 || enc == 1011 || enc == 1002) {
            // Length-prefixed Apple metadata: always carries a u16_be byte count.
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> b(n);
            return n == 0 || RecvExact(tcp_fd_, b.data(), n);
        }
        if (enc == 1110) {  // 0x456 DeviceInfo: u16 msg_size (includes after u16)
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> b(n);
            return n == 0 || RecvExact(tcp_fd_, b.data(), n);
        }
        return true;
    }

    bool ReadUntilEnc1103(const uint8_t wrap_key16[16], std::string *err_out) {
        const int64_t deadline = NowNs() + 10'000'000'000LL;
        while (NowNs() < deadline) {
            uint8_t mtype = 0;
            if (!RecvExact(tcp_fd_, &mtype, 1, 5000)) break;
            if (mtype == 0x00) {  // FramebufferUpdate
                uint8_t hdr[3];
                if (!RecvExact(tcp_fd_, hdr, 3)) break;
                const uint16_t nrects = ReadBe16(hdr + 1);
                for (uint16_t i = 0; i < nrects; ++i) {
                    uint8_t rh[12];
                    if (!RecvExact(tcp_fd_, rh, 12)) {
                        if (err_out) *err_out = "truncated FramebufferUpdate rect before 0x44f";
                        return false;
                    }
                    const uint16_t w = ReadBe16(rh + 4);
                    const uint16_t h = ReadBe16(rh + 6);
                    const int32_t enc = int32_t(ReadBe32(rh + 8));
                    if (enc == 1103) {  // 0x44f EncodeEncryptionInfo
                        uint8_t rekey[36];
                        if (!RecvExact(tcp_fd_, rekey, 36) || !cipher_.InitFromRekey0x44f(rekey, wrap_key16)) {
                            if (err_out) *err_out = "0x44f rekey read/unwrap failed";
                            return false;
                        }
                        return true;
                    }
                    if (!SkipCleartextRectPayload(enc, w, h)) {
                        if (err_out) *err_out = "unsupported cleartext rect before 0x44f: " + std::to_string(enc);
                        return false;
                    }
                }
            } else if (mtype == 0x02) {
                continue;  // Bell
            } else if (mtype == 0x03) {
                uint8_t hdr[7];
                if (!RecvExact(tcp_fd_, hdr, 7)) break;
                if (!SkipBytes(ReadBe32(hdr + 3))) break;
            } else if (mtype == 0x14) {
                uint8_t b[7];
                if (!RecvExact(tcp_fd_, b, 7)) break;
            } else {
                if (err_out) *err_out = "unexpected cleartext msg before 0x44f: " + std::to_string(mtype);
                return false;
            }
        }
        if (err_out) *err_out = "timed out waiting for 0x44f enc1103 rekey";
        return false;
    }

    bool SendEncrypted(const std::vector<uint8_t> &msg) {
        std::vector<uint8_t> wire;
        return cipher_.EncryptMessage(msg, &wire) && SendAll(tcp_fd_, wire);
    }

    void InspectControlMessage(const std::vector<uint8_t> &msg) {
        if (msg.size() >= 18 && msg[0] == 0x00) {
            const uint16_t nrects = ReadBe16(&msg[2]);
            if (nrects >= 1) {
                const int32_t enc = int32_t(ReadBe32(&msg[12]));
                if (enc == 1105 && msg.size() >= 18) {
                    const uint16_t plen = ReadBe16(&msg[16]);
                    if (size_t(18) + plen <= msg.size()) {
                        ParseDisplayLayout0x451(&msg[18], plen, &layout_);
                    }
                }
            }
        }
    }

    bool ReadOneEncryptedRecord(int timeout_ms, std::vector<uint8_t> *msg_out) {
        pollfd pfd{tcp_fd_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        uint8_t lb[2];
        if (!RecvExact(tcp_fd_, lb, 2, 3000)) return false;
        const uint16_t clen = ReadBe16(lb);
        if (clen < 32 || (clen % 16) != 0) return false;
        std::vector<uint8_t> cbuf(clen);
        if (!RecvExact(tcp_fd_, cbuf.data(), clen, 5000)) return false;
        if (!cipher_.DecryptRecordBody(cbuf.data(), cbuf.size(), msg_out)) return false;
        InspectControlMessage(*msg_out);
        return true;
    }

    void DrainEncryptedControl(int total_ms) {
        const int64_t end = NowNs() + int64_t(total_ms) * 1'000'000LL;
        while (NowNs() < end) {
            std::vector<uint8_t> msg;
            if (!ReadOneEncryptedRecord(30, &msg)) break;
        }
    }

    bool WaitVideoAnswer(const std::vector<uint8_t> &msg_1c, std::string *err_out) {
        auto try_read_window = [&](int window_ms) -> bool {
            const int64_t deadline = NowNs() + int64_t(window_ms) * 1'000'000LL;
            while (NowNs() < deadline) {
                std::vector<uint8_t> msg;
                if (!ReadOneEncryptedRecord(100, &msg)) continue;
                if (ExtractCanvasDims(msg, &canvas_)) return true;
            }
            return false;
        };
        if (try_read_window(opt_.virtual_display ? 2500 : 1500)) return true;
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!SendEncrypted(msg_1c)) break;
            if (try_read_window(500)) return true;
        }
        if (err_out) *err_out = "0x1c MediaStreamOptions answer did not yield a non-zero video canvas";
        return false;
    }

    bool DrainNonBlockingTcp(std::string *err_out, bool *got_data = nullptr) {
        for (;;) {
            uint8_t chunk[4096];
            const ssize_t n = recv(tcp_fd_, chunk, sizeof chunk, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                if (err_out) *err_out = "TCP control recv error";
                return false;
            }
            if (n == 0) {
                if (err_out) *err_out = "server closed TCP session";
                return false;
            }
            if (got_data) *got_data = true;
            tcp_rx_buf_.insert(tcp_rx_buf_.end(), chunk, chunk + n);
        }
        while (tcp_rx_buf_.size() >= 2) {
            const uint16_t clen = ReadBe16(tcp_rx_buf_.data());
            if (clen < 32 || (clen % 16) != 0) {
                if (err_out) *err_out = "malformed enc1103 record length";
                return false;
            }
            if (tcp_rx_buf_.size() < size_t(2) + clen) break;
            std::vector<uint8_t> msg;
            if (!cipher_.DecryptRecordBody(tcp_rx_buf_.data() + 2, clen, &msg)) {
                if (err_out) *err_out = "enc1103 record integrity failure";
                return false;
            }
            tcp_rx_buf_.erase(tcp_rx_buf_.begin(), tcp_rx_buf_.begin() + 2 + clen);
            InspectControlMessage(msg);
        }
        return true;
    }

    void SendVideoSrtcp(const std::vector<uint8_t> &rtcp) {
        if (rtcp.empty() || udp_video_fd_ < 0) return;
        std::vector<uint8_t> srtcp;
        if (video_tx_srtp_.EncryptRtcp(rtcp.data(), rtcp.size(), &srtcp)) {
            sendto(udp_video_fd_, srtcp.data(), srtcp.size(), 0,
                   reinterpret_cast<const sockaddr *>(&dest_video_sin_), sizeof dest_video_sin_);
        }
    }

    void SendAudioSrtcp(const std::vector<uint8_t> &rtcp) {
        if (rtcp.empty() || udp_ctrl_fd_ < 0) return;
        std::vector<uint8_t> srtcp;
        if (audio_tx_srtp_.EncryptRtcp(rtcp.data(), rtcp.size(), &srtcp)) {
            sendto(udp_ctrl_fd_, srtcp.data(), srtcp.size(), 0,
                   reinterpret_cast<const sockaddr *>(&dest_ctrl_sin_), sizeof dest_ctrl_sin_);
        }
    }

    void SendRtcpKeepalive(bool include_sr) {
        auto send_pair = [&](bool video) {
            const uint32_t ssrc = video ? offers_.video_ssrc : offers_.audio_ssrc;
            std::vector<uint8_t> pkt = BuildEmptyRr(ssrc);
            if (include_sr) {
                const auto sr = BuildEmptySr(ssrc);
                std::vector<uint8_t> compound = sr;
                compound.insert(compound.end(), pkt.begin(), pkt.end());
                pkt = std::move(compound);
            }
            if (video) SendVideoSrtcp(pkt);
            else SendAudioSrtcp(pkt);
        };
        send_pair(true);
        send_pair(false);
    }

    SessionOptions opt_{};
    in_addr dest_addr_{};
    std::string dest_ip_;
    sockaddr_in dest_ctrl_sin_{};
    sockaddr_in dest_video_sin_{};
    int tcp_fd_ = -1;
    int udp_ctrl_fd_ = -1;
    int udp_video_fd_ = -1;
    uint16_t server_w_ = 0;
    uint16_t server_h_ = 0;
    std::string auth_used_;
    bool srp_not_offered_ = false;
    StreamCipher cipher_;
    BuiltOffers offers_{};
    NegotiationKeys keys_{};
    SrtpContext video_rx_srtp_;
    SrtpContext video_tx_srtp_;
    SrtpContext audio_rx_srtp_;
    SrtpContext audio_tx_srtp_;
    NegotiatedCanvas canvas_{};
    DisplayLayout0x451 layout_{};
    H264RtpAssembler assembler_;
    EvdevKeyMapper key_mapper_;
    uint8_t buttons_mask_ = 0;
    uint16_t last_ptr_x_ = 0;
    uint16_t last_ptr_y_ = 0;
    double scroll_accum_y_ = 0.0;
    std::vector<uint8_t> tcp_rx_buf_;
    uint32_t last_media_ssrc_ = 0;
    uint64_t video_rtp_pkts_ = 0;
    uint64_t video_rtp_bytes_ = 0;
    uint64_t audio_rtp_pkts_ = 0;
    int64_t last_tx_pulse_ns_ = 0;
    uint64_t pulse_tick_ = 0;
    SessionLiveness liveness_{};
};

}  // namespace ft_apple_hp
