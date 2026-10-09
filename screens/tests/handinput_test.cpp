// Offline tests for near hand input (handinput.cpp) and the hand matte (handcut.cpp).
//
//   handinput_test                       unit tests on synthetic hands and panels
//   handinput_test --replay FILE.dump... recorded hands (ft-handreplay --publish-dump) against
//                                        panels placed around them: press counts while hovering,
//                                        presses against fixed panels, the near claim, the matte
//
// Build and run: scripts/test-handinput.sh [dumps...] (in the dev container), or the justfile's
// test-c (unit tests only). No OpenVR, GPU or headset.
#include "../handcut.h"
#include "../handinput.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using handcut::Mat;
using namespace handinput;

namespace {

int g_fail = 0, g_checks = 0;
void Check(bool ok, int line, const char *what) {
    ++g_checks;
    if (!ok) ++g_fail, std::printf("handinput_test.cpp:%d: FAILED: %s\n", line, what);
}
#define CHECK(cond) Check((cond), __LINE__, #cond)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= (tol))

Mat Identity() {
    Mat m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    return m;
}
Mat At(double x, double y, double z) {
    Mat m = Identity();
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}
Mat RotY(double rad, double x, double y, double z) {
    Mat m = At(x, y, z);
    m.m[0][0] = m.m[2][2] = float(std::cos(rad));
    m.m[0][2] = float(std::sin(rad)), m.m[2][0] = float(-std::sin(rad));
    return m;
}

// A panel 0.5 m ahead of a head at the origin (OpenVR: -z is forward), facing it.
Panel TestPanel(double z = -0.5) {
    Panel p;
    p.index = 0, p.pose = At(0, 0, z), p.width = 1.0, p.height = 0.5625, p.pxW = 1920, p.pxH = 1080, p.glows = true;
    // vr.cpp's controls for chrome 0.3, grip 0.04: bar, curve, roll, tab, reset
    const double barY = -(p.height / 2 + 0.3 * 0.06 + 0.3 * 12 / 256), gap = 0.3 * 0.06;
    p.controls = {{Control::Bar, 0, barY, 0.15, 0.3 * 12 / 256},
                  {Control::Curve, 0.15 + gap + 0.02, barY, 0.02, 0.02},
                  {Control::Roll, 0.15 + gap * 2 + 0.06, barY, 0.02, 0.02},
                  {Control::Tab, 0.5 + 0.02, -(p.height / 2 + 0.02), 0.02, 0.02},
                  {Control::Reset, -(0.15 + gap + 0.02), barY, 0.02, 0.02}};
    return p;
}

// ------------------------------------------------------------------ synthetic hands files

// A right hand pointing ahead (-z), palm down, index straight, the others curled; points
// relative to the index tip.
void PointingHand(float out[21][3], bool bent = false) {
    static const float base[21][3] = {
        {0, 0, 0},           {-0.025f, -0.005f, -0.03f}, {-0.04f, -0.01f, -0.055f}, {-0.05f, -0.015f, -0.075f},
        {-0.055f, -0.02f, -0.09f}, {-0.02f, 0, -0.085f},  {-0.02f, 0, -0.125f},       {-0.02f, 0, -0.15f},
        {-0.02f, 0, -0.17f}, {0, 0, -0.088f},             {0, -0.03f, -0.10f},        {0, -0.035f, -0.08f},
        {0, -0.02f, -0.07f}, {0.018f, 0, -0.083f},        {0.018f, -0.03f, -0.095f},  {0.018f, -0.033f, -0.078f},
        {0.018f, -0.02f, -0.07f}, {0.034f, 0, -0.075f},   {0.034f, -0.025f, -0.085f}, {0.034f, -0.028f, -0.07f},
        {0.034f, -0.018f, -0.065f}};
    std::memcpy(out, base, sizeof base);
    if (bent) {
        const float b[3][3] = {{-0.02f, -0.03f, -0.10f}, {-0.02f, -0.035f, -0.08f}, {-0.02f, -0.02f, -0.07f}};
        std::memcpy(out[6], b, sizeof b);
    }
    for (int j = 0; j < 21; ++j)
        for (int i = 0; i < 3; ++i) out[j][i] -= base[8][i];
}

struct SynthHand {
    bool present = true, right = true, stereo = true, bent = false;
    uint32_t id = 1;
    double tip[3] = {0, 0, -0.4};   // where the index tip is (head frame = room here)
    // or, for grabs: where the pinch point (index and middle knuckles) is
    bool byPinch = false;
    double pinch[3] = {0, 0, 0};
};

struct Synth {
    fh_hands_v2_t file{};
    fh_gestures_t g{};
    uint64_t seq = 0;
    Synth() {
        std::memcpy(g.magic, FH_GESTURES_MAGIC, 8);
        g.version = FH_GESTURES_VERSION;
    }
    void Write(const std::vector<SynthHand> &hands, int64_t t) {
        std::memset(&file, 0, sizeof file);
        std::memcpy(file.v1.magic, FH_HANDS_MAGIC, 8);
        file.v1.version = 1, file.v1.size = sizeof(fh_hands_t);
        file.v1.seq = 2 * ++seq;
        file.v1.capture_ns = uint64_t(t), file.v1.publish_ns = uint64_t(t + 30'000'000);
        uint32_t n = 0;
        for (const SynthHand &h : hands) {
            if (!h.present) continue;
            fh_hand_t &o = file.v1.hands[n++];
            o.id = h.id, o.flags = (h.right ? FH_HAND_RIGHT : 0) | (h.stereo ? FH_HAND_STEREO : 0), o.confidence = 1;
            float pts[21][3];
            PointingHand(pts, h.bent);
            double off[3];
            if (h.byPinch)
                for (int i = 0; i < 3; ++i) off[i] = h.pinch[i] - (pts[5][i] + pts[9][i]) / 2;
            else
                for (int i = 0; i < 3; ++i) off[i] = h.tip[i];
            for (int j = 0; j < 21; ++j)
                for (int i = 0; i < 3; ++i) o.pts[j][i] = float(pts[j][i] + off[i]);
        }
        file.v1.nhands = n;
        g.capture_ns = uint64_t(t), g.publish_ns = uint64_t(t + 30'000'000);
        g.seq += 2;
    }
    // a pinch on side s begins or ends at time t, at the point p
    void Pinch(int s, bool down, int64_t t, const double p[3], uint32_t hand) {
        fh_pinch_t &o = g.pinch[s];
        o.hand_id = hand;
        o.flags = FH_PINCH_TRACKED | (down ? FH_PINCH_DOWN : 0);
        if (down) ++o.begins, o.begin_ns = uint64_t(t);
        else ++o.ends, o.end_ns = uint64_t(t);
        o.strength = down ? 1 : 0;
        for (int i = 0; i < 3; ++i) o.point[i] = float(p[i]), o.begin_point[i] = down ? float(p[i]) : o.begin_point[i];
    }
};

struct Rig {
    handcut::Hands hands;
    HandInput in;
    Synth synth;
    std::vector<Panel> panels{TestPanel()};
    int64_t t = 1'000'000'000;
    bool allowed[2] = {true, true};
    bool busy = false;
    std::vector<Event> events;
    std::vector<Carry> carries;
    // One hands frame (and the ticks until the next one, 30 Hz: three 90 Hz ticks)
    void Frame(const std::vector<SynthHand> &h, int ticks = 3) {
        synth.Write(h, t);
        for (int k = 0; k < ticks; ++k) {
            const int64_t now = t + 30'000'000 + k * 11'111'111;
            if (k == 0) hands.Feed(&synth.file, sizeof(fh_hands_t), &synth.g, Identity(), now);
            else hands.Feed(nullptr, 0, nullptr, Identity(), now);
            Input i;
            i.nowNs = now, i.hands = &hands, i.head = Identity(), i.haveHead = true;
            i.allowed[0] = allowed[0], i.allowed[1] = allowed[1], i.pointerBusy = busy;
            in.Update(i, panels);
            for (const Event &e : in.events()) events.push_back(e);
            carries = in.carries();
        }
        t += 33'333'333;
    }
    int Count(Event::Type type) const {
        return int(std::count_if(events.begin(), events.end(), [&](const Event &e) { return e.type == type; }));
    }
    std::optional<Event> Last(Event::Type type) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == type) return *it;
        return std::nullopt;
    }
};

