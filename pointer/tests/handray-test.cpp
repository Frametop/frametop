// Unit tests for the hand ray (hands/include/fh_ray.h): the body model (and the torso with the
// wrists), the pointing gate's timing and tests, the filter, the hand's axes and direction, and the
// pinch anchor, with made-up hands at ft-hands' 30 Hz.
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

// Pointing's hand with its palm joints: the wrist, then the middle, ring and little knuckles 8-9 cm
// on toward the fingers (ahead), spread across to the right; the back of the hand faces up.
HandFrame WithPalm(HandFrame f) {
    const V3 up = Norm(f.knuckle - f.wrist), across{1, 0, 0};
    f.middle = f.wrist + up * 0.09 + across * 0.01;
    f.ring = f.wrist + up * 0.085 + across * 0.03;
    f.little = f.wrist + up * 0.075 + across * 0.05;
    f.palm = true;
    return f;
}

void TestHandAxes() {
    const HandFrame f = WithPalm(Pointing(0));
    const Axes a = HandAxes(f.wrist, f.middle, f.ring, f.little, 1);
    CHECK(a.ok, "axes from a hand");
    CHECK(Dot(a.f, Norm(f.knuckle - f.wrist)) > 0.85, "forward toward the knuckles");
    CHECK(a.u.y > 0.9, "up out of the back of the hand: (%.2f %.2f %.2f)", a.u.x, a.u.y, a.u.z);
    CHECK(a.r.x > 0.8, "across to the right: %.2f", a.r.x);
    // the left hand, mirrored: the same axes, so its yaw turns out to the left
    auto mirror = [](V3 v) { return V3{-v.x, v.y, v.z}; };
    const Axes l = HandAxes(mirror(f.wrist), mirror(f.middle), mirror(f.ring), mirror(f.little), 0);
    CHECK(l.ok && l.u.y > 0.9 && l.r.x > 0.8, "left hand axes");
    const V3 dr = HandDir(a, 1, 0, 20), dl = HandDir(l, 0, 0, 20);
    CHECK(Dot(dr, a.r) > 0.3 && Dot(dl, l.r) < -0.3, "yaw turns out on both sides: %.2f %.2f", Dot(dr, a.r), Dot(dl, l.r));
    CHECK(Dot(HandDir(a, 1, -50, 0), a.u) < -0.7, "negative pitch tilts toward the palm");
    // a collapsed hand has no axes
    CHECK(!HandAxes(f.wrist, f.wrist, f.wrist, f.wrist, 1).ok, "no axes from one point");
    // a point pinned to the axes moves with the hand, turned and carried
    Anchor an;
    an.Pin(a, f.knuckle);
    Axes moved = a;
    moved.o = a.o + V3{0.1, 0, 0};
    CHECK(Len(an.At(moved, f.knuckle, 1) - (f.knuckle + V3{0.1, 0, 0})) < 1e-9, "pinned: carried with the hand");
    CHECK(Len(an.At(moved, f.knuckle, 0) - f.knuckle) < 1e-9, "weight 0: as measured");
}

void TestTorsoWrists() {
    // the head turns 40 deg left while the hands point ahead: the torso turns a third of that
    const V3 wrists[2] = {{-0.17, 1.33, -0.36}, {0.17, 1.33, -0.36}};
    Params on, off;
    off.torsoWrists = false;
    Body a(on), b(off);
    for (int i = 0; i <= 300; ++i) {
        const V3 fwd = i ? Dir(40 * kDeg, 0) : kForward;
        a.Update(i * 0.01, kEye, fwd, wrists, 2);
        b.Update(i * 0.01, kEye, fwd, wrists, 2);
    }
    CHECK(Near(b.TorsoYaw() / kDeg, 40, 0.5), "off: the torso follows the head (%.1f)", b.TorsoYaw() / kDeg);
    CHECK(a.TorsoYaw() / kDeg > 12 && a.TorsoYaw() / kDeg < 30, "on: the hands hold it back (%.1f)", a.TorsoYaw() / kDeg);
    // the right shoulder moved less, so the ray to a still hand turned less
    Body c(on);
    c.Update(0, kEye, kForward, wrists, 2);
    const V3 s0 = c.Shoulder(1);
    CHECK(Len(a.Shoulder(1) - s0) < Len(b.Shoulder(1) - s0) * 0.8, "the shoulder moved %.3f m, against %.3f",
          Len(a.Shoulder(1) - s0), Len(b.Shoulder(1) - s0));
    // facing the hands with the head: as before
    Body d(on), e(off);
    for (int i = 0; i <= 100; ++i) d.Update(i * 0.01, kEye, kForward, wrists, 2), e.Update(i * 0.01, kEye, kForward);
    CHECK(Near(d.TorsoYaw(), e.TorsoYaw(), 1e-6), "hands ahead, head ahead: the same torso");
    // never more than torsoMaxLag off the head
    Body f(on);
    const V3 side[1] = {{1.0, 1.3, 0}};
    for (int i = 0; i <= 300; ++i) f.Update(i * 0.01, kEye, kForward, side, 1);
    CHECK(std::fabs(f.TorsoYaw() / kDeg) <= 30 + 1e-6, "within 30 deg of the head (%.1f)", f.TorsoYaw() / kDeg);
}

