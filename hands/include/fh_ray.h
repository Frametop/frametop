// fh_ray - the hand ray: where a bare hand points, Quest-style, and when it counts as pointing.
// Shared by the pointer helper (the ray drives the ft_pointer SteamVR device: pointer/helper)
// and ft-screens (a second hand's ray for two-hand stretch), so both agree.
// Design: frame-hands/notes/hand-interaction.md, sections 2.4 and 2.5.
//
// Header-only C++17, no dependencies. Everything is in the room (OpenVR's standing universe:
// +x right, +y up, -z forward), metres, radians inside and degrees in Params, seconds on one
// steady clock (CLOCK_MONOTONIC: ft-hands' capture times, as seconds).
//
// Body model (no torso tracking): the torso's yaw is the head's, low-passed (torsoTau, 0.6 s) and
// never more than torsoMaxLag (30 deg) behind. The neck is the head moved by neck (10 cm down, 8 cm
// back), turned by the head's yaw only; a shoulder is the neck moved by (+-shoulderX, shoulderY,
// 0) (17 cm out, 13 cm down), turned by the torso's yaw. Head pitch and roll are ignored, so
// nodding doesn't swing the ray.
//
// Ray: from the shoulder S through the knuckle point K (the index and middle knuckles: the pinch
// point, which holds still as the fingers close, so pinching barely turns the ray). The direction
// is D = normalize(K - S); the origin is K + originAhead (3 cm) along D, in front of the fingers.
// K is predicted predict (20 ms) ahead with its room velocity (differenced across frames).
//
// Pointing (Gate): the ray shows once every "show" test has held for showFor (120 ms), and hides
// once some "keep" test has failed for hideFor (250 ms):
//   test        show                                   keep (hide past it)
//   side        the arbiter says hand (and the caller   (at once otherwise: "off")
//               allows it: in a game, the mouse)
//   tracked     a two-camera (STEREO) frame within      a frame within lostFor (250 ms); one-camera
//               stereoWithin (200 ms)                   depth is 2-6 cm off, fine for a short gap
//   raised      K at most raisedShow (0.30 m) below     raisedHide (0.40 m): gaze pinches sat
//               the eyes                                0.35-0.45 m below the eyes, elbow resting
//   forward     K at least forwardShow (0.25 m) ahead   forwardHide (0.20 m)
//               of the eyes, along the torso's yaw
//   palm away   palm normal . D >= palmShow (0.3)       palmHide (0.0): palm toward you is reserved
//   forearm     elbow-to-wrist pitch >= forearmShow     forearmHide (-50 deg); only with an elbow
//               (-35 deg)                               of elbowConfMin (0.5) confidence
//   aim         D's pitch in [pitchMin, pitchMax]       [pitchMinHide, pitchMaxHide] (-60, 75),
//               (-45, 60), yaw within yawShow (70)      yawHide (80)
//               of the head's
//   not near    not near a panel (ft-screens' near      near: at once (near wins)
//               claim, or the fingertip at an overlay)
//   not a fist  no grip down                            a grip down
// While a ray pinch is held (Hold), the ray stays up whatever the tests say, until hideFor after
// the release.
//
// Filter (RayFilter): One Euro filters (Casiez, Roussel and Vogel, CHI 2012) on the ray's yaw and
// pitch (minCutoff 0.8 Hz, beta 0.02 per deg/s, dCutoff 1 Hz) and per axis on the origin
// (originMinCutoff 1.5 Hz): the aim matters more than where the beam starts.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace fhray {

struct V3 {
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a) {
    const double l = Len(a);
    return l > 1e-12 ? a * (1 / l) : V3{0, 0, -1};
}
constexpr double kDeg = M_PI / 180;
// Yaw: 0 along -z, positive turning left (about +y), as OpenVR; pitch: up positive.
inline double YawOf(V3 d) { return std::atan2(-d.x, -d.z); }
inline double PitchOf(V3 d) { return std::asin(std::clamp(Norm(d).y, -1.0, 1.0)); }
inline V3 Dir(double yaw, double pitch) {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}
// v turned by yaw about +y.
inline V3 TurnYaw(V3 v, double yaw) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}
inline double WrapPi(double a) { return std::remainder(a, 2 * M_PI); }

