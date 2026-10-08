#include "io.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000 + uint64_t(ts.tv_nsec);
}

int64_t raw_minus_mono_ns() {
    timespec a, r, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_MONOTONIC_RAW, &r);
    clock_gettime(CLOCK_MONOTONIC, &b);
    const int64_t ma = int64_t(a.tv_sec) * 1'000'000'000 + a.tv_nsec, mb = int64_t(b.tv_sec) * 1'000'000'000 + b.tv_nsec;
    return int64_t(r.tv_sec) * 1'000'000'000 + r.tv_nsec - (ma + mb) / 2;
}

// ------------------------------------------------------------------------------ ring

bool Ring::open(const char *path, std::string &err) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return err = std::string(path) + ": " + std::strerror(errno), false;
    struct stat st;
    fstat(fd, &st);
    len_ = size_t(st.st_size);
    void *m = len_ >= sizeof(fh_ring_hdr_t) ? mmap(nullptr, len_, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return err = std::string(path) + ": can't map it", false;
    map_ = static_cast<const uint8_t *>(m);
    hdr_ = reinterpret_cast<const fh_ring_hdr_t *>(map_);
    if (std::memcmp(hdr_->magic, FH_RING_MAGIC, 8) || hdr_->version != FH_RING_VERSION || hdr_->file_bytes > len_)
        return err = std::string(path) + " is not an ft-camd ring", false;
    return true;
}

bool Ring::alive() const {
    const uint64_t hb = __atomic_load_n(&hdr_->heartbeat_ns, __ATOMIC_ACQUIRE);
    return hb && mono_ns() - hb < 1'000'000'000;
}

uint64_t Ring::latest(int i) const { return __atomic_load_n(&hdr_->cams[i].latest, __ATOMIC_ACQUIRE); }

bool Ring::read(int i, uint64_t n, std::vector<uint8_t> &out, fh_ring_slot_t *meta) const {
    const fh_ring_cam_t &c = hdr_->cams[i];
    if (!n || c.slot_offset + c.nslots * c.slot_bytes > len_) return false;
    const uint8_t *slot = map_ + c.slot_offset + (n % c.nslots) * c.slot_bytes;
    const auto *s = reinterpret_cast<const fh_ring_slot_t *>(slot);
    const uint64_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (seq != 2 * n + 2) return false;
    std::memcpy(meta, slot, sizeof *meta);
    out.resize(size_t(c.width) * c.height);
    for (uint32_t y = 0; y < c.height; ++y)
        std::memcpy(out.data() + size_t(y) * c.width, slot + sizeof(fh_ring_slot_t) + size_t(y) * c.stride, c.width);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&s->seq, __ATOMIC_RELAXED) == seq;
}

bool Ring::meta(int i, uint64_t n, fh_ring_slot_t *meta) const {
    const fh_ring_cam_t &c = hdr_->cams[i];
    if (!n || c.slot_offset + c.nslots * c.slot_bytes > len_) return false;
    const uint8_t *slot = map_ + c.slot_offset + (n % c.nslots) * c.slot_bytes;
    const auto *s = reinterpret_cast<const fh_ring_slot_t *>(slot);
    const uint64_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (seq != 2 * n + 2) return false;
    std::memcpy(meta, slot, sizeof *meta);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&s->seq, __ATOMIC_RELAXED) == seq;
}

// ------------------------------------------------------------------------- publisher

namespace {

// The hand's shape to cut out, as capsules.
// Radii are a real hand's half-widths plus a small margin for tracking noise.
const int kThumb[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}};
const int kFingers[][2] = {{5, 6}, {6, 7}, {7, 8}, {9, 10}, {10, 11}, {11, 12}, {13, 14}, {14, 15}, {15, 16},
                           {17, 18}, {18, 19}, {19, 20}};
const int kPalm[][2] = {{0, 5}, {0, 9}, {0, 13}, {0, 17}, {5, 9}, {9, 13}, {13, 17}, {1, 5}};
constexpr double kThumbR = 0.0095, kFinger = 0.0085, kPalmR = 0.015, kArm[2] = {0.028, 0.034}, kArmLen = 0.16,
                 kMargin = 0.004;
// Nothing is cut closer than this in front of the eyes (head frame, -z is forward). A
// point near the eyes' plane lands far across a screen with a huge radius, so one bad
// estimate there tears a hole through it; real hands that close aren't tracked anyway.
constexpr double kNear = 0.12;

// Adds the capsule, clipped to the part at least kNear in front of the eyes.
void put(fh_capsule_t *caps, uint32_t &n, V3 a, V3 b, double ra, double rb) {
    if (n >= FH_HANDS_MAX_CAPSULES) return;
    const double za = -a[2] - kNear, zb = -b[2] - kNear;   // >= 0: far enough in front
    if (za < 0 && zb < 0) return;
    if (za < 0 || zb < 0) {
        const double t = za / (za - zb);   // where the segment crosses the near plane
        const V3 m = a + (b - a) * t;
        const double rm = ra + (rb - ra) * t;
        if (za < 0) a = m, ra = rm;
        else b = m, rb = rm;
    }
    fh_capsule_t &c = caps[n++];
    for (int k = 0; k < 3; ++k) c.a[k] = float(a[k]), c.b[k] = float(b[k]);
    c.ra = float(ra + kMargin), c.rb = float(rb + kMargin);
}

}  // namespace

std::string run_dir() {
    const std::string dir = "/run/user/" + std::to_string(getuid()) + "/frametop-hands";
    mkdir(dir.c_str(), 0700);
    return dir;
}