Panel PanelAt(const Mat &pose, double width) {
    Panel p = TestPanel();
    p.pose = pose, p.width = width, p.height = width * 0.5625;
    return p;
}

SynthHand TipAt(double x, double y, double d) {   // d in front of TestPanel's surface
    SynthHand h;
    h.tip[0] = x, h.tip[1] = y, h.tip[2] = -0.5 + d;
    return h;
}

// ------------------------------------------------------------------------------ unit tests

void TestGeometry() {
    Panel p = TestPanel();
    const double pt[3] = {0.1, 0.05, -0.45};
    Hit h = Locate(p, pt);
    NEAR(h.u, 0.1, 1e-6), NEAR(h.v, 0.05, 1e-6), NEAR(h.d, 0.05, 1e-6);
    double x, y;
    CHECK(ToBuffer(p, 0, 0, &x, &y)), NEAR(x, 960, 1e-6), NEAR(y, 540, 1e-6);
    CHECK(ToBuffer(p, 0.25, 0.140625, &x, &y)), NEAR(x, 1440, 1e-6), NEAR(y, 270, 1e-6);
    CHECK(!ToBuffer(p, 0.6, 0, &x, &y));
    // curved: a point on the surface at u has d 0, and FromSurface undoes Locate
    p.curve = 0.8;
    double back[3];
    FromSurface(p, 0.3, -0.1, 0.04, back);
    h = Locate(p, back);
    NEAR(h.u, 0.3, 1e-6), NEAR(h.v, -0.1, 1e-6), NEAR(h.d, 0.04, 1e-6);
    FromSurface(p, 0.3, 0, 0, back);
    NEAR(back[2], -0.5 + 0.8 - 0.8 * std::cos(0.3 / 0.8), 1e-6);   // bent toward the viewer
    // a turned panel
    p.curve = 0, p.pose = RotY(0.5, 0.2, 0, -0.6);
    FromSurface(p, -0.1, 0.2, 0.03, back);
    h = Locate(p, back);
    NEAR(h.u, -0.1, 1e-6), NEAR(h.v, 0.2, 1e-6), NEAR(h.d, 0.03, 1e-6);
    // a floating window's crop and a popup over it
    Panel f = TestPanel();
    f.floating = true, f.pxW = 2400, f.pxH = 1500, f.cropX = 300, f.cropY = 200, f.cropW = 1600, f.cropH = 900;
    f.width = 1.0, f.height = 0.5625;
    CHECK(ToBuffer(f, -0.5, 0.28125, &x, &y)), NEAR(x, 300, 1e-6), NEAR(y, 200, 1e-6);
    f.popups.push_back({0.55, 0, 0.2, 0.1, 2000, 600, 320, 160});   // past the window's right edge
    CHECK(ToBuffer(f, 0.55, 0, &x, &y)), NEAR(x, 2160, 1e-6), NEAR(y, 680, 1e-6);
    // controls
    CHECK(ControlAt(p, 0.15 + 0.018 + 0.02, -(0.28125 + 0.018 + 0.3 * 12 / 256), 0) == Control::Curve);
}

