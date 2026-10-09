// Gaze + hands: with gaze mode and POINTER_HANDS both on, the eyes aim and the hands click (Vision
// Pro style), and a pinch held and moved nudges the pointer with low sensitivity. The pointer helper's
// side of "Gaze pinch" in ft-pointer.cpp. Header-only and free of OpenVR, so
// pointer/tests/gazehand-test.cpp can drive it with made-up gaze and hand samples.
//
// Ray: in gaze mode the hand ray stays off (POINTER_GAZE_HAND_RAY=0, the default), so every pinch is
// a gaze pinch; 1 keeps the ray beside the gaze (RayAllowed).
//
// Gaze lock: where a pinch clicks. ft-hands sees a pinch once the tips are within begin_m, and by the
// time it's read (its latency, and up to 60 ms waiting for ft-screens' near report) the eyes may have
// moved on: to the next target, or down to the hand. So the click goes where you looked just before
// the fingers began to close: the newest gaze sample that came at least lead (75 ms) before the
// begin's capture time. ft-gazed's samples come with the eye tracker's own delay on top, so the look
// itself is a little older still, and its fixation lock (the running mean of the fixation) holds
// steady through a fixation, so a sample anywhere in it will do. None within stale (0.15 s) before
// that (a blink, the service stopped): the oldest one between then and the begin; none at all: no
// lock, and the pointer stays where the gaze last put it.
//
// Nudge: the pinch point's direction from the eye (in the room, from where the eye was at the
// begin), against where it was at the begin. Nothing moves until it has moved deadzone (4 deg,
// POINTER_GAZE_PINCH_DEADZONE; about 3 cm at arm's length): a tap's jitter, the pinch point shifting
// as the fingers close, and a hand drifting while you look don't nudge. Past it, the pointer follows
// at gain (0.3, POINTER_GAZE_PINCH_GAIN) times the hand's angle from that point on, so it doesn't
// jump when the nudge engages.
//
// Hold: a pinch held inside the dead zone for hold (POINTER_GAZE_HOLD, 0.5 s) becomes a real press
// at the locked point, and the hand drags from there: past dragDeadzone (0.5 deg) at dragGain
// (POINTER_GRIP_GAIN, 1). A pinch that has nudged only clicks, as the mouse's held-back press does
// once it has moved. A middle pinch (the right button) never becomes a press.
//
// Times are seconds on one steady clock (CLOCK_MONOTONIC: ft-hands' capture times as seconds).
// Angles are degrees: the gaze's head-relative as ft-gazed sends it (yaw +left, pitch +up), the
// pointer's and the hand's in the room.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

namespace gazehand {

struct Params {
    double deadzone = 4.0;      // POINTER_GAZE_PINCH_DEADZONE: hand degrees before a nudge
    double gain = 0.3;          // POINTER_GAZE_PINCH_GAIN: pointer degrees per hand degree
    double hold = 0.5;          // POINTER_GAZE_HOLD: held still this long, a real press
    double dragDeadzone = 0.5;  // after the press (the grip's)
    double dragGain = 1.0;      // POINTER_GRIP_GAIN
    double lead = 0.075;        // gaze lock: the newest sample at least this long before the begin
    double stale = 0.15;        // ... and at most this much older than that
};

// The hand ray may show: with gaze mode on, only with POINTER_GAZE_HAND_RAY=1.
inline bool RayAllowed(bool rayOn, bool gazeOn, bool gazeHandRay) { return rayOn && (!gazeOn || gazeHandRay); }

// Why a gaze pinch doesn't begin, for its log line, or nullptr. The helper has already checked
// what holds for every gesture (hands allowed, a controller on that side, ft-screens' near claims).
struct Situation {
    bool busy = false;        // a press or a hold is already going (the mouse's, a key's, a hand's)
    bool active = true;       // the pointer is awake (the first gesture only wakes it)
    double sinceKey = 1e9;    // seconds since the relay's last "typing"
    double typingHold = 1.0;  // POINTER_PINCH_TYPING
};
inline const char *Refused(const Situation &s) {
    if (s.busy) return "ignored (a press is already held)";
    if (!s.active) return "it woke the pointer, no click";
    if (s.sinceKey < s.typingHold) return "ignored (typing)";
    return nullptr;
}

// One gaze sample as it came from ft-gazed: corrected and raw, head-relative; t is when it came.
struct GazeSample {
    double t = 0, hy = 0, hp = 0, rhy = 0, rhp = 0;
};

class GazeHistory {
public:
    void Add(const GazeSample &s) {
        h_.push_back(s);
        while (!h_.empty() && h_.front().t < s.t - 1.0) h_.pop_front();
    }
    // The gaze a pinch that began at tBegin clicks at (see "Gaze lock" at the top). age: how long
    // before the begin it came.
    bool Lock(double tBegin, const Params &p, GazeSample &out, double &age) const {
        const double want = tBegin - p.lead;
        const GazeSample *pick = nullptr;
        for (auto it = h_.rbegin(); it != h_.rend(); ++it) {
            if (it->t > want + 1e-9) continue;
            if (it->t >= want - p.stale - 1e-9) pick = &*it;
            break;
        }
        if (!pick)
            for (const GazeSample &s : h_)
                if (s.t >= want - p.stale - 1e-9 && s.t <= tBegin + 1e-9) {
                    pick = &s;
                    break;
                }
        if (!pick) return false;
        out = *pick, age = tBegin - pick->t;
        return true;
    }
    void Clear() { h_.clear(); }

private:
    std::deque<GazeSample> h_;
};

// One gaze pinch, from its begin to its end.
class Pinch {
public:
    enum class Phase { Off, Aim, Nudge, Press };

