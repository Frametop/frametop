// Unit tests for the pinch and grip detectors' hold on a drag (track/pinch.h: PinchParams' drag_*
// and grip_takeover_s), the dip rule (PinchParams::dip) and the fist guard (fist_curl): made-up hands
// at 30 Hz, the distances from their points (no views).
// Build and run: make check (in the dev container; hands/build.sh's environment).
#include "../track/pinch.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

constexpr int64_t kFrame = 33'333'333;

// A right hand 0.4 m ahead (moved by shift), the thumb tip d from the index tip, the other fingers
// open (curl 2) or closed around (fist: curl 0.85); middle_gap > 0 puts the middle tip that far
// from the thumb tip (a hand closing into a grab). curled: the fingers closed (bit 0 index .. 3 little)
// when not a fist.
Hand MakeHand(double d, bool fist = false, V3 shift = {0, 0, 0}, double middle_gap = 0, int curled = 0) {
    Hand h;
    h.id = 1, h.side = 1, h.frames = 30, h.has_pts = true;
    const V3 wrist = V3{0, -0.1, -0.35} + shift;
    auto at = [&](double x, double y, double z) { return V3{wrist[0] + x, wrist[1] + y, wrist[2] + z}; };
    const double knuckle = 0.08;
    h.pts[0] = at(0, 0, 0);
    for (int f = 0; f < 4; ++f) {
        const double x = -0.02 + 0.015 * f, tip = fist || (curled >> f & 1) ? 0.068 : 0.16;
        h.pts[5 + 4 * f] = at(x, 0, -knuckle);
        h.pts[6 + 4 * f] = at(x, 0, -knuckle - 0.02);
        h.pts[7 + 4 * f] = at(x, 0, -knuckle - 0.04);
        h.pts[8 + 4 * f] = at(x, 0, -tip);
    }
    // the thumb: its tip d from the index tip, beside it
    h.pts[1] = at(-0.03, 0, -0.02), h.pts[2] = at(-0.04, 0, -0.04), h.pts[3] = at(-0.045, 0, -0.06);
    const V3 index = h.pts[8];
    h.pts[4] = {index[0] - d, index[1], index[2]};
    if (middle_gap > 0) h.pts[12] = {h.pts[4][0], h.pts[4][1] + middle_gap, h.pts[4][2]};
    for (int i = 0; i < 21; ++i) h.smooth[i] = h.pts[i];
    return h;
}

struct Run {
    Pinch pinch;
    Grip grip;
    int64_t t = 1'000'000'000;
    int ends = 0, lost = 0;
    int begins = 0;
    V3 shift{0, 0, 0}, velocity{0, 0, 0};   // the hand's offset and its motion, m/s
    double middle_gap = 0;
    int curled = 0;
    void Step(double d, bool fist = false) {
        Hand h = MakeHand(d, fist, shift, middle_gap, curled);
        shift = shift + velocity * (kFrame / 1e9);
        const std::vector<const Hand *> hands{&h};
        grip.update(hands, {}, t, pinch.holding(t));
        pinch.update(hands, {}, t, grip.gripping());
        for (const Pinch::Event &e : pinch.events) {
            begins += std::string(e.what) == "begin";
            ends += std::string(e.what) != "begin";
            lost += std::string(e.what) == "lost";
        }
        t += kFrame;
    }
    bool down() const { return pinch.side(1).flags & FH_PINCH_DOWN; }
};

// The values these tests were written for (the defaults before 2026-10-14): they test the rules,
// TestDefaults the defaults.
PinchParams Params() {
    PinchParams p;
    p.other_finger = 12;   // as ft-hands
    p.end_m = 0.035, p.drag_open_m = 0.05, p.drag_open_s = 0.12, p.grip_takeover_s = 0.3;
    return p;
}

