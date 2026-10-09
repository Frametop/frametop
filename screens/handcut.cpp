// Hand cutouts, the CPU side (see handcut.h): the hands and gestures files, the matte and
// where the eyes see it on a panel. No GPU or SteamVR calls here (handrender.cpp has those).
#include "handcut.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace handcut {
namespace {

// The hands file ft-hands publishes (hands/include/fh_hands.h).
constexpr uint32_t kMaxHands = FH_HANDS_MAX_HANDS, kMaxCapsules = FH_HANDS_MAX_CAPSULES;
constexpr int64_t kStaleNs = 300'000'000;      // hands older than this are gone
constexpr int64_t kReopenNs = 3'000'000'000;   // a file this stale is opened again (ft-hands may have made a new one)
constexpr int64_t kHistoryNs = 1'000'000'000;
constexpr double kNear = 0.12;   // metres: nothing closer to an eye than this is cut
constexpr double kMaxSpeed = 2.5;     // m/s: faster is a tracking jump, not a hand
constexpr double kStillSpeed = 0.05;  // m/s: below this, a hand's velocity is noise
constexpr double kMaxAhead = 0.12;    // s: never predict further than this
constexpr double kPalmLen = 0.0815;   // wrist to middle knuckle of the skeleton's average hand

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void Apply(const Mat &m, const float p[3], float out[3]) {
    float r[3];
    for (int i = 0; i < 3; ++i) r[i] = m.m[i][0] * p[0] + m.m[i][1] * p[1] + m.m[i][2] * p[2] + m.m[i][3];
    std::memcpy(out, r, sizeof r);
}

// Room -> panel-local: R^T (p - t).
void ToLocal(const Mat &m, const double p[3], double out[3]) {
    const double d[3] = {p[0] - m.m[0][3], p[1] - m.m[1][3], p[2] - m.m[2][3]};
    for (int i = 0; i < 3; ++i) out[i] = m.m[0][i] * d[0] + m.m[1][i] * d[1] + m.m[2][i] * d[2];
}

double Dist(const float a[3], const float b[3]) {
    return std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) + double(a[1] - b[1]) * (a[1] - b[1]) +
                     double(a[2] - b[2]) * (a[2] - b[2]));
}

void Mid(const float a[3], const float b[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = (a[i] + b[i]) / 2;
}

bool Finite(const float *v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 10) return false;
    return true;
}

// The skeleton's rough radii (hands/track/skeleton.cpp) at the average hand's size, for version 1.
float DefaultRadius(int i) { return i == 0 ? 0.020f : i == 1 || i % 4 == 1 ? 0.012f : i <= 4 ? 0.0095f : 0.0085f; }

// Turns p by `rad` about the unit axis through `at` (Rodrigues).
void Turn(float p[3], const double at[3], const double axis[3], double rad) {
    const double v[3] = {p[0] - at[0], p[1] - at[1], p[2] - at[2]};
    const double c = std::cos(rad), s = std::sin(rad), d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    const double x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
    for (int i = 0; i < 3; ++i) p[i] = float(at[i] + v[i] * c + x[i] * s + axis[i] * d * (1 - c));
}

}  // namespace

double Hand::speed() const { return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]); }

void Hand::Palm(double out[3]) const {
    out[0] = out[1] = out[2] = 0;
    for (int j : {0, 5, 9, 13, 17})
        for (int i = 0; i < 3; ++i) out[i] += p[j][i] / 5.0;
}

// ---------------------------------------------------------------------------- settings

std::string Setting(const char *name, const char *def) {
    const std::string env = std::string("FT_") + name;
    if (const char *v = std::getenv(env.c_str())) return v;
    const char *home = std::getenv("HOME");
    if (home) {
        std::ifstream f(std::string(home) + "/.config/frametop.conf");
        std::string line, found;
        bool have = false;
        const std::string key = std::string(name) + "=";
        while (std::getline(f, line)) {
            if (line.rfind(key, 0) != 0) continue;
            std::string v = line.substr(key.size());
            const size_t hash = v.find('#');
            if (hash != std::string::npos) v.resize(hash);
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
            if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
            found = v, have = true;   // the last one counts, as when the shell reads it
        }
        if (have) return found;
    }
    return def;
}

bool ParseShift(const std::string &text, std::vector<Shift> &out) {
    std::vector<Shift> s;
    std::istringstream in(text);
    std::string item;
    while (in >> item) {
        Shift k;
        if (std::sscanf(item.c_str(), "%lf:%lf,%lf", &k.dist, &k.yawDeg, &k.pitchDeg) != 3 || !(k.dist > 0) ||
            std::fabs(k.yawDeg) > 10 || std::fabs(k.pitchDeg) > 10)
            return false;
        s.push_back(k);
    }
    std::sort(s.begin(), s.end(), [](const Shift &a, const Shift &b) { return a.dist < b.dist; });
    out = s;
    return true;
}

