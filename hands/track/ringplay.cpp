// ft-ringplay: play a recording (ft-hands --record) into a frame ring in real time, the
// way ft-camd publishes live cameras, so ft-hands --ring PATH processes the same frames
// run after run. For A/B tests of how the tracker runs.
//
//   ft-ringplay DIR --ring PATH [--from S] [--to S] [--loop] [--cpus 0,1]
//               [--upper-skew MS] [--upper-drift MS_PER_S]
//
// Frames are stamped as they're published, so the tracker's latency figures stay
// meaningful. Cameras carry their calibration name and no device node (ft-hands maps
// them by name), so a recording made with the right names needs no --swap-sides.
// Dark frames (<name>_dk) are skipped. Needs no root: the ring is an ordinary file.
// --upper-skew and --upper-drift stamp the upper cameras' frames MS (plus MS_PER_S a second,
// modulo a 30 fps frame) later than the sides', as after a colour module hot plug that left
// them out of step (hands/README.md, "Frame sets").
#include "record.h"

extern "C" {
#include "../camd/fhring.h"
}

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

uint64_t clock_ns(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// Sets from a recording, reading only the pixels of the cameras that get published.
class Reader {
public:
    bool open(const std::string &path) {
        f_ = std::fopen(path.c_str(), "rb");
        if (f_) posix_fadvise(fileno(f_), 0, 0, POSIX_FADV_SEQUENTIAL);
        return f_ != nullptr;
    }
    void rewind() { std::fseek(f_, 0, SEEK_SET); }
    // False at the end. cams: every camera in the set; px[k]: pixels of camera k when
    // want(name), else left empty.
    template <class Want>
    bool next(std::vector<fh_set_cam_t> &cams, std::vector<std::vector<uint8_t>> &px, Want want) {
        fh_set_hdr_t h;
        if (std::fread(&h, sizeof h, 1, f_) != 1 || std::memcmp(h.magic, FH_SET_MAGIC, 8) || h.ncams == 0 || h.ncams > 16)
            return false;
        cams.resize(h.ncams);
        if (std::fread(cams.data(), sizeof(fh_set_cam_t), h.ncams, f_) != h.ncams) return false;
        px.resize(h.ncams);
        for (uint32_t k = 0; k < h.ncams; ++k) {
            cams[k].name[sizeof cams[k].name - 1] = 0;
            const size_t n = size_t(cams[k].width) * cams[k].height;
            if (want(cams[k].name)) {
                px[k].resize(n);
                if (std::fread(px[k].data(), 1, n, f_) != n) return false;
            } else {
                px[k].clear();
                if (std::fseek(f_, long(n), SEEK_CUR)) return false;
            }
        }
        if (++sets_ % 64 == 0) posix_fadvise(fileno(f_), 0, std::ftell(f_), POSIX_FADV_DONTNEED);   // RAM is tight
        return true;
    }

private:
    FILE *f_ = nullptr;
    uint64_t sets_ = 0;
};

bool is_dark(const char *name) {
    const size_t n = std::strlen(name);
    return n > 3 && !std::strcmp(name + n - 3, "_dk");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == '-') {
        std::fprintf(stderr, "usage: %s DIR --ring PATH [--from S] [--to S] [--loop] [--cpus 0,1] [--upper-skew MS] "
                             "[--upper-drift MS_PER_S]\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    std::string ring_path;
    double from = 0, to = 1e9, upper_skew_ms = 0, upper_drift_ms = 0;
    bool loop = false;
    std::vector<int> cpus = {0, 1};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--ring" && more) ring_path = argv[++i];
        else if (a == "--from" && more) from = std::atof(argv[++i]);
        else if (a == "--to" && more) to = std::atof(argv[++i]);
        else if (a == "--loop") loop = true;
        else if (a == "--upper-skew" && more) upper_skew_ms = std::atof(argv[++i]);
        else if (a == "--upper-drift" && more) upper_drift_ms = std::atof(argv[++i]);
        else if (a == "--cpus" && more) {
            cpus.clear();
            for (char *p = argv[++i]; *p;) {
                cpus.push_back(int(std::strtol(p, &p, 10)));
                if (*p == ',') ++p;
                else if (*p) break;
            }
        } else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 1;
    }
    if (ring_path.empty()) return std::fprintf(stderr, "--ring PATH is required\n"), 1;
    if (!cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : cpus) CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof set, &set) < 0) std::perror("sched_setaffinity");
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });

    Reader in;
    if (!in.open(dir + "/sets.bin")) return std::fprintf(stderr, "%s/sets.bin: %s\n", dir.c_str(), std::strerror(errno)), 1;
    std::vector<fh_set_cam_t> cams;
    std::vector<std::vector<uint8_t>> px;
    auto want = [](const char *name) { return !is_dark(name); };
    if (!in.next(cams, px, want)) return std::fprintf(stderr, "%s: no sets\n", dir.c_str()), 1;
    auto set_time = [&](const std::vector<fh_set_cam_t> &cs) {   // earliest bright capture, s
        uint64_t t = UINT64_MAX;
        for (const auto &c : cs)
            if (!is_dark(c.name)) t = std::min(t, c.capture_ns);
        return double(t) * 1e-9;
    };
    const double rec0 = set_time(cams);
    // skip to --from before the ring exists, so a reader never finds it without a heartbeat
    bool have = true;
    while (have && set_time(cams) - rec0 < from && !g_stop) have = in.next(cams, px, want);
    if (!have) return std::fprintf(stderr, "%s: nothing after %.1f s\n", dir.c_str(), from), 1;

    // the ring: the recording's bright cameras, as ft-camd lays them out
    std::vector<int> pub;   // set camera index of each ring camera
    for (size_t k = 0; k < cams.size() && pub.size() < FH_RING_MAX_CAMS; ++k)
        if (!is_dark(cams[k].name)) pub.push_back(int(k));
    size_t len = sizeof(fh_ring_hdr_t);
    std::vector<uint64_t> offset(pub.size()), slot_bytes(pub.size());
    for (size_t r = 0; r < pub.size(); ++r) {
        const fh_set_cam_t &c = cams[pub[r]];
        slot_bytes[r] = (sizeof(fh_ring_slot_t) + size_t(c.width) * c.height + 63) & ~size_t(63);
        offset[r] = len;
        len += FH_RING_SLOTS * slot_bytes[r];
    }
    const int fd = ::open(ring_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, off_t(len)) < 0)
        return std::fprintf(stderr, "%s: %s\n", ring_path.c_str(), std::strerror(errno)), 1;
    void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return std::fprintf(stderr, "mmap %s: %s\n", ring_path.c_str(), std::strerror(errno)), 1;
    auto *base = static_cast<uint8_t *>(m);
    auto *hdr = reinterpret_cast<fh_ring_hdr_t *>(base);
    for (size_t r = 0; r < pub.size(); ++r) {
        const fh_set_cam_t &c = cams[pub[r]];
        fh_ring_cam_t &rc = hdr->cams[r];
        std::snprintf(rc.sensor, sizeof rc.sensor, "ft-ringplay");
        std::snprintf(rc.name, sizeof rc.name, "%s", c.name);
        rc.node = -1;
        rc.format = FH_FMT_GREY8;
        rc.width = rc.stride = c.width;
        rc.height = c.height;
        rc.nslots = FH_RING_SLOTS;
        rc.slot_offset = offset[r];
        rc.slot_bytes = slot_bytes[r];
    }
    hdr->version = FH_RING_VERSION;
    hdr->header_bytes = sizeof(fh_ring_hdr_t);
    hdr->ncams = uint32_t(pub.size());
    hdr->file_bytes = len;
    hdr->writer_pid = getpid();
    std::memcpy(hdr->magic, FH_RING_MAGIC, 8);
    __atomic_store_n(&hdr->heartbeat_ns, clock_ns(CLOCK_MONOTONIC), __ATOMIC_RELEASE);
    std::printf("playing %s into %s:", dir.c_str(), ring_path.c_str());
    for (int k : pub) std::printf(" %s", cams[k].name);
    std::printf("\n");
    std::fflush(stdout);

    uint64_t published = 0, rounds = 0;
    const uint64_t t_play = clock_ns(CLOCK_MONOTONIC);
    for (;;) {
        // one pass over [from, to]: each set goes out at its recorded offset from the first
        while (have && set_time(cams) - rec0 < from && !g_stop) {
            __atomic_store_n(&hdr->heartbeat_ns, clock_ns(CLOCK_MONOTONIC), __ATOMIC_RELEASE);
            have = in.next(cams, px, want);
        }
        const double first = set_time(cams);
        const uint64_t start = clock_ns(CLOCK_MONOTONIC);
        while (have && !g_stop && set_time(cams) - rec0 <= to) {
            const uint64_t due = start + uint64_t((set_time(cams) - first) * 1e9);
            for (uint64_t now = clock_ns(CLOCK_MONOTONIC); now < due && !g_stop; now = clock_ns(CLOCK_MONOTONIC)) {
                __atomic_store_n(&hdr->heartbeat_ns, now, __ATOMIC_RELEASE);
                const uint64_t wait = std::min<uint64_t>(due - now, 100'000'000);
                const timespec ts{time_t(wait / 1'000'000'000), long(wait % 1'000'000'000)};
                nanosleep(&ts, nullptr);
            }
            const uint64_t raw = clock_ns(CLOCK_MONOTONIC_RAW), mono = clock_ns(CLOCK_MONOTONIC);
            const double skew_ms = std::fmod(upper_skew_ms + upper_drift_ms * (mono - t_play) * 1e-9, 1000.0 / 30);
            for (size_t r = 0; r < pub.size(); ++r) {
                fh_ring_cam_t &rc = hdr->cams[r];
                if (px[pub[r]].size() != size_t(rc.width) * rc.height) continue;
                const uint64_t n = rc.latest + 1;
                auto *s = reinterpret_cast<fh_ring_slot_t *>(base + rc.slot_offset + (n % rc.nslots) * rc.slot_bytes);
                __atomic_store_n(&s->seq, 2 * n + 1, __ATOMIC_RELAXED);
                __atomic_thread_fence(__ATOMIC_RELEASE);
                std::memcpy(reinterpret_cast<uint8_t *>(s + 1), px[pub[r]].data(), px[pub[r]].size());
                s->frame = n;
                s->capture_ns = raw;   // taken now, as a live camera's frame would be
                if (!std::strncmp(rc.name, "upper", 5)) s->capture_ns += uint64_t(std::max(0.0, skew_ms) * 1e6);
                s->dqbuf_ns = mono;
                s->publish_ns = mono;
                __atomic_store_n(&s->seq, 2 * n + 2, __ATOMIC_RELEASE);
                __atomic_store_n(&rc.latest, n, __ATOMIC_RELEASE);
                ++rc.published;
            }
            __atomic_store_n(&hdr->heartbeat_ns, mono, __ATOMIC_RELEASE);
            ++published;
            have = in.next(cams, px, want);
        }
        ++rounds;
        if (g_stop || !loop) break;
        in.rewind();
        have = in.next(cams, px, want);
    }
    std::printf("published %llu sets in %llu pass(es)\n", (unsigned long long)published, (unsigned long long)rounds);
    __atomic_store_n(&hdr->heartbeat_ns, 0, __ATOMIC_RELEASE);   // readers see the writer gone
    return 0;
}
