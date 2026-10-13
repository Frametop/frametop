// The pointer helper's view of hand tracking: ft-hands' files (hands/include/fh_hands.h,
// fh_gestures.h) mapped read-only, and the hands turned into the room (the standing universe)
// with the head's pose at their capture time. Header-only; ft-pointer.cpp includes it once.
#pragma once

#include <openvr.h>

#include "feedage.h"
#include "vrmath.h"

extern "C" {
#include "../../hands/include/fh_gestures.h"
#include "../../hands/include/fh_hands.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hin {

using md::Vec3;

// A file ft-hands rewrites in place under a sequence lock (read seq, copy, read seq again; use the
// copy only if both are the same even number), mapped read-only. It's opened, and checked to be
// the same file, at most once a second.
class ShmFile {
public:
    ShmFile(const char *name, const char *magic, size_t minSize) : name_(name), magic_(magic), min_(minSize) {}
    ~ShmFile() { Close(); }
    ShmFile(const ShmFile &) = delete;
    ShmFile &operator=(const ShmFile &) = delete;
    // Copies up to `size` bytes into out (zero past the file's end). seqOffset: where the file's
    // 64-bit sequence number is.
    bool Read(void *out, size_t size, size_t seqOffset) {
        const auto now = std::chrono::steady_clock::now();
        if (now - checked_ > std::chrono::seconds(1)) {
            checked_ = now;
            const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/" + name_;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || size_t(st.st_size) != len_ || st.st_ino != ino_) {
                Close();
                const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd >= 0 && fstat(fd, &st) == 0 && size_t(st.st_size) >= min_) {
                    void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
                    if (m != MAP_FAILED) map_ = m, len_ = size_t(st.st_size), ino_ = st.st_ino, ++opens;
                }
                if (fd >= 0) close(fd);
            }
        }
        if (!map_ || std::memcmp(map_, magic_, 8) != 0) return false;
        const auto *seqp = reinterpret_cast<const volatile uint64_t *>(static_cast<const char *>(map_) + seqOffset);
        const size_t n = std::min(len_, size);
        for (int tries = 0; tries < 3; ++tries) {
            const uint64_t seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
            if (seq & 1) continue;
            std::memset(out, 0, size);
            std::memcpy(out, map_, n);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(seqp, __ATOMIC_RELAXED) != seq) continue;
            return true;
        }
        return false;
    }
    size_t Size() const { return len_; }
    int opens = 0;  // a new file: counters start over

private:
    void Close() {
        if (map_) munmap(map_, len_);
        map_ = nullptr, len_ = 0, ino_ = 0;
    }
    const char *name_, *magic_;
    size_t min_;
    void *map_ = nullptr;
    size_t len_ = 0;
    ino_t ino_ = 0;
    std::chrono::steady_clock::time_point checked_{};
};

// Hand gestures (fh_gestures.h). Version 1 files have pinches only; their grips read as zero.
// Before version 3, the middle pinches read as zero.
class HandGestures {
public:
    bool Read(fh_gestures_t &out) {
        if (!file_.Read(&out, sizeof out, offsetof(fh_gestures_t, seq))) return false;
        if (out.version < 2) std::memset(out.grip, 0, sizeof out.grip);
        if (out.version < 3 || file_.Size() < sizeof out) std::memset(out.pinch2, 0, sizeof out.pinch2);
        opens = file_.opens;
        return true;
    }
    int opens = 0;

private:
    ShmFile file_{"gestures", FH_GESTURES_MAGIC, offsetof(fh_gestures_t, grip)};
};

// The HMD's recent poses (standing universe), to turn head-frame points into the room as the
// head was when the cameras took them.
class PoseHistory {
public:
    void Add(std::chrono::steady_clock::time_point t, const vr::HmdMatrix34_t &m) {
        poses_.push_back({t, m});
        while (poses_.size() > 128) poses_.pop_front();  // about a second
    }
    // The pose at CLOCK_MONOTONIC time t_ns (steady_clock's), or the nearest kept.
    bool At(uint64_t t_ns, vr::HmdMatrix34_t &out) const {
        if (poses_.empty()) return false;
        const std::chrono::steady_clock::time_point t{std::chrono::nanoseconds(t_ns)};
        const auto *best = &poses_.front();
        for (const auto &p : poses_)
            if (std::chrono::abs(p.first - t) < std::chrono::abs(best->first - t)) best = &p;
        out = best->second;
        return true;
    }

private:
    std::deque<std::pair<std::chrono::steady_clock::time_point, vr::HmdMatrix34_t>> poses_;
};

inline Vec3 ToRoom(const vr::HmdMatrix34_t &head, const float p[3]) {
    return md::Position(head) + md::Rotate(head, Vec3{p[0], p[1], p[2]});
}