std::string ReadMatteSettings(MatteParams &p) {
    std::string log;
    auto num = [&](const char *name, double &v, double lo, double hi) {
        const std::string s = Setting(name, "");
        if (s.empty()) return;
        char *end;
        const double x = std::strtod(s.c_str(), &end);
        if (end != s.c_str() && !*end && x >= lo && x <= hi) v = x, log += std::string(name) + "=" + s + " ";
        else log += std::string(name) + "=" + s + " (ignored) ";
    };
    const std::string m = Setting("HANDS_MATTE", "");
    if (m == "capsules" || m == "mesh") p.mesh = m == "mesh", log += "HANDS_MATTE=" + m + " ";
    num("HANDS_MATTE_MARGIN", p.marginDeg, 0, 10);
    num("HANDS_MATTE_MOTION", p.motionMs, 0, 200);
    num("HANDS_MATTE_MOTION_MAX", p.motionMaxDeg, 0, 10);
    const std::string sh = Setting("HANDS_MATTE_SHIFT", "");
    if (!sh.empty()) log += "HANDS_MATTE_SHIFT=" + sh + (ParseShift(sh, p.shift) ? " " : " (ignored) ");
    return log;
}

double MatteParams::Margin(double dist, double speed) const {
    const double still = dist * std::tan(marginDeg * M_PI / 180);
    const double moving = std::min(speed * motionMs / 1000, dist * std::tan(motionMaxDeg * M_PI / 180));
    return std::max(minMarginM, still + moving);
}

// ------------------------------------------------------------------------------- hands

Hands::~Hands() {
    if (map_) munmap(const_cast<void *>(map_), mapLen_);
    if (gmap_) munmap(const_cast<void *>(gmap_), sizeof(fh_gestures_t));
    if (fd_ >= 0) close(fd_);
    if (gfd_ >= 0) close(gfd_);
}

namespace {

// Maps up to `want` bytes of a file in the hands folder, if it's ours and at least `least` long.
const void *MapFile(const char *name, size_t least, size_t want, int *fd, size_t *len) {
    const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/" + name;
    *fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (*fd < 0) return nullptr;
    struct stat st;
    if (fstat(*fd, &st) < 0 || st.st_uid != getuid() || size_t(st.st_size) < least) {
        close(*fd), *fd = -1;
        return nullptr;
    }
    *len = std::min(size_t(st.st_size), want);
    void *m = mmap(nullptr, *len, PROT_READ, MAP_SHARED, *fd, 0);
    if (m == MAP_FAILED) {
        close(*fd), *fd = -1;
        return nullptr;
    }
    return m;
}

// A copy of a seqlocked file (both files start with magic, version, size, seq). False if it
// was being written, or hasn't changed since `last`.
bool CopySeq(const void *map, size_t len, uint64_t *last, void *out) {
    const auto *seq = reinterpret_cast<const uint64_t *>(static_cast<const char *>(map) + 16);
    const uint64_t s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if ((s1 & 1) || s1 == *last) return false;
    std::memcpy(out, map, len);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(seq, __ATOMIC_RELAXED) != s1) return false;
    *last = s1;
    return true;
}

}  // namespace

bool Hands::ReadFiles() {
    const int64_t now = MonoNs();
    bool got = false;
    // a long-stale file: ft-hands may have stopped and made a new one since
    if (map_ && now - publishNs_ > kReopenNs && now - lastOpenTry_ > kReopenNs) {
        munmap(const_cast<void *>(map_), mapLen_), close(fd_);
        map_ = nullptr, fd_ = -1, seq_ = 0;
    }
    if (!map_ && now - lastOpenTry_ >= 1'000'000'000) {
        lastOpenTry_ = now;
        map_ = MapFile("hands", sizeof(fh_hands_t), sizeof(fh_hands_v2_t), &fd_, &mapLen_);
    }
    static fh_hands_v2_t copy;
    if (map_ && CopySeq(map_, mapLen_, &seq_, &copy)) {
        if (std::memcmp(copy.v1.magic, FH_HANDS_MAGIC, 8) == 0) {
            if (mapLen_ < sizeof copy) std::memset(reinterpret_cast<char *>(&copy) + mapLen_, 0, sizeof copy - mapLen_);
            const bool v2 = copy.v1.version >= 2 && mapLen_ >= sizeof copy && copy.v1.size >= sizeof copy;
            // A version 2 file grown under a version 1 mapping: map it again next time.
            if (copy.v1.version >= 2 && mapLen_ < sizeof copy) {
                munmap(const_cast<void *>(map_), mapLen_), close(fd_);
                map_ = nullptr, fd_ = -1, seq_ = 0, lastOpenTry_ = 0;
            }
            version_ = int(copy.v1.version);
            Ingest(copy.v1, v2 ? copy.fits : nullptr, nullptr);
            got = true;
        }
    }
    // the gestures file, written right after each hands update
    if (gmap_ && now - publishNs_ > kReopenNs && now - lastGOpenTry_ > kReopenNs) {
        munmap(const_cast<void *>(gmap_), sizeof(fh_gestures_t)), close(gfd_);
        gmap_ = nullptr, gfd_ = -1, gseq_ = 0;
    }
    if (!gmap_ && now - lastGOpenTry_ >= 1'000'000'000) {
        lastGOpenTry_ = now;
        size_t len = 0;
        gmap_ = MapFile("gestures", sizeof(fh_gestures_t), sizeof(fh_gestures_t), &gfd_, &len);
    }
    static fh_gestures_t g;
    if (gmap_ && CopySeq(gmap_, sizeof g, &gseq_, &g) && std::memcmp(g.magic, FH_GESTURES_MAGIC, 8) == 0 && g.version >= 2)
        Ingest(copy.v1, nullptr, &g);
    return got;
}

