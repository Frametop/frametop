// Unit tests for the smoothing of what hands move (hands/include/fh_smooth.h) and the hand ray's
// use of it (hands/include/fh_ray.h): made-up hands at ft-hands' 30 Hz, read at the display's
// 90 Hz. Jitter of a still noisy hand, lag behind a fast move, no overshoot when it stops, no
// steps between tracker frames, a fresh start for a new grab, a drag's precision, and the steady
// shoulder during a ray pinch.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/smooth-test.cpp).
#include "hands/include/fh_ray.h"
#include "hands/include/fh_smooth.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace fhsmooth;

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

constexpr double kDeg = M_PI / 180, kFrame = 1 / 30.0, kTick = 1 / 90.0, kLatency = 0.035;
// The ray's defaults (fh_ray.h's Params).
const Params kRay{0.8, 0.06, 1.0};

V3 Yaw(double deg) { return {-std::sin(deg * kDeg), 0, -std::cos(deg * kDeg)}; }
double YawDeg(V3 d) { return std::atan2(-d.x, -d.z) / kDeg; }
double YawDeg(const fhray::Ray &r) { return YawDeg(r.dir); }

double Sd(const std::vector<double> &v) {
    double m = 0, q = 0;
    for (double x : v) m += x;
    m /= double(v.size());
    for (double x : v) q += (x - m) * (x - m);
    return std::sqrt(q / double(v.size()));
}

// A direction filter fed `yaw(t)` (degrees) at 30 Hz, each sample arriving kLatency after its
// capture, read every 90 Hz tick: the ticks' times and yaws.
struct Run {
    std::vector<double> t, out;
};
template <class F>
Run Drive(Direction &f, double from, double to, F yaw, std::mt19937 *rng = nullptr, double noise = 0) {
    std::normal_distribution<double> n(0, noise > 0 ? noise : 1);
    Run r;
    double next = from;
    for (double tick = from; tick < to; tick += kTick) {
        while (next + kLatency <= tick + 1e-9) {
            f.Sample(Yaw(yaw(next) + (rng ? n(*rng) : 0)), next, next + kLatency);
            next += kFrame;
        }
        if (!f.Ready()) continue;
        r.t.push_back(tick), r.out.push_back(YawDeg(f.At(tick)));
    }
    return r;
}

void TestStillJitter() {
    // a still hand: 0.25 deg of raw wobble on the ray (2 mm on the knuckles at 0.45 m)
    std::mt19937 rng(1);
    Direction f(kRay);
    std::vector<double> raw;
    std::normal_distribution<double> n(0, 0.25);
    for (int i = 0; i < 300; ++i) raw.push_back(n(rng));
    std::mt19937 rng2(1);
    Run r = Drive(f, 0, 10, [](double) { return 0.0; }, &rng2, 0.25);
    const std::vector<double> settled(r.out.begin() + 180, r.out.end());
    const double a = Sd(raw), c = Sd(settled);
    std::printf("still: raw sd %.3f deg, at 90 Hz %.4f deg (%.0fx less)\n", a, c, a / c);
    CHECK(c < a / 3, "a still hand's jitter down 3x at least: %.4f of %.3f", c, a);
    // a drag's precision (fh_ray's holdMinCutoff 0.5): less still
    Direction h({0.5, 0.06, 1.0});
    std::mt19937 rng3(1);
    Run rh = Drive(h, 0, 10, [](double) { return 0.0; }, &rng3, 0.25);
    const double ch = Sd(std::vector<double>(rh.out.begin() + 180, rh.out.end()));
    CHECK(ch < c, "held: less jitter still (%.4f against %.4f)", ch, c);
}

void TestFastMove() {
    // a deliberate 30 deg turn at 100 deg/s, then still
    Direction f(kRay);
    auto yaw = [](double t) { return t < 1 ? 0.0 : std::min(30.0, (t - 1) * 100); };
    Run r = Drive(f, 0, 3, yaw);
    // lag while it turns: the delay behind the hand's yaw at mid-turn, latency included
    double atMid = 0;
    for (size_t i = 0; i < r.t.size(); ++i)
        if (r.t[i] >= 1.15 && atMid == 0) atMid = r.out[i];
    const double lagMs = (1.15 - (1 + atMid / 100)) * 1000;
    std::printf("fast: 100 deg/s turn, %.0f ms behind at mid-turn (%.0f ms of it the latency)\n", lagMs, kLatency * 1000);
    CHECK(lagMs < 120, "under 120 ms behind a fast turn: %.0f", lagMs);
    // ...and within 1 deg of where it stopped 150 ms after the hand did
    double at = 0;
    for (size_t i = 0; i < r.t.size(); ++i)
        if (r.t[i] <= 1.3 + 0.15) at = r.out[i];
    CHECK(at > 29, "150 ms after the turn ended: at %.2f of 30 deg", at);
    // no overshoot: never past 30, never back
    double most = -1, back = 0;
    for (size_t i = 0; i < r.out.size(); ++i) {
        most = std::max(most, r.out[i]);
        if (i && r.out[i] < r.out[i - 1] - 1e-9) back = std::max(back, r.out[i - 1] - r.out[i]);
    }
    CHECK(most <= 30 + 1e-6, "no overshoot: reached %.4f of 30", most);
    CHECK(back < 1e-6, "never turns back: %.6f", back);
}

