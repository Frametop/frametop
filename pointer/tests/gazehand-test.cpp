// Unit tests for gaze pinches (pointer/helper/gazehand.h): the gaze lock, the nudge's dead zone and
// gain, the press after a still hold, the two-hand chord (both buttons held), typing holding pinches
// back (typing.h, which the helper asks before gazehand), and the ray switch, with made-up gaze at
// ft-gazed's 90 Hz and hands at ft-hands' 30 Hz.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/gazehand-test.cpp).
#include "pointer/helper/gazehand.h"
#include "pointer/helper/typing.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace gazehand;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

namespace {

constexpr double kGaze = 1 / 90.0;
constexpr double kFrame = 1 / 30.0;

bool Has(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }

// Gaze at (hy, hp) from t0 up to t1, at 90 Hz; raw a degree off the corrected.
void Look(GazeHistory &h, double t0, double t1, double hy, double hp) {
    for (double t = t0; t < t1 - 1e-9; t += kGaze) h.Add({t, hy, hp, hy + 1, hp});
}

// The live failure this is for: you look at A, start pinching, and the eyes are on their way to the
// next thing before ft-hands calls the pinch. The click goes to A.
void TestLockTakesTheLookBeforeThePinch() {
    GazeHistory h;
    Look(h, 0, 1.0, 10, 0);       // A
    Look(h, 1.0, 1.4, 25, -10);   // B, from 1.0 s
    GazeSample s;
    double age = 0;
    // the begin's capture at 1.05: 50 ms after the eyes left A
    CHECK(h.Lock(1.05, Params(), s, age), "a lock");
    CHECK(s.hy == 10 && s.hp == 0, "at A, not B: %.1f %.1f", s.hy, s.hp);
    CHECK(age >= 0.075 - 1e-9 && age < 0.075 + kGaze + 1e-9, "the newest at least 75 ms before: %.3f", age);
    CHECK(s.rhy == 11, "with its raw gaze (the lesson's look): %.1f", s.rhy);
    // a pinch well into the fixation on B clicks B
    CHECK(h.Lock(1.3, Params(), s, age) && s.hy == 25, "later: B (%.1f)", s.hy);
}

void TestLockWithoutFreshGaze() {
    GazeHistory h;
    Look(h, 0, 0.5, 10, 0);
    GazeSample s;
    double age = 0;
    CHECK(!h.Lock(1.0, Params(), s, age), "the gaze stopped half a second before: no lock");
    // a blink: nothing from 0.5 to 0.98, then the eyes are back: the first sample after it
    Look(h, 0.98, 1.1, 12, 1);
    CHECK(h.Lock(1.0, Params(), s, age), "back just before the begin: a lock");
    CHECK(s.hy == 12 && std::fabs(s.t - 0.98) < 1e-6, "the first sample back: %.1f at %.3f", s.hy, s.t);
    GazeHistory none;
    CHECK(!none.Lock(1.0, Params(), s, age), "no gaze at all: no lock");
}

// A tap: the hand jitters (and the pinch point shifts as the fingers close) inside the dead zone.
void TestTapClicksAtTheLock() {
    Pinch p;
    p.Begin(Params(), 1.0, 30, 5, -20, -35, false, 0.08);
    double t = 1.0;
    const double jitter[] = {0.4, -0.6, 1.1, 1.8, 2.2, 1.5};  // degrees, along both axes
    for (double j : jitter) {
        t += kFrame;
        CHECK(!p.Hand(-20 + j, -35 - j * 0.5), "jitter doesn't move it");
        CHECK(!p.Promote(t), "a tap isn't a press");
    }
    CHECK(p.Yaw() == 30 && p.Pitch() == 5 && p.Moved() == 0, "still at the lock: %.2f %.2f", p.Yaw(), p.Pitch());
    CHECK(p.State() == Pinch::Phase::Aim, "still aiming");
    const std::string says = p.Says(t, false, "", "frametop.screen.0", "");
    CHECK(Has(says, "clicked at the gaze on frametop.screen.0") && Has(says, "gaze 80 ms before"), "%s", says.c_str());
}

// Slow drift under the dead zone (a hand sagging while you read) never nudges.
void TestDriftUnderTheDeadZone() {
    Pinch p;
    p.Begin(Params(), 0, 0, 0, -10, -30, false, 0.08);
    for (int i = 0; i <= 40; ++i) p.Hand(-10 + i * 0.06, -30 - i * 0.075);  // 3.9 deg over 1.3 s at 30 Hz, down and in
    CHECK(p.Yaw() == 0 && p.Pitch() == 0 && p.Moved() == 0, "no nudge: %.3f %.3f", p.Yaw(), p.Pitch());
}

// Past the dead zone: the pointer follows at the gain, from where it was (no jump).
void TestNudgePastTheDeadZone() {
    Params prm;
    Pinch p;
    p.Begin(prm, 0, 30, 5, -20, -35, false, 0.08);
    double hy = -20, lastYaw = 30;
    double maxStep = 0, engagedAt = 0;
    // the hand moves left (yaw +) half a degree a frame, 10 degrees in all
    for (int i = 1; i <= 20; ++i) {
        hy += 0.5;
        p.Hand(hy, -35);
        maxStep = std::max(maxStep, std::fabs(p.Yaw() - lastYaw));
        lastYaw = p.Yaw();
        if (p.State() == Pinch::Phase::Nudge && engagedAt == 0) engagedAt = hy;
    }
    CHECK(std::fabs(engagedAt - (-20 + prm.deadzone + 0.5)) < 1e-9, "engaged just past the dead zone: at %.2f", engagedAt);
    CHECK(maxStep <= prm.gain * 0.5 + 1e-9, "no jump: the biggest step %.3f deg (gain x 0.5)", maxStep);
    // from the engagement (hand at -15.5) to -10: 5.5 deg of hand, 1.65 of pointer
    CHECK(std::fabs(p.Yaw() - (30 + prm.gain * (hy - engagedAt))) < 1e-9, "at the gain: %.3f", p.Yaw());
    CHECK(p.Pitch() == 5, "only across: %.3f", p.Pitch());
    // the click: at the corrected point, a correction the helper sends as a lesson (>= 0.2 deg moved)
    CHECK(p.Moved() >= 0.2 && std::fabs(p.Moved() - (p.Yaw() - 30)) < 1e-9, "moved %.3f", p.Moved());
    CHECK(!p.Promote(10), "a pinch that nudged never becomes a press");
    std::string says = p.Says(1.2, false, "", "frametop.screen.1", "a lesson");
    CHECK(Has(says, "nudged 1.6 deg, then clicked on frametop.screen.1 (a lesson;"), "%s", says.c_str());
    says = p.Says(1.2, false, "", "frametop.screen.1", "the quick check");
    CHECK(Has(says, "(the quick check;"), "%s", says.c_str());
    says = p.Says(1.2, false, "", "frametop.screen.1", "");
    CHECK(Has(says, "(not learned;"), "%s", says.c_str());
    // back again: the pointer comes back with the hand
    for (int i = 0; i < 11; ++i) hy -= 0.5, p.Hand(hy, -35);
    CHECK(std::fabs(p.Yaw() - 30) < 1e-9, "back where it engaged: %.3f", p.Yaw());
}

void TestGainAndDeadZoneSettings() {
    Params prm;
    prm.deadzone = 1.0, prm.gain = 0.5;
    Pinch p;
    p.Begin(prm, 0, 0, 0, 0, 0, false, -1);
    p.Hand(0, -1.2);  // engages
    p.Hand(0, -3.2);
    CHECK(std::fabs(p.Pitch() + 1.0) < 1e-9, "POINTER_GAZE_PINCH_GAIN 0.5: %.3f", p.Pitch());
    const std::string says = p.Says(1, false, "", "", "");
    CHECK(Has(says, "no fresh gaze: where the pointer was") && Has(says, "on nothing"), "%s", says.c_str());
}

// Held still for POINTER_GAZE_HOLD: a real press at the lock, then the hand drags at the grip's gain.
void TestHoldStillBecomesAPress() {
    Params prm;
    Pinch p;
    p.Begin(prm, 0, 30, 5, -20, -35, false, 0.08);
    double t = 0;
    bool pressed = false;
    int presses = 0;
    while (t < 1.0) {
        t += 0.008;
        if (std::fmod(t, kFrame) < 0.008) p.Hand(-20 + 1.0, -35 + 0.5);  // still, a little off the begin
        if (p.Promote(t)) {
            ++presses;
            CHECK(!pressed && t >= prm.hold - 1e-9 && t < prm.hold + 0.009, "pressed at %.3f s", t);
            pressed = true;
        }
    }
    CHECK(presses == 1, "one press: %d", presses);
    CHECK(p.State() == Pinch::Phase::Press && p.Yaw() == 30 && p.Pitch() == 5, "at the lock");
    // the drag counts from the hand at the press: inside 0.5 deg nothing, then gain 1
    p.Hand(-19 + 0.4, -34.5);
    CHECK(p.Yaw() == 30, "the drag's dead zone");
    p.Hand(-19 + 0.6, -34.5);  // engages
    p.Hand(-19 + 6.6, -34.5);
    CHECK(std::fabs(p.Yaw() - 36) < 1e-9 && p.Moved() == 0 && std::fabs(p.Dragged() - 6) < 1e-9,
          "dragged at gain 1: %.3f (moved %.3f, dragged %.3f)", p.Yaw(), p.Moved(), p.Dragged());
    p.pressHit = "frametop.float.3";
    std::string says = p.Says(1.4, false, "", "", "");
    CHECK(Has(says, "became a press on frametop.float.3 (held still 0.50 s;") && Has(says, "dragged 6.0 deg, let go (held 1.40 s)"),
          "%s", says.c_str());
    says = p.Says(1.4, true, "its side has a controller in use", "", "");
    CHECK(Has(says, "let go: its side has a controller in use"), "%s", says.c_str());
}

void TestMiddlePinch() {
    Pinch p;
    p.Begin(Params(), 0, 10, 0, 0, -30, true, 0.09);
    CHECK(!p.Promote(2.0), "a middle pinch never becomes a press");
    std::string says = p.Says(2.0, false, "", "frametop.screen.0", "");
    CHECK(Has(says, "right click at the gaze on frametop.screen.0"), "%s", says.c_str());
    for (double hy = 0; hy >= -10; hy -= 0.5) p.Hand(hy, -30);
    says = p.Says(2.0, false, "", "frametop.screen.0", "a lesson");
    CHECK(Has(says, "then a right click on"), "%s", says.c_str());
    says = p.Says(2.0, true, "ft-hands lost the hand, or a grip took the pinch", "", "");
    CHECK(Has(says, "no click: ft-hands lost the hand"), "%s", says.c_str());
}

void TestRefused() {
    Situation s;
    CHECK(Refused(s) == nullptr, "nothing in the way");
    // The key guard is typing.h's (the helper asks it before gazehand): keys alone, no hands seen.
    typing::Typing ty;
    ty.Key(0);
    CHECK(ty.Why(1, 0.3) && std::string(ty.Why(1, 0.3)) == "typing", "a key 0.3 s ago");
    CHECK(ty.Blocked(1, 1.2), "a key 1.2 s ago: still typing (POINTER_PINCH_TYPING is 2 s, was 1)");
    CHECK(!ty.Blocked(1, 2.1), "a key 2.1 s ago");
    typing::Params off;
    off.hold = 0;
    ty.SetParams(off);
    CHECK(!ty.Blocked(1, 0), "POINTER_PINCH_TYPING=0: no guard");
    s = Situation();
    s.active = false;
    CHECK(std::string(Refused(s)) == "it woke the pointer, no click", "asleep: it only wakes");
    s.busy = true;
    CHECK(std::string(Refused(s)) == "ignored (a press is already held)", "busy first");
}

// The chord (see "Chord" in gazehand.h): which second pinches make one.
void TestChordable() {
    Pinch left;
    CHECK(!Chordable(left, -1, 1, false, false), "nothing held: a plain pinch");
    left.Begin(Params(), 0, 10, 0, -20, -35, false, 0.08);
    CHECK(Chordable(left, 0, 1, false, false), "the other hand's index pinch: a chord");
    CHECK(!Chordable(left, 0, 0, false, false), "the same hand: no");
    CHECK(!Chordable(left, 0, 1, true, false), "a middle pinch by the other hand: no (ignored, as before)");
    CHECK(!Chordable(left, 0, 1, false, true), "a chord held already: no third button");
    Pinch middle;
    middle.Begin(Params(), 0, 10, 0, -20, -35, true, 0.08);
    CHECK(!Chordable(middle, 0, 1, false, false), "during a middle pinch (a right click held back): no");
    Pinch chord;
    chord.BeginChord(Params(), 0, 10, 0, -20, -35);
    CHECK(!Chordable(chord, 1, 0, false, false), "the chord's own pinch can't make another");
    // The helper's busy test: a chord isn't busy, so only typing (typing.h, asked first) and the wake
    // stand in its way.
    Situation s;
    s.busy = !Chordable(left, 0, 1, false, false);
    CHECK(Refused(s) == nullptr, "a chord goes ahead");
    {
        // Keys 0.3 s ago: the left holds its gaze pinch out of the keyboard zone (busy: free), the
        // right is on the keys. The right's pinch is held back before it can be the chord's.
        typing::Typing ty;
        typing::Head head;
        head.pos = {0, 1.6, 0};
        typing::Hand hands[2];
        hands[0].seen = true, hands[0].palm = {-0.1, 1.5, -0.4}, hands[0].down = 0.1, hands[0].busy = true;
        hands[1].seen = true, hands[1].palm = {0.1, 1.25, -0.25}, hands[1].down = 0.95;
        ty.Key(0);
        double t = 0;
        for (; t < 0.3; t += kFrame) ty.Frame(t, head, hands);
        CHECK(!ty.Blocked(0, t), "the left's gaze pinch, held out: free (%s)", ty.Why(0, t));
        CHECK(ty.Why(1, t) && std::string(ty.Why(1, t)) == "typing: the hand is on the keyboard",
              "a chord's pinch 0.3 s after a key, by a hand on the keys: typing (%s)", ty.Why(1, t));
        // The right lifted 0.11 m off the keys: free at once, keys or not, so its pinch makes the chord.
        hands[1].palm.y += 0.11;
        for (const double end = t + 0.2; t < end; t += kFrame) ty.Frame(t, head, hands);
        ty.Key(t);
        CHECK(!ty.Blocked(1, t), "the right lifted off the keys: its chord goes ahead (%s)", ty.Why(1, t));
    }
    s = Situation();
    s.busy = !Chordable(left, 0, 1, true, false);
    CHECK(std::string(Refused(s)) == "ignored (a press is already held)", "a middle pinch: busy, as before");
}

// L hold + R pinch: the left, still held back, becomes a real press where the pointer is (the lock);
// the right goes down 30 ms later and stays down until its pinch ends.
void TestChordPressesBoth() {
    Params prm;
    Pinch left;
    left.Begin(prm, 0, 30, 5, -20, -35, false, 0.08);
    double t = 0;
    for (int i = 0; i < 6; ++i) t += kFrame, left.Hand(-20 + 0.5, -35);  // still: aiming
    CHECK(left.State() == Pinch::Phase::Aim, "aiming before the chord");
    CHECK(left.Chord(t), "the chord makes the press");
    CHECK(left.State() == Pinch::Phase::Press && left.Yaw() == 30 && left.Pitch() == 5, "pressed at the lock");
    CHECK(!left.Promote(t + 1), "the hold has nothing left to do");
    Pinch right;
    right.BeginChord(prm, t, left.Yaw(), left.Pitch(), 15, -35);
    ChordButton b;
    b.Begin(t, true);
    CHECK(b.Tick(t) == 0 && !b.Down(), "not yet: the left lands first");
    CHECK(b.Tick(t + 0.02) == 0, "20 ms on: not yet");
    CHECK(b.Tick(t + 0.031) == 1 && b.Down(), "30 ms on: the right goes down");
    for (double u = t + 0.04; u < t + 1; u += 0.008) CHECK(b.Tick(u) == 0, "held while its pinch is: %.3f", u);
    // the left hand drags meanwhile, at the grip's gain past 0.5 deg; both buttons stay down
    left.Hand(-19.5 + 0.6, -35);  // engages
    left.Hand(-19.5 + 4.6, -35);
    CHECK(std::fabs(left.Yaw() - 34) < 1e-9 && std::fabs(left.Dragged() - 4) < 1e-9, "dragged at gain 1: %.3f", left.Yaw());
    left.pressHit = right.pressHit = "frametop.screen.0";
    const std::string says = left.Says(t + 1.2, false, "", "", "");
    CHECK(Has(says, "became a press on frametop.screen.0 (the other hand pinched after 0.20 s; gaze 80 ms before)") &&
              Has(says, "dragged 4.0 deg, let go") && Has(says, ", with the right button)"),
          "%s", says.c_str());
    const std::string rs = right.Says(t + 1.0, false, "", "", "");
    CHECK(Has(rs, "the right button with the other hand's press, on frametop.screen.0, let go (held 1.00 s)"), "%s", rs.c_str());
    // a left already pressed by the hold: the chord only adds the right
    Pinch held;
    held.Begin(prm, 0, 0, 0, 0, 0, false, -1);
    CHECK(held.Promote(0.5), "held still: a press");
    CHECK(!held.Chord(0.7) && held.State() == Pinch::Phase::Press && held.ChordCount() == 1, "already a press");
    CHECK(Has(held.Says(1, false, "", "", ""), "(held still 0.50 s;"), "the hold made it");
}

// R release first: the right comes up at once, the left stays down; the other hand can chord again.
void TestChordRightLetsGoFirst() {
    Pinch left;
    left.Begin(Params(), 0, 0, 0, 0, 0, false, 0.08);
    CHECK(left.Chord(0.1), "chord");
    ChordButton b;
    b.Begin(0.1, true);
    b.Tick(0.14);
    CHECK(b.Down(), "down");
    CHECK(b.End(false) == -1 && !b.Down() && b.Finished(), "its pinch ended: released now");
    CHECK(left.State() == Pinch::Phase::Press, "the left is still held");
    b.Reset();
    CHECK(Chordable(left, 0, 1, false, b.On()), "a second chord (right pressed again)");
    CHECK(!left.Chord(0.6), "the left is a press already");
    b.Begin(0.6, false);
    CHECK(b.Tick(0.6) == 1, "a press the chord didn't have to make first: at once");
    CHECK(b.End(false) == -1, "and up");
    CHECK(Has(left.Says(1.0, false, "", "", ""), "with the right button 2 times"), "%s", left.Says(1.0, false, "", "", "").c_str());
}

// L release first: the left comes up (the helper's), the right stays down, and the second hand drags
// from where the pointer is, without a jump.
void TestChordLeftLetsGoFirst() {
    Params prm;
    Pinch right;
    right.BeginChord(prm, 0, 30, 5, 10, -30);
    for (int i = 1; i <= 10; ++i) right.Track(10 + i * 0.3, -30);  // moves 3 deg while the left drags
    CHECK(right.Yaw() == 30 && right.Dragged() == 0, "only followed while the left drags");
    right.Take(36, 5);  // the left let go with the pointer at 36
    right.Hand(13.4, -30);
    CHECK(right.Yaw() == 36, "the drag's dead zone, from the hand at the handover");
    right.Hand(13.6, -30);  // engages
    right.Hand(15.6, -30);
    CHECK(std::fabs(right.Yaw() - 38) < 1e-9 && std::fabs(right.Dragged() - 2) < 1e-9, "dragged from 36: %.3f", right.Yaw());
    ChordButton b;
    b.Begin(0, false);
    CHECK(b.Tick(0) == 1, "down");
    for (double u = 0.1; u < 2; u += 0.1) CHECK(b.Tick(u) == 0, "stays down while its pinch does");
    CHECK(b.End(false) == -1, "up with its pinch");
    right.pressHit = "frametop.float.2";
    CHECK(Has(right.Says(2, false, "", "", ""), "on frametop.float.2, dragged 2.0 deg, let go (held 2.00 s)"), "%s",
          right.Says(2, false, "", "", "").c_str());
}

// The chord after a nudge: the left presses where the nudge took the pointer (a lesson), then drags.
void TestChordAfterANudge() {
    Params prm;
    Pinch left;
    left.Begin(prm, 0, 30, 5, -20, -35, false, 0.08);
    double hy = -20;
    for (int i = 0; i < 16; ++i) hy += 0.5, left.Hand(hy, -35);  // 8 deg: nudges 0.3 x (8 - 4.5), 1.05
    CHECK(left.State() == Pinch::Phase::Nudge && left.Moved() > 0.2, "nudged %.3f", left.Moved());
    const double at = left.Yaw();
    CHECK(left.Chord(0.6), "a nudged pinch becomes a press too (the chord asks for both buttons)");
    CHECK(left.Yaw() == at && left.State() == Pinch::Phase::Press, "pressed where the nudge took it: %.3f", left.Yaw());
    left.Hand(hy + 0.4, -35);
    CHECK(left.Yaw() == at, "the drag's dead zone counts from the hand at the chord");
    left.Hand(hy + 0.6, -35);  // engages
    left.Hand(hy + 2.6, -35);
    CHECK(std::fabs(left.Yaw() - (at + 2)) < 1e-9, "then gain 1: %.3f", left.Yaw());
    const std::string says = left.Says(1.2, false, "", "", "a lesson");
    CHECK(Has(says, "the other hand pinched after 0.60 s, nudged 1.1 deg; a lesson;"), "%s", says.c_str());
}

// A quick second pinch: a right click during the left's press. One lost before its press: nothing.
// One lost while down: released at once.
void TestChordTapsAndLosses() {
    ChordButton b;
    b.Begin(1.0, true);
    CHECK(b.End(false) == 0 && !b.Finished(), "a tap that ended before its press");
    CHECK(b.Tick(1.03) == 1, "still pressed when due");
    CHECK(b.Tick(1.05) == 0, "held 40 ms");
    CHECK(b.Tick(1.07) == -1 && b.Finished() && b.Pressed(), "then released: a right click");
    b.Begin(1.0, true);
    CHECK(b.End(true) == 0 && b.Finished() && !b.Pressed(), "lost before its press: no button at all");
    CHECK(b.Tick(1.1) == 0, "and nothing later");
    b.Begin(1.0, false);
    CHECK(b.Tick(1.0) == 1, "down");
    CHECK(b.End(true) == -1 && b.Finished(), "the hand lost mid-chord: released at once");
    CHECK(b.End(true) == 0 && b.Tick(2) == 0, "once");
    b.Begin(1.0, false);
    b.Tick(1.0);
    b.End(false);  // a tap whose release is due
    CHECK(b.End(true) == 0 && !b.Down() && b.Finished(), "cut short after its release: nothing more");
    b.Begin(1.0, true);
    b.End(false);
    b.Tick(1.03);
    CHECK(b.Down() && b.End(true) == -1, "a tap cut short while its click is down: released now");
    Pinch right;
    right.BeginChord(Params(), 0, 0, 0, 0, 0);
    CHECK(Has(right.Says(0.4, true, "ft-hands lost the hand, or a grip took the pinch", "", ""),
              "let go: ft-hands lost the hand"),
          "%s", right.Says(0.4, true, "ft-hands lost the hand, or a grip took the pinch", "", "").c_str());
    // the left's hand lost mid-chord: its press ends as lost (the helper releases the left)
    Pinch left;
    left.Begin(Params(), 0, 0, 0, 0, 0, false, -1);
    left.Chord(0.1);
    CHECK(Has(left.Says(0.5, true, "ft-hands lost the hand", "", ""), "let go: ft-hands lost the hand (held 0.50 s, with the right button)"),
          "%s", left.Says(0.5, true, "ft-hands lost the hand", "", "").c_str());
}

void TestRaySwitch() {
    CHECK(RayAllowed(true, false, false), "no gaze mode: the ray");
    CHECK(!RayAllowed(true, true, false), "gaze mode: no ray (POINTER_GAZE_HAND_RAY=0)");
    CHECK(RayAllowed(true, true, true), "gaze mode with POINTER_GAZE_HAND_RAY=1: the ray");
    CHECK(!RayAllowed(false, false, true), "POINTER_HAND_RAY=0: never");
}

}  // namespace

int main() {
    TestLockTakesTheLookBeforeThePinch();
    TestLockWithoutFreshGaze();
    TestTapClicksAtTheLock();
    TestDriftUnderTheDeadZone();
    TestNudgePastTheDeadZone();
    TestGainAndDeadZoneSettings();
    TestHoldStillBecomesAPress();
    TestMiddlePinch();
    TestRefused();
    TestChordable();
    TestChordPressesBoth();
    TestChordRightLetsGoFirst();
    TestChordLeftLetsGoFirst();
    TestChordAfterANudge();
    TestChordTapsAndLosses();
    TestRaySwitch();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("gazehand-test: ok\n");
    return 0;
}