void Hands::Ingest(const fh_hands_t &v1, const fh_hand_fit_t *fits, const fh_gestures_t *g) {
    if (g) {   // gestures only
        const Mat head = HeadAt(int64_t(g->capture_ns));
        for (int k = 0; k < 4; ++k) {
            const fh_pinch_t &in = k < 2 ? g->pinch[k] : g->grip[k - 2];
            Gesture &o = k < 2 ? pinch_[k] : grip_[k - 2];
            o.tracked = in.flags & FH_PINCH_TRACKED, o.down = in.flags & FH_PINCH_DOWN, o.lost = in.flags & FH_PINCH_LOST;
            o.handId = in.hand_id, o.begins = in.begins, o.ends = in.ends;
            o.beginNs = int64_t(in.begin_ns), o.endNs = int64_t(in.end_ns);
            o.distance = in.distance, o.strength = in.strength;
            if (Finite(in.point, 3)) Apply(head, in.point, o.point);
            if (Finite(in.begin_point, 3)) Apply(HeadAt(o.beginNs), in.begin_point, o.beginPoint);
        }
        return;
    }
    ++frames_;
    const uint32_t nhands = std::min<uint32_t>(v1.nhands, kMaxHands), ncaps = std::min<uint32_t>(v1.ncapsules, kMaxCapsules);
    captureNs_ = int64_t(v1.capture_ns), publishNs_ = int64_t(v1.publish_ns);
    const Mat head = HeadAt(captureNs_);

    seen_.clear();
    std::vector<int> owners;   // the hand each capsule belongs to, in file order (-1: none kept)
    for (uint32_t k = 0; k < nhands; ++k) {
        const fh_hand_t &in = v1.hands[k];
        const uint32_t nc = std::min<uint32_t>(in.ncapsules, kMaxCapsules);
        Hand h;
        h.id = in.id;
        h.right = in.flags & FH_HAND_RIGHT;
        h.stereo = in.flags & FH_HAND_STEREO;
        h.confidence = in.confidence;
        h.captureNs = captureNs_;
        const fh_hand_fit_t *f = fits && (fits[k].flags & FH_FIT_VALID) && fits[k].id == in.id ? &fits[k] : nullptr;
        bool ok = true;
        if (f) {
            h.fit = true;
            for (int j = 0; j < 21; ++j) {
                ok = ok && Finite(f->joints[j].p, 3);
                Apply(head, f->joints[j].p, h.p[j]);
                h.radius[j] = std::isfinite(f->joints[j].radius) && f->joints[j].radius > 0 && f->joints[j].radius < 0.05
                                  ? f->joints[j].radius : DefaultRadius(j);
            }
            for (int b = 0; b < 20; ++b) h.bone[b] = f->bone_len[b];
            ok = ok && Finite(f->elbow, 3) && Finite(f->pinch_point, 3) && Finite(f->contact, 3);
            Apply(head, f->elbow, h.elbow);
            Apply(head, f->pinch_point, h.pinchPoint);
            Apply(head, f->contact, h.contact);
            h.elbowConf = f->elbow_conf, h.hasElbow = true;
            h.pinchD[0] = f->pinch_d[0], h.pinchD[1] = f->pinch_d[1];
            if (f->tracked_ns) h.captureNs = int64_t(f->tracked_ns);
        } else {
            for (int j = 0; j < 21; ++j) {
                ok = ok && Finite(in.pts[j], 3);
                Apply(head, in.pts[j], h.p[j]);
                h.radius[j] = DefaultRadius(j);
            }
            for (int b = 0; b < 20; ++b) {
                const int a = b % 4 == 0 ? 0 : b;   // bones 0, 4, 8, 12, 16 start at the wrist
                h.bone[b] = float(Dist(h.p[a], h.p[b + 1]));
            }
            Mid(h.p[5], h.p[9], h.pinchPoint);
            Mid(h.p[4], h.p[8], h.contact);
            h.pinchD[0] = float(Dist(h.p[4], h.p[8])), h.pinchD[1] = float(Dist(h.p[4], h.p[12]));
            // no elbow: the forearm carries on along the hand's axis (as ft-hands' capsule)
            const double n = Dist(h.p[0], h.p[9]) + 1e-9;
            for (int i = 0; i < 3; ++i) h.elbow[i] = float(h.p[0][i] + (h.p[0][i] - h.p[9][i]) / n * 0.25);
        }
        if (!ok) {
            owners.insert(owners.end(), nc, -1);
            motion_.erase(h.id);
            continue;
        }
        // its palm's velocity in the room, from the last time it was seen
        double palm[3];
        h.Palm(palm);
        Motion &m = motion_[h.id];
        const double dt = (captureNs_ - m.ns) / 1e9;
        if (m.ns && dt > 0.005 && dt < 0.2) {
            double speed = 0;
            for (int i = 0; i < 3; ++i) {
                m.v[i] += 0.5 * ((palm[i] - m.palm[i]) / dt - m.v[i]);
                speed += m.v[i] * m.v[i];
            }
            speed = std::sqrt(speed);
            if (speed > kMaxSpeed)
                for (double &v : m.v) v *= kMaxSpeed / speed;
        } else if (!m.ns || dt >= 0.2) {
            m.v[0] = m.v[1] = m.v[2] = 0;
        }
        m.ns = captureNs_;
        std::memcpy(m.palm, palm, sizeof palm);
        for (int i = 0; i < 3; ++i) h.v[i] = float(m.v[i]);
        owners.insert(owners.end(), nc, int(seen_.size()));
        seen_.push_back(h);
    }
    for (auto it = motion_.begin(); it != motion_.end();)
        it = captureNs_ - it->second.ns > kStaleNs ? motion_.erase(it) : std::next(it);
    if (owners.size() != ncaps) owners.assign(ncaps, -1);

    base_.clear(), owner_.clear();
    for (uint32_t k = 0; k < ncaps; ++k) {
        const fh_capsule_t &f = v1.capsules[k];
        bool ok = Finite(f.a, 3) && Finite(f.b, 3) && std::isfinite(f.ra) && std::isfinite(f.rb);
        if (!ok || f.ra <= 0 || f.rb <= 0 || f.ra > 1 || f.rb > 1) continue;
        Capsule c;
        Apply(head, f.a, c.a);
        Apply(head, f.b, c.b);
        c.ra = f.ra, c.rb = f.rb;
        base_.push_back(c);
        owner_.push_back(owners[k]);
    }
}