void TestNoSteps() {
    // a steady 20 deg/s turn sampled at 30 Hz, read at 90 Hz: the ticks move evenly (the filter
    // before this held still for two ticks and jumped on the third)
    Direction f(kRay);
    Run r = Drive(f, 0, 3, [](double t) { return 20 * t; });
    std::vector<double> steps;
    for (size_t i = 1; i < r.out.size(); ++i)
        if (r.t[i] > 1.5) steps.push_back(r.out[i] - r.out[i - 1]);
    double lo = 1e9, hi = 0, m = 0;
    for (double s : steps) lo = std::min(lo, s), hi = std::max(hi, s), m += s / double(steps.size());
    std::printf("steady turn: ticks move %.3f-%.3f deg (mean %.3f; a held filter: 0 or %.3f)\n", lo, hi, m, 20 * kFrame);
    CHECK(lo > 0.7 * m && hi < 1.3 * m, "even steps at the display's rate: %.3f-%.3f around %.3f", lo, hi, m);
}

void TestReset() {
    // a new grab starts from the hand as it is, not from where the last one ended
    Point3 p({1.0, 2.0, 1.0});
    for (int i = 0; i < 30; ++i) p.Sample({0.1, 0, 0}, i * kFrame, i * kFrame);
    p.Reset();
    p.Sample({0.5, 0.2, -0.3}, 2, 2);
    const V3 v = p.At(2.01);
    CHECK(Len(v - V3{0.5, 0.2, -0.3}) < 1e-9, "after Reset the first sample is the output: (%.3f %.3f %.3f)", v.x, v.y, v.z);
    // a settings change (a drag's precision) doesn't move the output
    Point3 q({1.0, 2.0, 1.0});
    q.Sample({0, 0, 0}, 0, 0), q.Sample({0.01, 0, 0}, kFrame, kFrame);
    const V3 before = q.At(0.05);
    q.Set({0.3, 2.0, 1.0});
    const V3 after = q.At(0.05);
    CHECK(Len(after - before) < 1e-12, "Set doesn't jump the output");
}

void TestPointJitter() {
    // a pinch point held still: 3 mm of noise per axis (ft-hands' depth), read at 90 Hz
    std::mt19937 rng(2);
    std::normal_distribution<double> n(0, 0.003);
    Point3 p({1.0, 2.0, 1.0});
    std::vector<double> raw, out;
    double next = 0;
    for (double tick = 0; tick < 10; tick += kTick) {
        while (next + kLatency <= tick) {
            const V3 v{n(rng), n(rng), n(rng)};
            raw.push_back(v.x);
            p.Sample(v, next, next + kLatency);
            next += kFrame;
        }
        if (p.Ready() && tick > 2) out.push_back(p.At(tick).x);
    }
    const double a = Sd(raw), c = Sd(out);
    std::printf("pinch point still: raw sd %.2f mm, at 90 Hz %.2f mm\n", a * 1000, c * 1000);
    CHECK(c < a / 3, "3 mm of noise down 3x at least: %.2f mm", c * 1000);
}

void TestDirectionWrap() {
    // straight behind (yaw +-180) and straight up: no angle to wrap, no pole
    Direction f(kRay);
    Run r = Drive(f, 0, 2, [](double t) { return 170 + 20 * t; });   // through 180
    double worst = 0;
    for (size_t i = 1; i < r.out.size(); ++i) worst = std::max(worst, std::fabs(std::remainder(r.out[i] - r.out[i - 1], 360.0)));
    CHECK(worst < 1, "through yaw 180 without a jump: %.2f deg in a tick", worst);
    Direction up(kRay);
    up.Sample({0, 1, 0}, 0, 0);
    up.Sample(Norm({0.01, 1, 0}), kFrame, kFrame);
    const V3 u = up.At(0.2);
    CHECK(std::fabs(Len(u) - 1) < 1e-9 && u.y > 0.999, "straight up stays a unit vector near up");
}

