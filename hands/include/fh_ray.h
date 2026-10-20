// fh_ray - the hand ray: where a bare hand points, Quest-style, and when it counts as pointing.
// Shared by the pointer helper (the ray drives the ft_pointer SteamVR device: pointer/helper)
// and ft-screens (a second hand's ray for two-hand stretch), so both agree.
// Design: frame-hands/notes/hand-interaction.md, sections 2.4 and 2.5.
//
// Header-only C++17, no dependencies. Everything is in the room (OpenVR's standing universe:
// +x right, +y up, -z forward), metres, radians inside and degrees in Params, seconds on one
// steady clock (CLOCK_MONOTONIC: ft-hands' capture times, as seconds).
//
// Body model (no torso tracking): the torso faces the head's yaw, low-passed (torsoTau, 0.6 s) and
// never more than torsoMaxLag (30 deg) off the head's. With torsoWrists (on), what it faces is the
// head's forward counted twice plus the direction from the neck to each wrist seen, levelled (the
// formula is StereoKit's, MIT: systems/input.cpp, body_make_shoulders): you point where your body
// faces, so looking aside while pointing turns the shoulders less, and the ray with them (with both
// hands ahead, a third as far). On the probe-moves replay (pointer/tools/rayeval) the share of a
// still hand's ray motion that came from the shoulder went from 20 to 18% (p90 1.7-3.6 deg/s from
// 2.0-3.7); on the guided tap capture, where the head mostly faced the panel, nothing got worse.
// The neck is the head moved by neck (10 cm down, 8 cm back), turned by the head's yaw only; a
// shoulder is the neck moved by (+-shoulderX, shoulderY, 0) (17 cm out, 13 cm down), turned by the
// torso's yaw. Head pitch and roll are ignored, so nodding doesn't swing the ray.
//
// Ray: from the shoulder S through the knuckle point K (the index and middle knuckles: the pinch
// point, which holds still as the fingers close, so pinching barely turns the ray). The direction
// is D = normalize(K - S); the origin is K + originAhead (3 cm) along D, in front of the fingers.
// K is predicted predict (20 ms) ahead with its room velocity (differenced across frames).
//
// Hand direction (handWeight, 0 = off, the default): D turned toward where the hand itself points
// by that fraction of the angle between them (StereoKit's aim, MIT: xr_backends/extensions/
// hand_tracking.cpp, which slerps half way). The hand's axes come from the joints that hold still in
// a thumb-index pinch (HandAxes: the wrist and the middle, ring and little knuckles), and its
// direction is their forward (wrist to knuckles) tilted handPitch toward the back of the hand
// (negative: the palm) and handYaw out, away from the thumb. The gate's tests still read the
// shoulder's ray. Off because every weight measured worse (rayeval, the guided tap capture and
// probe-moves, fitted skeleton): our palm's orientation is far noisier than K seen from the
// shoulder 45 cm away. At 0.3 the 90 Hz judder went from 0.07-0.14 to 0.11-0.33 deg rms, a still
// hand's wobble more than doubled, the ray's turn 150 ms into a pinch went from 0.4-0.5 to 1.2-1.7
// deg (p50), and frames where the raw ray turned over 3 deg from 159 to 330. Where the hand's
// direction sits against the shoulder's ray depends on how the hand is held: pitch -15 to -41, yaw
// -4 to 19 (the recordings' medians); handPitch and handYaw are in the middle of that. StereoKit's
// -50 and 20 suit its own axes.
//
// Pinch anchor (pinchAnchor, off by default): while a ray pinch is held, K is pinned to the hand's
// axes where it was at the press, rebuilt from them each frame (StereoKit's pinch point, MIT:
// hands/input_hand.cpp, pinned to the index finger's root while the pinch is active), and eased
// back to the measured K on the steady shoulder's schedule (below). StereoKit pins a point between
// the fingertips, which travels as they open; ours is the knuckles already, and on the guided tap
// capture it held stiller than the axes do: pinned, a held pinch's point (as a near grab carries
// it) shook as much or more (0.15-0.29 against 0.12-0.19 mm per 90 Hz tick, p50) and its release
// kick was no smaller (1.4-2.0 against 1.2-1.6 mm, p50, palm still), from the fitted skeleton and
// from the filtered landmarks alike. The palm's turn, times the 7 cm from its centre to K, is the
// noise it adds.
//
// Pointing (Gate): the ray shows once every "show" test has held for showFor (120 ms), and hides
// once some "keep" test has failed for hideFor (250 ms):
//   test        show                                   keep (hide past it)
//   side        the arbiter says hand (and the caller   (at once otherwise: "off")
//               allows it: in a game, the mouse)
//   tracked     a two-camera (STEREO) frame within      a frame within lostFor (250 ms); one-camera
//               stereoWithin (200 ms)                   depth is 2-6 cm off, fine for a short gap
//   raised      K at most raisedShow (0.30 m) below     raisedHide (0.40 m): gaze pinches sat
//               the eyes (0: off; the pointer helper    0.35-0.45 m below the eyes, elbow resting
//               turns it off: user, 2026-10-19, "hands
//               still recognized even if they are low,
//               just not if they are typing", which its
//               typing guard holds back)
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
//   ready       the thumb tip within readyShow (0.06    readyHide (0.10 m) apart: the laser is
//               m) of the index or the middle tip       for pinching, not a hand held up (0: off;
//               (user, 2026-10-11: "the aiming laser    frames without either gap pass). The
//               only activates when your fingers get    middle counts for its right click and the
//               within a distance"; 2026-10-15: also    three-finger scroll
//               for the middle and three-finger taps)
//   open        not a fist: HandFrame::curl (the more   fistCurl - 0.05: a hand closing into a
//               open of the index and the other three   fist drops its laser, so the fist's
//               fingers) at least fistCurl (1.1)        thumb-to-index touch isn't a click
// While a ray pinch is held (Hold), the ray stays up whatever the tests say, until hideFor after
// the release. A pinch may also bring a ray up (ShowForPinch): one that hid within a moment (the
// pointer helper's grace; not one hidden for "off", "near" or a reset), or one whose show tests
// all pass but not yet for showFor. Closing the fingers turns the hand a little, so a ray near a
// hide limit can drop just as the pinch begins.
//
// Filter (RayFilter, hands/include/fh_smooth.h): One Euro filters (Casiez, Roussel and Vogel, CHI
// 2012) run in continuous time, so the beam moves at the display's rate rather than in steps at
// the tracker's: the direction on the sphere (minCutoff, beta per deg/s, dCutoff), the origin
// (originMinCutoff, originBeta per m/s): the aim matters more than where the beam starts. While a
// ray pinch is held (a drag), the direction's minimum cutoff drops to holdMinCutoff: a slow drag
// is steadier, a fast one still keeps up. The reach (|K - S|, push and pull's input) has its own
// (reachMinCutoff, reachBeta per m/s): push and pull turns a reach's change into 3 times its share
// of the panel's distance (13 times it for a panel 2 m away). The caller reads the ray at
// the time it draws (At(now)); between tracker frames it glides toward the newest one.
//
// Steady shoulder (holdShoulder): while a ray pinch is held, and hideFor after, the ray's shoulder
// stays where it was in the room at the press; then it eases back to the body model's over
// shoulderBlend (0.5 s). The model's shoulder moves with the head (the neck follows its position,
// the torso its yaw): on a recording, with the hand still, a fifth of the ray's motion came from
// there, up to 2-4 deg/s (p90). Looking around while dragging moved what was dragged. The pinch
// anchor (when on) holds and eases back with it.
#pragma once