void TestHandWeight() {
    // weight 0: the palm joints change nothing
    Params p;
    p.handWeight = 0;
    Body b;
    HandRay a(1, p), c(1, p);
    for (int i = 0; i < 10; ++i) {
        const double t = i * kFrame;
        b.Update(t, kEye, kForward);
        a.Frame(b, Pointing(t), true);
        c.Frame(b, WithPalm(Pointing(t)), true);
    }
    CHECK(Len(a.Raw().dir - c.Raw().dir) < 1e-12 && Len(a.Raw().origin - c.Raw().origin) < 1e-12, "weight 0 = the shoulder's ray");
    CHECK(a.Shown() && c.Shown(), "both shown");
    // weight 1: the hand's own direction; between: on the way, the gate unchanged
    p.handWeight = 1;
    HandRay h(1, p);
    const HandFrame f = WithPalm(Pointing(0));
    h.Frame(b, f, true);
    const V3 hd = HandDir(HandAxes(f.wrist, f.middle, f.ring, f.little, 1), 1, p.handPitch, p.handYaw);
    CHECK(fhsmooth::Angle(h.Raw().dir, hd) < 1e-6, "weight 1 = the hand's direction");
    p.handWeight = 0.3;
    HandRay m(1, p);
    m.Frame(b, f, true);
    const double whole = fhsmooth::Angle(a.Raw().dir, hd), part = fhsmooth::Angle(a.Raw().dir, m.Raw().dir);
    CHECK(Near(part, 0.3 * whole, 1e-6), "weight 0.3 turns it %.1f of %.1f deg", part / kDeg, whole / kDeg);
    CHECK(m.LastTests().pitch == a.LastTests().pitch && m.LastTests().palm == a.LastTests().palm, "the tests read the shoulder's ray");
}

void TestPinchAnchor() {
    // While a ray pinch is held, the knuckle point moving on its own (the fingers closing and
    // opening, a knuckle misread) doesn't move the ray; the hand moving does; the release eases back.
    Params p;
    p.pinchAnchor = true, p.holdShoulder = false, p.predict = 0;  // (a jump's prediction would overshoot)
    Body b;
    HandRay r(1, p);
    double t = 0;
    for (; t < 0.3; t += kFrame) b.Update(t, kEye, kForward), r.Frame(b, WithPalm(Pointing(t)), true);
    CHECK(r.Shown(), "shown");
    r.Hold(true, t);
    const V3 d0 = r.Raw().dir;
    double worst = 0;
    for (int i = 0; i < 15; ++i, t += kFrame) {
        HandFrame f = WithPalm(Pointing(t));
        f.knuckle = f.knuckle + V3{0.01 * std::sin(i), -0.015, 0.01};  // the knuckles alone, up to 2 cm
        b.Update(t, kEye, kForward);
        r.Frame(b, f, true);
        worst = std::max(worst, fhsmooth::Angle(r.Raw().dir, d0) / kDeg);
    }
    CHECK(worst < 0.3, "the knuckles alone moved the held ray %.2f deg", worst);
    // the whole hand moves 5 cm right: the ray follows
    auto shifted = [](double t, V3 by, V3 kOff) {
        HandFrame f = WithPalm(Pointing(t));
        for (V3 *v : {&f.knuckle, &f.wrist, &f.middle, &f.ring, &f.little}) *v = *v + by;
        f.knuckle = f.knuckle + kOff;
        return f;
    };
    const V3 by{0.05, 0, 0}, kOff{0.0, -0.015, 0.01};
    b.Update(t, kEye, kForward);
    r.Frame(b, shifted(t, by, kOff), true);
    t += kFrame;
    const V3 want = Norm(Pointing(0).knuckle + by - b.Shoulder(1));
    CHECK(fhsmooth::Angle(r.Raw().dir, want) / kDeg < 0.3, "the hand moved: the ray followed (off by %.2f deg)",
          fhsmooth::Angle(r.Raw().dir, want) / kDeg);
    // released: held hideFor, then eases to the measured knuckles over shoulderBlend, no jump
    r.Hold(false, t);
    V3 last = r.Raw().dir;
    double biggest = 0;
    for (double end = t + p.hideFor + p.shoulderBlend + 0.2; t < end; t += kFrame) {
        b.Update(t, kEye, kForward);
        r.Frame(b, shifted(t, by, kOff), true);
        biggest = std::max(biggest, fhsmooth::Angle(r.Raw().dir, last) / kDeg);
        last = r.Raw().dir;
    }
    const V3 measured = Norm(Pointing(0).knuckle + by + kOff - b.Shoulder(1));
    CHECK(fhsmooth::Angle(r.Raw().dir, measured) / kDeg < 0.05, "back on the measured knuckles");
    CHECK(biggest < 0.5, "eased back: at most %.2f deg a frame", biggest);
    // off: the knuckles alone move it
    p.pinchAnchor = false;
    HandRay o(1, p);
    for (t = 0; t < 0.3; t += kFrame) b.Update(t, kEye, kForward), o.Frame(b, WithPalm(Pointing(t)), true);
    o.Hold(true, t);
    const V3 o0 = o.Raw().dir;
    HandFrame f = WithPalm(Pointing(t));
    f.knuckle = f.knuckle + V3{0, -0.015, 0.01};
    o.Frame(b, f, true);
    CHECK(fhsmooth::Angle(o.Raw().dir, o0) / kDeg > 1, "anchor off: the knuckles move the ray");
}

}  // namespace

int main() {
    TestHandAxes();
    TestTorsoWrists();
    TestHandWeight();
    TestPinchAnchor();
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