// The hand ray (fh_ray.h): a right hand pointing ahead; the head steps sideways.
fhray::HandFrame Pointing(double t) {
    fhray::HandFrame f;
    f.t = t, f.stereo = true;
    f.knuckle = {0.18, 1.35, -0.45};
    f.wrist = {0.17, 1.33, -0.36};
    f.palmNormal = Norm({0, -0.5, -1});
    f.elbow = {0.2, 1.15, -0.1};
    return f;
}

void TestSteadyShoulder() {
    // A ray pinch held: the head moves 4 cm sideways (a lean, a glance), the hand stays. Without
    // the steady shoulder the ray turns about 5 deg; with it, it holds.
    for (int steady = 0; steady < 2; ++steady) {
        fhray::Params p;
        p.holdShoulder = steady;
        fhray::Body b(p);
        fhray::HandRay r(1, p);
        double t = 0;
        for (int i = 0; i < 30; ++i, t += kFrame) b.Update(t, {0, 1.6, 0}, {0, 0, -1}), r.Frame(b, Pointing(t), true);
        CHECK(r.Shown(), "the ray shows");
        const double y0 = YawDeg(r.At(t));
        r.Hold(true, t);
        for (int i = 0; i < 45; ++i, t += kFrame) {
            const double x = 0.04 * std::min(1.0, i / 15.0);
            b.Update(t, {x, 1.6, 0}, {0, 0, -1}), r.Frame(b, Pointing(t), true);
        }
        const double turned = std::fabs(YawDeg(r.At(t)) - y0);
        if (steady) CHECK(turned < 0.2, "steady shoulder: the head's lean leaves the held ray (%.2f deg)", turned);
        else CHECK(turned > 3, "without: the lean turns the ray (%.2f deg)", turned);
        // released: hideFor (0.25 s) on, it eases to the body model's shoulder over 0.5 s
        r.Hold(false, t);
        const double y1 = YawDeg(r.At(t));
        double worst = 0, last = y1;
        for (int i = 0; i < 60; ++i, t += kFrame) {
            b.Update(t, {0.04, 1.6, 0}, {0, 0, -1}), r.Frame(b, Pointing(t), true);
            for (int k = 0; k < 3; ++k) {
                const double y = YawDeg(r.At(t + k * kTick));
                worst = std::max(worst, std::fabs(y - last)), last = y;
            }
        }
        fhray::HandRay live(1, p);
        fhray::Body lb(p);
        double tl = 0;
        for (int i = 0; i < 60; ++i, tl += kFrame) lb.Update(tl, {0.04, 1.6, 0}, {0, 0, -1}), live.Frame(lb, Pointing(tl), true);
        if (steady) {
            CHECK(std::fabs(YawDeg(r.At(t)) - YawDeg(live.At(tl))) < 0.1, "back on the body model after the release: %.2f vs %.2f",
                  YawDeg(r.At(t)), YawDeg(live.At(tl)));
            CHECK(worst < 0.25, "...without a jump: %.3f deg in a tick at most", worst);
        }
    }
}

void TestRayBetweenFrames() {
    // the ray read between tracker frames moves on (the helper draws it at its own rate)
    fhray::Body b;
    fhray::HandRay r(1);
    double t = 0;
    for (int i = 0; i < 30; ++i, t += kFrame) b.Update(t, {0, 1.6, 0}, {0, 0, -1}), r.Frame(b, Pointing(t), true);
    fhray::HandFrame f = Pointing(t);
    f.knuckle = f.knuckle + V3{0.03, 0, 0};   // the hand moved 3 cm right
    b.Update(t, {0, 1.6, 0}, {0, 0, -1}), r.Frame(b, f, true, t + kLatency);
    const double a = YawDeg(r.At(t + kLatency)), m = YawDeg(r.At(t + kLatency + kTick)), c = YawDeg(r.At(t + kLatency + 2 * kTick));
    CHECK(a > m && m > c, "between frames the ray turns toward the new one: %.3f %.3f %.3f", a, m, c);
}

}  // namespace

int main() {
    TestStillJitter();
    TestFastMove();
    TestNoSteps();
    TestReset();
    TestPointJitter();
    TestDirectionWrap();
    TestSteadyShoulder();
    TestRayBetweenFrames();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("smooth-test: ok\n");
    return 0;
}