void TestDefaults() {
    // since 2026-10-14 a pinch lets go less easily: a tap still reading 4 cm hasn't ended
    PinchParams p;
    p.other_finger = 12;
    Run tap{Pinch(p), Grip()};
    for (int k = 0; k < 5; ++k) tap.Step(0.06);
    for (int k = 0; k < 3; ++k) tap.Step(0.01);
    tap.Step(0.04), tap.Step(0.04);
    CHECK(tap.down(), "defaults: a tap at 4 cm is still down");
    tap.Step(0.05), tap.Step(0.05);
    CHECK(!tap.down() && tap.lost == 0, "defaults: a tap ends on the second frame past 4.5 cm");
    // a drag loosened to 5.5 cm ends after drag_open_s (0.2 s: six or seven frames), opened to 7 cm at once
    Run drag{Pinch(p), Grip()};
    for (int k = 0; k < 5; ++k) drag.Step(0.06);
    for (int k = 0; k < 30; ++k) drag.Step(0.01);
    int frames = 0;
    while (drag.down() && frames < 20) drag.Step(0.055), ++frames;
    CHECK(!drag.down() && frames >= 6 && frames <= 8, "defaults: a drag at 5.5 cm ended after %d frames", frames);
    Run open{Pinch(p), Grip()};
    for (int k = 0; k < 5; ++k) open.Step(0.06);
    for (int k = 0; k < 30; ++k) open.Step(0.01);
    open.Step(0.07), open.Step(0.07);
    CHECK(!open.down() && open.lost == 0, "defaults: a drag opened to 7 cm ends on the second frame");
}

void TestDragSpike() {
    // a drag (held 1 s): the tips read 3.8 cm for two frames (a hidden thumb), then close again
    Run r{Pinch(Params()), Grip()};
    for (int k = 0; k < 5; ++k) r.Step(0.06);
    for (int k = 0; k < 30; ++k) r.Step(0.01);
    CHECK(r.down(), "the pinch is down");
    r.Step(0.038), r.Step(0.038), r.Step(0.01);
    CHECK(r.down() && r.ends == 0, "a two-frame spike to 3.8 cm doesn't drop a drag");
    // loosened a little and staying there: it ends after drag_open_s (0.12 s: four frames)
    int frames = 0;
    while (r.down() && frames < 20) r.Step(0.04), ++frames;
    CHECK(!r.down() && frames >= 4 && frames <= 5 && r.lost == 0, "loosened to 4 cm: ended after %d frames", frames);
    // fingers hovering 3.3-3.8 cm apart (a relaxed hand) end it too: the dips under 3.5 cm aren't
    // a close
    Run h{Pinch(Params()), Grip()};
    for (int k = 0; k < 5; ++k) h.Step(0.06);
    for (int k = 0; k < 30; ++k) h.Step(0.01);
    frames = 0;
    while (h.down() && frames < 30) h.Step(frames % 2 ? 0.033 : 0.038), ++frames;
    CHECK(!h.down() && frames <= 6, "hovering 3.3-3.8 cm apart: ended after %d frames", frames);
    // a real release: the tips 6 cm apart end it after end_frames (2), as before
    Run q{Pinch(Params()), Grip()};
    for (int k = 0; k < 5; ++k) q.Step(0.06);
    for (int k = 0; k < 30; ++k) q.Step(0.01);
    q.Step(0.06);
    CHECK(q.down(), "one frame open: still down");
    q.Step(0.07);
    CHECK(!q.down() && q.lost == 0, "a release to 6-7 cm ends it on the second frame");
}

void TestTapUnchanged() {
    // a tap (0.1 s): ends after two frames past 3.5 cm, wherever they are
    Run r{Pinch(Params()), Grip()};
    for (int k = 0; k < 5; ++k) r.Step(0.06);
    for (int k = 0; k < 3; ++k) r.Step(0.01);
    r.Step(0.038);
    CHECK(r.down(), "a tap's first frame open");
    r.Step(0.038);
    CHECK(!r.down(), "a tap ends on the second frame past 3.5 cm");
}

void TestGripTakeover() {
    // closing the hand passes through a pinch: a grip within 0.3 s takes it (ends it, as lost)
    Run a{Pinch(Params()), Grip()};
    for (int k = 0; k < 10; ++k) a.Step(0.06);
    a.Step(0.015);
    CHECK(a.down(), "the pinch began");
    for (int k = 0; k < 4 && a.down(); ++k) a.Step(0.032, true);
    CHECK(!a.down() && a.lost == 1, "a grip right after the pinch took it (lost %d)", a.lost);
    // a drag held 1 s, the other fingers curling around it: no grip, and the pinch stays
    Run b{Pinch(Params()), Grip()};
    for (int k = 0; k < 10; ++k) b.Step(0.06);
    for (int k = 0; k < 30; ++k) b.Step(0.015);
    for (int k = 0; k < 15; ++k) b.Step(0.032, true);
    CHECK(b.down() && b.ends == 0, "a held pinch survives the hand closing around it");
    CHECK(!(b.grip.side(1).flags & FH_PINCH_DOWN), "and no grip begins on it");
}

