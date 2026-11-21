// ft-appless: Apple macOS High Performance Screen Sharing backend for ft-screens (MIT).
// Connects directly to macOS screensharingd (TCP/UDP 5900/5901, RFB 003.889 + RSA1/SRP-6a +
// enc1103 + AVCMediaStreamNegotiator SRTP H.264 4:2:0 + AAC-ELD), decodes with zero frame hold
// on Qualcomm Iris (/dev/video-dec0 -> Q08C UBWC), converts to a 3-buffer RGBA UBWC ring on
// Adreno GLES, and speaks ft-screens' exact SOCK_SEQPACKET + SCM_RIGHTS remote-screen protocol.
//
//   ft-appless auth HOST[:PORT] [--user USER --password-stdin | --login] [--via ADDRESS] [--no-verify]
//   ft-appless check HOST[:PORT]
//   ft-appless unauth HOST[:PORT]
//   ft-appless probe HOST[:PORT]
//   ft-appless stream HOST[:PORT] --id NAME --fd N --app apple:virtual|apple:virtual@2|apple:virtual@1|apple:physical
//                     [--size 3840x2160] [--fps 60] [--bitrate kbps]
//
// Credentials live in ~/.local/share/frametop-stream/hosts/HOST.apple (0600, O_NOFOLLOW), never
// in ~/.config/frametop-layout.json or command-line arguments.
#include "iris.h"
#include "apple_hp.h"

#include <dlfcn.h>
#include <nlohmann/json.hpp>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

constexpr int kDefaultApplePort = 5900;
constexpr unsigned kExitWatchdogS = 25;
constexpr int kHiddenFps = 10;

std::atomic<bool> g_hidden{false};

bool SplitHostPort(std::string *host, int *port) {
    const size_t colon = host->rfind(':');
    if (colon == std::string::npos) return true;
    const std::string sub = host->substr(colon + 1);
    if (sub.empty()) return false;
    for (char c : sub) {
        if (c < '0' || c > '9') return false;
    }
    const int p = std::atoi(sub.c_str());
    if (p <= 0 || p > 65535) return false;
    *port = p;
    host->resize(colon);
    return !host->empty();
}

bool Answers(const std::string &address, int port, int ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(address.c_str(), std::to_string(port ? port : kDefaultApplePort).c_str(), &hints, &res) != 0)
        return false;
    bool ok = false;
    for (addrinfo *a = res; a && !ok; a = a->ai_next) {
        const int s = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0) ok = true;
        else if (errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            ok = poll(&p, 1, ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

std::string LayoutPath() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/frametop-layout.json";
}

nlohmann::json Layout() {
    std::ifstream f(LayoutPath());
    const nlohmann::json layout = nlohmann::json::parse(f, nullptr, false);
    return layout.is_object() && layout.contains("hosts") && layout["hosts"].is_array() ? layout : nlohmann::json();
}

std::string Route(const std::string &host, int port) {
    const nlohmann::json layout = Layout();
    if (layout.is_null()) return host;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host) continue;
        const std::string route = h.value("route", "auto");
        if (route == "network") return host;
        if (h.contains("direct") && h["direct"].is_array()) {
            for (const auto &d : h["direct"]) {
                if (d.is_string() && Answers(d.get<std::string>(), port, 1000)) return d.get<std::string>();
            }
        }
        return route == "dongle" ? "" : host;
    }
    return host;
}