bool Publisher::open(std::string &err) {
    const std::string path = run_dir() + "/hands";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(fh_hands_v2_t)) < 0) return err = path + ": " + std::strerror(errno), false;
    void *m = mmap(nullptr, sizeof(fh_hands_v2_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return err = path + ": can't map it", false;
    file_ = static_cast<fh_hands_v2_t *>(m);
    out_ = &file_->v1;
    std::memset(file_, 0, sizeof *file_);
    std::memcpy(out_->magic, FH_HANDS_MAGIC, 8);
    out_->version = FH_HANDS_VERSION;
    out_->size = sizeof(fh_hands_v2_t);
    return true;
}

static_assert(sizeof(fh_hand_fit_t::angles) / sizeof(float) == skel::kAngles, "fh_hand_fit_t angles");

void Publisher::fill_fit(const Hand &h, uint64_t capture_ns, fh_hand_fit_t &o) {
    std::memset(&o, 0, sizeof o);
    o.id = uint32_t(h.id);
    const skel::Fit &f = h.fit;
    if (!f.valid) return;
    auto put = [](float *out, V3 v) {
        for (int k = 0; k < 3; ++k) out[k] = float(v[k]);
    };
    o.flags = FH_FIT_VALID | FH_FIT_ELBOW_HEAD_UP | (f.side > 0 ? FH_FIT_RIGHT : 0) | (f.settled >= 1 ? FH_FIT_SETTLED : 0) |
              (f.flipped ? FH_FIT_FLIPPED : 0);
    o.tracked_ns = capture_ns;
    for (int i = 0; i < 21; ++i) {
        put(o.joints[i].p, f.pts[i]);
        o.joints[i].radius = float(f.radius[i]);
        double q[4];
        skel::to_quat(f.frames[i], q);
        for (int k = 0; k < 4; ++k) o.joints[i].q[k] = float(q[k]);
    }
    for (int b = 0; b < 20; ++b) o.bone_len[b] = float(f.len[b]);
    for (int a = 0; a < skel::kAngles; ++a) o.angles[a] = float(f.qf[a]);
    put(o.elbow, f.elbow);
    o.elbow_conf = 0;
    put(o.pinch_point, (f.pts[5] + f.pts[9]) * 0.5);
    put(o.contact, (f.pts[4] + f.pts[8]) * 0.5);
    o.pinch_d[0] = float(norm(f.pts[4] - f.pts[8]));
    o.pinch_d[1] = float(norm(f.pts[4] - f.pts[12]));
    o.fit_rms = float(f.rms);
    o.settled = float(f.settled);
    put(o.vel_head, f.vel);
    put(o.angvel_head, f.angvel);
}

void Publisher::write(const std::vector<const Hand *> &in, uint64_t capture_ns) {
    std::vector<const Hand *> hands = in;
    std::sort(hands.begin(), hands.end(), [](const Hand *a, const Hand *b) { return a->frames > b->frames; });
    if (hands.size() > FH_HANDS_MAX_HANDS) hands.resize(FH_HANDS_MAX_HANDS);
    __atomic_store_n(&out_->seq, 2 * ++seq_ - 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t nc = 0;
    for (size_t k = 0; k < FH_HANDS_MAX_HANDS; ++k) {
        fh_hand_t &o = out_->hands[k];
        std::memset(&o, 0, sizeof o);
        if (k >= hands.size()) continue;
        const Hand &h = *hands[k];
        // the fitted skeleton's points when there is one (fingers keep their length), else the
        // filtered landmarks (ft-hands --skeleton 0)
        const V3 *pts = h.fit.valid ? h.fit.pts : h.smooth;
        o.id = uint32_t(h.id);
        o.flags = (h.right() ? FH_HAND_RIGHT : 0) | (h.nviews >= 2 ? FH_HAND_STEREO : 0);
        o.confidence = float(std::min(1.0, h.frames / 5.0));
        for (int i = 0; i < 21; ++i)
            for (int j = 0; j < 3; ++j) o.pts[i][j] = float(pts[i][j]);
        const uint32_t first = nc;
        for (auto &b : kThumb) put(out_->capsules, nc, pts[b[0]], pts[b[1]], kThumbR, kThumbR);
        for (auto &b : kFingers) put(out_->capsules, nc, pts[b[0]], pts[b[1]], kFinger, kFinger);
        for (auto &b : kPalm) put(out_->capsules, nc, pts[b[0]], pts[b[1]], kPalmR, kPalmR);
        // the forearm carries on from the hand's own axis (middle knuckle -> wrist); the
        // wrist bends, but much less than a guess at where the elbow is gets wrong
        const V3 wrist = pts[0], d = wrist - pts[9];
        const double n = norm(d);
        if (n > 0.02) put(out_->capsules, nc, wrist, wrist + d * (kArmLen / n), kArm[0], kArm[1]);
        o.ncapsules = nc - first;
    }
    for (size_t k = 0; k < FH_HANDS_MAX_HANDS; ++k) {
        if (k < hands.size()) fill_fit(*hands[k], capture_ns, file_->fits[k]);
        else std::memset(&file_->fits[k], 0, sizeof file_->fits[k]);
    }
    for (uint32_t k = nc; k < FH_HANDS_MAX_CAPSULES; ++k) std::memset(&out_->capsules[k], 0, sizeof(fh_capsule_t));
    out_->capture_ns = capture_ns;
    out_->publish_ns = mono_ns();
    out_->nhands = uint32_t(hands.size());
    out_->ncapsules = nc;
    __atomic_store_n(&out_->seq, 2 * seq_, __ATOMIC_RELEASE);
}