void Hands::SetPrediction(bool on, double leadMs) {
    predict_ = on;
    leadNs_ = int64_t(std::clamp(leadMs, 0.0, 100.0) * 1e6);
}

Mat Hands::HeadAt(int64_t ns) const {
    const Past *best = nullptr;
    for (const Past &p : history_)
        if (!best || std::llabs(p.ns - ns) < std::llabs(best->ns - ns)) best = &p;
    return best ? best->head : Mat{};
}

bool Hands::Update(const Mat &head, int64_t nowNs) {
    history_.push_back({nowNs, head});
    while (!history_.empty() && nowNs - history_.front().ns > kHistoryNs) history_.erase(history_.begin());
    ReadFiles();
    Advance(nowNs);
    return !world_.empty() || !shown_.empty();
}

bool Hands::Feed(const void *hands, size_t size, const fh_gestures_t *gestures, const Mat &head, int64_t nowNs) {
    history_.push_back({nowNs, head});
    while (!history_.empty() && nowNs - history_.front().ns > kHistoryNs) history_.erase(history_.begin());
    static fh_hands_v2_t copy;
    std::memset(&copy, 0, sizeof copy);
    if (hands && size >= sizeof(fh_hands_t)) {
        std::memcpy(&copy, hands, std::min(size, sizeof copy));
        if (std::memcmp(copy.v1.magic, FH_HANDS_MAGIC, 8) == 0) {
            const bool v2 = copy.v1.version >= 2 && size >= sizeof copy;
            version_ = int(copy.v1.version);
            Ingest(copy.v1, v2 ? copy.fits : nullptr, nullptr);
        }
    }
    if (gestures && std::memcmp(gestures->magic, FH_GESTURES_MAGIC, 8) == 0 && gestures->version >= 2)
        Ingest(copy.v1, nullptr, gestures);
    Advance(nowNs);
    return !shown_.empty();
}

