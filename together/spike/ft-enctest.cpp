// ft-enctest: spikes T1, T2 and T4 for Frametop Together (docs/together.md). Runs N sessions of
// the Frame's hardware encoder at once (media/irisenc), each fed a desktop-like test picture at a
// steady rate, and reports what a shared panel would cost.
//
//   ft-enctest [--sessions n] [--size WxH] [--fps f] [--seconds s] [--bitrate bits] [--dmabuf]
//              [--h264] [--busy] [--step] [--out FILE]
//
// --dmabuf feeds GBM buffers (ABGR8888, linear: what ft-screens' ring holds) with no copy, and
// says whether the encoder takes their layout as it is (T2). --busy changes the whole picture
// every frame (video) and not just a moving bar (a desktop). --step cuts the bitrate to a quarter
// and asks for a keyframe halfway, to show both controls work on a running session. --out writes
// session 0's stream (Annex B) to FILE.
//
// RGBA only: the encoder of SteamOS 0.5.10 takes none, so it ends there with "the encoder takes no RGBA".
//
// For each session: pictures in and out, bytes, the bitrate it made, keyframes, and the time from
// queueing a picture to holding its encoded unit. Then this process's CPU use over the run, and
// the codec's and the hottest thermal zone before and after. Build: together/build.sh --spikes; run in the dev
// container through frame-job.
#include "../media/irisenc.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

struct Args {
    int sessions = 1, width = 1920, height = 1080, fps = 30, seconds = 10, bitrate = 3000000;
    bool dmabuf = false, h264 = false, busy = false, step = false;
    const char *out = nullptr;
};

struct Result {
    bool opened = false;
    std::string err;
    int in = 0, out = 0, keys = 0, dropped = 0;
    size_t bytes = 0, bytesAfterStep = 0;
    int outAfterStep = 0;
    int64_t latencySum = 0, latencyMax = 0;
    int encStride = 0, bufStride = 0;
    size_t encSize = 0, bufSize = 0;
};

int64_t NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// A grey desktop with a bar that moves a little every frame; with `busy`, a whole picture that
// scrolls.
void Draw(uint8_t *px, int stride, int w, int h, int frame, bool busy, bool first) {
    if (busy) {
        for (int y = 0; y < h; ++y) {
            uint32_t *row = reinterpret_cast<uint32_t *>(px + size_t(y) * stride);
            for (int x = 0; x < w; ++x) row[x] = 0xff000000u | uint32_t((x + frame * 4) & 0xff) | uint32_t((y + frame) & 0xff) << 8;
        }
        return;
    }
    // Every buffer is painted whole once, and after that only the rows the bar touched.
    const int barH = 48, travel = std::max(1, h - barH), span = 8;
    auto fill = [&](int y0, int y1, uint32_t c) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
            uint32_t *row = reinterpret_cast<uint32_t *>(px + size_t(y) * stride);
            std::fill(row, row + w, c);
        }
    };
    if (first) fill(0, h, 0xff303030u);
    for (int back = 1; back <= span; ++back) {   // where the bar was when this buffer was last used
        const int y = ((frame - back) * 6 % travel + travel) % travel;
        fill(y, y + barH, 0xff303030u);
    }
    const int y = frame * 6 % travel;
    fill(y, y + barH, 0xff40c0f0u);
}

struct GbmPicture {
    gbm_bo *bo = nullptr;
    int fd = -1;
    int slot = -1;   // the encoder slot it's queued in, or -1
    bool painted = false;
};