// Rotate v by quaternion q (x, y, z, w).
inline Vec3 QuatRotate(const float q[4], Vec3 v) {
    const Vec3 u{q[0], q[1], q[2]};
    const double s = q[3];
    return u * (2 * md::Dot(u, v)) + v * (s * s - md::Dot(u, u)) + md::Cross(u, v) * (2 * s);
}

// One tracked hand in the room, at its capture time.
struct RoomHand {
    uint32_t id = 0;
    int side = 0;              // 0 left, 1 right (ft-hands' call)
    bool stereo = false;       // triangulated from two or more cameras
    bool fitted = false;       // points from the skeleton fit (fh_hands v2), else as measured
    Vec3 pts[21];              // MediaPipe landmarks
    Vec3 palm;                 // centre of the wrist and four knuckles
    Vec3 knuckle;              // K: the index and middle knuckles (the pinch point)
    Vec3 palmNormal;           // unit, out of the palm (the side you see in your palm)
    Vec3 elbow;
    double elbowConf = 0;
    bool hasElbow = false;
    Vec3 vel;                  // the palm's room velocity, m/s (from the previous frame)
    bool velValid = false;
    double pinchD[2] = {-1, -1};  // thumb to index, thumb to middle (fit only), m
};

// The hands file, read each frame, in the room.
class HandsInRoom {
public:
    // A new frame turned into the room: true. Keeps the last one otherwise.
    bool Update(const PoseHistory &poses) {
        static fh_hands_v2_t f;
        if (!file_.Read(&f, sizeof f, offsetof(fh_hands_t, seq))) return false;
        const bool v2 = f.v1.version >= 2 && file_.Size() >= sizeof f;
        if (f.v1.seq == seq_ && file_.opens == opens_) return false;
        seq_ = f.v1.seq, opens_ = file_.opens;
        publish_ns = f.v1.publish_ns;
        vr::HmdMatrix34_t head;
        if (!poses.At(f.v1.capture_ns, head)) return false;
        std::vector<RoomHand> prev;
        prev.swap(hands);
        const uint64_t prevNs = capture_ns;
        capture_ns = f.v1.capture_ns;
        for (uint32_t k = 0; k < std::min<uint32_t>(f.v1.nhands, FH_HANDS_MAX_HANDS); ++k) {
            const fh_hand_t &h = f.v1.hands[k];
            const fh_hand_fit_t *fit = v2 && (f.fits[k].flags & FH_FIT_VALID) && f.fits[k].id == h.id ? &f.fits[k] : nullptr;
            RoomHand r;
            r.id = h.id;
            r.side = (h.flags & FH_HAND_RIGHT) ? 1 : 0;
            r.stereo = h.flags & FH_HAND_STEREO;
            r.fitted = fit != nullptr;
            for (int i = 0; i < 21; ++i) r.pts[i] = ToRoom(head, fit ? fit->joints[i].p : h.pts[i]);
            r.palm = (r.pts[0] + r.pts[5] + r.pts[9] + r.pts[13] + r.pts[17]) * 0.2;
            r.knuckle = fit ? ToRoom(head, fit->pinch_point) : (r.pts[5] + r.pts[9]) * 0.5;
            if (fit) {
                // the wrist's frame: +Y out of the back of the hand
                r.palmNormal = md::Normalize(md::Rotate(head, QuatRotate(fit->joints[0].q, Vec3{0, -1, 0})));
                r.elbow = ToRoom(head, fit->elbow);
                r.elbowConf = fit->elbow_conf;
                r.hasElbow = true;
                r.pinchD[0] = fit->pinch_d[0], r.pinchD[1] = fit->pinch_d[1];
            } else {
                const Vec3 n = md::Cross(r.pts[5] - r.pts[0], r.pts[17] - r.pts[0]);
                r.palmNormal = md::Normalize(r.side ? n : n * -1.0);
            }
            for (const RoomHand &p : prev)
                if (p.id == r.id && capture_ns > prevNs && capture_ns - prevNs < 200'000'000ull) {
                    r.vel = (r.palm - p.palm) * (1e9 / double(capture_ns - prevNs));
                    r.velValid = true;
                }
            hands.push_back(r);
        }
        return true;
    }
    // ft-hands publishes (a file written within the last 1.5 s, or after nowNs: feedage.h).
    bool Live(uint64_t nowNs) const { return Fresh(nowNs, publish_ns, kLiveNs); }
    static constexpr uint64_t kLiveNs = 1'500'000'000ull;
    std::vector<RoomHand> hands;
    uint64_t capture_ns = 0, publish_ns = 0;

private:
    ShmFile file_{"hands", FH_HANDS_MAGIC, sizeof(fh_hands_t)};
    uint64_t seq_ = ~0ull;
    int opens_ = -1;
};

}  // namespace hin