void Hands::Advance(int64_t nowNs) {
    nowNs_ = nowNs;
    fresh_ = nowNs - publishNs_ <= kStaleNs;
    if (!fresh_) base_.clear(), owner_.clear(), seen_.clear();
    // move each hand ahead to when this frame will be on the displays; a slow hand's
    // velocity is mostly tracking noise, so it fades out below kStillSpeed
    const double ahead = std::clamp((nowNs + leadNs_ - captureNs_) / 1e9, 0.0, kMaxAhead);
    auto gain = [](const float v[3]) {
        const double speed = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        return std::clamp((speed - kStillSpeed) / kStillSpeed, 0.0, 1.0);
    };
    shown_ = seen_;
    world_ = base_;
    if (predict_) {
        for (Hand &h : shown_) {
            const double k = gain(h.v) * ahead;
            auto move = [&](float p[3]) {
                for (int i = 0; i < 3; ++i) p[i] += float(h.v[i] * k);
            };
            for (auto &p : h.p) move(p);
            move(h.elbow), move(h.pinchPoint), move(h.contact);
        }
        for (size_t k = 0; k < world_.size(); ++k) {
            if (owner_[k] < 0) continue;
            const Hand &h = seen_[owner_[k]];
            const double a = gain(h.v) * ahead;
            for (int i = 0; i < 3; ++i) world_[k].a[i] += float(h.v[i] * a), world_[k].b[i] += float(h.v[i] * a);
        }
    }
    // the correction hook: Room View's picture of the hand, off by distance (off by default)
    if (!shift_.empty() && !history_.empty()) {
        const Mat &head = history_.back().head;
        const double at[3] = {head.m[0][3], head.m[1][3], head.m[2][3]};
        const double up[3] = {head.m[0][1], head.m[1][1], head.m[2][1]}, right[3] = {head.m[0][0], head.m[1][0], head.m[2][0]};
        for (size_t k = 0; k < shown_.size(); ++k) {
            Hand &h = shown_[k];
            double palm[3];
            h.Palm(palm);
            const double d = std::sqrt((palm[0] - at[0]) * (palm[0] - at[0]) + (palm[1] - at[1]) * (palm[1] - at[1]) +
                                       (palm[2] - at[2]) * (palm[2] - at[2]));
            double yaw = shift_.front().yawDeg, pitch = shift_.front().pitchDeg;
            for (size_t i = 1; i < shift_.size(); ++i) {
                if (d <= shift_[i - 1].dist) break;
                const Shift &a = shift_[i - 1], &b = shift_[i];
                const double t = std::clamp((d - a.dist) / (b.dist - a.dist), 0.0, 1.0);
                yaw = a.yawDeg + (b.yawDeg - a.yawDeg) * t, pitch = a.pitchDeg + (b.pitchDeg - a.pitchDeg) * t;
            }
            auto turn = [&](float p[3]) {
                Turn(p, at, up, -yaw * M_PI / 180);    // positive yaw: to the right
                Turn(p, at, right, pitch * M_PI / 180);
            };
            for (auto &p : h.p) turn(p);
            turn(h.elbow), turn(h.pinchPoint), turn(h.contact);
            for (size_t c = 0; c < world_.size(); ++c)
                if (owner_[c] == int(k)) turn(world_[c].a), turn(world_[c].b);
        }
    }
}

void Hands::Points(int64_t nowNs, bool predict, double leadMs, std::vector<HandPoints> &out) const {
    out.clear();
    const double ahead = std::clamp((nowNs + leadMs * 1e6 - captureNs_) / 1e9, 0.0, kMaxAhead);
    for (const Hand &h : seen_) {
        HandPoints hp{h.id, h.right, {}};
        std::memcpy(hp.p, h.p, sizeof hp.p);
        const double speed = h.speed();
        const double gain = predict ? std::clamp((speed - kStillSpeed) / kStillSpeed, 0.0, 1.0) : 0.0;   // as Advance
        for (auto &p : hp.p)
            for (int i = 0; i < 3; ++i) p[i] += float(h.v[i] * gain * ahead);
        out.push_back(hp);
    }
}

// ------------------------------------------------------------------------------- matte

