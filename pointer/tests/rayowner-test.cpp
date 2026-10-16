// Unit tests for the hand ray's owner, slots and aim lock (pointer/helper/rayowner.h), and a pinch
// bringing a ray back (hands/include/fh_ray.h's ShowForPinch): made-up timelines at the helper's
// 8 ms frame and ft-hands' 30 Hz.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/rayowner-test.cpp).
#include "pointer/helper/rayowner.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace rayown;
using fhray::kDeg;

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

constexpr double kDt = 0.008;
constexpr double kFrame = 1 / 30.0;

// The owner, stepped with each side's ray up or down (and allowed to point).
struct World {
    Owner o;
    double t = 0;
    bool shown[2] = {false, false}, may[2] = {true, true};
    std::string why[2] = {"palm", "palm"};
    std::vector<Change> changes;
    std::vector<std::string> log;
    explicit World(Params p = {}) : o(p) {}
    void Step() {
        o.Update(t, shown, may, why);
        for (const Change &c : o.changes) changes.push_back(c);
        for (const std::string &l : o.log) log.push_back(l);
        o.changes.clear(), o.log.clear();
        t += kDt;
    }
    void Run(double secs, const std::function<void()> &each = {}) {
        const double end = t + secs - 1e-9;
        while (t < end) {
            if (each) each();
            Step();
        }
    }
};

void TestFirstRaised() {
    World w;
    w.Run(0.2);
    CHECK(w.o.Side() == -1 && w.changes.empty(), "no ray: no owner");
    w.shown[1] = true;
    w.Step();
    CHECK(w.o.Side() == 1, "the first ray up takes it: %d", w.o.Side());
    CHECK(w.changes.size() == 1 && w.changes[0].from == -1 && w.changes[0].to == 1 && w.changes[0].why == "raised",
          "one change, none -> right (raised)");
    // the other one showing later doesn't take it
    w.shown[0] = true;
    w.Run(2);
    CHECK(w.o.Side() == 1 && w.changes.size() == 1, "a second ray up leaves the owner: %d, %zu changes", w.o.Side(),
          w.changes.size());
}

// The live complaint: both hands up, the owner's ray dropping for a moment as the fingers close.
void TestBriefDropKeepsTheOwner() {
    World w;
    w.shown[1] = true;
    w.Step();
    w.shown[0] = true;  // the left hand up too, all along
    w.Run(0.5);
    w.changes.clear();
    // ten drops of 0.3 s, one a second
    for (int i = 0; i < 10; ++i) {
        w.Run(0.7);
        w.shown[1] = false;
        w.Step();
        w.Run(0.29, [&] { CHECK(w.o.Side() == 1 && w.o.Waiting(), "right keeps it while its ray is down"); });
        w.shown[1] = true;
    }
    w.Run(0.1);
    CHECK(w.changes.empty(), "brief drops: no owner change (%zu)", w.changes.size());
    CHECK(!w.o.Waiting(), "back up: not waiting");
    int downs = 0, backs = 0;
    for (const std::string &l : w.log) {
        downs += l.find(": down (palm)") != std::string::npos;
        backs += l.find(": back after") != std::string::npos;
    }
    CHECK(downs == 10 && backs == 10, "each drop logs once down, once back: %d %d", downs, backs);

    // as it was (no grace): every drop handed the laser to the other hand, and it stayed there
    Params none;
    none.grace = 0;
    World old(none);
    old.shown[1] = true;
    old.Step();
    old.shown[0] = true;
    old.Run(0.5);
    old.shown[1] = false;
    old.Run(0.3);
    old.shown[1] = true;
    old.Run(0.5);
    CHECK(old.o.Side() == 0, "without the grace a drop gives the other hand the ray: %d", old.o.Side());
}

void TestGraceRunsOut() {
    World w;
    w.shown[0] = w.shown[1] = true;
    w.Step();
    CHECK(w.o.Side() == 0 || w.o.Side() == 1, "an owner");
    const int o = w.o.Side(), other = 1 - o;
    w.Run(0.5);
    w.changes.clear();
    w.shown[o] = false;
    const double from = w.t;
    double took = -1;
    w.Run(1, [&] {
        if (took < 0 && w.o.Side() == other) took = w.t;
    });
    CHECK(took >= from + 0.5 - 1e-9 && took < from + 0.5 + 2 * kDt, "the other hand's ray takes it after the grace: %.3f",
          took - from);
    CHECK(w.changes.size() == 1 && w.changes[0].why.find("down 0.5 s (palm)") != std::string::npos &&
              w.changes[0].why.find("up: ") != std::string::npos,
          "why: %s", w.changes.empty() ? "-" : w.changes[0].why.c_str());
    // with no other ray up, the owner goes after the grace
    World v;
    v.shown[1] = true;
    v.Run(0.3);
    v.shown[1] = false;
    v.Run(0.45);
    CHECK(v.o.Side() == 1, "within the grace: still right");
    v.Run(0.1);
    CHECK(v.o.Side() == -1 && v.changes.back().to == -1 && !v.changes.back().off, "after it: none, not off");
}

