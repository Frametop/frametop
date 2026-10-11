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
    // (0.45/0.30 until 2026-10-11; user: "you have to uncomfortably turn your wrist to have your palm
    // facing it; it should look like you're just reaching out to the display": a hand reaching out
    // naturally has its palm half down, 0.2-0.4 forward)
    double palmArm = 0.2, palmKeep = 0.1;
    double fwdArm = 0.28, fwdKeep = 0.24;   // m
    double armMs = 200;
    double closeMs = 450;
    double claimCurl = 1.5;              // the middle, ring and little fingers' mean extension
    double claimThumb = 0.025;           // ...with the thumb tip this far (m) from the index tip: not a pinch
    double closePalm = 0.05, closePalmMs = 100;
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
    // of its edges), the nearest; else the one whose outline the line passes nearest, within
    // aimMarginDeg seen from the eyes; with headCone > 0, else the one nearest the head's forward
    // direction within it. The fourth live test (user, 2026-10-10: "make the trigger more precise")
    // made the line meet the panel itself (0, 0 and no head cone; 0.10 m, 3 deg and 20 deg before),
    // and the fifth found that too strict (user: "it couldn't be pulled back in"): the open palm is
    // held out below the line of sight, not over the window, so the line from the eyes through it
    // runs 12-15 degrees under where the head points (the user's flag 150115) and passes just under
    // a panel the user looks at the middle of (one thrown 3 m out, 1.35 m tall, is 12.7 degrees
    // each way). 6 degrees takes those; a line that meets a panel still beats one that only passes
    // near another. HANDS_GRAB_THAT_MARGIN (m), HANDS_GRAB_THAT_MARGIN_DEG.
    double smallDeg = 10;   // a panel spanning under this (half, deg) gets that much more margin (HANDS_GRAB_THAT_SMALL)
    double aimMargin = 0, aimMarginDeg = 10, headCone = 0;   // m, degrees, degrees (6 deg until 2026-10-11: "increase the size of the target")
    // ...and only a panel farther than `nearest` from the eyes (its centre): one at arm's reach is
    // the hands' already (user: "only activate if the window is not already held and closed up";
    // push, flick or throw it to send it away). 0: any, and the gesture at a near one sends it back
    // or centres it (Decide; before the fourth live test). HANDS_GRAB_THAT_FROM.
    double nearest = 0.8;
    // Precision: the arming hold (armMs) with the palm facing along the aim (its normal within aimFacing
    // degrees of the line from the eyes through it, on average over the hold) and still (the palm's way
    // over the hold under holdSpeed m/s: an open hand swept at a panel isn't held out to it); else the
    // hold starts again. The user's own example (flag 150115) holds 41 and 51 deg off, at 0.25 and 0.46
    // m/s: 55 and 0.5 keep both its fires; tighter loses the first. 0: off. HANDS_GRAB_THAT_FACING.
    double aimFacing = 80, holdSpeed = 0.5;   // (55 until 2026-10-11: the natural reach, as palmArm)
    // After a panel was sent away (a push, a flick or a throw), no gesture for this long: a push's open
    // hand held out, then pulled back closing, is the gesture's shape. (In the fourth live test every
    // fire came after a send-away, but the view recording shows the user reaching for the far panel
    // 1.5 s or more after it went: deliberate. This only keeps the push's own pull-back from firing.)
    double quietAfterSendMs = 1000;
};

// What a hand frame shows of the gesture (head frame at capture time).
// aim: the palm's normal along the line from the eyes through the palm (1: facing straight along it).
struct Features {
    double open = 0, curl3 = 0, palmFwd = 0, fwd = 0, aim = 0;
};
Features Measure(const handcut::Hand &h, const Mat &head);

