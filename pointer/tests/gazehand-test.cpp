// Unit tests for gaze pinches (pointer/helper/gazehand.h): the gaze lock, the nudge's dead zone and
// gain, the press after a still hold, the typing guard and the ray switch, with made-up gaze at
// ft-gazed's 90 Hz and hands at ft-hands' 30 Hz.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/gazehand-test.cpp).
#include "pointer/helper/gazehand.h"

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
    s.sinceKey = 0.3;
    CHECK(Refused(s) && std::string(Refused(s)) == "ignored (typing)", "a key 0.3 s ago");
    s.sinceKey = 1.2;
    CHECK(Refused(s) == nullptr, "a key 1.2 s ago");
    s.typingHold = 0;
    s.sinceKey = 0;
    CHECK(Refused(s) == nullptr, "POINTER_PINCH_TYPING=0: no guard");
    s = Situation();
    s.active = false, s.sinceKey = 0.1;
    CHECK(std::string(Refused(s)) == "it woke the pointer, no click", "asleep: it only wakes");
    s.busy = true;
    CHECK(std::string(Refused(s)) == "ignored (a press is already held)", "busy first");
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
    TestRaySwitch();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("gazehand-test: ok\n");
    return 0;
}