void TestFreshRaiseTakesAtOnce() {
    World w;
    w.shown[1] = true;
    w.Run(0.3);
    w.shown[1] = false;  // the right hand comes down
    w.Run(0.1);
    w.shown[0] = true;  // and the left goes up (its ray shows, after the right's went)
    w.Step();
    CHECK(w.o.Side() == 0, "a ray raised while the owner's is down takes it at once: %d", w.o.Side());
    CHECK(w.changes.back().why.find("left raised") != std::string::npos, "why: %s", w.changes.back().why.c_str());
}

void TestOffLeavesAtOnce() {
    World w;
    w.shown[0] = w.shown[1] = true;
    w.Run(0.2);
    const int o = w.o.Side();
    w.may[o] = false, w.shown[o] = false;  // its controller in use: fh_ray hides it ("off")
    w.Step();
    CHECK(w.changes.back().from == o && w.changes.back().to == -1 && w.changes.back().off, "off: leaves at once, off");
    w.Step();
    CHECK(w.o.Side() == 1 - o, "then the other ray up takes it: %d", w.o.Side());
    // near a panel (ft-screens' poke): at once too, not off
    World v;
    v.shown[1] = true;
    v.Run(0.2);
    v.shown[1] = false, v.why[1] = "near";
    v.Step();
    CHECK(v.o.Side() == -1 && !v.changes.back().off && v.changes.back().why == "near a panel", "near: leaves at once");
}

void TestPinches() {
    World w;
    w.shown[0] = w.shown[1] = true;
    w.Step();
    const int o = w.o.Side(), other = 1 - o;
    w.changes.clear();
    CHECK(w.o.Pinched(o, true) == Owner::Pinch::Ray, "the owner's pinch: a ray pinch");
    CHECK(w.o.Pinched(other, false) == Owner::Pinch::None, "the other hand without a ray: not a ray pinch");
    CHECK(w.o.Side() == o, "and it leaves the owner");
    CHECK(w.o.Pinched(other, true) == Owner::Pinch::Take, "the other hand with its ray up: takes it");
    CHECK(w.o.Side() == other && w.o.changes.back().why.find("pinched") != std::string::npos, "taken by the pinch");
    w.o.changes.clear();
    // the owner's ray down, a pinch on it within the grace: back, a ray pinch, no change
    w.shown[other] = false;
    w.Run(0.2);
    CHECK(w.o.Waiting(), "waiting");
    CHECK(w.o.Pinched(other, true) == Owner::Pinch::Ray && !w.o.Waiting(), "the owner's pinch brings it back");
    w.shown[other] = true;
    w.Run(1);
    CHECK(w.o.Side() == other && w.changes.empty(), "still the owner, no change");
    // no owner at all: a pinch with a ray that can come up takes it
    World v;
    CHECK(v.o.Pinched(1, true) == Owner::Pinch::Take && v.o.Side() == 1, "no owner: the pinch takes it");
    v.o.Drop("the mouse");
    CHECK(v.o.Side() == -1 && v.o.changes.back().why == "the mouse", "dropped");
}