PinchParams DipParams() {
    PinchParams p = Params();
    p.dip = true;
    return p;
}

// open, a quick dip to `low` (two frames) and back open
void QuickDip(Run &r, double low) {
    for (int k = 0; k < 10; ++k) r.Step(0.06);
    r.Step(0.045), r.Step(low), r.Step(low + 0.001), r.Step(0.045), r.Step(0.06), r.Step(0.06);
}

void TestDip() {
    // a quick tap that reads 2.8 cm at contact: no pinch without the dip rule, a click with it
    Run off{Pinch(Params()), Grip()};
    QuickDip(off, 0.028);
    CHECK(off.begins == 0, "without the dip rule a 2.8 cm dip is no pinch (%d)", off.begins);
    Run on{Pinch(DipParams()), Grip()};
    QuickDip(on, 0.028);
    CHECK(on.begins == 1 && on.ends == 1 && on.lost == 0 && !on.down(), "a 2.8 cm dip clicks once (%d begins, %d ends)",
          on.begins, on.ends);
    CHECK(on.pinch.dip_begins[1] == 1, "counted as a dip");
    // too shallow: 6 to 3.5 cm (over dip_max_m)
    Run shallow{Pinch(DipParams()), Grip()};
    QuickDip(shallow, 0.035);
    CHECK(shallow.begins == 0, "a dip to 3.5 cm isn't a tap");
    // closing slowly (1 mm a frame) to 2.8 cm and staying: not a dip
    Run slow{Pinch(DipParams()), Grip()};
    for (int k = 0; k < 10; ++k) slow.Step(0.06);
    for (double d = 0.06; d > 0.028; d -= 0.001) slow.Step(d);
    for (int k = 0; k < 10; ++k) slow.Step(0.028);
    CHECK(slow.begins == 0, "a slow close to 2.8 cm isn't a tap (%d)", slow.begins);
    // the same quick dip while the hand moves at 0.5 m/s: no tap
    Run moving{Pinch(DipParams()), Grip()};
    moving.velocity = {0.5, 0, 0};
    QuickDip(moving, 0.028);
    CHECK(moving.begins == 0, "a dip while the hand moves fast isn't a tap (%d)", moving.begins);
    // the middle fingertip coming in with the thumb (a hand closing into a grab): no tap
    Run grab{Pinch(DipParams()), Grip()};
    for (int k = 0; k < 10; ++k) grab.Step(0.06);
    grab.middle_gap = 0.03;
    grab.Step(0.045), grab.Step(0.028), grab.Step(0.028);
    CHECK(grab.begins == 0, "a dip with the middle tip 3 cm from the thumb isn't a tap (%d)", grab.begins);
    // a dip that goes on to contact is an ordinary pinch: held 1 s, a two-frame spike doesn't end it
    Run hold{Pinch(DipParams()), Grip()};
    for (int k = 0; k < 10; ++k) hold.Step(0.06);
    hold.Step(0.045), hold.Step(0.028);
    CHECK(hold.down(), "the dip began it");
    for (int k = 0; k < 30; ++k) hold.Step(0.01);
    hold.Step(0.038), hold.Step(0.038), hold.Step(0.01);
    CHECK(hold.down() && hold.ends == 0, "after contact it holds like any drag");
    // a dip held without contact ends once the tips spring back dip_rebound_m (6 mm) over its low
    Run spring{Pinch(DipParams()), Grip()};
    for (int k = 0; k < 10; ++k) spring.Step(0.06);
    spring.Step(0.045), spring.Step(0.026);
    for (int k = 0; k < 10; ++k) spring.Step(0.027);
    CHECK(spring.down(), "held at 2.7 cm: still down");
    spring.Step(0.033);
    CHECK(spring.down(), "one frame 7 mm over the low: still down");
    spring.Step(0.033);
    CHECK(!spring.down() && spring.lost == 0, "two frames 7 mm over the low: released, under end_m");
    // two taps in a row click twice; one held low after its release doesn't click again: a new dip
    // needs the hand open again since the release
    Run twice{Pinch(DipParams()), Grip()};
    QuickDip(twice, 0.028);
    QuickDip(twice, 0.028);
    CHECK(twice.begins == 2, "two dips, two clicks (%d)", twice.begins);
    Run settle{Pinch(DipParams()), Grip()};
    for (int k = 0; k < 10; ++k) settle.Step(0.06);
    settle.Step(0.045), settle.Step(0.026), settle.Step(0.033), settle.Step(0.033);   // released at 3.3 cm
    for (int k = 0; k < 10; ++k) settle.Step(k % 2 ? 0.033 : 0.028);
    CHECK(settle.begins == 1 && !settle.down(), "jitter at 2.8-3.3 cm after a dip's release: no second click (%d)",
          settle.begins);
}