void TestZones() {
    Panel p = TestPanel();
    Params k;
    int sx, sy;
    double e;
    const double barY = p.controls[0].v;
    CHECK(GrabZone(p, {0.05, barY, 0.01}, k, &sx, &sy, &e) == Zone::Bar && e <= 0);
    CHECK(GrabZone(p, {0.05, barY - 0.035, 0.0}, k, &sx, &sy, &e) == Zone::Bar && e <= 0);
    CHECK(GrabZone(p, {0.05, barY, 0.05}, k, &sx, &sy, &e) == Zone::Bar && e > 0);   // too far in front
    CHECK(GrabZone(p, {0.52, -0.30, 0}, k, &sx, &sy, &e) == Zone::Tab && e <= 0);
    CHECK(GrabZone(p, {-0.5, 0.28, 0.01}, k, &sx, &sy, &e) == Zone::Corner && e <= 0 && sx == -1 && sy == 1);
    CHECK(GrabZone(p, {-0.51, 0.0, 0.01}, k, &sx, &sy, &e) == Zone::Edge && e <= 0);
    CHECK(GrabZone(p, {0.49, 0.1, -0.01}, k, &sx, &sy, &e) == Zone::Edge && e <= 0);
    CHECK(GrabZone(p, {0.0, 0.29, 0.0}, k, &sx, &sy, &e) == Zone::Edge && e <= 0);   // the top edge
    CHECK(GrabZone(p, {0.0, 0.0, 0.0}, k, &sx, &sy, &e) != Zone::None && e > 0.1);   // the content: no zone
    // a floating window: the title bar, and only the bottom right corner resizes
    Panel f = TestPanel();
    f.floating = true, f.cropW = 1600, f.cropH = 900, f.titleH = 60;
    CHECK(GrabZone(f, {0.1, 0.27, 0.0}, k, &sx, &sy, &e) == Zone::Title && e <= 0);
    CHECK(GrabZone(f, {-0.5, 0.28, 0.0}, k, &sx, &sy, &e) == Zone::Title);   // top left: no resize there
    f.controls.clear();
    CHECK(GrabZone(f, {0.5, -0.28, 0.0}, k, &sx, &sy, &e) == Zone::Corner && sx == 1 && sy == -1);
}

void TestCarry() {
    const double head[3] = {0, 0, 0};
    const Mat start = At(0, 0, -0.5);
    const double grab[3] = {0.3, -0.25, 0};   // the bottom right of the panel
    const double pinch0[3] = {0.3, -0.25, -0.5};
    Mat p = CarryPose(start, head, grab, pinch0, head, start);
    NEAR(p.m[0][3], 0, 1e-4), NEAR(p.m[2][3], -0.5, 1e-4), NEAR(p.m[2][2], 1, 1e-3);   // nothing moved: nothing changes
    // pulled 20 cm closer: it comes 1:1, facing the head
    const double closer[3] = {0.3, -0.25, -0.3};
    p = CarryPose(start, head, grab, closer, head, p);
    double gx = p.m[0][3] + p.m[0][0] * grab[0] + p.m[0][1] * grab[1], gz = p.m[2][3] + p.m[2][0] * grab[0] + p.m[2][1] * grab[1];
    NEAR(gx, 0.3, 1e-4), NEAR(gz, -0.3, 1e-4);
    // carried 90 degrees around to the right: the panel turns to keep facing the head
    const double side[3] = {0.5, -0.25, 0.0};
    Mat q = start;
    for (int k = 0; k < 4; ++k) q = CarryPose(start, head, grab, side, head, q);
    const double nx = q.m[0][2], nz = q.m[2][2];   // its front
    const double tx = -q.m[0][3], tz = -q.m[2][3];
    const double cosang = (nx * tx + nz * tz) / std::hypot(tx, tz);
    CHECK(cosang > 0.99);
    gx = q.m[0][3] + q.m[0][0] * grab[0] + q.m[0][1] * grab[1] + q.m[0][2] * grab[2];
    NEAR(gx, 0.5, 1e-3);
    // a panel turned 20 degrees off facing keeps that turn
    const Mat turned = RotY(20 * M_PI / 180, 0, 0, -0.5);
    q = CarryPose(turned, head, grab, side, head, turned);
    for (int k = 0; k < 4; ++k) q = CarryPose(turned, head, grab, side, head, q);
    const double c2 = (q.m[0][2] * -q.m[0][3] + q.m[2][2] * -q.m[2][3]) / std::hypot(q.m[0][3], q.m[2][3]);
    NEAR(std::acos(std::clamp(c2, -1.0, 1.0)) * 180 / M_PI, 20, 0.5);
}

void TestStretch() {
    const Mat start = At(0, 0, -0.5);
    const double l0[3] = {-0.5, 0, -0.5}, r0[3] = {0.5, 0, -0.5};
    double w;
    // apart by 20 cm: 1.0 -> (1.2 - 0.01) / 1.0
    const double l[3] = {-0.6, 0, -0.5}, r[3] = {0.6, 0, -0.5};
    Mat p = StretchPose(start, 1.0, l0, r0, l, r, 0.01, 0.15, 12, true, &w);
    NEAR(w, 1.19, 1e-6), NEAR(p.m[0][3], 0, 1e-6), NEAR(p.m[2][3], -0.5, 1e-6);
    // within the dead zone: as it was
    const double l2[3] = {-0.504, 0, -0.5};
    StretchPose(start, 1.0, l0, r0, l2, r0, 0.01, 0.15, 12, true, &w);
    NEAR(w, 1.0, 1e-9);
    // the right hand forward 20 cm: the panel turns about the vertical, its right edge away
    const double r3[3] = {0.5, 0, -0.7};
    p = StretchPose(start, 1.0, l0, r0, l0, r3, 0.01, 0.15, 12, true, &w);
    const double yaw = std::atan2(p.m[0][2], p.m[2][2]) * 180 / M_PI;   // its front's heading
    NEAR(yaw, std::atan2(0.2, 1.0) * 180 / M_PI, 0.1);
    CHECK(p.m[2][0] < 0);   // +x (its right) now points away (-z)
    // a floating window doesn't scale
    StretchPose(start, 1.0, l0, r0, l, r, 0.01, 0.15, 12, false, &w);
    CHECK(w < 0);
}

void TestHover() {
    Rig r;
    srand(1);
    for (int k = 0; k < 90; ++k) {   // 3 s at 2 cm, +-4 mm of depth noise
        const double n = ((rand() % 1000) / 1000.0 - 0.5) * 0.008;
        r.Frame({TipAt(0.1, 0.05, 0.02 + n)});
    }
    CHECK(r.Count(Event::Press) == 0);
    CHECK(r.Count(Event::Motion) > 0);   // the cursor follows within 5 cm
    CHECK(r.in.near(1) && !r.in.near(0));
    const auto m = r.Last(Event::Motion);
    CHECK(m && std::fabs(m->x - (0.6 * 1920)) < 20 && std::fabs(m->y - (0.5 - 0.05 / 0.5625) * 1080) < 20);
    // a glow ring under the fingertip
    CHECK(r.in.glows().count(0) && !r.in.glows().at(0).empty() && r.in.glows().at(0)[0].ring);
}