void TestSlots() {
    Params p;
    Slots sl(p);
    int pick[2];
    double t = 0;
    std::vector<SeenHand> hands = {{3, 1}, {4, 0}};
    sl.Update(t, hands, pick);
    CHECK(pick[0] == 1 && pick[1] == 0, "by ft-hands' call: %d %d", pick[0], pick[1]);
    CHECK(sl.SideOf(3) == 1 && sl.SideOf(4) == 0 && sl.SideOf(9) == -1, "SideOf");
    // the calls swap for 0.2 s: each ray keeps its hand
    for (int i = 0; i < 6; ++i) {
        t += kFrame;
        sl.Update(t, {{3, 0}, {4, 1}}, pick);
        CHECK(pick[1] == 0 && pick[0] == 1, "a brief swap: the rays keep their hands (%d %d)", pick[0], pick[1]);
    }
    t += kFrame;
    sl.Update(t, hands, pick);
    CHECK(pick[0] == 1 && pick[1] == 0, "back: unchanged");
    // ft-hands settles on the other call: after 0.3 s the rays move
    double moved = -1;
    const double from = t + kFrame;
    for (int i = 0; i < 20 && moved < 0; ++i) {
        t += kFrame;
        sl.Update(t, {{3, 0}, {4, 1}}, pick);
        if (pick[0] == 0) moved = t;
    }
    CHECK(moved >= from + 0.3 - 1e-9 && moved < from + 0.3 + kFrame + 1e-9, "a held call moves the rays after 0.3 s: %.3f",
          moved - from);
    CHECK(pick[1] == 1 && sl.SideOf(3) == 0 && sl.SideOf(4) == 1, "and both follow the call (%d %d)", pick[0], pick[1]);
    // both called right: the right ray keeps its hand, the left keeps its own for the hold
    Slots d(p);
    t = 0;
    d.Update(t, {{7, 0}, {8, 1}}, pick);
    t += kFrame;
    d.Update(t, {{7, 1}, {8, 1}}, pick);
    CHECK(pick[0] == 0 && pick[1] == 1, "both called right: no hand changes ray (%d %d)", pick[0], pick[1]);
    // one hand gone, a new one called that side
    t += kFrame;
    d.Update(t, {{8, 1}, {9, 0}}, pick);
    CHECK(pick[0] == 1 && pick[1] == 0 && d.SideOf(9) == 0, "a new hand takes the free ray (%d %d)", pick[0], pick[1]);
    // a lone hand's call flips for a frame: no ray changes hand
    Slots one(p);
    t = 0;
    one.Update(t, {{5, 1}}, pick);
    t += kFrame;
    one.Update(t, {{5, 0}}, pick);
    CHECK(pick[1] == 0 && pick[0] == -1, "a lone hand's flip: still the right ray's (%d %d)", pick[0], pick[1]);
}

fhray::Ray RayAt(double yawDeg) {
    fhray::Ray r;
    r.dir = fhray::Dir(yawDeg * kDeg, 0);
    r.origin = {0.2, 1.3, -0.4};
    return r;
}
double YawDeg(const fhray::Ray &r) { return fhray::YawOf(r.dir) / kDeg; }

void TestAimLock() {
    // pointing at yaw 0 with the fingers open (6 cm), then closing over 150 ms, turning the ray 3 deg
    // down to the begin (2 cm, when ft-hands sees the pinch)
    AimHistory h;
    const double begin = 2.0;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) {
        const double closing = std::clamp((t - (begin - 0.15)) / 0.15, 0.0, 1.0);
        h.Add(t, RayAt(-3 * closing), true, 0.06 - 0.04 * closing);
    }
    fhray::Ray r;
    double age = -1;
    CHECK(h.Lock(begin, 0.035, r, age), "a lock");
    CHECK(std::fabs(YawDeg(r)) < 1.3, "the aim before the fingers closed far: %.2f deg", YawDeg(r));
    CHECK(age > 0.05 && age < 0.15, "from the frame the tips were last apart: %.3f s", age);
    // without the distance (no skeleton): the frame 0.1 s before
    AimHistory n;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) n.Add(t, RayAt(-20 * (t - 1)), true, -1);
    CHECK(n.Lock(begin, 0.035, r, age), "a lock without distances");
    CHECK(age >= 0.1 - 1e-9 && age < 0.1 + kFrame, "0.1 s back: %.3f", age);
    // fingers resting nearly closed (never past end_m in the window): 0.1 s back too
    AimHistory c;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) c.Add(t, RayAt(0), true, 0.025);
    CHECK(c.Lock(begin, 0.035, r, age) && age >= 0.1 - 1e-9 && age < 0.1 + kFrame, "resting closed: %.3f", age);
    // frames with the ray down don't count; none up in the window: no lock
    AimHistory d;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) d.Add(t, RayAt(t > 1.8 ? 10 : 0), t > 1.8, 0.06);
    CHECK(d.Lock(begin, 0.035, r, age) && std::fabs(YawDeg(r) - 10) < 1e-6, "only frames with the ray up");
    AimHistory e;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) e.Add(t, RayAt(0), t < 1.5, 0.06);
    CHECK(!e.Lock(begin, 0.035, r, age), "nothing up within 0.35 s: no lock");
    // the ray just showed (less than 0.1 s of it, fingers closed): its first frame
    AimHistory f;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) f.Add(t, RayAt(t), t > begin - 0.07, 0.01);
    CHECK(f.Lock(begin, 0.035, r, age) && age <= 0.07 + 1e-9 && age > 0.03, "a ray just up: its oldest frame (%.3f)", age);
    // a middle pinch: the thumb closing on the middle tip with the index held out (its distance
    // never closes): the lock follows the middle's distance, as the index's for a pinch
    AimHistory m;
    for (double t = 1.0; t <= begin + 1e-9; t += kFrame) {
        const double closing = std::clamp((t - (begin - 0.15)) / 0.15, 0.0, 1.0);
        m.Add(t, RayAt(-3 * closing), true, 0.07, 0.06 - 0.04 * closing);
    }
    CHECK(m.Lock(begin, 0.035, r, age, 1) && std::fabs(YawDeg(r)) < 1.3 && age > 0.05 && age < 0.15,
          "a middle pinch: from before the middle closed (%.2f deg, %.3f s)", YawDeg(r), age);
    CHECK(m.Lock(begin, 0.035, r, age, 0) && age < kFrame, "by the index's distance: the begin's own frame (%.3f s)", age);
}