struct Params {
    // body
    double torsoTau = 0.6, torsoMaxLag = 30;
    V3 neck{0, -0.10, 0.08};
    double shoulderX = 0.17, shoulderY = -0.13;
    // ray
    double originAhead = 0.03, predict = 0.020;
    // gate
    double showFor = 0.12, hideFor = 0.25;
    double stereoWithin = 0.2, lostFor = 0.25;
    double raisedShow = 0.30, raisedHide = 0.40;
    double forwardShow = 0.25, forwardHide = 0.20;
    double palmShow = 0.3, palmHide = 0.0;
    double forearmShow = -35, forearmHide = -50, elbowConfMin = 0.5;
    double pitchMin = -45, pitchMax = 60, pitchMinHide = -60, pitchMaxHide = 75;
    double yawShow = 70, yawHide = 80;
    // filter
    double minCutoff = 0.8, beta = 0.02, dCutoff = 1.0, originMinCutoff = 1.5, originBeta = 0.5;
};

// The torso and shoulders, from the head.
class Body {
public:
    explicit Body(const Params &p = {}) : p_(p) {}
    void SetParams(const Params &p) { p_ = p; }
    // The head's position and forward direction (-z of its pose) at time t.
    void Update(double t, V3 head, V3 forward) {
        head_ = head;
        headYaw_ = YawOf(forward);
        if (!init_) {
            torso_ = headYaw_, t_ = t, init_ = true;
            return;
        }
        const double dt = std::clamp(t - t_, 0.0, 0.5);
        t_ = t;
        double lag = WrapPi(headYaw_ - torso_);
        lag *= std::exp(-dt / std::max(1e-3, p_.torsoTau));
        const double max = p_.torsoMaxLag * kDeg;
        lag = std::clamp(lag, -max, max);
        torso_ = WrapPi(headYaw_ - lag);
    }
    bool Ready() const { return init_; }
    V3 Head() const { return head_; }
    double HeadYaw() const { return headYaw_; }
    double TorsoYaw() const { return torso_; }
    V3 TorsoForward() const { return Dir(torso_, 0); }
    V3 Neck() const { return head_ + TurnYaw(p_.neck, headYaw_); }
    V3 Shoulder(int side) const {
        return Neck() + TurnYaw(V3{side ? p_.shoulderX : -p_.shoulderX, p_.shoulderY, 0}, torso_);
    }

private:
    Params p_;
    V3 head_;
    double headYaw_ = 0, torso_ = 0, t_ = 0;
    bool init_ = false;
};

struct Ray {
    V3 origin, dir{0, 0, -1};
};

// One hand at one capture time, in the room.
struct HandFrame {
    double t = 0;            // capture time, s
    bool stereo = false;     // triangulated from two or more cameras
    V3 knuckle;              // K: the index and middle knuckles
    V3 wrist;
    V3 palmNormal;           // unit, out of the palm
    V3 elbow;
    double elbowConf = 0;    // 0: the arm model's guess
    bool grip = false;       // a grip (closed hand) is down
    bool near = false;       // near a panel (see "not near" at the top)
};

// What each test said for one frame (degrees and metres, for logs and the offline evaluation).
struct Tests {
    double below = 0, ahead = 0, palm = 0, forearm = 0, pitch = 0, yawOff = 0;
    bool forearmUsed = false;
    bool show = false, keep = false;  // every show test, every keep test (not counting side, tracking)
    std::string fails;                // the show tests that failed
};

