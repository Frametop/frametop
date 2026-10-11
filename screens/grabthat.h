// "Grab that" (user, 2026-10-10): hold an open hand out toward a window, palm to it, then pull
// the arm back while making a fist, and the window comes to arm's reach (about 45 cm in front of
// the eyes, at the hand's height, facing you) so the hands can work on it. The same gesture at a
// window that came that way and is still within arm's reach sends it back where it was; at one
// that's within arm's reach without having come that way, it centres it at arm's reach (Decide).
// One at a time: a window that came and is still where it came goes back as another one comes.
//
// With a window that near, the open hand held out to it is in its touch zone (handinput.h): an
// open hand's fingers pointing up don't poke it, and its fist waits on this gesture before it
// grabs the window (handinput's Params::fistGrab): the fist pulled back and turned over is this,
// a fist made at the window without that is a fist grab.
//
// The gesture, from the user's own example (frame-hands captures/flags/flag-20261010-150115, the
// right hand, done twice; head frame at capture time):
//   open      the four fingers' tips from the wrist over their knuckles' (as handinput's
//             extension), averaged: 1.7-1.8 open, 0.75-0.8 the fist
//   palmFwd   the palm's normal (out of the palm, palmmenu's PalmNormal) along the head's forward:
//             +0.6..+0.72 held out to the window, -0.3..-0.9 once pulled in
//   fwd       the wrist ahead of the eyes along the head's forward: 0.30-0.42 m out, 0.19-0.25 in
// Held out 0.4-1.4 s; then within 0.2 s: the fingers close (open 1.7 -> 0.8 in 0.1 s; the palm
// still +0.4..+0.6 as they begin), the wrist comes back 9-11 cm and the palm turns over.
//
// The detector, per side, on each hands frame (two-camera frames only: one camera's depth is
// 2-6 cm off):
//   Idle -> Opening   held out: open >= openArm, palmFwd >= palmArm, fwd >= fwdArm
//   Opening -> Armed  ...for armMs (with the keep thresholds meanwhile, else back to Idle)
//   Armed             each frame within the keep thresholds marks the hand open (and where the
//                     palm is then, the aim); back to Idle closeMs after the last such frame
//   Armed -> Closing  the middle, ring and little fingers curl under claimCurl, with the palm still
//                     toward the window (palmFwd >= closePalm on some frame in the last closePalmMs):
//                     the hand closes on it. Turned over first, then closed, it's not this (Idle)
//   Closing -> fire   within closeMs of the last open frame: a fist (open <= fist), the wrist back
//                     at least `pull` from its farthest out in the last pullMs, the palm turned
//                     away from the window (palmFwd <= palmFlip); all on one frame
//   Closing -> Armed  the hand opens out again; -> Idle after closeMs
// After firing, no new one for againMs. A wave or a push never makes a fist, a fist made without
// pulling back never comes back far enough, a pinch keeps the other fingers open, and typing or a
// hand in the lap is never held out with the palm forward.
//
// The claim: in Closing, on frames where the thumb tip is claimThumb or more from the index tip,
// and for claimMs after a fire, the hand is ours. vr.cpp reports it near ("near ... <L> <R>" to the
// pointer helper, as the palm menu does), so the grip ft-hands sees as the fist closes isn't a
// gaze grip (a press and drag at the pointer). ft-hands' grip begins with every finger under 1.2,
// after the three fingers' mean is under claimCurl (1.5), and with the thumb 3 cm or more from the
// index tip; a pinch begins under 2 cm, so a pinch made from a hand held out (a ray click with the
// other fingers curled) stays the helper's. On recordings without the gesture (probe-moves, the
// 2026-09-29 and lit recordings, the PR #3 takes: 15 minutes), 1 of 143 pinches and 3 of 12 grips
// began on a claimed hand (11 pinches without the thumb rule), and nothing fired (one did before
// the palm rule above: an open palm turned over, then closed and pulled in); nor in the user's
// other 12 flags (3 minutes). scripts/test-handinput.sh replays the recordings, handinput-test
// --grabthat FLAG_DIR... the flags.
//
// Pure logic, like handinput.cpp: no OpenVR or GL calls (tests: screens/tests/handinput_test.cpp,
// which replays the user's flag). vr.cpp picks the window (Pick), moves it (Arrival, Blend) and
// remembers where it came from.
#pragma once

#include "handcut.h"

#include <cstdint>
#include <string>
#include <vector>