void TestPoke() {
    Rig r;
    // in from 10 cm at 0.3 m/s (1 cm a frame) to 1.5 cm behind, then out
    for (int mm = 100; mm >= -15; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0.1, 0.05, d)}); }
    CHECK(r.Count(Event::Press) == 1);
    const auto p = r.Last(Event::Press);
    CHECK(p && std::fabs(p->x - 1152) < 3 && std::fabs(p->y - (0.5 - 0.05 / 0.5625) * 1080) < 3);
    CHECK(r.in.pressing(1));
    for (int mm = -15; mm <= 50; mm += 10) { const double d = mm / 1000.0; r.Frame({TipAt(0.1, 0.05, d)}); }
    CHECK(r.Count(Event::Release) == 1);
    const auto u = r.Last(Event::Release);
    CHECK(u && !u->lost && std::fabs(u->x - p->x) < 0.01 && std::fabs(u->y - p->y) < 0.01);   // where it was pressed
    CHECK(!r.in.pressing(1));
}

void TestSlowDrift() {
    Rig r;
    // creeping in at 3 mm/s: no press at the surface, only once it's 1 cm deep
    double d = 0.02;
    int pressedAt = -1;
    for (int k = 0; k < 400 && pressedAt < 0; ++k) {
        d -= 0.0001;
        r.Frame({TipAt(0, 0, d)}, 1);
        if (r.Count(Event::Press)) pressedAt = k;
    }
    CHECK(pressedAt >= 0 && d <= -0.0099);
}

void TestGuards() {
    // one view, bent finger, busy pointer, not allowed: no press
    for (int which = 0; which < 4; ++which) {
        Rig r;
        if (which == 2) r.busy = true;
        if (which == 3) r.allowed[1] = false;
        for (int mm = 60; mm >= -30; mm -= 10) {
            const double d = mm / 1000.0;
            SynthHand h = TipAt(0, 0, d);
            if (which == 0) h.stereo = false;
            if (which == 1) h.bent = true;
            r.Frame({h});
        }
        CHECK(r.Count(Event::Press) == 0);
        if (which == 3) CHECK(!r.in.near(1));
    }
}

void TestRelease() {
    // the tracker has the finger 3 cm behind the panel when it touches: pulling back 1 cm releases
    Rig r;
    for (int mm = 50; mm >= -30; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0, 0, d)}); }
    CHECK(r.Count(Event::Press) == 1);
    r.Frame({TipAt(0, 0, -0.025)});
    CHECK(r.Count(Event::Release) == 0);
    r.Frame({TipAt(0, 0, -0.019)});
    CHECK(r.Count(Event::Release) == 1);
    // a new press needs 150 ms and a new push
    r.Frame({TipAt(0, 0, -0.03)});
    CHECK(r.Count(Event::Press) == 1);
}

void TestDeep() {
    Rig r;
    for (int mm = 50; mm >= -100; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0, 0, d)}); }
    CHECK(r.Count(Event::Press) == 1 && r.Count(Event::Release) == 1);   // through the panel: let go
    for (int k = 0; k < 10; ++k) r.Frame({TipAt(0, 0, -0.10 + 0.005 * k)});
    CHECK(r.Count(Event::Press) == 1);   // still behind: no new press until it's back out
    for (int mm = 20; mm >= -20; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0, 0, d)}); }
    CHECK(r.Count(Event::Press) == 2);
}

void TestDrag() {
    Rig r;
    for (int mm = 50; mm >= -12; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0, 0, d)}); }
    const int motions = r.Count(Event::Motion);
    r.Frame({TipAt(0.01, 0, -0.012)});   // 1 cm: still locked
    CHECK(r.Count(Event::Motion) == motions);
    r.Frame({TipAt(0.03, 0, -0.012)});   // 3 cm: a drag
    CHECK(r.Count(Event::Motion) == motions + 1);
    const auto m = r.Last(Event::Motion);
    CHECK(m && std::fabs(m->x - (0.53 * 1920)) < 3);
    r.Frame({TipAt(0.03, 0, 0.02)});
    const auto u = r.Last(Event::Release);
    CHECK(u && std::fabs(u->x - m->x) < 0.01);   // released where it was dragged to
}

void TestLost() {
    Rig r;
    for (int mm = 50; mm >= -12; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(0, 0, d)}); }
    CHECK(r.in.pressing(1));
    SynthHand gone;
    gone.present = false;
    for (int k = 0; k < 5; ++k) r.Frame({gone});   // 167 ms: held
    CHECK(r.in.pressing(1) && r.Count(Event::Release) == 0);
    for (int k = 0; k < 5; ++k) r.Frame({gone});
    CHECK(r.Count(Event::Release) == 1 && r.Last(Event::Release)->lost);
    // taken away mid-press (a controller picked up): released as lost at once
    Rig q;
    for (int mm = 50; mm >= -12; mm -= 10) { const double d = mm / 1000.0; q.Frame({TipAt(0, 0, d)}); }
    q.allowed[1] = false;
    q.Frame({TipAt(0, 0, -0.012)});
    CHECK(q.Count(Event::Release) == 1 && q.Last(Event::Release)->lost);
}

void TestControlPoke() {
    Rig r;
    const Spot &c = r.panels[0].controls[1];   // the curve button
    for (int mm = 50; mm >= -15; mm -= 10) { const double d = mm / 1000.0; r.Frame({TipAt(c.u, c.v, d)}); }
    CHECK(r.Count(Event::Control) == 1 && r.Count(Event::Press) == 0);
    for (int k = 0; k < 20; ++k) r.Frame({TipAt(c.u, c.v, -0.015)});   // held there: once
    CHECK(r.Count(Event::Control) == 1);
    // the bar takes no poke
    Rig b;
    const Spot &bar = b.panels[0].controls[0];
    for (int mm = 50; mm >= -15; mm -= 10) { const double d = mm / 1000.0; b.Frame({TipAt(bar.u, bar.v, d)}); }
    CHECK(b.Count(Event::Control) == 0 && b.Count(Event::Press) == 0);
}

