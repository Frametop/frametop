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
// Chord: both buttons held, as with both of a mouse's (user, 2026-10-10). While one hand's index
// pinch is held (held back, nudged, or pressed), an index pinch by the other hand presses the right
// button, and holds it until that pinch ends; the first hand's left button stays down until its own
// pinch ends. A first pinch still held back becomes a real left press at once, where the pointer is
// (the lock, or where its nudge took it: a nudge is then a lesson, as for a click), without waiting
// out the hold, so the chord is always left and right down together. The right press goes out 30 ms
// after a left press the chord made (ChordButton), so the left lands first. Both press where the
// pointer is, not where the eyes are now. The first hand drags while it's held; if it lets go first,
// the second hand drags from there (Take). A quick second pinch is a right click during the left's
// press (released 40 ms on). A chord's hand lost (or the chord cut short) releases its button. A
// middle pinch stays a right click on its own; one by the other hand during a pinch is ignored, as is
// a third pinch. The typing guard holds for the chord's pinch like any other.
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

    // The chord (see the top), on the left's pinch: the other hand pinched. Held back (aiming or
    // nudged), it becomes a real press now, where the pointer is, and the hand drags from there; true
    // when it did (the helper presses the left button, then the right).
    bool Chord(double t) {
        if (phase_ == Phase::Off || right_) return false;
        ++chords_;
        if (phase_ == Phase::Press) return false;
        phase_ = Phase::Press;
        engaged_ = false;
        refY_ = lastHy_, refP_ = lastHp_;
        pressAt_ = t, byChord_ = true;
        return true;
    }
    // The chord's own pinch: the right button, pressed where the pointer is (no lock, no nudge, no
    // hold). Its hand is only followed (Track) while the left's pinch drags; once that lets go, it
    // drags from where the pointer is (Take, then Hand).
    void BeginChord(const Params &p, double t, double yaw, double pitch, double hy, double hp) {
        Begin(p, t, yaw, pitch, hy, hp, true, -1);
        phase_ = Phase::Press, chord_ = true, pressAt_ = t;
    }
    void Track(double hy, double hp) {
        if (phase_ != Phase::Off) lastHy_ = hy, lastHp_ = hp;
    }
    void Take(double yaw, double pitch) {
        yaw_ = yaw, pitch_ = pitch;
        engaged_ = false;
        refY_ = lastHy_, refP_ = lastHp_;
    }
    bool IsChord() const { return chord_; }  // the chord's own pinch (the right button)
    bool ByChord() const { return byChord_; }  // the left's, pressed by a chord rather than the hold
    int ChordCount() const { return chords_; }  // the other hand's right presses during it

    // What it did, for its log line ("pinch right: " + this). lost: it ended without a click (why
    // says what); hit: what the click landed on; lesson: what the helper sent the gaze service for
    // it ("a lesson", "the quick check", or "" for nothing).
    std::string Says(double t, bool lost, const std::string &why, const std::string &hit, const std::string &lesson) const {
        char b[400];
        const std::string on = hit.empty() ? "nothing" : hit;
        char gaze[48] = "no fresh gaze: where the pointer was";
        if (lockAge_ >= 0) std::snprintf(gaze, sizeof gaze, "gaze %.0f ms before", lockAge_ * 1000);
        const double held = t - t0_;
        const char *button = right_ ? "right click" : "clicked";
        char drag[40] = "";
        if (dragged_ > 0) std::snprintf(drag, sizeof drag, "dragged %.1f deg, ", dragged_);
        if (chord_) {
            const std::string pressed = pressHit.empty() ? "nothing" : pressHit;
            std::snprintf(b, sizeof b, "the right button with the other hand's press, on %s, %slet go%s%s (held %.2f s)",
                          pressed.c_str(), drag, lost ? ": " : "", lost ? why.c_str() : "", held);
        } else if (phase_ == Phase::Press) {
            const std::string pressed = pressHit.empty() ? "nothing" : pressHit;
            char how[160];
            if (!byChord_) {
                std::snprintf(how, sizeof how, "held still %.2f s; %s", pressAt_ - t0_, gaze);
            } else if (moved_ >= 0.2) {
                std::snprintf(how, sizeof how, "the other hand pinched after %.2f s, nudged %.1f deg; %s; %s",
                              pressAt_ - t0_, moved_, lesson.empty() ? "not learned" : lesson.c_str(), gaze);
            } else {
                std::snprintf(how, sizeof how, "the other hand pinched after %.2f s; %s", pressAt_ - t0_, gaze);
            }
            char chords[48] = "";
            if (chords_ == 1) std::snprintf(chords, sizeof chords, ", with the right button");
            else if (chords_ > 1) std::snprintf(chords, sizeof chords, ", with the right button %d times", chords_);
            std::snprintf(b, sizeof b, "became a press on %s (%s), %slet go%s%s (held %.2f s%s)", pressed.c_str(), how,
                          drag, lost ? ": " : "", lost ? why.c_str() : "", held, chords);
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
    bool right_ = false, engaged_ = false, chord_ = false, byChord_ = false;
    int chords_ = 0;
    double t0_ = 0, pressAt_ = 0, lockAge_ = -1;
    double yaw_ = 0, pitch_ = 0, startY_ = 0, startP_ = 0;
    double refY_ = 0, refP_ = 0, lastHy_ = 0, lastHp_ = 0;
    double moved_ = 0, dragged_ = 0;
};

// A pinch that began makes a chord (see the top): an index pinch (not middle) by the other hand
// while held, the hand's gaze pinch on heldSide, is an index pinch, and no chord is held yet.
inline bool Chordable(const Pinch &held, int heldSide, int side, bool middle, bool chordHeld) {
    return held.On() && !held.Right() && !held.IsChord() && heldSide >= 0 && side != heldSide && !middle &&
           !chordHeld;
}

// The chord's right button as the helper sends it (see the top): pressed once due (30 ms after a left
// press the chord made, else at once), held until its pinch ends; a pinch that ended before its press
// went out still clicks (released 40 ms on), and one lost before it presses nothing.
class ChordButton {
public:
    void Begin(double t, bool afterLeft) {
        *this = ChordButton();
        on_ = true, dueAt_ = t + (afterLeft ? 0.03 : 0);
    }
    bool On() const { return on_; }
    bool Down() const { return down_; }
    bool Finished() const { return on_ && done_; }  // over: its log line, then Reset
    bool Pressed() const { return pressed_; }      // its press went out (at some point)
    void Reset() { *this = ChordButton(); }
    // Each frame: 1 press it now, -1 release it now, 0 nothing.
    int Tick(double t) {
        if (!on_ || done_) return 0;
        if (!down_ && t >= dueAt_ - 1e-9) {
            down_ = pressed_ = true, downAt_ = t;
            return 1;
        }
        if (down_ && ended_ && t >= downAt_ + 0.04 - 1e-9) {
            down_ = false, done_ = true;
            return -1;
        }
        return 0;
    }
    // Its pinch ended (lost: the hand was lost, or the chord was cut short): -1 release it now, or 0.
    int End(bool lost) {
        if (!on_ || done_) return 0;
        if (down_ && (lost || !ended_)) {
            down_ = false, done_ = true;
            return -1;
        }
        if (lost) done_ = true;  // never pressed: nothing to release
        else ended_ = true;      // a tap: its press, then the release 40 ms on (Tick)
        return 0;
    }

private:
    bool on_ = false, down_ = false, pressed_ = false, ended_ = false, done_ = false;
    double dueAt_ = 0, downAt_ = 0;
};

}  // namespace gazehand