#include "fh_smooth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace fhray {

using fhsmooth::Dot;
using fhsmooth::Len;
using fhsmooth::Norm;
using fhsmooth::V3;
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
    bool torsoWrists = true;
    V3 neck{0, -0.10, 0.08};
    double shoulderX = 0.17, shoulderY = -0.13;
    // ray
    double originAhead = 0.03, predict = 0.020;
    double handWeight = 0, handPitch = -30, handYaw = 8;
    bool pinchAnchor = false;
    // gate
    double showFor = 0.12, hideFor = 0.25;
    double stereoWithin = 0.2, lostFor = 0.25;
    double raisedShow = 0.30, raisedHide = 0.40;
    double forwardShow = 0.25, forwardHide = 0.20;
    double palmShow = 0.3, palmHide = 0.0;
    double forearmShow = -35, forearmHide = -50, elbowConfMin = 0.5;
    double pitchMin = -45, pitchMax = 60, pitchMinHide = -60, pitchMaxHide = 75;
    double yawShow = 70, yawHide = 80;
    double readyShow = 0.06, readyHide = 0.10;   // the thumb to the nearer of the index and middle tips (0: no "ready" test)
    double fistCurl = 1.1;                       // HandFrame::curl under this: a fist (0: no "open" test)
    // filter (see the top)
    double minCutoff = 0.8, beta = 0.06, dCutoff = 1.0, originMinCutoff = 1.5, originBeta = 0.5;
    double holdMinCutoff = 0.5;
    bool holdShoulder = true;
    double shoulderBlend = 0.5;
    double reachMinCutoff = 1.0, reachBeta = 5.0;
};

