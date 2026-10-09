// Gesture detection for input: look at something and pinch to click it (pinch and move to
// nudge the pointer first), or close the hand to press and drag it. Per side, from the
// tracker's hands after each step; published as fh_gestures.h.
#pragma once

#include "tracker.h"

#include <string>
#include <vector>

extern "C" {
#include "../include/fh_gestures.h"
}

struct PinchParams {
    double begin_m = 0.020;   // thumb and index tips closer than this: the pinch begins
    double end_m = 0.035;     // further apart than this: it ends (the gap keeps it from flickering)
    int end_frames = 2;       // processed frames in a row past end_m before it ends, so one
                              // noisy frame doesn't drop a drag
    double grace_s = 0.25;    // a pinching hand lost this long ends its pinch (FH_PINCH_LOST)
    // Where the distance comes from: MediaPipe's world landmarks (the model's own 3D hand
    // pose, averaged over the hand's views, at the user's hand size), or the tracker's
    // triangulated tips. The model's pose should hold up better when the fingers hide each
    // other; tomorrow's recordings will tell.
    bool triangulated = false;
    // Or the fitted skeleton's tips (track/skeleton.h), where the hand has one: for comparing
    // in ft-handreplay (--pinch-fitted); not used live yet.
    bool fitted = false;
    // No pinch begins while the palm faces down more than this (|palm normal . up| in the
    // head frame; 1 turns it off). Typing curls the thumb onto the index: in the 2026-09-30
    // lit recording, pinches that began while typing had 0.69-1.00, deliberate ones 0.00-0.50.
    // Looking down tilts the head frame, which lowers the reading for a hand on a keyboard.
    // Off by default since the first headset test (2026-09-30 14:55): the user's deliberate
    // pinches, hand raised in front, read 0.90-0.99 too. The pointer helper now leaves out
    // gestures that begin low (hands on a desk), which it can tell with the head's pose.
    double palm_down_max = 1.0;
};

class Pinch {
public:
    explicit Pinch(const PinchParams &p = {}) : p_(p) {}
    const PinchParams &params() const { return p_; }
    // After each processed set: the hands out of Tracker::step, the tracker's views (for
    // the world landmarks) and the capture time. Hands in `gripping` (their ids) are closed:
    // no pinch begins on them, and one that's down on them ends, as lost.
    void update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns,
                const std::vector<int> &gripping = {});
    // Ends any pinch that's down (as lost), e.g. when the tracker stops.
    void release(int64_t t_ns);
    const fh_pinch_t &side(int s) const { return side_[s]; }   // 0 left, 1 right
    // A pinch is down or closing: worth tracking at the full rate.
    bool engaged() const;

    // What changed in the last update, for logs.
    struct Event {
        int side;
        const char *what;   // "begin", "end", "lost"
        int64_t t_ns;
        double distance;
        V3 point;
    };
    std::vector<Event> events;
    // Both distance measures for the last update, per side (-1: no hand), for logs.
    double world_d[2] = {-1, -1}, tri_d[2] = {-1, -1}, fit_d[2] = {-1, -1};
    double palm_down[2] = {-1, -1};   // |palm normal . up| of each side's hand
    int held_back[2] = {0, 0};          // pinches that didn't begin because the palm faced down

private:
    void end(int s, int64_t t_ns, bool lost);
    PinchParams p_;
    fh_pinch_t side_[2]{};
    int follow_[2] = {0, 0};        // the hand id a pinch follows while down
    int open_frames_[2] = {0, 0};
    int64_t seen_ns_[2] = {0, 0};
    bool held_[2] = {false, false};  // a close held back (palm down) that hasn't opened yet
};

struct GripParams {
    // How curled a finger is: its tip's distance from the wrist over its knuckle's, from the
    // model's world landmarks (so the hand's size doesn't matter). About 1.8-2.0 straight,
    // 0.8-1.0 curled into a fist.
    double begin = 1.2;       // every finger under this: the grip begins
    double end = 1.45;        // their mean over this: it ends
    int end_frames = 2;       // processed frames in a row past end before it ends
    double grace_s = 0.25;    // a gripping hand lost this long ends its grip (FH_PINCH_LOST)
    // A grip begins only on a hand seen open (mean over end) within this long: closing the
    // hand is the gesture. A hand resting closed (in your lap, on a mouse) never grips.
    double armed_s = 1.0;
    // ...and only in front of you, where you'd hold a hand up to grab something: the palm no
    // more than max_down_deg below straight ahead (head frame) and at least min_ahead_m in
    // front of the eyes. Typing curls the fingers like a loose fist: in the 2026-09-30 lit
    // recording, typing hands sat about 47 degrees down (14 false grips without this),
    // deliberate pinches 5-15.
    double max_down_deg = 35;
    double min_ahead_m = 0.15;
    // ...and not with the thumb on the index fingertip, closer than this (m, the pinch's
    // measure): that's a pinch with the other fingers curled, which the first headset test
    // took for a grip.
    double thumb_off_m = 0.03;
};

// Grip (a closed hand) detection, per side like Pinch: press and drag.
class Grip {
public:
    explicit Grip(const GripParams &p = {}) : p_(p) {}
    const GripParams &params() const { return p_; }
    void update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns);
    void release(int64_t t_ns);
    const fh_pinch_t &side(int s) const { return side_[s]; }
    // The hands gripping now (for Pinch::update).
    std::vector<int> gripping() const;
    bool engaged() const;   // a grip is down or closing: worth tracking at the full rate

    std::vector<Pinch::Event> events;
    double curl[2] = {-1, -1};   // each side's hand: mean curl, for logs

private:
    void end(int s, int64_t t_ns, bool lost);
    GripParams p_;
    fh_pinch_t side_[2]{};
    int follow_[2] = {0, 0};
    int open_frames_[2] = {0, 0};
    int64_t seen_ns_[2] = {0, 0};
    int64_t open_ns_[2] = {0, 0};   // the side's hand was last seen open then
};

// Writes /run/user/UID/frametop-hands/gestures.
class GesturePublisher {
public:
    bool open(const Pinch &pinch, const Grip &grip, std::string &err);
    void write(const Pinch &pinch, const Grip &grip, uint64_t capture_ns);

private:
    fh_gestures_t *out_ = nullptr;
    uint64_t seq_ = 0;
    // the counters last written, per gesture (0 pinch, 1 grip) and side
    uint32_t last_begins_[2][2] = {}, last_ends_[2][2] = {};
};