void TestNearClaim() {
    Rig r;
    r.Frame({TipAt(0, 0, 0.30)});
    CHECK(!r.in.near(1));
    r.Frame({TipAt(0, 0, 0.12)});
    CHECK(r.in.near(1));
    r.Frame({TipAt(0, 0, 0.30)});
    CHECK(r.in.near(1));   // lingers 250 ms
    for (int k = 0; k < 10; ++k) r.Frame({TipAt(0, 0, 0.30)});
    CHECK(!r.in.near(1));
    r.Frame({TipAt(0.7, 0, 0.05)});   // off to the side of the panel
    CHECK(!r.in.near(1));
}

SynthHand PinchAt(double u, double v, double d, bool right = true, uint32_t id = 1) {
    SynthHand h;
    h.byPinch = true, h.right = right, h.id = id, h.bent = true;   // a pinching hand's index is bent
    h.pinch[0] = u, h.pinch[1] = v, h.pinch[2] = -0.5 + d;
    return h;
}

void TestGrabMove() {
    Rig r;
    const double barY = r.panels[0].controls[0].v;
    // the hand comes near the bar (its fingertip over the panel), then pinches there
    for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.05, barY, 0.02)});
    CHECK(r.in.near(1));
    const double at[3] = {0.05, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({PinchAt(0.05, barY, 0.02)});
    CHECK(r.Count(Event::GrabStart) == 1 && r.in.grabbing(1));
    CHECK(r.Last(Event::GrabStart)->zone == Zone::Bar);
    CHECK(r.in.hovered().count(0) && (r.in.hovered().at(0) & 1));   // the bar lights
    // carried 10 cm up and 20 cm closer, slowly (the carry follows the predicted pinch point)
    const double g0[3] = {0.05 + 0.01, barY, -0.5 + 0.02};   // where it took the panel (PinchAt's hand)
    (void)g0;
    for (int k = 1; k <= 30; ++k) {
        r.panels[0].pose = r.carries.empty() ? r.panels[0].pose : r.carries[0].pose;
        const int j = std::min(k, 20);
        r.Frame({PinchAt(0.05, barY + 0.005 * j, 0.02 + 0.01 * j)});
    }
    CHECK(!r.carries.empty() && r.carries[0].kind == Carry::Move);
    if (!r.carries.empty()) {
        const Mat &p = r.carries[0].pose;
        // the grab point (on the bar, 5 cm right of the middle) is under the pinch point, and the
        // panel faces the head
        const Hit g = Locate(PanelAt(p, 1.0), r.hands.shown()[0].pinchPoint);
        NEAR(g.u, 0.05, 0.005), NEAR(g.v, barY, 0.005), NEAR(g.d, 0.02, 0.005);
        const double c = -(p.m[0][2] * p.m[0][3] + p.m[1][2] * p.m[1][3] + p.m[2][2] * p.m[2][3]) /
                         std::sqrt(p.m[0][3] * p.m[0][3] + p.m[1][3] * p.m[1][3] + p.m[2][3] * p.m[2][3]);
        CHECK(c > 0.995);
        NEAR(r.hands.shown()[0].pinchPoint[2], -0.28, 0.002);
    }
    r.synth.Pinch(1, false, r.t, at, 1);
    r.Frame({PinchAt(0.05, barY + 0.1, 0.22)});
    CHECK(r.Count(Event::GrabEnd) == 1 && !r.in.grabbing(1));
    // a pinch on the content does nothing
    Rig c;
    for (int k = 0; k < 5; ++k) c.Frame({PinchAt(0, 0, 0.02)});
    const double mid[3] = {0, 0, -0.48};
    c.synth.Pinch(1, true, c.t, mid, 1);
    c.Frame({PinchAt(0, 0, 0.02)});
    CHECK(c.Count(Event::GrabStart) == 0 && c.Count(Event::Press) == 0);
    // nor a pinch that began before the hand was near (the helper's)
    Rig f;
    const double far[3] = {0.05, barY, -0.2};
    f.synth.Pinch(1, true, f.t, far, 1);
    f.Frame({PinchAt(0.05, barY, 0.3)});
    for (int k = 0; k < 5; ++k) f.Frame({PinchAt(0.05, barY, 0.02)});
    CHECK(f.Count(Event::GrabStart) == 0);
}

void TestGrabResize() {
    Rig r;
    // the bottom right corner, pulled 10 cm out along the diagonal
    for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.5, -0.28, 0.01)});
    const double at[3] = {0.5, -0.28, -0.49};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({PinchAt(0.5, -0.28, 0.01)});
    CHECK(r.Count(Event::GrabStart) == 1 && (r.Last(Event::GrabStart)->zone == Zone::Corner || r.Last(Event::GrabStart)->zone == Zone::Tab));
    for (int k = 1; k <= 20; ++k) {   // out along the diagonal to a 1.2 m panel, slowly, then held
        const int j = std::min(k, 10);
        r.Frame({PinchAt(0.5 + 0.01 * j, -0.28 - 0.005625 * j, 0.01)});
    }
    CHECK(!r.carries.empty() && r.carries[0].kind == Carry::Resize);
    if (!r.carries.empty()) NEAR(r.carries[0].width, 1.2, 0.03);
}