namespace grabthat {

using Mat = handcut::Mat;

struct Params {
    bool on = true;                      // HANDS_GRAB_THAT
    double openArm = 1.55, openKeep = 1.45;
    double palmArm = 0.45, palmKeep = 0.30;
    double fwdArm = 0.28, fwdKeep = 0.24;   // m
    double armMs = 200;
    double closeMs = 450;
    double claimCurl = 1.5;              // the middle, ring and little fingers' mean extension
    double claimThumb = 0.025;           // ...with the thumb tip this far (m) from the index tip: not a pinch
    double closePalm = 0.2, closePalmMs = 100;
    double fist = 1.0;
    double pull = 0.08, pullMs = 500;    // m, within ms
    double palmFlip = 0.0;
    double claimMs = 400;
    double againMs = 1000;
    double lostMs = 250;
    // Where it goes: distance m ahead of the eyes across the floor (the head's heading), its centre
    // at the fist's height, at most `below` under the eyes and `above` over them, facing the eyes.
    // A screen wider than maxWidth comes at that width (0: as it is); floating windows keep their
    // size (ft-floatd owns their density). HANDS_GRAB_THAT_DISTANCE, HANDS_GRAB_THAT_WIDTH.
    double distance = 0.45, below = 0.45, above = 0.10, maxWidth = 0.6;
    // Within arm's reach: its centre within distance + reachSlack of the eyes (Decide).
    double reachSlack = 0.35;
    double animMs = 250;
    // Which window: the one the line from the eyes through the open palm meets (within aimMargin
    // of its edges, the nearest), else the one nearest the head's forward direction within headCone.
    double aimMargin = 0.10, headCone = 20;   // m, degrees
};

// What a hand frame shows of the gesture (head frame at capture time).
struct Features {
    double open = 0, curl3 = 0, palmFwd = 0, fwd = 0;
};
Features Measure(const handcut::Hand &h, const Mat &head);

// A fired gesture: the side, the capture time, and in the room: the eyes and the palm's centre at
// the last open frame (the aim), the fist's palm centre now, and the head now.
struct Fire {
    int side = 0;
    int64_t t = 0;
    double eye[3] = {0, 0, 0}, palm[3] = {0, 0, 0}, fist[3] = {0, 0, 0};
    Mat head{};
};

class Detector {
public:
    explicit Detector(const Params &p = {}) : p_(p) {}
    Params &params() { return p_; }
    const Params &params() const { return p_; }
    // Each new hands frame: the hands as the cameras saw them (room), the head pose then, and per
    // side whether a bare hand may do this now (allowed, not busy with near input or the palm menu).
    void Frame(int64_t captureNs, const std::vector<handcut::Hand> &hands, const Mat &head, const bool allowed[2]);
    // What fired in the last Frame.
    const std::vector<Fire> &fired() const { return fired_; }
    // The hand is ours on the frame captured at t (see the top).
    bool claims(int side, int64_t t) const;
    // Held out and armed, or closing: ft-hands should track at its full rate.
    bool armed(int side) const { return side_[side].state == State::Armed || side_[side].state == State::Closing; }
    void Reset();
    std::string Describe() const;   // "left idle, right armed"

    struct Stats {
        int arms = 0, closings = 0, fires = 0, timeouts = 0;
    } stats;

private:
    enum class State { Idle, Opening, Armed, Closing };
    struct Side {
        State state = State::Idle;
        uint32_t handId = 0;
        int64_t since = 0, lastSeen = 0, lastOpen = 0, quietUntil = 0, claimUntil = 0;
        double eye[3] = {0, 0, 0}, palm[3] = {0, 0, 0};
        double thumb = 0;             // thumb tip to index tip now (m)
        struct Sample { int64_t t; double fwd, palm; };
        std::vector<Sample> recent;   // the wrist's fwd and the palm's palmFwd over the last pullMs
    };
    void Open(Side &sd, int64_t t, const handcut::Hand &h, const Mat &head);
    Params p_;
    Side side_[2];
    std::vector<Fire> fired_;
};

// A panel the gesture can take.
struct Target {
    int index = -1;
    Mat pose{};
    double width = 1, height = 1, curve = 0;
};
// Where the ray from `from` along `dir` (unit) meets the panel's surface (flat, or curved as
// handinput's Locate): u, v (m from its centre) and the distance along the ray. False if it
// doesn't, in front of `from`.
bool RayHit(const Target &p, const double from[3], const double dir[3], double *u, double *v, double *t);
// The panel a fired gesture means (see Params::aimMargin), or -1.
int Pick(const std::vector<Target> &panels, const Fire &f, const Params &k);
// Where a panel comes to (see Params::distance), facing the eyes.
Mat Arrival(const Fire &f, const Params &k);
// What the gesture does to the panel it picked: it came by the gesture (`came`) and is within arm's
// reach now (Params::reachSlack): Back where it was; within arm's reach, not come by it: Centre it
// at arm's reach (and remember where it was, so the gesture again sends it back); else Bring it.
enum class Outcome { Bring, Back, Centre };
Outcome Decide(bool came, const Mat &now, const Mat &head, const Params &k);
const char *OutcomeName(Outcome o);
// From a to b by k (0..1): the position along the line, the turn along the shortest arc.
Mat Blend(const Mat &a, const Mat &b, double k);
// Slow in, slow out (0..1).
double Ease(double t);

}  // namespace grabthat
