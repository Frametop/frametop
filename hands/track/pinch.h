// Gesture detection for input: look at something and pinch to click it (pinch and move to
// nudge the pointer first), or close the hand to press and drag it. Per side, from the
// tracker's hands after each step; published as fh_gestures.h.
#pragma once

#include "tracker.h"

#include <deque>
#include <string>
#include <vector>

extern "C" {
#include "../include/fh_gestures.h"
}

struct PinchParams {
    double begin_m = 0.020;   // thumb and index tips closer than this: the pinch begins
    // further apart than this: it ends (the gap keeps it from flickering). 0.045 since 2026-10-14
    // (0.035 before): taps and pinches let go while still held (user, 2026-10-11, gaze + hands: ends
    // came at 4-6 cm); these four were the headset's HANDS_PINCH_* settings since, now everyone's.
    double end_m = 0.045;
    int end_frames = 2;       // processed frames in a row past end_m before it ends, so one
                              // noisy frame doesn't drop a drag
    double grace_s = 0.25;    // a pinching hand lost this long ends its pinch (FH_PINCH_LOST)
    // A drag: a pinch held drag_s or longer ends only once the tips are drag_open_m apart (for
    // end_frames, as above), or drag_open_s after they first went past end_m, unless they closed
    // again within drag_reclose_m in between. A thumb hidden for a frame or two, or fingers loosened
    // a little mid-drag, read 3.5-4 cm and dropped what was dragged (2026-10-09 headset test: drags
    // ending at 3.5-4.0 cm). Fingers hovering 3-5 cm apart still end it: on the lit recording's
    // replay (pinches held 0.3 s or more, typing among them) the ends came 33-134 ms later, at most
    // 200, where any dip under end_m starting the wait over kept one held 6 s longer. A tap ends as
    // before: its release is the click.
    double drag_s = 0.3, drag_open_m = 0.065, drag_open_s = 0.2, drag_reclose_m = 0.03;   // 0.05, 0.12 before 2026-10-14
    // A grip takes this pinch's hand (ends it, as lost) only within grip_takeover_s of its begin:
    // closing the hand into a fist passes through a pinch. Later the pinch is being held (a drag),
    // and the other fingers curling around it don't end it (3 of 28 pinches ended so, the same test).
    // 0.1 since 2026-10-14 (0.3 before: grips took pinches 130 ms in).
    double grip_takeover_s = 0.1;
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
    // Which fingertip meets the thumb: 8 the index (the pinch), 12 the middle finger (the middle
    // pinch, fh_gestures v3: a right click or scrolling on the pointer helper's hand ray).
    int finger = 8;
    // The other fingertip the thumb could be on (0: none). A pinch begins only with the thumb
    // nearer its own fingertip than this one by at least other_margin (m): the middle pinch's
    // index tip is often within the begin distance too.
    int other_finger = 0;
    double other_margin = 0.0;
    // And only with this finger extended (0: any): its tip at least extended_min times as far from
    // the wrist as its knuckle (about 1.8-2.0 straight, under 1.2 curled). The middle pinch wants the
    // index extended: closing the hand into a grab brings the middle tip to the thumb with the index
    // curled too (6 false middle pinches in the recorder's gestures take, PR #8, all at curls 1.2-1.4).
    int extended_finger = 0;
    double extended_min = 1.5;
    // The fist guard (HANDS_PINCH_FIST_CURL; 0 turns it off): no pinch begins while the index and the
    // middle finger are both curled, each tip nearer the wrist than fist_curl times its knuckle (Grip's
    // curl measure). A fist puts the thumb tip on the curled index: in the held-out flags a median 28.5 mm
    // from it and under 20 mm on 22% of fist frames, and live ft-hands read 96% of fist frames as pinched
    // (frame-hands notes/taps.md, "Fists are partly a gesture-rule problem"). A close held back so has to
    // open past end_m before a pinch can begin: a held fist's curls flicker over the line for a frame now
    // and then. A pinch already down carries on: a drag's other fingers curling around it don't end it.
    // 1.2, not the flag analysis' 1.3 (FIST_CURL, on the teacher's 3D): the model's world landmarks read
    // pinching fingers more curled (index 1.06-1.34 at quick taps). Replays with student v11c against the
    // tap labels (2026-10-11): clear non-fist pinches lost 2 of 204 at 1.2 (4 at 1.3), the labelled fists
    // read as pinches 8 -> 5 at either; typing pinches on the desk recordings 29 -> 22.
    double fist_curl = 1.2;
    // And only on a hand tracked at least this many frames (a hand just found is often misread).
    int min_frames = 0;
    // The dip rule (HANDS_PINCH_DIP=1, off by default): a pinch also begins on a sharp dip that
    // doesn't reach begin_m. Quick taps with the hand raised often read 25-30 mm at contact and
    // spring back (the model doesn't read the touch; frame-hands notes/taps.md, "Dip detector"). It
    // begins when, within dip_win_s, the distance fell by dip_rel of its highest and by dip_drop_m,
    // to under dip_max_m, with the palm moving under dip_speed_max (m/s, over the last 150 ms) and
    // the thumb still dip_other_min_m from the other fingertip (other_finger: a hand closing into a
    // grab brings both in). It ends once the distance is dip_rebound_m over the lowest it read (or
    // past end_m) for end_frames; if it gets under begin_m it is an ordinary pinch from there on
    // (drags and all). Tuned on the probe-moves and lit recordings (frame-hands train/taps/dipsim.py);
    // in the guided tap capture's replay, quick taps at a 2 Hz click 10 -> 25 of 30 caught. The cost
    // is near misses: fingers stopped ~1 cm apart click too (5 -> 16 pinches in that segment).
    bool dip = false;
    double dip_win_s = 0.15, dip_rel = 0.4, dip_drop_m = 0.004, dip_max_m = 0.032;
    double dip_speed_max = 0.3, dip_other_min_m = 0.04, dip_rebound_m = 0.006;
};

