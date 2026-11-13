// The shared-file reader and 3D geometry used by FrameHands' overlay.
#include "handcut.h"

#include "../config/ft_conf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace handcut {
namespace {

// The hands file ft-hands publishes (hands/include/fh_hands.h).
constexpr uint32_t kMaxHands = FH_HANDS_MAX_HANDS, kMaxCapsules = FH_HANDS_MAX_CAPSULES;
constexpr int64_t kStaleNs = 300'000'000;      // hands older than this are gone
constexpr int64_t kHistoryNs = 1'000'000'000;
constexpr double kNear = 0.12;   // metres: nothing closer to an eye than this is cut
constexpr double kMaxSpeed = 2.5;     // m/s: faster is a tracking jump, not a hand
constexpr double kStillSpeed = 0.05;  // m/s: below this, a hand's velocity is noise
constexpr double kMaxAhead = 0.12;    // s: never predict further than this
constexpr double kPalmLen = 0.0815;   // wrist to middle knuckle of the skeleton's average hand

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

// By the conf's grammar (config/ft_conf.h, config/ft_conf.h), as every other reader: an indented
// line counts, "NAME = value" doesn't (bash can't read it either), and the last line wins. This
// read only lines starting "NAME=" before, so HANDS_NEAR could be off for the relay and Input
// Settings and on here.
std::string Setting(const char *name, const char *def) { return ft_conf::Setting(name, def); }

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

const char *ShapeName(MatteShape s) {
    return s == MatteShape::Circle ? "circle" : s == MatteShape::Mesh ? "mesh" : "capsules";
}

bool ParseShape(const std::string &name, MatteShape *out) {
    for (MatteShape s : {MatteShape::Circle, MatteShape::Mesh, MatteShape::Capsules})
        if (name == ShapeName(s)) return *out = s, true;
    return false;
}

std::string ReadMatteSettings(MatteParams &p) {
    std::string log;
    auto num = [&](const char *name, double &v, double lo, double hi) {
        const std::string s = Setting(name, "");
        if (s.empty()) return;
        double x;
        if (ft_conf_number(s.c_str(), &x) && x >= lo && x <= hi) v = x, log += std::string(name) + "=" + s + " ";
        else log += std::string(name) + "=" + s + " (ignored) ";
    };
    const std::string m = Setting("HANDS_MATTE", "");
    if (!m.empty()) log += "HANDS_MATTE=" + m + (ParseShape(m, &p.shape) ? " " : " (ignored) ");
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

Hands::~Hands() = default;

bool Hands::ReadFiles() {
    bool got = false;
    // A new frame: a new sequence number, or a new file (fh_shm.hpp).
    static fh_hands_v2_t copy;
    if (file_.Read(&copy, sizeof copy, offsetof(fh_hands_t, seq)) && (copy.v1.seq != seq_ || file_.opens != opens_)) {
        seq_ = copy.v1.seq, opens_ = file_.opens;
        const bool v2 = copy.v1.version >= 2 && file_.Size() >= sizeof copy && copy.v1.size >= sizeof copy;
        version_ = int(copy.v1.version);
        Ingest(copy.v1, v2 ? copy.fits : nullptr, nullptr);
        got = true;
    }
    // the gestures file, written right after each hands update: version 2 (the grips) or later
    static fh_gestures_t g;
    if (gfile_.Read(&g, sizeof g, offsetof(fh_gestures_t, seq)) && (g.seq != gseq_ || gfile_.opens != gopens_)) {
        gseq_ = g.seq, gopens_ = gfile_.opens;
        if (g.version >= 2) Ingest(copy.v1, nullptr, &g);
    }
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
    const Mat head = captureHead_ = HeadAt(captureNs_);

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
    if (!fresh_) {
        base_.clear(), owner_.clear(), seen_.clear();
        for (Gesture *g : {&pinch_[0], &pinch_[1], &grip_[0], &grip_[1]}) g->down = g->tracked = false;   // ft-hands stopped
    }
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
    if (p.shape == MatteShape::Capsules) {
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
        if (p.shape == MatteShape::Circle) {
            // the middle of what the head sees of the hand: the box around its joints (each
            // with its half-width; the wrist's is the hand's) in a plane facing the head...
            V w;
            V u[21];
            double rr[21], d[21];
            for (int j = 0; j < 21; ++j) {
                d[j] = std::max(1e-3, (P[j] - head).norm());
                u[j] = (P[j] - head) * (1 / d[j]), w = w + u[j];
                rr[j] = (j ? JointRadius(j) : 0.025) * k;
            }
            w = w.unit();
            const V a = (std::fabs(w.y) < 0.9 ? V(0, 1, 0) : V(1, 0, 0)).cross(w).unit(), b = w.cross(a);
            double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
            for (int j = 0; j < 21; ++j) {
                const double f = u[j].dot(w), t = rr[j] / d[j];
                if (f < 0.1) continue;   // beside the head: not seen
                const double x = u[j].dot(a) / f, y = u[j].dot(b) / f;
                lo[0] = std::min(lo[0], x - t), hi[0] = std::max(hi[0], x + t);
                lo[1] = std::min(lo[1], y - t), hi[1] = std::max(hi[1], y + t);
            }
            if (lo[0] > hi[0]) continue;
            const V c = (w + a * ((lo[0] + hi[0]) / 2) + b * ((lo[1] + hi[1]) / 2)).unit();
            // ...and the widest angle from there to the hand's edge, at the palm's distance
            double reach = 0;
            for (int j = 0; j < 21; ++j)
                reach = std::max(reach, std::acos(std::clamp(u[j].dot(c), -1.0, 1.0)) + std::atan(rr[j] / d[j]));
            const double r = dist * std::tan(std::min(reach, 1.2)) + m;
            const V at = head + c * dist;
            out.circles.push_back({{float(at.x), float(at.y), float(at.z)}, float(r)});
            continue;
        }
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


}  // namespace handcut