// fh_ray's ShowForPinch: a pinch brings back a ray that just dropped, not one sent away.
fhray::HandFrame Pointing(double t) {
    fhray::HandFrame f;
    f.t = t, f.stereo = true;
    f.knuckle = {0.18, 1.35, -0.45};
    f.wrist = {0.17, 1.33, -0.36};
    f.palmNormal = fhray::Norm({0, -0.5, -1});
    f.elbow = {0.2, 1.15, -0.1};
    return f;
}

void TestShowForPinch() {
    const fhray::V3 eye{0, 1.6, 0}, fwd{0, 0, -1};
    fhray::Body b;
    fhray::HandRay r(1);
    double t = 0;
    auto run = [&](double secs, const std::function<fhray::HandFrame(double)> &make, bool may = true) {
        const double end = t + secs - 1e-9;
        while (t < end) {
            b.Update(t, eye, fwd);
            r.Frame(b, make(t), may);
            t += kFrame;
        }
    };
    auto palmIn = [](double tt) {
        fhray::HandFrame f = Pointing(tt);
        f.palmNormal = {0, 0, 1};
        return f;
    };
    run(0.5, Pointing);
    CHECK(r.Shown(), "pointing: shown");
    run(0.4, palmIn);
    CHECK(!r.Shown() && r.event == "hide palm", "palm toward you: hidden (%s)", r.event.c_str());
    CHECK(r.CanShowForPinch(t, 0.5), "just hid: a pinch may bring it back");
    r.ShowForPinch();
    r.Hold(true, t);
    CHECK(r.Shown(), "brought back");
    run(1, palmIn);
    CHECK(r.Shown(), "held by the pinch whatever the pose");
    r.Hold(false, t);
    run(0.8, palmIn);
    CHECK(!r.Shown(), "after the pinch: hides again");
    run(0.6, palmIn);
    CHECK(!r.CanShowForPinch(t, 0.5), "hid more than the grace ago: no");
    // hidden for "off" (its controller in use): never
    fhray::HandRay q(1);
    t = 0;
    while (t < 0.5) b.Update(t, eye, fwd), q.Frame(b, Pointing(t), true), t += kFrame;
    b.Update(t, eye, fwd), q.Frame(b, Pointing(t), false);
    CHECK(!q.Shown() && !q.CanShowForPinch(t, 0.5), "hidden off: a pinch can't bring it back");
    // not shown yet but every show test passing (raised and pinched within 120 ms): yes
    fhray::HandRay s(1);
    t = 0;
    b.Update(t, eye, fwd), s.Frame(b, Pointing(t), true);
    CHECK(!s.Shown() && s.CanShowForPinch(t, 0.5), "on its way up: a pinch may show it");
    fhray::HandRay lap(1);
    lap.Frame(b, palmIn(0), true);
    CHECK(!lap.CanShowForPinch(0, 0.5), "never up, failing a test: no");
}

}  // namespace

int main() {
    TestFirstRaised();
    TestBriefDropKeepsTheOwner();
    TestGraceRunsOut();
    TestFreshRaiseTakesAtOnce();
    TestOffLeavesAtOnce();
    TestPinches();
    TestSlots();
    TestAimLock();
    TestShowForPinch();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("rayowner-test: ok\n");
    return 0;
}
