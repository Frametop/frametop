// Unit tests for the handoff arbiter (pointer/helper/handoff.h): made-up timelines of touch,
// presses, controller motion and hands, stepped at the helper's 8 ms frame.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/handoff-test.cpp).
#include "pointer/helper/handoff.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace handoff;

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

namespace {

constexpr double kDt = 0.008;

// A world: one controller per side (or none), bare hands, stepped by Run.
struct World {
    Handoff h;
    double t = 0;
    Controller ctl[2];
    bool have[2] = {true, true};
    std::vector<Hand> hands;
    bool handsLive = true;
    std::vector<std::string> log;
    World() {
        for (int s = 0; s < 2; ++s) {
            ctl[s].id = 2 + s, ctl[s].side = s, ctl[s].tracked = true;
            ctl[s].pos = {s ? 0.3 : -0.3, 0.75, -0.4};  // on a desk
        }
        h.Socket(true, 0);
        h.log.clear();
    }
    void Step() {
        std::vector<Controller> c;
        for (int s = 0; s < 2; ++s)
            if (have[s]) c.push_back(ctl[s]);
        h.Update(t, c, hands, handsLive);
        for (auto &l : h.log) log.push_back(l);
        h.log.clear();
        t += kDt;
    }
    // Run for `secs`, calling `each` before every step.
    void Run(double secs, const std::function<void()> &each = {}) {
        const double end = t + secs - 1e-9;
        while (t < end) {
            if (each) each();
            Step();
        }
    }
    // The first time `side` reads `o` while running up to `secs`; -1 if never.
    double Until(int side, Owner o, double secs, const std::function<void()> &each = {}) {
        const double end = t + secs;
        while (t < end) {
            if (each) each();
            Step();
            if (h.State(side) == o) return t;
        }
        return -1;
    }
};

void TestPickUpAndSetDown() {
    World w;
    w.Run(1);
    CHECK(w.h.State(1) == Owner::Hand, "resting controller: hand, got %s", Name(w.h.State(1)));
    // pick up: touch, then moving for 2 s
    w.h.Touch(1, true, w.t);
    w.Step();
    CHECK(w.h.State(1) == Owner::Controller, "touch: controller at once");
    w.Run(2, [&] {
        w.ctl[1].vel = {0.3, 0, 0};
        w.ctl[1].pos.x += 0.3 * kDt;
    });
    // set down: still, and the touch lets go 0.2 s later
    w.ctl[1].vel = {};
    const double down = w.t;
    w.Run(0.2);
    w.h.Touch(1, false, w.t);
    const double at = w.Until(1, Owner::Hand, 6);
    // back to hand when both hold: no touch 1.5 s (from down + 0.2) and still 3 s (from down)
    CHECK(at > 0 && std::fabs(at - (down + 3.0)) < 0.03, "set down: hand at %.3f s after, want 3.0", at - down);
    CHECK(!w.log.empty() && w.log.back().find("controller -> hand") != std::string::npos, "logged: %s",
          w.log.empty() ? "" : w.log.back().c_str());
}

void TestDeskKnock() {
    World w;
    w.Run(1);
    // two fast samples (16 ms): under the 100 ms rule
    for (int i = 0; i < 2; ++i) {
        w.ctl[0].vel = {0.8, 0, 0};
        w.Step();
    }
    w.ctl[0].vel = {};
    w.Run(1);
    CHECK(w.h.State(0) == Owner::Hand, "a knock stays hand, got %s", Name(w.h.State(0)));
    CHECK(w.log.empty(), "nothing logged, got %zu lines", w.log.size());
    // a spin over 1 rad/s for 120 ms: moving
    const double at = w.Until(0, Owner::Controller, 0.5, [&] { w.ctl[0].spin = 1.5; });
    CHECK(at > 0, "a 1.5 rad/s spin for 100 ms is a pickup");
}

void TestMovingNeedsTracking() {
    World w;
    w.ctl[1].tracked = false, w.ctl[1].lost = true;
    w.Run(0.5, [&] { w.ctl[1].vel = {2, 0, 0}; });
    CHECK(w.h.State(1) == Owner::Hand, "a tracking jump (not Running_OK) doesn't count as moving");
}

void TestPress() {
    World w;
    w.Run(0.5);
    w.h.Press(0, w.t);
    w.Step();
    CHECK(w.h.State(0) == Owner::Controller, "a press: controller at once");
    // no touch at all: back after the press hold (3 s)
    const double from = w.t;
    const double at = w.Until(0, Owner::Hand, 5);
    CHECK(at > 0 && std::fabs(at - from - 3.0) < 0.03, "press: hand %.3f s later, want 3.0", at - from);
}

void TestHeldByTheRing() {
    World w;
    w.Run(0.5);
    // the hand holds the controller without a sensor: both move together
    Hand hand;
    hand.side = 1, hand.velValid = true;
    w.hands = {hand};
    auto carry = [&] {
        w.ctl[1].vel = {0.12, 0.02, 0}, w.ctl[1].pos.x += 0.12 * kDt;
        w.hands[0].pos = {w.ctl[1].pos.x + 0.03, w.ctl[1].pos.y + 0.02, w.ctl[1].pos.z};
        w.hands[0].vel = {0.13, 0.02, 0};
    };
    const double start = w.t;
    const double at = w.Until(1, Owner::Controller, 1, carry);
    // 0.12 m/s is under the moving limit (0.15): only the together rule picks it up, after 200 ms
    CHECK(at > 0 && at - start >= 0.19 && at - start < 0.25, "held by the ring: controller after %.3f s", at - start);
    // held still in the hand (no sensor): the hand stays on it, so it stays controller
    w.ctl[1].vel = {}, w.hands[0].vel = {};
    w.Run(5);
    CHECK(w.h.State(1) == Owner::Controller, "held still with the hand on it: still controller");
    // the hand leaves: back to hand 3 s after the last motion... which was 5 s ago, so at once
    w.hands[0].pos.x += 0.3;
    w.Step();
    CHECK(w.h.State(1) == Owner::Hand, "hand gone from it: hand");
}

void TestNearControllerBlocks() {
    World w;
    w.Run(0.2);
    CHECK(w.h.NearController({0.3, 0.75, -0.35}), "5 cm from the right controller: near");
    CHECK(!w.h.NearController({0.3, 0.95, -0.4}), "20 cm above it: not near");
    // reaching for the resting controller: near but not moving together, so still hand
    Hand hand;
    hand.side = 0;  // ft-hands calls it the wrong side: position decides
    hand.pos = {0.32, 0.78, -0.4}, hand.velValid = true, hand.vel = {0.2, 0, 0};
    w.hands = {hand};
    w.Run(0.5);
    CHECK(w.h.State(1) == Owner::Hand, "a hand reaching for a resting controller doesn't make it controller");
    CHECK(w.h.NearController(hand.pos), "its gestures are blocked");
}

void TestChargingAndGone() {
    World w;
    w.h.Touch(0, true, 0);
    w.Run(0.5);
    CHECK(w.h.State(0) == Owner::Controller, "touched");
    w.ctl[0].charging = true;
    w.Step();
    CHECK(w.h.State(0) == Owner::Hand, "charging: hand at once");
    w.ctl[0].charging = false;
    w.Step();
    CHECK(w.h.State(0) == Owner::Controller, "off the charger and touched: controller");
    w.have[0] = false;
    w.Step();
    CHECK(w.h.State(0) == Owner::Hand, "disconnected: hand at once");
    w.have[0] = true;
    w.Step();
    w.h.Gone(0, w.t);
    w.Step();
    CHECK(w.h.State(0) == Owner::Hand, "gone from the web socket: hand");
}

void TestLostTracking() {
    World w;
    w.h.Touch(1, true, 0);
    w.Run(0.3);
    w.h.Touch(1, false, w.t);  // so only the lost rule can end it early
    w.Run(0.1);
    w.ctl[1].tracked = false, w.ctl[1].lost = true;
    Hand hand;
    hand.side = 1, hand.pos = {0.3, 1.2, -0.4};  // 45 cm above where it was last tracked
    w.hands = {hand};
    const double from = w.t;
    const double at = w.Until(1, Owner::Hand, 3);
    CHECK(at > 0 && std::fabs(at - from - 1.0) < 0.03, "lost 1 s with the hand away: hand after %.3f s", at - from);
}

void TestNoSocketHeldStill() {
    World w;
    w.h.Socket(false, 0);
    w.Run(1.5);
    CHECK(w.h.State(1) == Owner::Hand, "resting (no spread): hand");
    // held as still as possible: 2-3 mm of wobble
    int i = 0;
    const double at = w.Until(1, Owner::Controller, 2, [&] {
        w.ctl[1].pos.y = 0.75 + 0.003 * std::sin(++i * 0.3);
    });
    CHECK(at > 0, "held still (3 mm wobble) without the web socket: controller");
    // put down: resting noise 0.2 mm
    w.ctl[1].pos.y = 0.75;
    const double from = w.t;
    const double back = w.Until(1, Owner::Hand, 6, [&] { w.ctl[1].pos.y = 0.75 + 0.0002 * std::sin(++i); });
    CHECK(back > 0 && back - from < 4, "set down without the web socket: hand after %.2f s", back - from);
}

void TestMixedUse() {
    World w;
    w.h.Touch(0, true, 0);
    w.Run(1, [&] { w.ctl[0].vel = {0.5, 0, 0}; });
    CHECK(w.h.State(0) == Owner::Controller && w.h.State(1) == Owner::Hand, "left controller, right hand bare");
}

void TestOff() {
    World w;
    w.handsLive = false;
    w.Run(0.2);
    CHECK(w.h.State(0) == Owner::Off && w.h.State(1) == Owner::Off, "no hand tracking: off");
    w.h.Touch(1, true, w.t);
    w.Step();
    CHECK(w.h.State(1) == Owner::Controller, "a controller in use still reads controller");
}

}  // namespace

int main() {
    TestPickUpAndSetDown();
    TestDeskKnock();
    TestMovingNeedsTracking();
    TestPress();
    TestHeldByTheRing();
    TestNearControllerBlocks();
    TestChargingAndGone();
    TestLostTracking();
    TestNoSocketHeldStill();
    TestMixedUse();
    TestOff();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("handoff-test: ok\n");
    return 0;
}