void TestStretchHands() {
    Rig r;
    // both hands on the left and right edges, pinch
    for (int k = 0; k < 5; ++k) r.Frame({PinchAt(-0.5, 0, 0.01, false, 1), PinchAt(0.5, 0, 0.01, true, 2)});
    const double l[3] = {-0.5, 0, -0.49}, rr[3] = {0.5, 0, -0.49};
    r.synth.Pinch(0, true, r.t, l, 1);
    r.synth.Pinch(1, true, r.t, rr, 2);
    r.Frame({PinchAt(-0.5, 0, 0.01, false, 1), PinchAt(0.5, 0, 0.01, true, 2)});
    CHECK(r.Count(Event::GrabStart) == 2);
    for (int k = 1; k <= 20; ++k) {
        const int j = std::min(k, 10);
        r.Frame({PinchAt(-0.5 - 0.01 * j, 0, 0.01, false, 1), PinchAt(0.5 + 0.01 * j, 0, 0.01, true, 2)});
    }
    CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch && r.carries[0].sides == 3);
    if (r.carries.size() != 1) return;
    NEAR(r.carries[0].width, 1.19, 0.01);
    r.panels[0].pose = r.carries[0].pose, r.panels[0].width = r.carries[0].width;
    // the left hand lets go: the right one carries on alone, from where the panel is
    r.synth.Pinch(0, false, r.t, l, 1);
    r.Frame({PinchAt(-0.6, 0, 0.01, false, 1), PinchAt(0.6, 0, 0.01, true, 2)});
    CHECK(r.Count(Event::GrabEnd) == 1 && r.in.grabbing(1) && !r.in.grabbing(0));
    for (int k = 1; k <= 20; ++k) {
        const int j = std::min(k, 10);
        r.panels[0].pose = r.carries.empty() ? r.panels[0].pose : r.carries[0].pose;
        r.Frame({PinchAt(-0.6, 0, 0.01, false, 1), PinchAt(0.6, 0.01 * j, 0.01, true, 2)});
    }
    CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Move && r.carries[0].sides == 2);
    if (r.carries.size() == 1) {   // the grab point stays under the right hand
        const Hit g = Locate(PanelAt(r.carries[0].pose, 1.19), r.hands.shown()[0].right ? r.hands.shown()[0].pinchPoint : r.hands.shown()[1].pinchPoint);
        NEAR(g.u, 0.595, 0.01), NEAR(g.v, 0, 0.01);
    }
    // the right hand lost: its pinch ends as lost
    r.synth.Pinch(1, false, r.t, rr, 2);
    r.synth.g.pinch[1].flags |= FH_PINCH_LOST;
    r.Frame({PinchAt(-0.6, 0, 0.01, false, 1)});
    CHECK(r.Count(Event::GrabEnd) == 2 && r.Last(Event::GrabEnd)->lost);
}

void TestV2Ingest() {
    // a version 2 file: the skeleton's joints, elbow and pinch point are used
    static fh_hands_v2_t f{};
    std::memcpy(f.v1.magic, FH_HANDS_MAGIC, 8);
    f.v1.version = 2, f.v1.size = sizeof f, f.v1.nhands = 1, f.v1.capture_ns = 1000, f.v1.publish_ns = 2000;
    f.v1.hands[0].id = 7, f.v1.hands[0].flags = FH_HAND_STEREO;
    f.fits[0].id = 7, f.fits[0].flags = FH_FIT_VALID;
    float pts[21][3];
    PointingHand(pts);
    for (int j = 0; j < 21; ++j)
        for (int i = 0; i < 3; ++i) f.fits[0].joints[j].p[i] = pts[j][i] + (i == 2 ? -0.4f : 0), f.v1.hands[0].pts[j][i] = 9;
    for (int j = 0; j < 21; ++j) f.fits[0].joints[j].radius = 0.01f;
    f.fits[0].elbow[1] = -0.3f, f.fits[0].pinch_point[0] = 0.5f, f.fits[0].tracked_ns = 1000;
    handcut::Hands h;
    h.Feed(&f, sizeof f, nullptr, Identity(), 3000);
    CHECK(h.seen().size() == 1 && h.seen()[0].fit && h.seen()[0].hasElbow && !h.seen()[0].right);
    NEAR(h.seen()[0].p[8][2], -0.4, 1e-6), NEAR(h.seen()[0].elbow[1], -0.3, 1e-6), NEAR(h.seen()[0].pinchPoint[0], 0.5, 1e-6);
    CHECK(h.version() == 2);
    // a version 1 reader's view of it (the file cut at sizeof(fh_hands_t)): the points
    handcut::Hands v1;
    v1.Feed(&f, sizeof(fh_hands_t), nullptr, Identity(), 3000);
    CHECK(v1.seen().size() == 1 && !v1.seen()[0].fit);
    NEAR(v1.seen()[0].p[8][0], 9, 1e-6);
}

void TestMatte() {
    Rig r;
    r.Frame({TipAt(0, 0, 0.1)});
    handcut::MatteParams mp;
    handcut::Matte m;
    const double head[3] = {0, 0, 0};
    handcut::BuildMatte(r.hands.shown(), r.hands.capsules(), head, mp, m);
    CHECK(m.caps.size() == 15 && m.hulls.size() == 3);
    handcut::Panel p{At(0, 0, -0.5), 1.0, 0.5625, 0, 1920, 1080};
    const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}};
    handcut::Spots s[2];
    CHECK(handcut::Project(p, m, eyes, s));
    CHECK(s[0].caps.size() == 15 && s[0].hulls.size() >= 2);   // the forearm may reach the eye's near plane
    // the index tip's capsule sits where the eye sees the tip, and its radius is the finger's
    // plus the margin, magnified by the distance ratio
    const handcut::Capsule2D &c = s[0].caps[5];   // index DIP -> tip
    const double tipX = (-0.032 + (0.0 + 0.032) * 0.5 / 0.4 + 0.5) * 1920;   // eye -> tip at -0.4, onto -0.5
    NEAR(c.bx, tipX, 6);
    const double radius = (0.0075 + mp.Margin(0.4, 0)) * 1.25 * 1920;
    NEAR(c.rb, radius, radius * 0.25);
    // the palm hull: a point inside it (the middle knuckle, seen from the left eye) is inside
    const handcut::Hull2D &palm = s[0].hulls[0];
    const double mx = (-0.032 + (0.02 + 0.032) * 0.5 / 0.483 + 0.5) * 1920, my = (0.5 - 0) * 1080;
    double worst = -1e9;
    for (int k = 0; k < palm.n; ++k) worst = std::max(worst, palm.e[k][0] * mx + palm.e[k][1] * my - double(palm.e[k][2]));
    CHECK(worst < 0);
    // the hand pushed through the panel: nothing cut there
    Rig t;
    t.Frame({TipAt(0, 0, -0.30)});
    handcut::BuildMatte(t.hands.shown(), t.hands.capsules(), head, mp, m);
    handcut::Project(p, m, eyes, s);
    for (const handcut::Capsule2D &k : s[0].caps) CHECK(k.ra < 400 && k.rb < 400);
    // shift settings parse
    std::vector<handcut::Shift> sh;
    CHECK(handcut::ParseShift("0.5:-1,0.5 0.3:1.2,-0.4", sh) && sh.size() == 2 && sh[0].dist == 0.3);
    CHECK(!handcut::ParseShift("0.5:x", sh));
    NEAR(mp.Margin(0.4, 0), 0.4 * std::tan(1.5 * M_PI / 180), 1e-9);
    NEAR(mp.Margin(0.4, 10), 0.4 * std::tan(1.5 * M_PI / 180) + 0.4 * std::tan(1.5 * M_PI / 180), 1e-9);
}