    // A pinch began at t: the pointer locked at (yaw, pitch), the pinch point seen from the eye at
    // (hy, hp). right: a middle pinch. lockAge: the gaze lock's age (s), < 0 without one.
    void Begin(const Params &p, double t, double yaw, double pitch, double hy, double hp, bool right, double lockAge) {
        *this = Pinch();
        p_ = p, phase_ = Phase::Aim, right_ = right, t0_ = t, lockAge_ = lockAge;
        yaw_ = yaw, pitch_ = pitch;
        refY_ = lastHy_ = hy, refP_ = lastHp_ = hp;
    }
    bool On() const { return phase_ != Phase::Off; }
    Phase State() const { return phase_; }
    bool Right() const { return right_; }
    double Yaw() const { return yaw_; }
    double Pitch() const { return pitch_; }
    double Moved() const { return moved_; }      // the nudge's path (deg), before any press
    double Dragged() const { return dragged_; }  // the drag's path (deg), after the press
    double LockAge() const { return lockAge_; }
    double Began() const { return t0_; }
    std::string pressHit;  // what the press landed on (the helper's, for the log)

    // A hands frame: where the pinch point is now, seen from the eye at the begin. True when the
    // pointer moved (Yaw, Pitch).
    bool Hand(double hy, double hp) {
        if (phase_ == Phase::Off) return false;
        lastHy_ = hy, lastHp_ = hp;
        const bool press = phase_ == Phase::Press;
        const double dy = std::remainder(hy - refY_, 360.0), dp = hp - refP_;
        if (!engaged_) {
            if (std::hypot(dy, dp) <= (press ? p_.dragDeadzone : p_.deadzone)) return false;
            engaged_ = true;
            refY_ = hy, refP_ = hp;  // from here, so the pointer doesn't jump
            startY_ = yaw_, startP_ = pitch_;
            if (!press) phase_ = Phase::Nudge;
            return false;
        }
        const double gain = press ? p_.dragGain : p_.gain;
        const double ny = std::remainder(startY_ + gain * dy, 360.0);
        const double np = std::clamp(startP_ + gain * dp, -85.0, 85.0);
        const double step = std::hypot(std::remainder(ny - yaw_, 360.0), np - pitch_);
        if (step <= 0) return false;
        (press ? dragged_ : moved_) += step;
        yaw_ = ny, pitch_ = np;
        return true;
    }

    // Each frame: true once, when a pinch held inside the dead zone for hold becomes a real press.
    bool Promote(double t) {
        if (phase_ != Phase::Aim || right_ || t - t0_ < p_.hold - 1e-9) return false;
        phase_ = Phase::Press;
        engaged_ = false;
        refY_ = lastHy_, refP_ = lastHp_;  // the drag counts from the hand now
        pressAt_ = t;
        return true;
    }

    // What it did, for its log line ("pinch right: " + this). lost: it ended without a click (why
    // says what); hit: what the click landed on; lesson: what the helper sent the gaze service for
    // it ("a lesson", "the quick check", or "" for nothing).
    std::string Says(double t, bool lost, const std::string &why, const std::string &hit, const std::string &lesson) const {
        char b[320];
        const std::string on = hit.empty() ? "nothing" : hit;
        char gaze[48] = "no fresh gaze: where the pointer was";
        if (lockAge_ >= 0) std::snprintf(gaze, sizeof gaze, "gaze %.0f ms before", lockAge_ * 1000);
        const double held = t - t0_;
        const char *button = right_ ? "right click" : "clicked";
        if (phase_ == Phase::Press) {
            const std::string pressed = pressHit.empty() ? "nothing" : pressHit;
            char drag[40] = "";
            if (dragged_ > 0) std::snprintf(drag, sizeof drag, "dragged %.1f deg, ", dragged_);
            std::snprintf(b, sizeof b, "became a press on %s (held still %.2f s; %s), %slet go%s%s (held %.2f s)",
                          pressed.c_str(), pressAt_ - t0_, gaze, drag, lost ? ": " : "", lost ? why.c_str() : "", held);
        } else if (lost) {
            std::snprintf(b, sizeof b, "no click: %s (%s, held %.2f s)", why.c_str(), gaze, held);
        } else if (moved_ < 0.2) {
            std::snprintf(b, sizeof b, "%s at the gaze on %s (%s, held %.2f s)", button, on.c_str(), gaze, held);
        } else {
            std::snprintf(b, sizeof b, "nudged %.1f deg, then %s on %s (%s; %s, held %.2f s)", moved_,
                          right_ ? "a right click" : "clicked", on.c_str(),
                          lesson.empty() ? "not learned" : lesson.c_str(), gaze, held);
        }
        return b;
    }

private:
    Params p_;
    Phase phase_ = Phase::Off;
    bool right_ = false, engaged_ = false;
    double t0_ = 0, pressAt_ = 0, lockAge_ = -1;
    double yaw_ = 0, pitch_ = 0, startY_ = 0, startP_ = 0;
    double refY_ = 0, refP_ = 0, lastHy_ = 0, lastHp_ = 0;
    double moved_ = 0, dragged_ = 0;
};

}  // namespace gazehand