// The torso and shoulders, from the head (and the wrists: torsoWrists).
class Body {
public:
    explicit Body(const Params &p = {}) : p_(p) {}
    void SetParams(const Params &p) { p_ = p; }
    // The head's position and forward direction (-z of its pose) at time t, and the wrists seen in
    // the same frame (none: nullptr, n 0).
    void Update(double t, V3 head, V3 forward, const V3 *wrists = nullptr, int n = 0) {
        head_ = head;
        headYaw_ = YawOf(forward);
        // what the torso turns toward: the head's yaw, or with the wrists (see the top)
        double target = headYaw_;
        if (p_.torsoWrists && n > 0) {
            V3 face = Dir(headYaw_, 0) * 2;
            for (int i = 0; i < n; ++i) face = face + Norm(wrists[i] - Neck());
            face.y = 0;
            if (Len(face) > 0.1) target = YawOf(face);
        }
        if (!init_) {
            torso_ = target, t_ = t, init_ = true;
        } else {
            const double dt = std::clamp(t - t_, 0.0, 0.5);
            t_ = t;
            const double lag = WrapPi(target - torso_) * std::exp(-dt / std::max(1e-3, p_.torsoTau));
            torso_ = WrapPi(target - lag);
        }
        const double max = p_.torsoMaxLag * kDeg;
        torso_ = WrapPi(headYaw_ - std::clamp(WrapPi(headYaw_ - torso_), -max, max));
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

// A hand's axes from the joints that hold still in a thumb-index pinch: the wrist and the middle,
// ring and little knuckles (MediaPipe 0, 9, 13, 17). f: forward, from the wrist to the ring and
// little knuckles; u: out of the back of the hand; r = f x u (toward the little finger on a right
// hand palm away, toward the thumb on a left one: to the right on both). o: their centre. side: 0
// left, 1 right. Built as StereoKit's hand orientation (MIT: xr_backends/extensions/hand_tracking.cpp),
// with the middle knuckle for the index one and the wrist for the metacarpals' roots.
struct Axes {
    V3 o, r, u, f;
    bool ok = false;
};
inline Axes HandAxes(V3 wrist, V3 middle, V3 ring, V3 little, int side) {
    Axes a;
    const V3 fwd = (ring + little) * 0.5 - wrist, across = (little - middle) * (side ? 1.0 : -1.0);
    const V3 back = Cross(across, fwd);
    if (Len(fwd) < 0.02 || Len(across) < 0.005 || Len(back) < 1e-6) return a;  // a collapsed hand
    a.f = Norm(fwd);
    a.u = Norm(back - a.f * Dot(back, a.f));
    a.r = Cross(a.f, a.u);
    a.o = (wrist + middle + ring + little) * 0.25;
    a.ok = true;
    return a;
}
inline V3 ToAxes(const Axes &a, V3 p) {
    const V3 d = p - a.o;
    return {Dot(d, a.r), Dot(d, a.u), Dot(d, a.f)};
}
inline V3 FromAxes(const Axes &a, V3 l) { return a.o + a.r * l.x + a.u * l.y + a.f * l.z; }
// Where the hand points (see "Hand direction" at the top): the axes' forward turned pitch degrees
// toward the back of the hand (negative: toward the palm) and yaw degrees out, away from the thumb.
// StereoKit's is pitch -50, yaw 20 from its own axes (MIT, quat_from_angles(-50, +-20, 0)).
inline V3 HandDir(const Axes &a, int side, double pitch, double yaw) {
    const double p = pitch * kDeg, y = (side ? -yaw : yaw) * kDeg;
    return Norm(a.r * (-std::cos(p) * std::sin(y)) + a.u * std::sin(p) + a.f * (std::cos(p) * std::cos(y)));
}

// A point pinned to a hand's axes (see "Pinch anchor" at the top): Pin it, then At rebuilds it from
// the axes now, weighted against the point as measured (1: pinned, 0: as measured, between: easing
// back). Axes that can't be built leave it as measured.
class Anchor {
public:
    void Pin(const Axes &a, V3 p) {
        pinned_ = a.ok;
        if (a.ok) local_ = ToAxes(a, p);
    }
    void Unpin() { pinned_ = false; }
    bool Pinned() const { return pinned_; }
    V3 At(const Axes &a, V3 measured, double weight) const {
        if (!pinned_ || !a.ok || weight <= 0) return measured;
        return measured + (FromAxes(a, local_) - measured) * std::min(weight, 1.0);
    }

private:
    V3 local_;
    bool pinned_ = false;
};

// One hand at one capture time, in the room.
struct HandFrame {
    double t = 0;            // capture time, s
    bool stereo = false;     // triangulated from two or more cameras
    V3 knuckle;              // K: the index and middle knuckles
    V3 wrist;
    V3 middle, ring, little; // their knuckles (palm: set), for the hand's axes
    bool palm = false;
    V3 palmNormal;           // unit, out of the palm
    V3 elbow;
    double elbowConf = 0;    // 0: the arm model's guess
    bool grip = false;       // a grip (closed hand) is down
    bool near = false;       // near a panel (see "not near" at the top)
    double gap = -1;         // the thumb and index tips apart (m); < 0 unknown
    double gap2 = -1;        // the thumb and middle tips apart (m); < 0 unknown (both: the "ready" test passes)
    double curl = -1;        // how open the hand is (FistCurl): about 1.9 open, under 1.1 a fist; < 0 unknown
};

// HandFrame::curl from MediaPipe's 21 landmarks (any 3D type with operator-, and len its length): a
// finger's tip-to-wrist distance over its knuckle's; the larger of the index's and the mean of the
// middle, ring and little's. A fist has both low; a pinch with the other fingers curled keeps the index
// out (the flags of 2026-10-09..11, 47k hands: the other three alone read under 1.25 in a fifth of the
// pinches with the index out; both under 1.1 in 70% of the frames with the index curled, a fist).
template <class P, class L> double FistCurl(const P *pts, L len) {
    double sum = 0;
    for (int tip : {12, 16, 20}) {
        const double k = len(pts[tip - 3] - pts[0]);
        if (k < 1e-4) return -1;
        sum += len(pts[tip] - pts[0]) / k;
    }
    const double k = len(pts[5] - pts[0]);
    if (k < 1e-4) return -1;
    return std::max(sum / 3, len(pts[8] - pts[0]) / k);
}

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
    if (p.raisedShow > 0) test(r.below <= p.raisedShow, r.below <= std::max(p.raisedShow, p.raisedHide), "raised");
    test(r.ahead >= p.forwardShow, r.ahead >= p.forwardHide, "forward");
    test(r.palm >= p.palmShow, r.palm >= p.palmHide, "palm");
    if (r.forearmUsed) test(r.forearm >= p.forearmShow, r.forearm >= p.forearmHide, "forearm");
    test(r.pitch >= p.pitchMin && r.pitch <= p.pitchMax, r.pitch >= p.pitchMinHide && r.pitch <= p.pitchMaxHide, "aim");
    test(r.yawOff <= p.yawShow, r.yawOff <= p.yawHide, "yaw");
    test(!f.grip, !f.grip, "grip");
    const double gap = f.gap < 0 ? f.gap2 : f.gap2 < 0 ? f.gap : std::min(f.gap, f.gap2);
    if (p.readyShow > 0 && gap >= 0) test(gap <= p.readyShow, gap <= std::max(p.readyShow, p.readyHide), "ready");
    if (p.fistCurl > 0 && f.curl >= 0) test(f.curl >= p.fistCurl, f.curl >= p.fistCurl - 0.05, "fist");
    r.show = show, r.keep = keep;
    return r;
}

// The ray's filter (see the top).
class RayFilter {
public:
    explicit RayFilter(const Params &p = {}) { SetParams(p); }
    void SetParams(const Params &p) {
        p_ = p;
        dir_.Set({held_ ? p.holdMinCutoff : p.minCutoff, p.beta, p.dCutoff});
        origin_.Set({p.originMinCutoff, p.originBeta, p.dCutoff});
        reach_.Set({p.reachMinCutoff, p.reachBeta, p.dCutoff});
    }
    // A ray pinch held: the direction's precision (holdMinCutoff), from now on.
    void SetHeld(bool on) {
        if (on == held_) return;
        held_ = on;
        SetParams(p_);
    }
    void Reset() { dir_.Reset(), origin_.Reset(), reach_.Reset(); }
    // A tracker frame's ray and reach, captured at t, arrived at now.
    void Sample(const Ray &raw, double reach, double t, double now) {
        dir_.Sample(raw.dir, t, now), origin_.Sample(raw.origin, t, now), reach_.Sample(reach, t, now);
    }
    // The ray at now (the filters' timeline moves there).
    Ray At(double now) {
        Ray r;
        r.dir = dir_.At(now), r.origin = origin_.At(now);
        reach_.At(now);
        return r;
    }
    Ray Last() const {
        Ray r;
        r.dir = dir_.Last(), r.origin = origin_.Last();
        return r;
    }
    double Reach() const { return reach_.Last(); }

private:
    Params p_;
    fhsmooth::Direction dir_;
    fhsmooth::Point3 origin_;
    fhsmooth::Scalar reach_;
    bool held_ = false;
};

// One side's ray: prediction, the gate and the filter.
class HandRay {
public:
    explicit HandRay(int side, const Params &p = {}) : side_(side), p_(p), filter_(p) {}
    void SetParams(const Params &p) { p_ = p, filter_.SetParams(p); }
    int Side() const { return side_; }