void Session(const Args &a, int index, Result &r, const std::atomic<bool> &stop) {
    irisenc::Config cfg;
    cfg.width = a.width;
    cfg.height = a.height;
    cfg.fps = a.fps;
    cfg.bitrate = a.bitrate;
    cfg.hevc = !a.h264;
    cfg.dmabuf = a.dmabuf;
    irisenc::Encoder enc;
    if (!enc.Open(cfg, r.err)) return;
    if (enc.format() != irisenc::Format::kRgba) {
        r.err = "the encoder takes no RGBA";
        return;
    }
    r.opened = true;
    r.encStride = enc.stride();
    r.encSize = enc.frameSize();

    // A GBM device of its own, as each shared panel's program will have: one device isn't safe
    // to use from several threads at once.
    std::vector<GbmPicture> pics;
    gbm_device *gbm = nullptr;
    if (a.dmabuf) {
        const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        gbm = drm >= 0 ? gbm_create_device(drm) : nullptr;
        if (!gbm) {
            r.err = "no GBM device";
            r.opened = false;
            return;
        }
        pics.resize(4);
        for (GbmPicture &p : pics) {
            p.bo = gbm_bo_create(gbm, a.width, a.height, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
            p.fd = p.bo ? gbm_bo_get_fd(p.bo) : -1;
            if (p.fd < 0) {
                r.err = "gbm buffer";
                r.opened = false;
                return;
            }
        }
        r.bufStride = int(gbm_bo_get_stride(pics[0].bo));
        r.bufSize = size_t(lseek(pics[0].fd, 0, SEEK_END));
    }
    std::vector<bool> painted(8, false);
    FILE *out = a.out && index == 0 ? std::fopen(a.out, "wb") : nullptr;

    const int64_t start = NowUs(), period = 1000000 / a.fps, end = start + int64_t(a.seconds) * 1000000;
    int64_t next = start;
    bool stepped = false;
    std::vector<int> back;
    auto drain = [&] {
        irisenc::Unit u;
        while (enc.Dequeue(u)) {
            ++r.out;
            r.bytes += u.size;
            if (stepped) r.bytesAfterStep += u.size, ++r.outAfterStep;
            if (u.key) ++r.keys;
            const int64_t lat = NowUs() - u.ptsUs;
            r.latencySum += lat;
            r.latencyMax = std::max(r.latencyMax, lat);
            if (out) std::fwrite(u.data, 1, u.size, out);
        }
        enc.Returned(back);
        for (const int slot : back)
            for (GbmPicture &p : pics)
                if (p.slot == slot) p.slot = -1;
    };
    while (!stop.load() && NowUs() < end) {
        const int64_t now = NowUs();
        if (now < next) {
            pollfd pfd = {enc.fd(), POLLIN, 0};
            poll(&pfd, 1, int((next - now + 999) / 1000));
            drain();
            continue;
        }
        if (a.step && !stepped && now - start > int64_t(a.seconds) * 500000) {
            stepped = true;
            if (!enc.SetBitrate(a.bitrate / 4) || !enc.ForceKey()) r.err = enc.lastError();
        }
        drain();
        bool queued = false;
        if (a.dmabuf) {
            for (GbmPicture &p : pics) {
                if (p.slot >= 0) continue;
                uint32_t pitch = 0;
                void *data = nullptr;
                auto *px = static_cast<uint8_t *>(gbm_bo_map(p.bo, 0, 0, a.width, a.height, GBM_BO_TRANSFER_WRITE, &pitch, &data));
                if (!px) break;
                Draw(px, int(pitch), a.width, a.height, r.in, a.busy, !p.painted);
                p.painted = true;
                gbm_bo_unmap(p.bo, data);
                int slot = -1;
                queued = enc.QueueDmabuf(p.fd, r.bufSize, NowUs(), slot);
                if (queued) p.slot = slot;
                else if (r.err.empty()) r.err = enc.lastError();
                break;
            }
        } else {
            int slot = -1;
            if (uint8_t *px = enc.Map(slot)) {
                const bool first = slot < int(painted.size()) && !painted[slot];
                Draw(px, enc.stride(), a.width, a.height, r.in, a.busy, first);
                if (slot < int(painted.size())) painted[slot] = true;
                queued = enc.Queue(slot, NowUs());
                if (!queued && r.err.empty()) r.err = enc.lastError();
            }
        }
        if (queued) ++r.in;
        else ++r.dropped;
        next += period;
        if (NowUs() - next > 4 * period) next = NowUs();   // fell behind: don't burst to catch up
    }
    // What is still inside the encoder.
    for (int64_t until = NowUs() + 300000; r.out < r.in && NowUs() < until;) {
        pollfd pfd = {enc.fd(), POLLIN, 0};
        poll(&pfd, 1, 20);
        drain();
    }
    if (out) std::fclose(out);
    enc.Close();
    for (GbmPicture &p : pics) {
        if (p.fd >= 0) close(p.fd);
        if (p.bo) gbm_bo_destroy(p.bo);
    }
}

// The hottest thermal zone, or with `type` the zone of that name (the codec's is "video-thermal").
double ZoneC(const char *type = nullptr) {
    double best = 0;
    for (int i = 0; i < 64; ++i) {
        char path[64], name[64] = "";
        std::snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        if (FILE *f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%63s", name) != 1) name[0] = 0;
            std::fclose(f);
        }
        if (type && std::strcmp(type, name) != 0) continue;
        std::snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *f = std::fopen(path, "r");
        if (!f) continue;
        long milli = 0;
        if (std::fscanf(f, "%ld", &milli) == 1 && milli < 150000) best = std::max(best, milli / 1000.0);
        std::fclose(f);
    }
    return best;
}

double CpuSeconds() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

}  // namespace