namespace {

// A finger's half-width at each joint, metres, at the average hand's size: real hands
// (thumb and index about 20 mm across at the base, 15 at the tip), not ft-hands' capsules
// (which have a margin in them). The margin is added on top (MatteParams::Margin).
float JointRadius(int j) {
    static const float r[21] = {0,      0.0125f, 0.0110f, 0.0095f, 0.0085f,    // wrist, thumb
                                0.0105f, 0.0095f, 0.0085f, 0.0075f,           // index
                                0.0108f, 0.0098f, 0.0088f, 0.0078f,           // middle
                                0.0102f, 0.0092f, 0.0083f, 0.0074f,           // ring
                                0.0092f, 0.0083f, 0.0076f, 0.0068f};          // little
    return r[j];
}

struct V {
    double x, y, z;
    V(double a = 0, double b = 0, double c = 0) : x(a), y(b), z(c) {}
    explicit V(const float p[3]) : x(p[0]), y(p[1]), z(p[2]) {}
    V operator+(const V &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(const V &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(double k) const { return {x * k, y * k, z * k}; }
    double dot(const V &o) const { return x * o.x + y * o.y + z * o.z; }
    V cross(const V &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const { return std::sqrt(dot(*this)); }
    V unit() const {
        const double n = norm();
        return n > 1e-9 ? *this * (1 / n) : V{};
    }
};

std::array<float, 3> F3(const V &v) { return {float(v.x), float(v.y), float(v.z)}; }

}  // namespace

void BuildMatte(const std::vector<Hand> &hands, const std::vector<Capsule> &fileCapsules, const double headPos[3],
                const MatteParams &p, Matte &out) {
    out.clear();
    const V head(headPos[0], headPos[1], headPos[2]);
    if (!p.mesh) {
        // ft-hands' capsules have a margin of 4 mm in them already; the angular margin is
        // the mesh's (the capsule matte stays as it was, for comparison)
        out.caps = fileCapsules;
        return;
    }
    for (const Hand &h : hands) {
        V P[21];
        for (int j = 0; j < 21; ++j) P[j] = V(h.p[j]);
        double palm[3];
        h.Palm(palm);
        const double dist = (V(palm[0], palm[1], palm[2]) - head).norm();
        const double m = p.Margin(dist, h.speed());
        const double k = std::clamp((P[9] - P[0]).norm() / kPalmLen, 0.7, 1.4);   // this hand's size
        // fingers: a tapered capsule per bone; the tip's end pulled back (MediaPipe's tip
        // point is the very end of the finger, a capsule's round end reaches past it)
        static const int kBones[][2] = {{1, 2},   {2, 3},   {3, 4},   {5, 6},   {6, 7},   {7, 8},   {9, 10},  {10, 11},
                                        {11, 12}, {13, 14}, {14, 15}, {15, 16}, {17, 18}, {18, 19}, {19, 20}};
        for (const auto &b : kBones) {
            const double ra = JointRadius(b[0]) * k, rb = JointRadius(b[1]) * k;
            V a = P[b[0]], e = P[b[1]];
            if (b[1] % 4 == 0) {
                const V d = e - a;
                const double n = d.norm();
                if (n > 1e-6) e = e - d * (std::min(0.5 * rb, 0.4 * n) / n);
            }
            out.caps.push_back({{float(a.x), float(a.y), float(a.z)}, {float(e.x), float(e.y), float(e.z)}, float(ra + m),
                                float(rb + m)});
        }
        // the palm: a slab through the wrist and the knuckles, widened to the hand's edges
        const V across = (P[17] - P[5]).unit();   // from the index side to the little finger's
        const V fwd = (P[9] - P[0]).unit();
        const V normal = across.cross(fwd).unit();
        Hull palmHull;
        const double half = 0.012 * k;
        const V edge[] = {P[0] + across * (0.026 * k), P[0] - across * (0.020 * k), P[1],
                          P[5] - across * (0.008 * k),  P[9],                      P[13],
                          P[17] + across * (0.009 * k), (P[0] + P[17]) * 0.5 + across * (0.014 * k)};
        for (const V &q : edge)
            for (double s : {-half, half}) palmHull.v.push_back(F3(q + normal * s));
        out.hulls.push_back(palmHull), out.hullMargin.push_back(float(m));
        // the thumb's web, between the thumb's base and the index knuckle
        Hull web;
        const double wh = 0.007 * k;
        for (const V &q : {P[1], P[2], P[5] + (P[6] - P[5]) * 0.15, P[5]})
            for (double s : {-wh, wh}) web.v.push_back(F3(q + normal * s));
        out.hulls.push_back(web), out.hullMargin.push_back(float(m));
        // the forearm: a stub of a tapered tube from the wrist toward the elbow (flatter than
        // round). A stub, as the capsules' (16 cm): the elbow is a guess (13-15 degrees off in
        // the forearm probe), and a longer piece costs a bigger redraw for little.
        V dir = V(h.elbow) - P[0];
        double len = dir.norm();
        if (len < 0.05) continue;
        dir = dir * (1 / len);
        len = std::min(len, 0.18 * k);
        V a = across - dir * across.dot(dir);
        if (a.norm() < 1e-6) a = normal.cross(dir);
        a = a.unit();
        const V b = dir.cross(a).unit();
        Hull arm;
        const double rw[2] = {0.027 * k, 0.019 * k}, re[2] = {0.032 * k, 0.025 * k};
        for (int ring = 0; ring < 2; ++ring) {
            const V c = P[0] + dir * (ring ? len : 0.0);
            const double *r = ring ? re : rw;
            for (int i = 0; i < 8; ++i) {
                const double t = i * M_PI / 4;
                arm.v.push_back(F3(c + a * (r[0] * std::cos(t)) + b * (r[1] * std::sin(t))));
            }
        }
        out.hulls.push_back(arm), out.hullMargin.push_back(float(m));
    }
}

// ----------------------------------------------------------------------------- project

namespace {

// Where the line from eye e through point q (both panel-local) meets the panel, as texture
// pixels, and how much a size at q grows there. False if q isn't between the eye and it.
bool OnPanel(const Panel &p, const double e[3], const double q[3], double *x, double *y, double *grow) {
    const double d[3] = {q[0] - e[0], q[1] - e[1], q[2] - e[2]};
    double s, u, v;
    if (p.curve <= 0) {
        if (e[2] <= q[2] || q[2] <= 0) return false;
        s = e[2] / (e[2] - q[2]);
        u = e[0] + s * d[0];
        v = e[1] + s * d[1];
    } else {
        // OpenVR bends a curved panel into a cylinder around (0, *, r), toward its front.
        const double r = p.curve, ez = e[2] - r;
        const double A = d[0] * d[0] + d[2] * d[2], B = 2 * (e[0] * d[0] + ez * d[2]), C = e[0] * e[0] + ez * ez - r * r;
        const double disc = B * B - 4 * A * C;
        if (A < 1e-12 || disc < 0) return false;
        s = (-B + std::sqrt(disc)) / (2 * A);   // the far side: the panel, seen from inside
        const double px = e[0] + s * d[0], pz = e[2] + s * d[2];
        if (pz > r) return false;
        u = r * std::atan2(px, r - pz);
        v = e[1] + s * d[1];
    }
    if (s <= 1) return false;  // the hand is behind the panel
    *x = (u / p.width + 0.5) * p.pxWidth;
    *y = (0.5 - v / p.height) * p.pxHeight;
    *grow = s;
    return true;
}

constexpr double kEps = 0.002;   // a hand pushed through the panel: only the part in front is cut

bool ProjectCapsule(const Panel &p, const Capsule &c, const double eye[3], Capsule2D *out) {
    const double pxPerM = p.pxWidth / p.width;
    double a[3], b[3];
    const double wa[3] = {c.a[0], c.a[1], c.a[2]}, wb[3] = {c.b[0], c.b[1], c.b[2]};
    ToLocal(p.pose, wa, a);
    ToLocal(p.pose, wb, b);
    // a hand pushed through the panel: keep the part in front
    if (a[2] < kEps && b[2] < kEps) return false;
    if (a[2] < kEps || b[2] < kEps) {
        double *in = a[2] < kEps ? b : a, *out3 = a[2] < kEps ? a : b;
        const double t = (in[2] - kEps) / (in[2] - out3[2]);
        for (int i = 0; i < 3; ++i) out3[i] = in[i] + t * (out3[i] - in[i]);
    }
    // and the part near the eye's plane: it would land far across the panel with a
    // huge radius, so one bad hand estimate there tears a hole through the screen
    const double zmax = eye[2] - kNear;
    if (a[2] > zmax && b[2] > zmax) return false;
    if (a[2] > zmax || b[2] > zmax) {
        double *in = a[2] > zmax ? b : a, *out3 = a[2] > zmax ? a : b;
        const double t = (zmax - in[2]) / (out3[2] - in[2]);
        for (int i = 0; i < 3; ++i) out3[i] = in[i] + t * (out3[i] - in[i]);
    }
    double ax, ay, ga, bx, by, gb;
    if (!OnPanel(p, eye, a, &ax, &ay, &ga) || !OnPanel(p, eye, b, &bx, &by, &gb)) return false;
    const float ra = float(c.ra * ga * pxPerM), rb = float(c.rb * gb * pxPerM);
    const float r = std::max(ra, rb);
    if (std::max(ax, bx) + r < 0 || std::min(ax, bx) - r > p.pxWidth || std::max(ay, by) + r < 0 ||
        std::min(ay, by) - r > p.pxHeight)
        return false;
    *out = {float(ax), float(ay), float(bx), float(by), ra, rb};
    return true;
}

// Keeps the part of a convex point set with lo <= z <= hi: the points inside, and where the
// segments between an inside and an outside point cross the plane (the clipped hull's corners
// are among them).
void ClipZ(std::vector<std::array<double, 3>> &pts, double plane, bool keepAbove) {
    std::vector<std::array<double, 3>> out;
    auto in = [&](const std::array<double, 3> &q) { return keepAbove ? q[2] >= plane : q[2] <= plane; };
    for (size_t i = 0; i < pts.size(); ++i) {
        if (in(pts[i])) {
            out.push_back(pts[i]);
            continue;
        }
        for (size_t j = 0; j < pts.size(); ++j) {
            if (!in(pts[j])) continue;
            const double t = (plane - pts[i][2]) / (pts[j][2] - pts[i][2]);
            out.push_back({pts[i][0] + t * (pts[j][0] - pts[i][0]), pts[i][1] + t * (pts[j][1] - pts[i][1]), plane});
        }
    }
    pts.swap(out);
}

double Cross2(const std::array<double, 2> &o, const std::array<double, 2> &a, const std::array<double, 2> &b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// The convex hull (counterclockwise in x right, y down... either way: normals are set
// pointing away from the centroid), at most kMaxHullEdges corners.
std::vector<std::array<double, 2>> Hull2(std::vector<std::array<double, 2>> p) {
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end(), [](auto &a, auto &b) { return std::fabs(a[0] - b[0]) < 1e-3 && std::fabs(a[1] - b[1]) < 1e-3; }),
            p.end());
    if (p.size() < 3) return p;
    std::vector<std::array<double, 2>> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && Cross2(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && Cross2(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    // too many corners: drop the ones that take the least area away
    while (h.size() > size_t(kMaxHullEdges)) {
        size_t best = 0;
        double least = 1e300;
        for (size_t i = 0; i < h.size(); ++i) {
            const double a = std::fabs(Cross2(h[(i + h.size() - 1) % h.size()], h[i], h[(i + 1) % h.size()]));
            if (a < least) least = a, best = i;
        }
        h.erase(h.begin() + long(best));
    }
    return h;
}

constexpr float kHullFeather = 2.0f;   // pixels either side of a hull's edge

bool ProjectHull(const Panel &p, const Hull &hull, float margin, const double eye[3], Hull2D *out) {
    const double pxPerM = p.pxWidth / p.width;
    std::vector<std::array<double, 3>> pts;
    for (const auto &q : hull.v) {
        const double w[3] = {q[0], q[1], q[2]};
        double l[3];
        ToLocal(p.pose, w, l);
        pts.push_back({l[0], l[1], l[2]});
    }
    ClipZ(pts, kEps, true);              // the part in front of the panel
    if (pts.size() < 3) return false;
    ClipZ(pts, eye[2] - kNear, false);   // ...and not near the eye's plane (see ProjectCapsule)
    if (pts.size() < 3) return false;
    std::vector<std::array<double, 2>> flat;
    double grow = 0;
    for (const auto &q : pts) {
        double x, y, g;
        if (!OnPanel(p, eye, q.data(), &x, &y, &g)) continue;
        flat.push_back({x, y});
        grow = std::max(grow, g);
    }
    const auto h = Hull2(flat);
    if (h.size() < 3) return false;
    double cx = 0, cy = 0;
    for (const auto &q : h) cx += q[0] / h.size(), cy += q[1] / h.size();
    Hull2D o;
    o.r = float(margin * grow * pxPerM);
    // the edges, in order around it (edge i from corner i to i + 1), normals pointing out
    std::vector<std::array<double, 2>> corner;
    for (size_t i = 0; i < h.size(); ++i) {
        const auto &a = h[i], &b = h[(i + 1) % h.size()];
        double nx = b[1] - a[1], ny = -(b[0] - a[0]);
        const double n = std::hypot(nx, ny);
        if (n < 1e-6) continue;
        nx /= n, ny /= n;
        double c = nx * a[0] + ny * a[1];
        if (nx * cx + ny * cy - c > 0) nx = -nx, ny = -ny, c = -c;   // point away from the middle
        o.e[o.n][0] = float(nx), o.e[o.n][1] = float(ny), o.e[o.n][2] = float(c);
        corner.push_back(a);
        ++o.n;
    }
    if (o.n < 3) return false;
    // what's drawn: each corner pushed out along its bisector so the edges move out by `reach`
    // (the margin and the feather), at most 3 times that at a sharp corner
    const double reach = o.r + kHullFeather + 1;
    o.box[0] = o.box[1] = 1e9f, o.box[2] = o.box[3] = -1e9f;
    for (int i = 0; i < o.n; ++i) {
        const float *n1 = o.e[(i + o.n - 1) % o.n], *n2 = o.e[i];
        double bx = n1[0] + n2[0], by = n1[1] + n2[1];
        const double bl = std::hypot(bx, by);
        double k = reach;
        if (bl > 1e-6) {
            bx /= bl, by /= bl;
            k = reach / std::max(1.0 / 3, bx * n2[0] + by * n2[1]);
        } else {
            bx = n2[0], by = n2[1];
        }
        o.fan[i][0] = float(corner[i][0] + bx * k), o.fan[i][1] = float(corner[i][1] + by * k);
        o.box[0] = std::min(o.box[0], o.fan[i][0]), o.box[1] = std::min(o.box[1], o.fan[i][1]);
        o.box[2] = std::max(o.box[2], o.fan[i][0]), o.box[3] = std::max(o.box[3], o.fan[i][1]);
    }
    if (o.box[2] < 0 || o.box[0] > p.pxWidth || o.box[3] < 0 || o.box[1] > p.pxHeight) return false;
    *out = o;
    return true;
}

}  // namespace

bool Project(const Panel &p, const std::vector<Capsule> &caps, const double eyes[2][3], std::vector<Capsule2D> out[2]) {
    bool any = false;
    for (int e = 0; e < 2; ++e) {
        out[e].clear();
        double eye[3];
        ToLocal(p.pose, eyes[e], eye);
        if (eye[2] <= 0.01) continue;  // behind the panel
        for (const Capsule &c : caps) {
            Capsule2D d;
            if (ProjectCapsule(p, c, eye, &d)) out[e].push_back(d), any = true;
        }
    }
    return any;
}

bool Project(const Panel &p, const Matte &m, const double eyes[2][3], Spots out[2]) {
    bool any = false;
    for (int e = 0; e < 2; ++e) {
        out[e].clear();
        double eye[3];
        ToLocal(p.pose, eyes[e], eye);
        if (eye[2] <= 0.01) continue;  // behind the panel
        for (const Capsule &c : m.caps) {
            Capsule2D d;
            if (ProjectCapsule(p, c, eye, &d)) out[e].caps.push_back(d), any = true;
        }
        for (size_t k = 0; k < m.hulls.size(); ++k) {
            Hull2D d;
            if (ProjectHull(p, m.hulls[k], k < m.hullMargin.size() ? m.hullMargin[k] : 0, eye, &d))
                out[e].hulls.push_back(d), any = true;
        }
    }
    return any;
}

}  // namespace handcut
