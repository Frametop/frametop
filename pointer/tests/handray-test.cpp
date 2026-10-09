// Unit tests for the hand ray (hands/include/fh_ray.h): the body model, the pointing gate's
// timing and tests, and the filter, with made-up hands at ft-hands' 30 Hz.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/handray-test.cpp).
#include "hands/include/fh_ray.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace fhray;

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

const V3 kEye{0, 1.6, 0};
const V3 kForward{0, 0, -1};
constexpr double kFrame = 1 / 30.0;

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// A right hand pointing ahead: knuckles 0.25 m below the eyes, 0.45 m ahead, palm facing away
// and down.
HandFrame Pointing(double t) {
    HandFrame f;
    f.t = t, f.stereo = true;
    f.knuckle = {0.18, 1.35, -0.45};
    f.wrist = {0.17, 1.33, -0.36};
    f.palmNormal = Norm({0, -0.5, -1});
    f.elbow = {0.2, 1.15, -0.1};
    return f;
}

void TestBody() {
    Body b;
    b.Update(0, kEye, kForward);
    const V3 s = b.Shoulder(1);
    CHECK(Near(s.x, 0.17, 1e-9) && Near(s.y, 1.37, 1e-9) && Near(s.z, 0.08, 1e-9), "right shoulder at (%.3f %.3f %.3f)",
          s.x, s.y, s.z);
    CHECK(Near(b.Shoulder(0).x, -0.17, 1e-9), "left shoulder");
    // the head turns 90 deg left at once: the torso lags at most 30 deg, then follows
    b.Update(0.01, kEye, Dir(90 * kDeg, 0));
    CHECK(Near(b.TorsoYaw() / kDeg, 60, 0.5), "torso lags at most 30 deg: %.1f", b.TorsoYaw() / kDeg);
    for (int i = 1; i <= 300; ++i) b.Update(0.01 + i * 0.01, kEye, Dir(90 * kDeg, 0));
    CHECK(Near(b.TorsoYaw() / kDeg, 90, 0.5), "and catches up: %.2f", b.TorsoYaw() / kDeg);
    // nodding doesn't move the shoulders
    Body c;
    c.Update(0, kEye, kForward);
    const V3 s0 = c.Shoulder(1);
    c.Update(0.01, kEye, Dir(0, -40 * kDeg));
    CHECK(Len(c.Shoulder(1) - s0) < 1e-9, "a nod leaves the shoulders");
}

// Runs frames from `make` (frame time -> frame) for `secs`; returns the time it first showed (or
// hid, with wantShown false), or -1.
double RunUntil(HandRay &r, Body &b, double &t, double secs, bool wantShown, const std::function<HandFrame(double)> &make,
                bool sideIsHand = true) {
    const double end = t + secs;
    while (t < end) {
        b.Update(t, kEye, kForward);
        r.Frame(b, make(t), sideIsHand);
        if (r.Shown() == wantShown) return t;
        t += kFrame;
    }
    return -1;
}

void TestGateShowHide() {
    Body b;
    HandRay r(1);
    double t = 0;
    const double shown = RunUntil(r, b, t, 1, true, Pointing);
    CHECK(shown >= 0.12 - 1e-9 && shown < 0.12 + kFrame + 1e-9, "a pointing hand shows after 120 ms: %.3f", shown);
    CHECK(r.event == "show", "show event: %s", r.event.c_str());
    // its ray: from the right shoulder through the knuckles
    const V3 d = r.Raw().dir;
    CHECK(d.z < -0.8 && d.y < 0.05, "aimed ahead: (%.2f %.2f %.2f)", d.x, d.y, d.z);
    // the hand drops to 0.35 m below the eyes: inside the keep band, stays
    auto lower = [](double y) {
        return [y](double tt) {
            HandFrame f = Pointing(tt);
            f.knuckle.y = 1.6 - y, f.wrist.y = 1.6 - y - 0.02;
            return f;
        };
    };
    t += kFrame;
    CHECK(RunUntil(r, b, t, 1, false, lower(0.35)) < 0, "0.35 m below the eyes: still shown");
    const double from = t;
    const double hid = RunUntil(r, b, t, 1, false, lower(0.45));
    CHECK(hid > 0 && hid - from >= 0.25 - 1e-9 && hid - from < 0.25 + 2 * kFrame, "0.45 m below: hidden after %.3f s",
          hid - from);
    CHECK(r.event.rfind("hide raised", 0) == 0, "hide reason: %s", r.event.c_str());
}

void TestLapPinch() {
    Body b;
    HandRay r(0);
    double t = 0;
    auto lap = [](double tt) {
        HandFrame f;
        f.t = tt, f.stereo = true;
        f.knuckle = {-0.15, 1.0, -0.35};  // 0.6 m below the eyes
        f.wrist = {-0.15, 1.0, -0.27};
        f.palmNormal = {0, 1, 0};  // palm up
        return f;
    };
    CHECK(RunUntil(r, b, t, 2, true, lap) < 0, "a hand in the lap never shows");
    CHECK(r.Why().find("raised") != std::string::npos && r.Why().find("palm") != std::string::npos, "why: %s",
          r.Why().c_str());
}

void TestPalmTowardYou() {
    Body b;
    HandRay r(1);
    double t = 0;
    RunUntil(r, b, t, 1, true, Pointing);
    t += kFrame;
    const double from = t;
    const double hid = RunUntil(r, b, t, 1, false, [](double tt) {
        HandFrame f = Pointing(tt);
        f.palmNormal = {0, 0, 1};
        return f;
    });
    CHECK(hid > 0 && hid - from < 0.3, "palm toward you hides it (%.3f s)", hid - from);
}