void TestFistGuard() {
    // a fist with the thumb tip 1.5 cm from the curled index tip: no pinch, counted as held back
    Run fist{Pinch(Params()), Grip()};
    for (int k = 0; k < 10; ++k) fist.Step(0.06);
    for (int k = 0; k < 15; ++k) fist.Step(0.015, true);
    CHECK(fist.begins == 0 && fist.pinch.fist_held[1] == 1, "a fist isn't a pinch (%d begins)", fist.begins);
    // its curls flickering open for a frame, the thumb still on the index: still no pinch
    fist.Step(0.015), fist.Step(0.015, true);
    CHECK(fist.begins == 0, "a fist's curls flickering open don't begin a pinch (%d)", fist.begins);
    // the hand opens (the tips past end_m), then pinches: it begins
    fist.Step(0.06), fist.Step(0.015), fist.Step(0.015);
    CHECK(fist.begins == 1, "a pinch once the fist opened (%d)", fist.begins);
    // the index and middle curled, ring and little open: still a fist for the guard
    Run two{Pinch(Params()), Grip()};
    two.curled = 0b0011;
    for (int k = 0; k < 15; ++k) two.Step(0.015);
    CHECK(two.begins == 0, "index and middle curled: no pinch (%d)", two.begins);
    // ring and little curled (a pinch with the other fingers tucked in), or the index alone: a pinch
    Run tucked{Pinch(Params()), Grip()};
    tucked.curled = 0b1100;
    for (int k = 0; k < 5; ++k) tucked.Step(0.015);
    CHECK(tucked.begins == 1, "ring and little curled: a pinch (%d)", tucked.begins);
    Run index{Pinch(Params()), Grip()};
    index.curled = 0b0001;
    for (int k = 0; k < 5; ++k) index.Step(0.015);
    CHECK(index.begins == 1, "the index alone curled: a pinch (%d)", index.begins);
    // a pinch already down carries on while the index and middle curl around it
    Run held{Pinch(Params()), Grip()};
    for (int k = 0; k < 10; ++k) held.Step(0.015);
    held.curled = 0b0011;
    for (int k = 0; k < 15; ++k) held.Step(0.015);
    CHECK(held.down() && held.ends == 0, "a pinch down survives its fingers curling");
    // the fist opening with the fingers apart, then a pinch: it begins
    held.curled = 0;
    held.Step(0.06), held.Step(0.06);
    for (int k = 0; k < 3; ++k) held.Step(0.015);
    CHECK(held.begins == 2, "a pinch after the fist opened (%d)", held.begins);
    // fist_curl 0: the guard off, a fist with the thumb on the index pinches as before
    PinchParams off = Params();
    off.fist_curl = 0;
    Run old{Pinch(off), Grip()};
    for (int k = 0; k < 5; ++k) old.Step(0.015, true);
    CHECK(old.begins == 1, "guard off: the fist pinches (%d)", old.begins);
}

}  // namespace

int main() {
    TestDragSpike();
    TestTapUnchanged();
    TestGripTakeover();
    TestDip();
    TestFistGuard();
    TestDefaults();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pinch-test: ok\n");
    return 0;
}