ft_apple_hp::KeyboardMappingConfig LoadKeyboardMapping(const std::string &host, const std::string &id) {
    ft_apple_hp::KeyboardMappingConfig out;
    const nlohmann::json layout = Layout();
    if (layout.is_null()) return out;

    const nlohmann::json *matched_host = nullptr;
    const nlohmann::json *matched_disp = nullptr;
    for (int pass = 0; pass < 2 && !matched_disp && !id.empty(); ++pass) {
        for (const auto &h : layout["hosts"]) {
            if (!h.is_object() || !h.contains("displays") || !h["displays"].is_array()) continue;
            if (pass == 0 && h.value("address", "") != host) continue;
            for (const auto &d : h["displays"]) {
                if (d.is_object() && d.value("client", "") == id) {
                    matched_host = &h;
                    matched_disp = &d;
                    break;
                }
            }
            if (matched_disp) break;
        }
    }
    if (!matched_host) {
        for (const auto &h : layout["hosts"]) {
            if (h.is_object() && h.value("address", "") == host) {
                matched_host = &h;
                break;
            }
        }
    }

    std::string kb_layout;
    std::vector<std::pair<std::string, std::string>> keymap;
    auto merge_from = [&](const nlohmann::json *obj) {
        if (!obj || !obj->is_object()) return;
        if (obj->contains("kb_layout") && (*obj)["kb_layout"].is_string()) {
            const std::string v = (*obj)["kb_layout"].get<std::string>();
            if (!v.empty()) kb_layout = v;
        }
        if (obj->contains("keymap") && (*obj)["keymap"].is_object()) {
            for (auto it = (*obj)["keymap"].begin(); it != (*obj)["keymap"].end(); ++it) {
                if (it.value().is_string()) keymap.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
    };
    merge_from(matched_host);
    merge_from(matched_disp);
    ft_apple_hp::ApplyKeyboardSettings(kb_layout, keymap, &out);
    return out;
}

struct Viewer {
    int fd = -1;

    bool Send(const std::string &msg, const int *fds = nullptr, int nfds = 0) const {
        if (fd < 0) return false;
        iovec iov = {const_cast<char *>(msg.data()), msg.size()};
        msghdr m{};
        m.msg_iov = &iov, m.msg_iovlen = 1;
        char control[CMSG_SPACE(sizeof(int) * 4)] = {};
        if (nfds) {
            m.msg_control = control, m.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
            cmsghdr *c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
            std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
        }
        return sendmsg(fd, &m, MSG_NOSIGNAL) == ssize_t(msg.size());
    }

    bool Read(std::string *msg, bool wait) const {
        char buf[512];
        const ssize_t n = recv(fd, buf, sizeof buf - 1, wait ? 0 : MSG_DONTWAIT);
        if (n > 0) return *msg = std::string(buf, size_t(n)), true;
        msg->clear();
        return n < 0 && (errno == EAGAIN || errno == EINTR);
    }
};

// Dynamic fdk-aac + PipeWire/PulseAudio stereo AAC-ELD player.
// Uses dlopen("libfdk-aac.so.2") so no fdk-aac-devel headers are needed in setup/packages.txt.
class AacEldPlayer {
public:
    ~AacEldPlayer() { Stop(); }

    bool ClaimAndInit(const std::string &host) {
        name_ = "Remote display: " + host;
        const char *dir = std::getenv("XDG_RUNTIME_DIR");
        const std::string lock_path = std::string(dir && *dir ? dir : "/tmp") + "/frametop-audio-" + host + ".lock";
        lock_fd_ = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            if (lock_fd_ >= 0) close(lock_fd_), lock_fd_ = -1;
            return false;
        }
        if (!LoadFdk()) {
            close(lock_fd_), lock_fd_ = -1;
            return false;
        }
        running_ = true;
        done_ = false;
        worker_ = std::thread(&AacEldPlayer::Loop, this);
        std::printf("audio: this stream plays %s's sound\n", host.c_str());
        std::fflush(stdout);
        return true;
    }

    bool active() const { return running_; }

    void Enqueue(std::vector<std::vector<uint8_t>> &&aus) {
        if (!running_ || aus.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto &au : aus) {
                if (queue_.size() >= 16) queue_.pop_front();
                queue_.push_back(std::move(au));
            }
        }
        cv_.notify_one();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_ && !worker_.joinable()) {
                if (lock_fd_ >= 0) close(lock_fd_), lock_fd_ = -1;
                return;
            }
            running_ = false;
        }
        cv_.notify_all();
        for (int i = 0; i < 40 && !done_; ++i) usleep(10'000);
        if (worker_.joinable()) {
            if (done_) worker_.join();
            else worker_.detach();
        }
        if (lock_fd_ >= 0) close(lock_fd_), lock_fd_ = -1;
    }