inline Tests Evaluate(const Params &p, const Body &body, const HandFrame &f, const Ray &ray) {
    Tests r;
    const V3 eye = body.Head();
    r.below = eye.y - f.knuckle.y;
    const V3 fwd = body.TorsoForward();
    r.ahead = Dot(f.knuckle - eye, fwd);
    r.palm = Dot(f.palmNormal, ray.dir);
    r.pitch = PitchOf(ray.dir) / kDeg;
    r.yawOff = std::fabs(WrapPi(YawOf(ray.dir) - body.HeadYaw())) / kDeg;
    r.forearmUsed = f.elbowConf >= p.elbowConfMin;
    if (r.forearmUsed) r.forearm = PitchOf(f.wrist - f.elbow) / kDeg;
    bool show = true, keep = true;
    auto test = [&](bool s, bool k, const char *name) {
        if (!s) r.fails += r.fails.empty() ? name : std::string(",") + name;
        show = show && s, keep = keep && k;
    };
    test(r.below <= p.raisedShow, r.below <= p.raisedHide, "raised");
    test(r.ahead >= p.forwardShow, r.ahead >= p.forwardHide, "forward");
    test(r.palm >= p.palmShow, r.palm >= p.palmHide, "palm");
    if (r.forearmUsed) test(r.forearm >= p.forearmShow, r.forearm >= p.forearmHide, "forearm");
    test(r.pitch >= p.pitchMin && r.pitch <= p.pitchMax, r.pitch >= p.pitchMinHide && r.pitch <= p.pitchMaxHide, "aim");
    test(r.yawOff <= p.yawShow, r.yawOff <= p.yawHide, "yaw");
    test(!f.grip, !f.grip, "grip");
    r.show = show, r.keep = keep;
    return r;
}

// One Euro filter for one value.
class OneEuro {
public:
    OneEuro(double minCutoff = 1, double beta = 0, double dCutoff = 1) : min_(minCutoff), beta_(beta), dc_(dCutoff) {}
    void Set(double minCutoff, double beta, double dCutoff) { min_ = minCutoff, beta_ = beta, dc_ = dCutoff; }
    void Reset() { init_ = false; }
    double operator()(double v, double t) {
        if (!init_ || t <= t_) {
            if (!init_) x_ = v, dx_ = 0, t_ = t, init_ = true;
            return x_;
        }
        const double dt = t - t_;
        t_ = t;
        const double d = (v - x_) / dt;
        dx_ += Alpha(dc_, dt) * (d - dx_);
        x_ += Alpha(min_ + beta_ * std::fabs(dx_), dt) * (v - x_);
        return x_;
    }
    double Value() const { return x_; }

private:
    static double Alpha(double cutoff, double dt) {
        const double tau = 1 / (2 * M_PI * cutoff);
        return 1 / (1 + tau / dt);
    }
    double min_, beta_, dc_, x_ = 0, dx_ = 0, t_ = 0;
    bool init_ = false;
};

// The ray's filter: yaw and pitch in degrees, the origin per axis.
class RayFilter {
public:
    explicit RayFilter(const Params &p = {}) { SetParams(p); }
    void SetParams(const Params &p) {
        yaw_.Set(p.minCutoff, p.beta, p.dCutoff), pitch_.Set(p.minCutoff, p.beta, p.dCutoff);
        for (OneEuro &o : o_) o.Set(p.originMinCutoff, p.originBeta, p.dCutoff);
    }
    void Reset() {
        yaw_.Reset(), pitch_.Reset();
        for (OneEuro &o : o_) o.Reset();
        have_ = false;
    }
    Ray operator()(const Ray &raw, double t) {
        double y = YawOf(raw.dir) / kDeg;
        if (have_) y = lastYaw_ + std::remainder(y - lastYaw_, 360.0);  // unwrapped
        lastYaw_ = y, have_ = true;
        const double fy = yaw_(y, t), fp = pitch_(PitchOf(raw.dir) / kDeg, t);
        Ray r;
        r.dir = Dir(fy * kDeg, fp * kDeg);
        r.origin = {o_[0](raw.origin.x, t), o_[1](raw.origin.y, t), o_[2](raw.origin.z, t)};
        return r;
    }

private:
    OneEuro yaw_, pitch_, o_[3];
    double lastYaw_ = 0;
    bool have_ = false;
};

// One side's ray: prediction, the gate and the filter.
class HandRay {
public:
    explicit HandRay(int side, const Params &p = {}) : side_(side), p_(p), filter_(p) {}
    void SetParams(const Params &p) { p_ = p, filter_.SetParams(p); }
    int Side() const { return side_; }