class Pinch {
public:
    explicit Pinch(const PinchParams &p = {}) : p_(p) {}
    const PinchParams &params() const { return p_; }
    // After each processed set: the hands out of Tracker::step, the tracker's views (for
    // the world landmarks) and the capture time. Hands in `gripping` (their ids) are closed:
    // no pinch begins on them, and one that's down on them ends, as lost.
    // Hands in `blocked` (their ids: another pinch is down on them) begin no pinch, and one that's
    // down on them carries on.
    void update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns,
                const std::vector<int> &gripping = {}, const std::vector<int> &blocked = {});
    // The hands a pinch is down on now (for another pinch's `blocked`).
    std::vector<int> down() const;
    // Ends any pinch that's down (as lost), e.g. when the tracker stops.
    void release(int64_t t_ns);
    const fh_pinch_t &side(int s) const { return side_[s]; }   // 0 left, 1 right
    // A pinch is down or closing: worth tracking at the full rate.
    bool engaged() const;
    // The hands a pinch has been down on for grip_takeover_s or longer at t_ns (for Grip::update's
    // `blocked`: no grip begins on them).
    std::vector<int> holding(int64_t t_ns) const;

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
    double other_d[2] = {-1, -1};       // the thumb to the other fingertip (other_finger), same measure
    double palm_down[2] = {-1, -1};   // |palm normal . up| of each side's hand
    int held_back[2] = {0, 0};          // pinches that didn't begin because the palm faced down
    int fist_held[2] = {0, 0};          // closes the fist guard held back
    double curl[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};   // each side's hand: the fingers' curls, for logs
    int dip_begins[2] = {0, 0};         // pinches the dip rule began (PinchParams::dip)

private:
    void end(int s, int64_t t_ns, bool lost);
    // the dip rule's recent distances on each side's hand (PinchParams::dip)
    struct Sample {
        int64_t t_ns;
        double d;
        V3 palm;
    };
    bool dip_fires(int s, int64_t t_ns, double d, double other, const V3 &palm) const;
    std::deque<Sample> recent_[2];
    int recent_id_[2] = {0, 0};
    bool dip_[2] = {false, false};      // the pinch down began on a dip and hasn't reached begin_m
    double dip_min_[2] = {1, 1};        // its lowest distance so far
    PinchParams p_;
    fh_pinch_t side_[2]{};
    int follow_[2] = {0, 0};        // the hand id a pinch follows while down
    int open_frames_[2] = {0, 0};
    int64_t open_since_[2] = {0, 0};   // the first frame of this run past end_m
    int64_t seen_ns_[2] = {0, 0};
    bool held_[2] = {false, false};  // a close held back (palm down, a fist) that hasn't opened yet
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
    // blocked: hands no grip begins on (a pinch held on them: Pinch::holding).
    void update(const std::vector<const Hand *> &hands, const std::vector<Seen> &views, int64_t t_ns,
                const std::vector<int> &blocked = {});
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
    // middle: the middle pinches (fh_gestures v3), or none (written as zero).
    bool open(const Pinch &pinch, const Grip &grip, std::string &err);
    // Writes into `mem` instead of the gestures file (ft-handreplay --publish-dump).
    void use(fh_gestures_t *mem, const Pinch &pinch, const Grip &grip);
    void write(const Pinch &pinch, const Grip &grip, uint64_t capture_ns, const Pinch *middle = nullptr);

private:
    fh_gestures_t *out_ = nullptr;
    uint64_t seq_ = 0;
    // the counters last written, per gesture (0 pinch, 1 grip, 2 middle pinch) and side
    uint32_t last_begins_[3][2] = {}, last_ends_[3][2] = {};
};