private:
    // Minimal C ABI for libfdk-aac.so.2 (TT_MP4_RAW = 0).
    // On LP64 aarch64, CStreamInfo starts with { int sampleRate; int frameSize; int numChannels; ... }.
    struct FdkStreamInfoPrefix {
        int sample_rate;
        int frame_size;
        int num_channels;
    };
    using FnOpen = void *(*)(int transport_type, unsigned int nr_of_layers);
    using FnConfigRaw = int (*)(void *self, unsigned char *conf[], const unsigned int length[]);
    using FnFill = int (*)(void *self, unsigned char *p_buffer[], const unsigned int buffer_size[],
                           unsigned int *bytes_valid);
    using FnDecodeFrame = int (*)(void *self, int16_t *p_time_data, int time_data_size, unsigned int flags);
    using FnGetStreamInfo = const FdkStreamInfoPrefix *(*)(void *self);
    using FnClose = void (*)(void *self);

    bool LoadFdk() {
        dl_ = dlopen("libfdk-aac.so.2", RTLD_NOW);
        if (!dl_) return false;
        fn_open_ = reinterpret_cast<FnOpen>(dlsym(dl_, "aacDecoder_Open"));
        fn_config_raw_ = reinterpret_cast<FnConfigRaw>(dlsym(dl_, "aacDecoder_ConfigRaw"));
        fn_fill_ = reinterpret_cast<FnFill>(dlsym(dl_, "aacDecoder_Fill"));
        fn_decode_ = reinterpret_cast<FnDecodeFrame>(dlsym(dl_, "aacDecoder_DecodeFrame"));
        fn_info_ = reinterpret_cast<FnGetStreamInfo>(dlsym(dl_, "aacDecoder_GetStreamInfo"));
        fn_close_ = reinterpret_cast<FnClose>(dlsym(dl_, "aacDecoder_Close"));
        return fn_open_ && fn_config_raw_ && fn_fill_ && fn_decode_ && fn_info_ && fn_close_;
    }

    void *OpenWithAsc(const uint8_t *asc, unsigned int asc_len) {
        void *dec = fn_open_(0, 1);
        if (!dec) return nullptr;
        unsigned char *p = const_cast<unsigned char *>(asc);
        if (fn_config_raw_(dec, &p, &asc_len) != 0) {
            fn_close_(dec);
            return nullptr;
        }
        return dec;
    }

    void Loop() {
        // Candidate Apple AVCAudioStream AAC-ELD AudioSpecificConfigs:
        // 0: 48 kHz stereo, 480 samples (10 ms, AVCAudioStream mode 8 default)
        // 1: 48 kHz stereo, 512 samples
        // 2: 24 kHz stereo, 480 samples
        // 3: 16 kHz stereo, 480 samples
        static constexpr uint8_t kAscCandidates[4][4] = {
            {0xf8, 0xe6, 0x50, 0x00},
            {0xf8, 0xe6, 0x40, 0x00},
            {0xf8, 0xec, 0x50, 0x00},
            {0xf8, 0xf0, 0x50, 0x00},
        };
        void *dec = OpenWithAsc(kAscCandidates[0], 4);
        bool locked_asc = false;
        pa_simple *pa = nullptr;
        int pa_rate = 0, pa_ch = 0;
        bool pa_failed = false;
        std::vector<int16_t> pcm(4096);

        for (;;) {
            std::vector<uint8_t> au;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
                if (!running_) break;
                au = std::move(queue_.front());
                queue_.pop_front();
            }
            if (au.empty() || !dec) continue;

            auto try_decode = [&](void *d, const std::vector<uint8_t> &pkt) -> int {
                unsigned char *ptr = const_cast<unsigned char *>(pkt.data());
                const unsigned int sz = unsigned(pkt.size());
                unsigned int valid = sz;
                if (fn_fill_(d, &ptr, &sz, &valid) != 0) return -1;
                return fn_decode_(d, pcm.data(), int(pcm.size()), 0);
            };

            int rc = try_decode(dec, au);
            if (rc != 0 && !locked_asc) {
                for (size_t k = 1; k < 4; ++k) {
                    void *alt = OpenWithAsc(kAscCandidates[k], 4);
                    if (!alt) continue;
                    if (try_decode(alt, au) == 0) {
                        fn_close_(dec);
                        dec = alt;
                        rc = 0;
                        break;
                    }
                    fn_close_(alt);
                }
            }
            if (rc != 0) continue;
            locked_asc = true;

            const FdkStreamInfoPrefix *info = fn_info_(dec);
            if (!info || info->sample_rate <= 0 || info->frame_size <= 0 || info->num_channels <= 0) continue;
            if ((!pa || pa_rate != info->sample_rate || pa_ch != info->num_channels) && !pa_failed) {
                if (pa) pa_simple_free(pa), pa = nullptr;
                const pa_sample_spec spec = {PA_SAMPLE_S16LE, uint32_t(info->sample_rate), uint8_t(info->num_channels)};
                pa_buffer_attr attr{};
                attr.maxlength = uint32_t(-1);
                attr.prebuf = uint32_t(-1);
                attr.minreq = uint32_t(-1);
                attr.fragsize = uint32_t(-1);
                attr.tlength = uint32_t(pa_usec_to_bytes(40'000, &spec));
                int err = 0;
                pa = pa_simple_new(nullptr, "Frametop", PA_STREAM_PLAYBACK, nullptr, name_.c_str(), &spec, nullptr,
                                   &attr, &err);
                if (!pa) {
                    pa_failed = true;
                    std::fprintf(stderr, "audio: can't play: %s\n", pa_strerror(err));
                } else {
                    pa_rate = info->sample_rate;
                    pa_ch = info->num_channels;
                }
            }
            if (pa) {
                int err = 0;
                const size_t bytes = size_t(info->frame_size) * size_t(info->num_channels) * sizeof(int16_t);
                if (bytes <= pcm.size() * sizeof(int16_t)) pa_simple_write(pa, pcm.data(), bytes, &err);
            }
        }

        if (pa) pa_simple_free(pa);
        if (dec) fn_close_(dec);
        if (dl_) dlclose(dl_), dl_ = nullptr;
        done_ = true;
    }

    std::string name_;
    int lock_fd_ = -1;
    void *dl_ = nullptr;
    FnOpen fn_open_ = nullptr;
    FnConfigRaw fn_config_raw_ = nullptr;
    FnFill fn_fill_ = nullptr;
    FnDecodeFrame fn_decode_ = nullptr;
    FnGetStreamInfo fn_info_ = nullptr;
    FnClose fn_close_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> done_{true};
    std::thread worker_;
};