    // A new frame of this side's hand. sideIsHand: the ray may show (the arbiter says this side is a
    // bare hand, and the caller allows it); without it, a ray shown hides at once ("hide off").
    // now: when it arrived, on the capture times' clock (< 0: at its capture time).
    void Frame(const Body &body, const HandFrame &f, bool sideIsHand, double now = -1) {
        if (now < 0) now = f.t;
        const bool wasShown = shown_;
        // K: pinned to the hand's axes during a pinch (see "Pinch anchor" at the top)
        axes_ = f.palm ? HandAxes(f.wrist, f.middle, f.ring, f.little, side_) : Axes{};
        lastMeasured_ = f.knuckle;
        if (pinAt_) anchor_.Pin(axes_, f.knuckle), pinAt_ = false;
        const double pinned = Steady(f.t);
        const V3 kNow = anchor_.At(axes_, f.knuckle, pinned);
        if (pinned <= 0) anchor_.Unpin();
        // K's room velocity, for the prediction
        V3 vk;
        if (haveLast_ && f.t > lastT_ && f.t - lastT_ < 0.2) vk = (kNow - lastK_) * (1 / (f.t - lastT_));
        lastK_ = kNow, lastT_ = f.t, haveLast_ = true;
        const V3 k = kNow + vk * p_.predict;
        const V3 s = Shoulder(body, f.t);
        raw_.dir = Norm(k - s);
        raw_.origin = k + raw_.dir * p_.originAhead;
        reach_ = Len(kNow - s);
        lastFrame_ = f.t, arrived_ = now;
        if (f.stereo) lastStereo_ = f.t;
        tests_ = Evaluate(p_, body, f, raw_);  // the shoulder's ray: the tests were tuned on it
        // turned toward the hand's own direction (see "Hand direction" at the top)
        if (p_.handWeight > 0 && axes_.ok) {
            const V3 hand = HandDir(axes_, side_, p_.handPitch, p_.handYaw);
            raw_.dir = fhsmooth::Slerp(raw_.dir, hand, std::min(p_.handWeight, 1.0));
            raw_.origin = k + raw_.dir * p_.originAhead;
        }
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
                if (f.t - showSince_ >= p_.showFor - 1e-9) Show();
            } else {
                showSince_ = -1;
            }
        } else {
            if (!sideIsHand) {
                Hide("off", f.t);
            } else if (f.near && !held_) {
                Hide("near", f.t);
            } else if (keepOk || held_ || f.t < holdUntil_) {
                failSince_ = -1;
            } else {
                if (failSince_ < 0) failSince_ = f.t;
                if (f.t - failSince_ >= p_.hideFor - 1e-9) Hide(tests_.fails.empty() ? "pose" : tests_.fails, f.t);
            }
        }
        if (shown_ && wasShown) filter_.Sample(raw_, reach_, f.t, now);  // (Show took this frame already)
    }
    // Each tick, with the time now on the capture clock: the hand lost (no frame for lostFor) hides it.
    void Tick(double now, bool sideIsHand) {
        if (!shown_) return;
        if (!sideIsHand) Hide("off", now);
        else if (!held_ && now - lastFrame_ > p_.lostFor && now >= holdUntil_) Hide("lost", now);
    }
    // A ray pinch is held: the ray stays up until hideFor after it ends, and its filter takes the
    // drag's precision (holdMinCutoff) meanwhile.
    void Hold(bool on, double t) {
        if (held_ && !on) holdUntil_ = t + p_.hideFor;
        if (on && !held_) {
            if (p_.holdShoulder) steady_ = true, steadyAt_ = lastS_;
            // K pinned where it was in the newest frame, against the axes then (or the next frame's)
            if (p_.pinchAnchor) {
                if (axes_.ok) anchor_.Pin(axes_, lastMeasured_);
                else pinAt_ = true;
            }
        }
        held_ = on;
        filter_.SetHeld(on);
    }
    void Reset() { Hide("reset", -1e9); }
    // A pinch began on this hand at t: may its ray show for it (see the top)? within: how long ago
    // it may have hidden.
    bool CanShowForPinch(double t, double within) const {
        return shown_ || showSince_ >= 0 || (revivable_ && t - hidAt_ <= within);
    }
    // Show it (filter started afresh from the hand now), for a pinch that may (CanShowForPinch).
    void ShowForPinch() {
        if (!shown_) Show();
    }

    bool Shown() const { return shown_; }
    Ray Current() const { return filter_.Last(); }   // filtered, where its timeline stands (the last frame or At)
    Ray At(double now) { return filter_.At(now); }  // filtered, at now (between frames: on its way to the newest)
    const Ray &Raw() const { return raw_; }         // unfiltered, predicted
    double Reach() const { return filter_.Reach(); }  // |K - S|, filtered: arm out or in (push and pull)
    double RawReach() const { return reach_; }
    const Tests &LastTests() const { return tests_; }
    const std::string &Why() const { return why_; }   // what fails now ("" if nothing)
    // Changes since the caller last cleared it: "show" or "hide <reason>".
    std::string event;