void TestTrackingAndControllerAndNear() {
    Body b;
    double t = 0;
    {
        HandRay r(1);
        CHECK(RunUntil(r, b, t, 1, true, [](double tt) {
                  HandFrame f = Pointing(tt);
                  f.stereo = false;
                  return f;
              }) < 0,
              "one camera only: never shows");
    }
    {
        HandRay r(1);
        t = 0;
        RunUntil(r, b, t, 1, true, Pointing);
        t += kFrame;
        // a short one-camera stretch keeps it
        CHECK(RunUntil(r, b, t, 0.3, false, [](double tt) {
                  HandFrame f = Pointing(tt);
                  f.stereo = false;
                  return f;
              }) < 0,
              "a one-camera stretch keeps it");
        // the side turns controller: hides at once
        r.Frame(b, Pointing(t), false);
        CHECK(!r.Shown() && r.event == "hide off", "controller: hidden at once (%s)", r.event.c_str());
    }
    {
        HandRay r(1);
        t = 0;
        RunUntil(r, b, t, 1, true, Pointing);
        HandFrame f = Pointing(t + kFrame);
        f.near = true;
        r.Frame(b, f, true);
        CHECK(!r.Shown() && r.event == "hide near", "near: hidden at once");
        // and doesn't show again while near
        t += 2 * kFrame;
        CHECK(RunUntil(r, b, t, 0.5, true, [](double tt) {
                  HandFrame g = Pointing(tt);
                  g.near = true;
                  return g;
              }) < 0,
              "no ray while near");
    }
    {
        HandRay r(1);
        t = 0;
        RunUntil(r, b, t, 1, true, Pointing);
        const double last = t;
        r.Tick(last + 0.2, true);
        CHECK(r.Shown(), "200 ms without a frame: kept");
        r.Tick(last + 0.26, true);
        CHECK(!r.Shown() && r.event == "hide lost", "260 ms without a frame: lost");
    }
}

void TestHold() {
    Body b;
    HandRay r(1);
    double t = 0;
    RunUntil(r, b, t, 1, true, Pointing);
    r.Hold(true, t);
    t += kFrame;
    // a pinch dragged down into the lap keeps the ray while held
    auto low = [](double tt) {
        HandFrame f = Pointing(tt);
        f.knuckle.y = 1.0;
        return f;
    };
    CHECK(RunUntil(r, b, t, 1, false, low) < 0, "held: stays up");
    r.Hold(false, t);
    const double from = t;
    const double hid = RunUntil(r, b, t, 1, false, low);
    CHECK(hid > 0 && hid - from >= 0.25 - 1e-9 && hid - from < 0.55, "released: hides after the hold (%.3f s)",
          hid - from);
}

void TestFilter() {
    // 2 mm of landmark jitter on K at 0.45 m from the shoulder: about 0.25 deg of raw wobble
    std::mt19937 rng(1);
    std::normal_distribution<double> n(0, 0.002);
    Body b;
    HandRay r(1);
    double t = 0;
    std::vector<double> rawYaw, outYaw;
    for (int i = 0; i < 300; ++i, t += kFrame) {
        b.Update(t, kEye, kForward);
        HandFrame f = Pointing(t);
        f.knuckle = f.knuckle + V3{n(rng), n(rng), n(rng)};
        r.Frame(b, f, true);
        if (r.Shown() && i > 60) rawYaw.push_back(YawOf(r.Raw().dir) / kDeg), outYaw.push_back(YawOf(r.Current().dir) / kDeg);
    }
    auto sd = [](const std::vector<double> &v) {
        double m = 0, q = 0;
        for (double x : v) m += x;
        m /= double(v.size());
        for (double x : v) q += (x - m) * (x - m);
        return std::sqrt(q / double(v.size()));
    };
    const double a = sd(rawYaw), c = sd(outYaw);
    std::printf("filter: yaw sd raw %.3f deg (prediction included), filtered %.3f deg\n", a, c);
    CHECK(c < 0.1, "resting wobble under 0.1 deg after the filter: %.3f", c);
    CHECK(c < a / 3, "the filter takes most of it out: %.3f of %.3f", c, a);
    // a deliberate 20 deg sweep in 0.3 s gets through (lag under 3 deg at the end + 0.2 s)
    const double y0 = YawOf(r.Current().dir) / kDeg;
    for (int i = 0; i < 15; ++i, t += kFrame) {
        b.Update(t, kEye, kForward);
        HandFrame f = Pointing(t);
        const double k = std::min(1.0, i * kFrame / 0.3);
        const V3 s = b.Shoulder(1);
        const V3 d = Dir(-20 * k * kDeg + YawOf(Pointing(0).knuckle - s), PitchOf(Pointing(0).knuckle - s));
        f.knuckle = s + d * Len(Pointing(0).knuckle - s);
        r.Frame(b, f, true);
    }
    const double moved = y0 - YawOf(r.Current().dir) / kDeg;
    CHECK(moved > 17, "a 20 deg sweep: the filtered ray followed %.1f deg", moved);
}

}  // namespace

int main() {
    TestBody();
    TestGateShowHide();
    TestLapPinch();
    TestPalmTowardYou();
    TestTrackingAndControllerAndNear();
    TestHold();
    TestFilter();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("handray-test: ok\n");
    return 0;
}