    // A new frame of this side's hand. sideIsHand: the ray may show (the arbiter says this side is a
    // bare hand, and the caller allows it); without it, a ray shown hides at once ("hide off").
    void Frame(const Body &body, const HandFrame &f, bool sideIsHand) {
        // K's room velocity, for the prediction
        V3 vk;
        if (haveLast_ && f.t > lastT_ && f.t - lastT_ < 0.2) vk = (f.knuckle - lastK_) * (1 / (f.t - lastT_));
        lastK_ = f.knuckle, lastT_ = f.t, haveLast_ = true;
        const V3 k = f.knuckle + vk * p_.predict;
        const V3 s = body.Shoulder(side_);
        raw_.dir = Norm(k - s);
        raw_.origin = k + raw_.dir * p_.originAhead;
        reach_ = Len(f.knuckle - s);
        lastFrame_ = f.t;
        if (f.stereo) lastStereo_ = f.t;
        tests_ = Evaluate(p_, body, f, raw_);
        const bool tracked = f.t - lastStereo_ <= p_.stereoWithin;
        const bool showOk = sideIsHand && tracked && tests_.show && !f.near;
        const bool keepOk = tests_.keep;
        if (!tests_.fails.empty()) why_ = tests_.fails;
        else if (!tracked) why_ = "one camera";
        else if (f.near) why_ = "near";
        else why_ = "";
        if (!sideIsHand) why_ = "off";
        // the timers run on capture times
        if (!shown_) {
            if (showOk) {
                if (showSince_ < 0) showSince_ = f.t;
                if (f.t - showSince_ >= p_.showFor - 1e-9) Show(f.t);
            } else {
                showSince_ = -1;
            }
        } else {
            if (!sideIsHand) {
                Hide("off");
            } else if (f.near && !held_) {
                Hide("near");
            } else if (keepOk || held_ || f.t < holdUntil_) {
                failSince_ = -1;
            } else {
                if (failSince_ < 0) failSince_ = f.t;
                if (f.t - failSince_ >= p_.hideFor - 1e-9) Hide(tests_.fails.empty() ? "pose" : tests_.fails);
            }
        }
        if (shown_) out_ = filter_(raw_, f.t);
    }
    // Each tick, with the time now on the capture clock: the hand lost (no frame for lostFor) hides it.
    void Tick(double now, bool sideIsHand) {
        if (!shown_) return;
        if (!sideIsHand) Hide("off");
        else if (!held_ && now - lastFrame_ > p_.lostFor && now >= holdUntil_) Hide("lost");
    }
    // A ray pinch is held: the ray stays up until hideFor after it ends.
    void Hold(bool on, double t) {
        if (held_ && !on) holdUntil_ = t + p_.hideFor;
        held_ = on;
    }
    void Reset() { Hide("reset"); }

    bool Shown() const { return shown_; }
    const Ray &Current() const { return out_; }   // filtered
    const Ray &Raw() const { return raw_; }       // unfiltered, predicted
    double Reach() const { return reach_; }       // |K - S|: arm out or in (push and pull)
    const Tests &LastTests() const { return tests_; }
    const std::string &Why() const { return why_; }   // what fails now ("" if nothing)
    // Changes since the caller last cleared it: "show" or "hide <reason>".
    std::string event;

private:
    void Show(double t) {
        shown_ = true, failSince_ = -1, showSince_ = -1;
        filter_.Reset();
        out_ = filter_(raw_, t);
        event = "show";
    }
    void Hide(const std::string &why) {
        if (!shown_) return;
        shown_ = false, failSince_ = -1, showSince_ = -1, held_ = false, holdUntil_ = 0;
        event = "hide " + why;
    }
    int side_;
    Params p_;
    RayFilter filter_;
    Ray raw_, out_;
    Tests tests_;
    std::string why_;
    V3 lastK_;
    double lastT_ = 0, lastFrame_ = -1e9, lastStereo_ = -1e9, reach_ = 0;
    double showSince_ = -1, failSince_ = -1, holdUntil_ = 0;
    bool haveLast_ = false, shown_ = false, held_ = false;
};

}  // namespace fhray