// ------------------------------------------------------------------------------- replay

struct Record {
    std::vector<char> hands;
    fh_gestures_t g;
    int64_t capture, publish;
};

bool Load(const char *path, std::vector<Record> &out) {
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    uint32_t sizes[2];
    if (!f.read(magic, 8) || std::memcmp(magic, "FHDUMP01", 8) || !f.read(reinterpret_cast<char *>(sizes), 8) ||
        sizes[0] < sizeof(fh_hands_t) || sizes[1] != sizeof(fh_gestures_t))
        return false;
    while (true) {
        Record r;
        r.hands.resize(sizes[0]);
        if (!f.read(r.hands.data(), sizes[0]) || !f.read(reinterpret_cast<char *>(&r.g), sizeof r.g)) break;
        const auto *v1 = reinterpret_cast<const fh_hands_t *>(r.hands.data());
        r.capture = int64_t(v1->capture_ns), r.publish = int64_t(v1->publish_ns);
        out.push_back(std::move(r));
    }
    return !out.empty();
}

struct Run {
    int presses = 0, controls = 0, grabs = 0, frames = 0, nearFrames = 0;
    HandInput::Stats stats;
};

// Plays records [from, to) through hand input at 90 Hz ticks, against `panels`.
Run Play(const std::vector<Record> &recs, size_t from, size_t to, const std::vector<Panel> &panels, int sides = 3) {
    handcut::Hands hands;
    HandInput in;
    Run run;
    for (size_t k = from; k < to; ++k) {
        const Record &r = recs[k];
        const int64_t next = k + 1 < to ? recs[k + 1].publish : r.publish + 33'000'000;
        for (int64_t t = r.publish; t < next; t += 11'111'111) {
            if (t == r.publish) hands.Feed(r.hands.data(), r.hands.size(), &r.g, Identity(), t);
            else hands.Feed(nullptr, 0, nullptr, Identity(), t);
            Input i;
            i.nowNs = t, i.hands = &hands, i.head = Identity(), i.haveHead = true, i.allowed[0] = sides & 1, i.allowed[1] = sides & 2;
            in.Update(i, panels);
            for (const Event &e : in.events()) {
                run.presses += e.type == Event::Press, run.controls += e.type == Event::Control;
                run.grabs += e.type == Event::GrabStart;
            }
            if (t == r.publish) ++run.frames, run.nearFrames += in.anyNear();
        }
    }
    run.stats = in.stats;
    return run;
}

// A panel facing the head (at the origin) whose surface is `offset` beyond the point p, along
// the line from the head.
Panel Facing(const double p[3], double offset) {
    const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const double c[3] = {p[0] / n * (n + offset), p[1] / n * (n + offset), p[2] / n * (n + offset)};
    double z[3] = {-p[0] / n, -p[1] / n, -p[2] / n};
    double x[3] = {z[2], 0, -z[0]};
    const double xl = std::hypot(x[0], x[2]);
    x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Panel P = TestPanel();
    for (int i = 0; i < 3; ++i) P.pose.m[i][0] = float(x[i]), P.pose.m[i][1] = float(y[i]), P.pose.m[i][2] = float(z[i]), P.pose.m[i][3] = float(c[i]);
    P.controls.clear();
    P.width = 1.2, P.height = 0.675;
    return P;
}