int RunStream(const std::string &host, const std::string &route, int port, const std::string &id,
              const std::string &app, int viewer_fd, int width, int height, int fps, int bitrate) {
    Viewer viewer{viewer_fd};
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);

    const std::string layout_path = LayoutPath();
    struct stat layout_st{};
    bool has_layout_st = (stat(layout_path.c_str(), &layout_st) == 0);

    ft_apple_hp::SessionOptions opt;
    opt.host = route;
    opt.port = uint16_t(port ? port : kDefaultApplePort);
    opt.fps = fps;
    opt.keyboard = LoadKeyboardMapping(host, id);
    ft_apple_hp::ResolveAppleDisplayMode(app, width, height, &opt);

    const std::string creds_path = ft_apple_hp::AppleCredsPath(host);
    // Reconnects use the mode that worked at sign-in, never the auto fallback.
    if (!ft_apple_hp::LoadAppleCreds(creds_path, &opt.username, &opt.password, &opt.auth_mode)) {
        viewer.Send("state lost not signed in");
        std::fprintf(stderr, "no credentials in %s: sign in with Frametop Settings or ft-appless auth %s\n",
                     creds_path.c_str(), host.c_str());
        return 1;
    }

    // Read SteamVR-importable modifiers from ft-screens before initializing GBM/EGL.
    Converter conv;
    conv.toSteamVR = false;
    conv.matrix = EGL_ITU_REC709_EXT;
    conv.range = EGL_YUV_FULL_RANGE_EXT;
    for (std::string msg; viewer.Read(&msg, true) && !msg.empty();) {
        if (msg.rfind("modifiers", 0) != 0) continue;
        std::istringstream words(msg.substr(9));
        for (std::string m; words >> m;) conv.modifiers.push_back(std::strtoull(m.c_str(), nullptr, 16));
        break;
    }

    Decoder dec;
    dec.codec = V4L2_PIX_FMT_H264;
    dec.fourcc = V4L2_PIX_FMT_QC08C;
    dec.toSteamVR = false;
    if (!dec.Open("/dev/video-dec0", 8) || !conv.Init()) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        return viewer.Send("state lost no decoder"), 1;
    }

    AacEldPlayer audio;
    opt.audio_enabled = audio.ClaimAndInit(host);

    viewer.Send("state connecting");
    ft_apple_hp::SessionClient client;
    std::string err;
    if (!client.Connect(opt, &err)) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        audio.Stop();
        dec.Close();
        std::fprintf(stderr, "apple-hp connect failed (%s): %s\n", route.c_str(), err.c_str());
        viewer.Send("state lost " + err.substr(0, 64));
        return 1;
    }
    OPENSSL_cleanse(opt.password.data(), opt.password.size());

    std::printf("apple-hp live: host=%s%s auth=%s app=%s server_init=%ux%u layout=%ux%u(backing %ux%u) canvas=%ux%u fps=%d bitrate=%d kb_layout=%s keymap=%zu\n",
                host.c_str(), route != host ? (" through " + route).c_str() : "",
                client.auth_used().c_str(), app.c_str(),
                client.server_init_w(), client.server_init_h(),
                client.layout().scaled_w, client.layout().scaled_h,
                client.layout().backing_w, client.layout().backing_h,
                client.canvas().width, client.canvas().height, fps, bitrate,
                ft_apple_hp::KeyboardLayoutName(client.keyboard_mapping().layout),
                client.keyboard_mapping().keymap.size());
    std::fflush(stdout);

    bool owned[3] = {};
    bool live = false;
    bool ended_by_host = false;
    std::string end_reason;
    uint64_t seq = 0;
    uint32_t cur_sps_w = 0, cur_sps_h = 0;
    uint64_t prev_bytes = 0;
    int64_t last_report = MonoNs(), last_kb_check = last_report, cpu0 = CpuNs(), last_shown = 0, all_held_since = -1;
    int fed_cnt = 0, shown = 0, dropped = 0, rested = 0, last_sent = -1;

    while (!g_stop) {
        std::vector<ft_apple_hp::H264RtpAssembler::AccessUnit> aus;
        std::vector<std::vector<uint8_t>> audio_aus;
        std::string poll_err;
        // Poll network + decoder fd; also check viewer socket every <= 8 ms for crisp pointer/key latency.
        if (!client.PollStep(4, &aus, &poll_err, dec.fd, opt.audio_enabled ? &audio_aus : nullptr)) {
            std::fprintf(stderr, "apple-hp session ended: %s\n", poll_err.c_str());
            ended_by_host = true;
            end_reason = poll_err;
            break;
        }
        if (!audio_aus.empty()) audio.Enqueue(std::move(audio_aus));

        // Process messages from ft-screens: input, released ring buffers, attention, quit.
        std::string msg;
        bool open = true;
        while ((open = viewer.Read(&msg, false)) && !msg.empty()) {
            double x = 0, y = 0;
            unsigned c = 0;
            int down = 0, idx = 0;
            const uint32_t stream_w = conv.width ? conv.width : (client.canvas().width ? client.canvas().width : uint32_t(width));
            const uint32_t stream_h = conv.height ? conv.height : (client.canvas().height ? client.canvas().height : uint32_t(height));
            if (std::sscanf(msg.c_str(), "move %lf %lf", &x, &y) == 2) {
                client.SendPointerMove(x, y, stream_w, stream_h);
            } else if (std::sscanf(msg.c_str(), "button %u %d", &c, &down) == 2) {
                client.SendMouseButton(c, down != 0);
            } else if (std::sscanf(msg.c_str(), "scroll %lf %lf", &x, &y) == 2) {
                client.SendMouseScroll(x, y);
            } else if (std::sscanf(msg.c_str(), "key %u %d", &c, &down) == 2 ||
                       std::sscanf(msg.c_str(), "peerkey %u %d", &c, &down) == 2) {
                client.SendEvdevKey(c, down != 0);
            } else if (msg == "blur") {
                client.SendInputBlur();
            } else if (msg.rfind("pad ", 0) == 0) {
                // Apple Screen Sharing has no virtual Xbox controller channel; ignore cleanly.
            } else if (std::sscanf(msg.c_str(), "release %d", &idx) == 1) {
                if (idx >= 0 && idx < 3) owned[idx] = false;
            } else if (msg.rfind("attention ", 0) == 0) {
                g_hidden = (msg == "attention hidden");
            } else if (msg == "quit") {
                g_stop = 1;
            }
        }
        if (!open) break;

        // If SPS dimensions change mid-stream (e.g. display mode switch), reset decoder smoothly.
        const auto &sps = client.assembler().raw_sps_info();
        if (sps.width > 0 && sps.height > 0 && cur_sps_w > 0 &&
            (sps.width != cur_sps_w || sps.height != cur_sps_h)) {
            std::printf("apple-hp resolution changed %ux%u -> %ux%u, resetting decoder\n",
                        cur_sps_w, cur_sps_h, sps.width, sps.height);
            std::fflush(stdout);
            conv.Reset();
            dec.Close();
            if (!dec.Open("/dev/video-dec0", 8)) break;
            cur_sps_w = sps.width;
            cur_sps_h = sps.height;
            client.RequestKeyframe();
        } else if (sps.width > 0 && cur_sps_w == 0) {
            cur_sps_w = sps.width;
            cur_sps_h = sps.height;
        }

        if (dec.Events()) {
            uint32_t crop_w = 0, crop_h = 0;
            ft_apple_hp::ResolveVisibleCrop(dec.width, dec.height, sps.width, sps.height,
                                            client.backing_pointer_w(), client.backing_pointer_h(),
                                            &crop_w, &crop_h);
            if (crop_w != dec.width || crop_h != dec.height) {
                std::printf("apple-hp crop: decoder %ux%u -> visible %ux%u (sps %ux%u, backing %ux%u)\n",
                            dec.width, dec.height, crop_w, crop_h, sps.width, sps.height,
                            uint32_t(client.backing_pointer_w()), uint32_t(client.backing_pointer_h()));
                std::fflush(stdout);
            }
            if (!conv.Setup(dec, crop_w, crop_h)) break;
            const int fds[3] = {conv.out[0].fd, conv.out[1].fd, conv.out[2].fd};
            char b[160];
            std::snprintf(b, sizeof b, "buffers %u %u %x %llx %u %u", conv.width, conv.height, DRM_FORMAT_ABGR8888,
                          (unsigned long long)conv.out[0].modifier, conv.out[0].offset, conv.out[0].stride);
            if (!viewer.Send(b, fds, 3)) break;
            for (bool &o : owned) o = false;
        }

        dec.Reclaim();
        for (const auto &au : aus) {
            if (dec.Feed(au.annex_b.data(), au.annex_b.size(), seq)) {
                ++seq;
                ++fed_cnt;
            } else {
                client.RequestKeyframe();
            }
        }

        uint64_t frame = 0;
        bool last = false;
        int cap_idx = -1;
        while ((cap_idx = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t at0 = MonoNs();
            if (g_hidden && at0 - last_shown < 1'000'000'000 / kHiddenFps) {
                dec.Requeue(cap_idx);
                ++rested;
                continue;
            }
            int at = -1;
            for (int k = 0; k < 3 && at < 0; ++k) {
                if (!owned[(conv.next + k) % 3]) at = (conv.next + k) % 3;
            }
            if (at < 0) {
                dec.Requeue(cap_idx);
                ++dropped;
                if (all_held_since < 0) all_held_since = at0;
                else if (at0 - all_held_since > 2'000'000'000) {
                    std::printf("ring: no buffer back from ft-screens for 2 s, taking them back\n");
                    for (int k = 0; k < 3; ++k) owned[k] = (k == last_sent);
                    all_held_since = -1;
                }
                continue;
            }
            all_held_since = -1;
            conv.ConvertInto(cap_idx, at);
            conv.next = (at + 1) % 3;
            dec.Requeue(cap_idx);
            owned[at] = true;
            if (!viewer.Send("frame " + std::to_string(at))) g_stop = 1;
            last_sent = at;
            last_shown = at0;
            ++shown;
            if (!live) {
                live = true;
                viewer.Send(route != host ? "state live via dongle" : "state live via network");
            }
        }

        const int64_t now = MonoNs();
        if (now - last_kb_check >= 2'000'000'000LL) {
            last_kb_check = now;
            struct stat cur_st{};
            const bool ok_st = (stat(layout_path.c_str(), &cur_st) == 0);
            if (ok_st && (!has_layout_st || cur_st.st_ino != layout_st.st_ino || cur_st.st_size != layout_st.st_size ||
                          cur_st.st_mtim.tv_sec != layout_st.st_mtim.tv_sec ||
                          cur_st.st_mtim.tv_nsec != layout_st.st_mtim.tv_nsec)) {
                layout_st = cur_st;
                has_layout_st = true;
                const ft_apple_hp::KeyboardMappingConfig cur_kb = LoadKeyboardMapping(host, id);
                if (client.SetKeyboardMapping(cur_kb)) {
                    std::printf("apple-hp keyboard updated: kb_layout=%s keymap=%zu\n",
                                ft_apple_hp::KeyboardLayoutName(cur_kb.layout), cur_kb.keymap.size());
                    std::fflush(stdout);
                }
            }
        }
        if (now - last_report >= 10'000'000'000LL) {
            const double secs = (now - last_report) / 1e9;
            const uint64_t cur_bytes = client.video_rtp_bytes();
            const double mbps = (cur_bytes - prev_bytes) * 8.0 / secs / 1e6;
            prev_bytes = cur_bytes;
            std::printf("%s: in %.1f fps, shown %.1f fps (%d dropped, %d out of sight), %.1f Mbit/s, "
                        "convert %.2f ms, lost %llu pkts, CPU %.1f%% of a core%s\n",
                        g_hidden ? "hidden" : "shown", fed_cnt / secs, shown / secs, dropped, rested, mbps,
                        shown ? conv.msSum / shown : 0.0,
                        (unsigned long long)client.assembler().lost_packets(),
                        100.0 * (CpuNs() - cpu0) / 1e9 / secs,
                        audio.active() ? ", playing its sound" : "");
            std::fflush(stdout);
            conv.msSum = 0;
            fed_cnt = shown = dropped = rested = 0;
            last_report = now;
            cpu0 = CpuNs();
        }
    }

    g_stop = 1;
    alarm(kExitWatchdogS);
    viewer.Send(ended_by_host ? ("state lost " + (end_reason.empty() ? std::string("the host ended the stream")
                                                                     : end_reason.substr(0, 64)))
                              : "state lost");
    if (!ended_by_host) client.SendInputBlur();
    audio.Stop();
    client.Close();
    dec.Close();
    return ended_by_host ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: ft-appless auth|check|unauth|probe|stream HOST[:PORT] [options]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::string host = argv[2];
    int port = 0;
    if (!SplitHostPort(&host, &port)) return std::fprintf(stderr, "bad port in %s\n", argv[2]), 2;

    std::string id, app = "apple:virtual", user, via_addr;
    int width = 3840, height = 2160, fps = 60, bitrate = 0, fd = -1;
    bool pw_stdin = false, login = false, no_verify = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--id") id = v, ++i;
        else if (a == "--app") app = v, ++i;
        else if (a == "--user") user = v, ++i;
        else if (a == "--via") via_addr = v, ++i;
        else if (a == "--password-stdin") pw_stdin = true;
        else if (a == "--login") login = true;
        else if (a == "--no-verify") no_verify = true;
        else if (a == "--size") std::sscanf(v, "%dx%d", &width, &height), ++i;
        else if (a == "--fps") fps = std::atoi(v), ++i;
        else if (a == "--bitrate") bitrate = std::atoi(v), ++i;
        else if (a == "--fd") fd = std::atoi(v), ++i;
        else return std::fprintf(stderr, "unknown option %s\n", argv[i]), 2;
    }

    const std::string creds_path = ft_apple_hp::AppleCredsPath(host);
    if (cmd == "check") {
        std::string u, p;
        const bool ok = ft_apple_hp::LoadAppleCreds(creds_path, &u, &p);
        OPENSSL_cleanse(p.data(), p.size());
        if (!ok) return std::fprintf(stderr, "no credentials in %s\n", creds_path.c_str()), 1;
        std::printf("credentials ready for %s (%s)\n", host.c_str(), u.c_str());
        return 0;
    }
    if (cmd == "unauth") {
        unlink(creds_path.c_str());
        std::printf("removed credentials for %s\n", host.c_str());
        return 0;
    }
    if (cmd == "probe") {
        const std::string target = !via_addr.empty() ? via_addr : Route(host, port);
        if (target.empty()) return std::fprintf(stderr, "%s: no direct address answers\n", host.c_str()), 1;
        ft_apple_hp::ProbeInfo info;
        std::string err;
        if (!ft_apple_hp::ProbeServer(target, uint16_t(port ? port : kDefaultApplePort), "", &info, &err))
            return std::fprintf(stderr, "probe failed: %s\n", err.c_str()), 1;
        std::printf("apple-hp %s: rsa_bits=%d\n", target.c_str(), info.rsa_key_bits);
        return 0;
    }
    if (cmd == "auth") {
        std::string password;
        if (login) {
            if (!std::getline(std::cin, user) || !std::getline(std::cin, password))
                return std::fprintf(stderr, "--login: expected username and password lines on stdin\n"), 2;
            if (!user.empty() && user.back() == '\r') user.pop_back();
            if (!password.empty() && password.back() == '\r') password.pop_back();
        } else if (pw_stdin) {
            if (user.empty()) return std::fprintf(stderr, "auth --password-stdin requires --user USER\n"), 2;
            if (!std::getline(std::cin, password)) return std::fprintf(stderr, "no password on stdin\n"), 2;
            if (!password.empty() && password.back() == '\r') password.pop_back();
        } else {
            return std::fprintf(stderr, "usage: ft-appless auth HOST[:PORT] (--login | --user USER --password-stdin) [--via ADDRESS] [--no-verify]\n"), 2;
        }
        if (user.empty() || password.empty()) {
            OPENSSL_cleanse(password.data(), password.size());
            return std::fprintf(stderr, "username and password must not be empty\n"), 2;
        }
        // --no-verify saves SRP: the mode is only RSA when a Mac showed it has no SRP. A Mac
        // that signed in with SRP before stays SRP: signing in again never falls back to RSA,
        // which a spoofed server could otherwise ask for (unauth first to allow it).
        ft_apple_hp::AuthMode mode = ft_apple_hp::AuthMode::kSrp, known = ft_apple_hp::AuthMode::kAuto;
        {
            std::string old_user, old_password;
            if (!ft_apple_hp::LoadAppleCreds(creds_path, &old_user, &old_password, &known))
                known = ft_apple_hp::AuthMode::kAuto;
            OPENSSL_cleanse(old_password.data(), old_password.size());
        }
        if (!no_verify) {
            std::string target;
            if (!via_addr.empty() && Answers(via_addr, port, 1000)) target = via_addr;
            else target = Route(host, port);
            if (target.empty()) target = host;
            ft_apple_hp::SessionOptions opt;
            opt.host = target;
            opt.port = uint16_t(port ? port : kDefaultApplePort);
            opt.username = user;
            opt.password = password;
            if (known == ft_apple_hp::AuthMode::kSrp) opt.auth_mode = ft_apple_hp::AuthMode::kSrp;
            ft_apple_hp::SessionClient client;
            std::string err;
            if (!client.VerifyCredentials(opt, &err)) {
                OPENSSL_cleanse(password.data(), password.size());
                OPENSSL_cleanse(opt.password.data(), opt.password.size());
                if (known == ft_apple_hp::AuthMode::kSrp && client.srp_not_offered())
                    err += " (it signed in with SRP before; ft-appless unauth " + host + " first to sign in without it)";
                return std::fprintf(stderr, "%s\n", err.c_str()), 1;
            }
            OPENSSL_cleanse(opt.password.data(), opt.password.size());
            if (client.auth_used() == "nonsrp") mode = ft_apple_hp::AuthMode::kNonSrp;
        }
        const bool saved = ft_apple_hp::SaveAppleCreds(creds_path, user, password, mode);
        OPENSSL_cleanse(password.data(), password.size());
        if (!saved) return std::fprintf(stderr, "failed to write %s\n", creds_path.c_str()), 1;
        std::printf("signed in to %s as %s\n", host.c_str(), user.c_str());
        return 0;
    }
    if (cmd == "stream") {
        if (id.empty()) return std::fprintf(stderr, "--id NAME is required\n"), 2;
        if (fd < 0) return std::fprintf(stderr, "stream needs --fd: ft-screens starts it\n"), 2;
        if (width < 64 || height < 64 || width > 8192 || height > 8192 || fps < 1 || fps > 240)
            return std::fprintf(stderr, "bad --size or --fps\n"), 2;
        const std::string route = Route(host, port);
        if (route.empty()) {
            std::fprintf(stderr, "%s: no direct address answers (route dongle)\n", host.c_str());
            Viewer{fd}.Send("state lost the dongle link is down");
            return 1;
        }
        return RunStream(host, route, port, id, app, fd, width, height, fps, bitrate);
    }
    return std::fprintf(stderr, "unknown command %s\n", cmd.c_str()), 2;
}
