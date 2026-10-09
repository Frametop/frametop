// Unit tests for the pinch and grip detectors' hold on a drag (track/pinch.h: PinchParams' drag_*
// and grip_takeover_s): made-up hands at 30 Hz, the distances from their points (no views).
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

// A right hand 0.4 m ahead, the thumb tip d from the index tip, the other fingers open (curl 2)
// or closed around (fist: curl 0.85).
Hand MakeHand(double d, bool fist = false) {
    Hand h;
    h.id = 1, h.side = 1, h.frames = 30, h.has_pts = true;
    const V3 wrist{0, -0.1, -0.35};
    auto at = [&](double x, double y, double z) { return V3{wrist[0] + x, wrist[1] + y, wrist[2] + z}; };
    const double knuckle = 0.08, tip = fist ? 0.068 : 0.16;
    h.pts[0] = at(0, 0, 0);
    for (int f = 0; f < 4; ++f) {
        const double x = -0.02 + 0.015 * f;
        h.pts[5 + 4 * f] = at(x, 0, -knuckle);
        h.pts[6 + 4 * f] = at(x, 0, -knuckle - 0.02);
        h.pts[7 + 4 * f] = at(x, 0, -knuckle - 0.04);
        h.pts[8 + 4 * f] = at(x, 0, -tip);
    }
    // the thumb: its tip d from the index tip, beside it
    h.pts[1] = at(-0.03, 0, -0.02), h.pts[2] = at(-0.04, 0, -0.04), h.pts[3] = at(-0.045, 0, -0.06);
    const V3 index = h.pts[8];
    h.pts[4] = {index[0] - d, index[1], index[2]};
    for (int i = 0; i < 21; ++i) h.smooth[i] = h.pts[i];
    return h;
}

struct Run {
    Pinch pinch;
    Grip grip;
    int64_t t = 1'000'000'000;
    int ends = 0, lost = 0;
    void Step(double d, bool fist = false) {
        Hand h = MakeHand(d, fist);
        const std::vector<const Hand *> hands{&h};
        grip.update(hands, {}, t, pinch.holding(t));
        pinch.update(hands, {}, t, grip.gripping());
        for (const Pinch::Event &e : pinch.events) {
            ends += std::string(e.what) != "begin";
            lost += std::string(e.what) == "lost";
        }
        t += kFrame;
    }
    bool down() const { return pinch.side(1).flags & FH_PINCH_DOWN; }
};

PinchParams Params() {
    PinchParams p;
    p.other_finger = 12;   // as ft-hands
    return p;
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

}  // namespace

int main() {
    TestDragSpike();
    TestTapUnchanged();
    TestGripTakeover();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pinch-test: ok\n");
    return 0;
}