int Replay(const char *path) {
    std::vector<Record> recs;
    if (!Load(path, recs)) return std::printf("%s: not a publish dump\n", path), 1;
    const double secs = (recs.back().capture - recs.front().capture) / 1e9;
    std::printf("== %s: %zu frames, %.0f s\n", path, recs.size(), secs);

    // 1. Hovering: stretches of 1 s and more where a hand holds still (its palm within 2 cm)
    // pointing (index straight), a panel facing the head 0.5-3 cm beyond the fingertip's
    // median place (along the line from the head). The fingertip's own wobble is all that
    // moves it toward the panel, so every press is a false one.
    {
        handcut::Hands hands;
        struct Win { size_t from, to; int side; double tip[3]; double spread; };
        std::vector<Win> wins;
        for (int s = 0; s < 2; ++s) {
            size_t start = 0;
            bool open = false;
            double anchor[3] = {};
            std::vector<std::array<double, 3>> tips;
            int pointing = 0;
            auto close = [&](size_t k) {
                open = false;
                if (tips.size() < 15 || recs[k - 1].capture - recs[start].capture < 1'000'000'000 || pointing < int(tips.size() * 0.8))
                    return;
                Win w{start, k, s, {0, 0, 0}, 0};
                // the median tip (per axis), and how far the tip strays along the head's line
                for (int i = 0; i < 3; ++i) {
                    std::vector<double> v;
                    for (auto &t : tips) v.push_back(t[i]);
                    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                    w.tip[i] = v[v.size() / 2];
                }
                const double n = std::sqrt(w.tip[0] * w.tip[0] + w.tip[1] * w.tip[1] + w.tip[2] * w.tip[2]);
                double lo = 1e9, hi = -1e9;
                for (auto &t : tips) {
                    const double along = (t[0] * w.tip[0] + t[1] * w.tip[1] + t[2] * w.tip[2]) / n;
                    lo = std::min(lo, along), hi = std::max(hi, along);
                }
                w.spread = hi - lo;
                wins.push_back(w);
            };
            handcut::Hands hs;
            for (size_t k = 0; k < recs.size(); ++k) {
                hs.Feed(recs[k].hands.data(), recs[k].hands.size(), &recs[k].g, Identity(), recs[k].publish);
                const handcut::Hand *h = nullptr;
                for (const handcut::Hand &x : hs.seen())
                    if (x.right == (s == 1)) h = &x;
                double palm[3] = {0, 0, 0};
                if (h) h->Palm(palm);
                bool ok = h != nullptr;
                if (ok && open) ok = std::hypot(std::hypot(palm[0] - anchor[0], palm[1] - anchor[1]), palm[2] - anchor[2]) < 0.02;
                if (!ok && open) close(k);
                if (!h) continue;
                if (!open) {
                    open = true, start = k, tips.clear(), pointing = 0;
                    std::copy(palm, palm + 3, anchor);
                }
                tips.push_back({h->p[8][0], h->p[8][1], h->p[8][2]});
                const double ext = std::hypot(std::hypot(h->p[8][0] - h->p[0][0], h->p[8][1] - h->p[0][1]), h->p[8][2] - h->p[0][2]) /
                                   std::hypot(std::hypot(h->p[5][0] - h->p[0][0], h->p[5][1] - h->p[0][1]), h->p[5][2] - h->p[0][2]);
                pointing += ext >= 1.6 && h->stereo;
            }
            if (open) close(recs.size());
        }
        double total = 0;
        std::vector<double> spreads;
        for (const Win &w : wins) total += (recs[w.to - 1].capture - recs[w.from].capture) / 1e9, spreads.push_back(w.spread);
        std::sort(spreads.begin(), spreads.end());
        std::printf("hovering: %zu still pointing stretches, %.1f s; the fingertip's depth range in each: median %.1f mm, max %.1f mm\n",
                    wins.size(), total, spreads.empty() ? 0.0 : spreads[spreads.size() / 2] * 1000,
                    spreads.empty() ? 0.0 : spreads.back() * 1000);
        for (double off : {0.005, 0.01, 0.02, 0.03}) {
            int presses = 0, held = 0, nearest = 0;
            for (const Win &w : wins) {
                const Run r = Play(recs, w.from, w.to, {Facing(w.tip, off)}, 1 << w.side);
                presses += r.presses;
                held += r.stats.heldSlow, nearest += r.stats.heldNotNearest;
            }
            std::printf("  panel %.1f cm beyond the fingertip: %d presses (%.2f per minute); frames at d <= 0 held back: %d slow, %d not nearest\n",
                        off * 100, presses, total > 0 ? presses / total * 60 : 0.0, held, nearest);
        }
    }
    // 2. Fixed panels where ft-handtest put its own (0.15 m below the eyes) at a range of
    // distances: the hands cross some of them. Presses, the near claim, grabs.
    for (double z : {0.30, 0.35, 0.40, 0.45, 0.50, 0.60}) {
        Panel P = TestPanel(-z);
        P.pose.m[1][3] = -0.15f;
        P.width = 1.2, P.height = 0.675;
        P.controls.clear();
        const Run r = Play(recs, 0, recs.size(), {P});
        const auto &s = r.stats;
        std::printf("panel %.2f m ahead: near %4.1f%% of frames, %3d presses (%.1f/min), %d grabs; held back: %d one view, "
                    "%d bent, %d not nearest, %d pinching, %d busy, %d slow\n",
                    z, 100.0 * r.nearFrames / std::max(1, r.frames), r.presses, r.presses / secs * 60, r.grabs,
                    s.heldNotStereo, s.heldNotExtended, s.heldNotNearest, s.heldPinching, s.heldBusy, s.heldSlow);
    }
    // 3. The matte: every frame's hands against a panel 0.6 m ahead, CPU cost of building and
    // projecting it, mesh against capsules.
    {
        handcut::Hands hands;
        handcut::Panel P{At(0, -0.15, -0.6), 1.2, 0.675, 0, 3440, 1935};
        const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}}, head[3] = {0, 0, 0};
        for (bool mesh : {true, false}) {
            handcut::MatteParams mp;
            mp.mesh = mesh;
            handcut::Matte m;
            handcut::Spots s[2];
            std::vector<double> us;
            size_t pieces = 0, bad = 0, withHands = 0;
            for (const Record &r : recs) {
                hands.Feed(r.hands.data(), r.hands.size(), &r.g, Identity(), r.publish);
                const auto t0 = std::chrono::steady_clock::now();
                handcut::BuildMatte(hands.shown(), hands.capsules(), head, mp, m);
                const bool any = handcut::Project(P, m, eyes, s);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
                withHands += any;
                for (int e = 0; e < 2; ++e) {
                    pieces += s[e].caps.size() + s[e].hulls.size();
                    for (const auto &c : s[e].caps) bad += !std::isfinite(c.ax) || !std::isfinite(c.ra);
                    for (const auto &h : s[e].hulls) bad += h.n < 3 || !std::isfinite(h.r);
                }
            }
            std::sort(us.begin(), us.end());
            std::printf("matte %-8s: %zu frames with hands on the panel, %.1f pieces per eye, %zu bad; build+project "
                        "median %.0f us, 90%% %.0f us, max %.0f us\n",
                        mesh ? "mesh" : "capsules", withHands, withHands ? pieces / 2.0 / withHands : 0.0, bad,
                        us[us.size() / 2], us[us.size() * 9 / 10], us.back());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--replay")) {
        int rc = 0;
        for (int i = 2; i < argc; ++i) rc |= Replay(argv[i]);
        return rc;
    }
    TestGeometry();
    TestZones();
    TestCarry();
    TestStretch();
    TestHover();
    TestPoke();
    TestSlowDrift();
    TestGuards();
    TestRelease();
    TestDeep();
    TestDrag();
    TestLost();
    TestControlPoke();
    TestNearClaim();
    TestGrabMove();
    TestGrabResize();
    TestStretchHands();
    TestV2Ingest();
    TestMatte();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