int main(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (k == "--sessions") a.sessions = std::atoi(v), ++i;
        else if (k == "--size") std::sscanf(v, "%dx%d", &a.width, &a.height), ++i;
        else if (k == "--fps") a.fps = std::atoi(v), ++i;
        else if (k == "--seconds") a.seconds = std::atoi(v), ++i;
        else if (k == "--bitrate") a.bitrate = std::atoi(v), ++i;
        else if (k == "--out") a.out = v, ++i;
        else if (k == "--dmabuf") a.dmabuf = true;
        else if (k == "--h264") a.h264 = true;
        else if (k == "--busy") a.busy = true;
        else if (k == "--step") a.step = true;
        else {
            std::fprintf(stderr, "ft-enctest: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (a.sessions < 1 || a.sessions > 32 || a.fps < 1 || a.seconds < 1 || a.width < 128 || a.height < 128) {
        std::fprintf(stderr, "ft-enctest: bad --sessions, --fps, --seconds or --size\n");
        return 2;
    }
    std::printf("%d session(s) %dx%d %d fps %s %d kbit/s, %s pictures, %s%s, %d s\n", a.sessions, a.width, a.height,
                a.fps, a.h264 ? "H.264" : "HEVC", a.bitrate / 1000, a.dmabuf ? "DMA-BUF" : "copied",
                a.busy ? "busy" : "desktop", a.step ? ", bitrate step" : "", a.seconds);
    const double hot0 = ZoneC(), video0 = ZoneC("video-thermal"), cpu0 = CpuSeconds();
    const int64_t t0 = NowUs();
    std::vector<Result> results(a.sessions);
    std::vector<std::thread> threads;
    std::atomic<bool> stop{false};
    for (int i = 0; i < a.sessions; ++i) threads.emplace_back(Session, std::cref(a), i, std::ref(results[i]), std::cref(stop));
    for (std::thread &t : threads) t.join();
    const double wall = (NowUs() - t0) / 1e6, cpu = CpuSeconds() - cpu0, hot1 = ZoneC(), video1 = ZoneC("video-thermal");

    int opened = 0;
    for (int i = 0; i < a.sessions; ++i) {
        const Result &r = results[i];
        if (!r.opened) {
            std::printf("session %d: did not open: %s\n", i, r.err.c_str());
            continue;
        }
        ++opened;
        std::printf("session %d: in %d out %d dropped %d keys %d, %.0f kbit/s, latency avg %.1f ms max %.1f ms", i, r.in,
                    r.out, r.dropped, r.keys, r.bytes * 8.0 / 1000 / a.seconds,
                    r.out ? r.latencySum / 1000.0 / r.out : 0.0, r.latencyMax / 1000.0);
        if (a.step && r.outAfterStep)
            std::printf(", after the step %.0f kbit/s", r.bytesAfterStep * 8.0 / 1000 / (a.seconds / 2.0));
        if (!r.err.empty()) std::printf(", error: %s", r.err.c_str());
        std::printf("\n");
        if (a.dmabuf && i == 0)
            std::printf("layout: encoder stride %d size %zu, GBM stride %d size %zu: %s\n", r.encStride, r.encSize,
                        r.bufStride, r.bufSize,
                        r.encStride == r.bufStride && r.bufSize >= r.encSize ? "same, no copy needed" : "DIFFERENT");
    }
    std::printf("opened %d of %d, CPU %.1f%% of one core over %.1f s, codec %.1f C -> %.1f C, hottest zone %.1f C -> %.1f C\n",
                opened, a.sessions, cpu / wall * 100, wall, video0, video1, hot0, hot1);
    return opened == a.sessions ? 0 : 1;
}