private:
    // How much of a held pinch's steadiness is left at t: 1 while it's held and hideFor after, then
    // down to 0 over shoulderBlend (see "Steady shoulder" at the top).
    double Steady(double t) const {
        if (held_) return 1;
        return 1 - std::clamp((t - holdUntil_) / std::max(1e-3, p_.shoulderBlend), 0.0, 1.0);
    }
    // The ray's shoulder (see "Steady shoulder" at the top).
    V3 Shoulder(const Body &body, double t) {
        V3 s = body.Shoulder(side_);
        if (steady_) {
            const double w = Steady(t);
            if (w <= 0) steady_ = false;
            else s = steadyAt_ + (s - steadyAt_) * (1 - w);
        }
        lastS_ = s;
        return s;
    }
    void Show() {
        shown_ = true, failSince_ = -1, showSince_ = -1;
        filter_.Reset();
        filter_.Sample(raw_, reach_, lastFrame_, arrived_);
        event = "show";
    }
    void Hide(const std::string &why, double t) {
        if (!shown_) return;
        shown_ = false, failSince_ = -1, showSince_ = -1, held_ = false, holdUntil_ = 0, steady_ = false;
        anchor_.Unpin(), pinAt_ = false;
        filter_.SetHeld(false);
        hidAt_ = t, revivable_ = why != "off" && why != "near" && why != "reset";
        event = "hide " + why;
    }
    int side_;
    Params p_;
    RayFilter filter_;
    Ray raw_;
    Tests tests_;
    std::string why_;
    V3 lastK_, lastS_, steadyAt_, lastMeasured_;
    Axes axes_;
    Anchor anchor_;
    bool pinAt_ = false;  // pin at the next frame (no axes when the pinch began)
    double lastT_ = 0, lastFrame_ = -1e9, lastStereo_ = -1e9, reach_ = 0, arrived_ = 0;
    double showSince_ = -1, failSince_ = -1, holdUntil_ = 0, hidAt_ = -1e9;
    bool haveLast_ = false, shown_ = false, held_ = false, revivable_ = false, steady_ = false;
};

}  // namespace fhray