// A fired gesture: the side, the capture time, and in the room: the eyes and the palm's centre at
// the last open frame (the aim), the fist's palm centre now, and the head now.
struct Fire {
    int side = 0;
    int64_t t = 0;
    double eye[3] = {0, 0, 0}, palm[3] = {0, 0, 0}, fist[3] = {0, 0, 0};
    double normal[3] = {0, 0, 0};   // the palm's normal at the last open frame (out of the palm)
    Mat head{};
    // how it was held out (for the log and the flag replays): the palm's facing off the aim on average
    // (degrees) and its speed (its way over the hold, m/s) over the arming hold
    double facing = 0, speed = 0;
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
    // The gesture under way on this side, for the target's glow (user, 2026-10-10: "a glow over the
    // window grab that is targeting"): the aim it would fire with now (Pick takes it), and how far
    // along it is: rising 0..1 over armMs as the hand is held out, 1 armed or closing. False idle.
    bool Aim(int side, Fire *out, double *progress) const;
    // A panel was sent away: no gesture on either side until `until` (Params::quietAfterSendMs).
    void Quiet(int64_t until);
    void Reset();
    std::string Describe() const;   // "left idle, right armed"

    struct Stats {
        int arms = 0, closings = 0, fires = 0, timeouts = 0, unsteady = 0;   // unsteady: holds that started again (facing, stillness)
    } stats;

private:
    enum class State { Idle, Opening, Armed, Closing };
    struct Side {
        State state = State::Idle;
        uint32_t handId = 0;
        int64_t since = 0, lastSeen = 0, lastOpen = 0, quietUntil = 0, claimUntil = 0;
        double eye[3] = {0, 0, 0}, palm[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
        Mat head{};                   // the head on the last frame
        double thumb = 0;             // thumb tip to index tip now (m)
        double holdFrom[3] = {0, 0, 0};   // the arming hold: where the palm was as it began,
        double facingSum = 0;             // ...its facing off the aim summed over its frames
        int facingN = 0;
        double facing = 0, speed = 0;     // the last hold that armed: its mean facing, the palm's speed over it (Fire's)
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
// Panels nearer the eyes than Params::nearest aren't taken.
// closest (optional): of those it could take, the one whose outline the line passes nearest, and
// how far off (degrees seen from the eyes; 0 on it): for the log when nothing is taken.
struct Closest {
    int index = -1;
    double deg = 180;
};
int Pick(const std::vector<Target> &panels, const Fire &f, const Params &k, Closest *closest = nullptr);
// How far (degrees, seen from `from`) the line along `dir` (unit) passes off the panel's outline
// (its edges, along the curve on a curved one); for a line that meets the panel, its nearest edge.
double OffDeg(const Target &p, const double from[3], const double dir[3]);
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

// A pose `distance` m straight ahead of the head (level), `drop` m under the eyes, facing them.
Mat InFront(const Mat &head, double distance, double drop);
// How far out a panel `width` m wide goes so it spans at most `maxDeg` of the view: at least `nearest`,
// at most `farthest`; at `farthest` and still wider, *fit is the width that spans maxDeg there (else width).
double ViewDistance(double width, double maxDeg, double nearest, double farthest, double *fit);

// How big a panel shows to be readable (user, 2026-10-11: "always appear about 1 m in front of the user,
// and it should always be large enough that the text is readable ... based on the display's resolution,
// correcting for resolution and aspect ratio"): its logical pixels (px / its UI scale) at ppd pixels per
// degree of view, shrunk (keeping its shape) to at most maxW wide and maxH tall, grown to at least
// minW wide; at `distance`, flat up to flatDeg wide, wider ones curved round the eyes (radius distance).
struct ReadParams {
    double ppd = 26, maxW = 100, maxH = 55, minW = 40, flatDeg = 60;
};
struct Readable {
    double width = 0, curve = 0;   // metres (along the arc when curved); curve: radius, 0 flat
    double degW = 0, degH = 0;     // the view it spans
    double ppd = 0;                // its pixels per degree as shown (over ReadParams::ppd: text smaller than asked)
};
Readable ReadableSize(double pxW, double pxH, double distance, const ReadParams &k);

// The lazy susan (user, 2026-10-11: "rather than just spinning your perspective, it pulls the window in
// front of you"): the panels by bearing from where you face (degrees, right positive, -180..180, where
// each one lives, not where it was brought). The next one round to the right (next) or left (!next) of
// `from` (the brought one's bearing, or 0 for straight ahead), skipping `current` and, with none brought,
// one within `aheadDeg` of straight ahead (it's in front already). -1: no other panel.
struct Bearing {
    int index;
    double deg;
};
int SusanPick(const std::vector<Bearing> &panels, int current, double from, bool next, double aheadDeg);

}  // namespace grabthat
