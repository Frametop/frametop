// Offline tests for near hand input (handinput.cpp), the palm menu (palmmenu.cpp) and the hand
// matte (handcut.cpp).
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
#include "../grabthat.h"
#include "../palmmenu.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
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

// A right hand pinching, pointing ahead (-z), palm down: the thumb and index tips together 3 cm
// ahead of the knuckles and 4.5 cm under them; `open` 0..1 parts them (the thumb down and in, the
// index up and out), 5 cm apart at 1. Points relative to the wrist.
void PinchingHand(float out[21][3], double open = 0, int fingers = 0) {
    static const float base[21][3] = {
        {0, 0, 0},           {-0.025f, -0.005f, -0.03f}, {-0.04f, -0.015f, -0.055f}, {-0.04f, -0.03f, -0.085f},
        {-0.03f, -0.045f, -0.108f}, {-0.02f, 0, -0.085f},  {-0.022f, -0.012f, -0.118f}, {-0.024f, -0.03f, -0.128f},
        {-0.026f, -0.046f, -0.118f}, {0, 0, -0.088f},     {0, -0.03f, -0.10f},        {0, -0.035f, -0.08f},
        {0, -0.02f, -0.07f}, {0.018f, 0, -0.083f},        {0.018f, -0.03f, -0.095f},  {0.018f, -0.033f, -0.078f},
        {0.018f, -0.02f, -0.07f}, {0.034f, 0, -0.075f},   {0.034f, -0.025f, -0.085f}, {0.034f, -0.028f, -0.07f},
        {0.034f, -0.018f, -0.065f}};
    std::memcpy(out, base, sizeof base);
    // fingers 1: the middle finger with the index, its tip beside the index tip (a card held between
    // the thumb and two fingers); 2: all four fingertips bunched at the thumb
    if (fingers >= 1) {
        static const float mid[3][3] = {{0.0f, -0.015f, -0.12f}, {-0.004f, -0.035f, -0.128f}, {-0.008f, -0.048f, -0.12f}};
        std::memcpy(out[10], mid, sizeof mid);
    }
    if (fingers >= 2) {
        static const float ring[3][3] = {{0.016f, -0.015f, -0.112f}, {0.01f, -0.035f, -0.12f}, {0.005f, -0.05f, -0.115f}};
        static const float little[3][3] = {{0.03f, -0.015f, -0.1f}, {0.02f, -0.035f, -0.108f}, {0.012f, -0.048f, -0.105f}};
        std::memcpy(out[14], ring, sizeof ring), std::memcpy(out[18], little, sizeof little);
    }
    const float th[3] = {-0.004f, -0.03f, 0.012f}, ix[3] = {0.0f, 0.012f, -0.02f};
    for (int i = 0; i < 3; ++i) {
        out[4][i] += float(open * th[i]), out[3][i] += float(open * th[i] * 0.5);
        out[8][i] += float(open * ix[i]), out[7][i] += float(open * ix[i] * 0.6);
        if (fingers >= 1) out[12][i] += float(open * ix[i]), out[11][i] += float(open * ix[i] * 0.6);
    }
}

std::mt19937 g_jitter(11);

struct SynthHand {
    bool present = true, right = true, stereo = true, bent = false;
    uint32_t id = 1;
    double tip[3] = {0, 0, -0.4};   // where the index tip is (head frame = room here)
    // or, for grabs: where the pinch point (index and middle knuckles) is
    bool byPinch = false;
    double pinch[3] = {0, 0, 0};
    // or a pinching hand (PinchingHand), placed by where its thumb and index tips meet (contact)
    bool pinching = false, byContact = false;
    double open = 0;
    int fingers = 0;                   // PinchingHand's: 1 the middle with the index, 2 all four bunched
    double contact[3] = {0, 0, 0};
    // ...turned by this (room from hand: its columns are the hand's x, y, z) instead of the angles below
    bool useRot = false;
    double rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double jitter = 0;                 // each joint off by this much (m, Gaussian per axis): landmark noise
    double pitchDeg = 0, yawDeg = 0;   // the hand turned about the tip: down (pitch < 0), to the right (yaw < 0)
    double twistDeg = 0;               // ...and first about its own axis (counterclockwise as seen from behind)
    // or an open hand for the palm menu (PalmHand): its palm's centre here
    bool palm = false;
    double palmAt[3] = {0, -0.25, -0.35};
    double rollDeg = 0;                // about the fingers' axis: 0 palm up, 180 palm down
    double tiltDeg = 20;               // fingertips up: the palm turns toward the head behind it
    int curl = 0;                      // middle, ring and little: 0 open, 1 half curled, 2 a fist
    int curlIndex = 0;                 // ...and the index, likewise (grab that's fist)
    // a pointing hand (PointingHand) whose middle finger the tracker has nearly straight: its tip
    // this much deeper (toward where the index points) than the index tip (NaN: curled)
    double middleTip = NAN;
};

// An open hand, palm up, fingers ahead (-z), as SynthHand says; the right hand's thumb is on +x.
void PalmHand(const SynthHand &h, float out[21][3]) {
    static const float base[21][3] = {
        {0, 0, 0},
        {0.02f, 0, -0.02f},       {0.04f, 0.005f, -0.04f},  {0.055f, 0.01f, -0.06f},  {0.065f, 0.015f, -0.075f},
        {0.025f, 0, -0.09f},      {0.027f, 0.005f, -0.13f}, {0.028f, 0.01f, -0.155f}, {0.029f, 0.012f, -0.175f},
        {0.005f, 0, -0.095f},     {0.005f, 0.005f, -0.14f}, {0.005f, 0.01f, -0.168f}, {0.005f, 0.012f, -0.19f},
        {-0.013f, 0, -0.09f},     {-0.013f, 0.005f, -0.13f}, {-0.013f, 0.01f, -0.155f}, {-0.013f, 0.012f, -0.175f},
        {-0.03f, 0, -0.08f},      {-0.03f, 0.005f, -0.11f}, {-0.03f, 0.01f, -0.13f},  {-0.03f, 0.012f, -0.145f}};
    std::memcpy(out, base, sizeof base);
    // curled: the last three joints of the middle, ring and little fingers fold toward the palm
    static const float half[3][3][3] = {{{0.005f, 0.02f, -0.13f}, {0.005f, 0.035f, -0.135f}, {0.005f, 0.04f, -0.13f}},
                                        {{-0.013f, 0.02f, -0.125f}, {-0.013f, 0.035f, -0.13f}, {-0.013f, 0.04f, -0.125f}},
                                        {{-0.03f, 0.02f, -0.11f}, {-0.03f, 0.03f, -0.115f}, {-0.03f, 0.035f, -0.115f}}};
    static const float fist[3][3][3] = {{{0.005f, 0.03f, -0.11f}, {0.005f, 0.04f, -0.09f}, {0.005f, 0.03f, -0.07f}},
                                        {{-0.013f, 0.03f, -0.105f}, {-0.013f, 0.04f, -0.085f}, {-0.013f, 0.03f, -0.068f}},
                                        {{-0.03f, 0.03f, -0.095f}, {-0.03f, 0.035f, -0.08f}, {-0.03f, 0.028f, -0.065f}}};
    for (int f = 0; f < 3 && h.curl; ++f) std::memcpy(out[10 + 4 * f], h.curl == 1 ? half[f] : fist[f], sizeof half[f]);
    static const float halfIndex[3][3] = {{0.027f, 0.02f, -0.13f}, {0.027f, 0.035f, -0.135f}, {0.027f, 0.04f, -0.13f}};
    static const float fistIndex[3][3] = {{0.027f, 0.03f, -0.11f}, {0.027f, 0.04f, -0.09f}, {0.027f, 0.03f, -0.07f}};
    if (h.curlIndex) std::memcpy(out[6], h.curlIndex == 1 ? halfIndex : fistIndex, sizeof halfIndex);
    const double r = h.rollDeg * M_PI / 180, a = h.tiltDeg * M_PI / 180;
    double c[3] = {0, 0, 0};
    for (int j = 0; j < 21; ++j) {
        float *q = out[j];
        const double x = h.right ? q[0] : -q[0], y = q[1], z = q[2];
        const double x1 = x * std::cos(r) - y * std::sin(r), y1 = x * std::sin(r) + y * std::cos(r);   // roll about z
        const double y2 = y1 * std::cos(a) - z * std::sin(a), z2 = y1 * std::sin(a) + z * std::cos(a); // tilt about x
        q[0] = float(x1), q[1] = float(y2), q[2] = float(z2);
    }
    for (int j : {0, 5, 9, 13, 17})
        for (int i = 0; i < 3; ++i) c[i] += out[j][i] / 5.0;
    for (int j = 0; j < 21; ++j)
        for (int i = 0; i < 3; ++i) out[j][i] = float(out[j][i] - c[i] + h.palmAt[i]);
}

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
            if (h.palm) {
                PalmHand(h, o.pts);
                continue;
            }
            float pts[21][3];
            if (h.pinching) PinchingHand(pts, h.open, h.fingers);
            else PointingHand(pts, h.bent);
            if (!h.pinching && !std::isnan(h.middleTip)) {   // the middle finger straight, beside the index
                const float tipZ = pts[8][2] - float(h.middleTip), mcpZ = pts[9][2];
                for (int j = 10; j <= 12; ++j)
                    pts[j][0] = pts[9][0], pts[j][1] = pts[9][1], pts[j][2] = mcpZ + (tipZ - mcpZ) * float(j - 9) / 3;
            }
            const double a = h.pitchDeg * M_PI / 180, b = h.yawDeg * M_PI / 180, w = h.twistDeg * M_PI / 180;
            for (auto &q : pts) {   // about z (twist), then x (pitch), then y (yaw); the tip is the origin here
                if (h.useRot) {
                    double r[3];
                    for (int i = 0; i < 3; ++i) r[i] = h.rot[i][0] * q[0] + h.rot[i][1] * q[1] + h.rot[i][2] * q[2];
                    q[0] = float(r[0]), q[1] = float(r[1]), q[2] = float(r[2]);
                    continue;
                }
                const double x0 = q[0] * std::cos(w) - q[1] * std::sin(w), y0 = q[0] * std::sin(w) + q[1] * std::cos(w);
                q[0] = float(x0), q[1] = float(y0);
                const double y = q[1] * std::cos(a) - q[2] * std::sin(a), z = q[1] * std::sin(a) + q[2] * std::cos(a);
                const double x = q[0] * std::cos(b) + z * std::sin(b), z2 = -q[0] * std::sin(b) + z * std::cos(b);
                q[0] = float(x), q[1] = float(y), q[2] = float(z2);
            }
            double off[3];
            if (h.byContact)
                for (int i = 0; i < 3; ++i) off[i] = h.contact[i] - (pts[4][i] + pts[8][i]) / 2;
            else if (h.byPinch)
                for (int i = 0; i < 3; ++i) off[i] = h.pinch[i] - (pts[5][i] + pts[9][i]) / 2;
            else
                for (int i = 0; i < 3; ++i) off[i] = h.tip[i];
            std::normal_distribution<double> jn(0, h.jitter > 0 ? h.jitter : 1);
            for (int j = 0; j < 21; ++j)
                for (int i = 0; i < 3; ++i) o.pts[j][i] = float(pts[j][i] + off[i] + (h.jitter > 0 ? jn(g_jitter) : 0));
        }
        file.v1.nhands = n;
        g.capture_ns = uint64_t(t), g.publish_ns = uint64_t(t + 30'000'000);
        g.seq += 2;
    }
    // a pinch (or a grip: Grip) on side s begins or ends at time t, at the point p
    void Pinch(int s, bool down, int64_t t, const double p[3], uint32_t hand, bool grip = false) {
        fh_pinch_t &o = grip ? g.grip[s] : g.pinch[s];
        o.hand_id = hand;
        o.flags = FH_PINCH_TRACKED | (down ? FH_PINCH_DOWN : 0);
        if (down) ++o.begins, o.begin_ns = uint64_t(t);
        else ++o.ends, o.end_ns = uint64_t(t);
        o.strength = down ? 1 : 0;
        for (int i = 0; i < 3; ++i) o.point[i] = float(p[i]), o.begin_point[i] = down ? float(p[i]) : o.begin_point[i];
    }
    void Grip(int s, bool down, int64_t t, const double p[3], uint32_t hand) { Pinch(s, down, t, p, hand, true); }
};

struct Rig {
    handcut::Hands hands;
    HandInput in;
    Synth synth;
    std::vector<Panel> panels{TestPanel()};
    int64_t t = 1'000'000'000;
    bool allowed[2] = {true, true};
    bool busy = false;
    bool typing[2] = {false, false};
    bool armed[2] = {false, false};   // "grab that" has the hand (Input::grabThatArmed)
    std::vector<Event> events;
    std::vector<Carry> carries;
    std::vector<std::string> logs;
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
            i.typing[0] = typing[0], i.typing[1] = typing[1];
            i.grabThatArmed[0] = armed[0], i.grabThatArmed[1] = armed[1];
            in.Update(i, panels);
            for (const Event &e : in.events()) events.push_back(e);
            for (const std::string &l : in.logs()) {
                logs.push_back(l);
                if (std::getenv("HANDINPUT_LOG")) std::printf("    [%.3f] %s\n", now / 1e9, l.c_str());
            }
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

// A pinching hand whose thumb and index tips meet at u, v, d in front of TestPanel (the head's
// frame is the room's: pointing ahead, at the panel).
SynthHand PinchAt(double u, double v, double d, bool right = true, uint32_t id = 1) {
    SynthHand h;
    h.pinching = h.byContact = true, h.right = right, h.id = id;
    h.contact[0] = u, h.contact[1] = v, h.contact[2] = -0.5 + d;
    return h;
}
// ...placed by its knuckles (the point a grab carries: they hold still while the hand turns about
// them), at u, v, d; its fingers meet 1.8 cm left of them, 4.5 cm lower and 2.6 cm nearer the panel.
SynthHand KnucklesAt(double u, double v, double d, bool right = true, uint32_t id = 1) {
    SynthHand h;
    h.pinching = h.byPinch = true, h.right = right, h.id = id;
    h.pinch[0] = u, h.pinch[1] = v, h.pinch[2] = -0.5 + d;
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
    CHECK(GrabZone(p, {0.05, barY, 0.09}, k, &sx, &sy, &e) == Zone::Bar && e > 0);   // too far in front (past the zone)
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

// The second live test's refusals (2026-10-10): the middle finger read nearly as straight as the
// index, its tip about as deep; a tap just past the bottom edge (the desktop's panel is there); a
// short tap; a hand reaching under the panel for its bar.
void TestPokeRefusals() {
    // the middle tip 2 mm behind the index tip's depth, then 4 mm deeper: presses (the old rule wanted
    // it 5 mm behind); 1.2 cm deeper (a flat hand, the middle finger longest): no press
    for (double gap : {-0.002, 0.004, 0.012}) {
        Rig r;
        for (int mm = 60; mm >= -15; mm -= 10) {
            SynthHand h = TipAt(0.1, 0.05, mm / 1000.0);
            h.middleTip = gap, h.pitchDeg = 25;
            r.Frame({h});
        }
        std::printf("poke with the middle finger straight, its tip %+.1f cm deeper than the index's: %d press(es)\n", gap * 100,
                    r.Count(Event::Press));
        CHECK(r.Count(Event::Press) == (gap < 0.01 ? 1 : 0));
        if (gap > 0.01) CHECK(r.in.stats.heldNotNearest > 0);
    }
    {
        // just under the bottom edge (1 cm): presses at the bottom row of pixels
        Rig r;
        r.panels[0].controls.clear();
        for (int mm = 60; mm >= -15; mm -= 10) r.Frame({TipAt(0.2, -0.28125 - 0.01, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 1 && r.in.stats.edgePresses == 1);
        const auto p = r.Last(Event::Press);
        CHECK(p && p->y > 1078 && std::fabs(p->x - 1344) < 3);
    }
    {
        // 4 cm under it (a hand reaching for the bar): no press, and not a refused poke
        Rig r;
        r.panels[0].controls.clear();
        for (int mm = 60; mm >= -50; mm -= 10) r.Frame({TipAt(0.2, -0.28125 - 0.04, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 0 && r.in.stats.refused == 0 && r.in.stats.offContent == 1);
    }
    {
        // a short tap: hovering 2 cm out, then 1.8 cm in at 0.15 m/s (5 mm a frame), one frame past
        Rig r;
        for (int k = 0; k < 10; ++k) r.Frame({TipAt(0, 0, 0.02)});
        for (int mm = 15; mm >= -2; mm -= 5) r.Frame({TipAt(0, 0, mm / 1000.0)});
        for (int mm = 10; mm <= 30; mm += 10) r.Frame({TipAt(0, 0, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 1 && r.in.stats.quickPresses == 1);
    }
}

void TestSlowDrift() {
    Rig r;
    // creeping in at 3 mm/s, 2 cm past the surface: not coming at it, no press
    double d = 0.02;
    for (int k = 0; k < 400; ++k) {
        d -= 0.0001;
        r.Frame({TipAt(0, 0, d)}, 1);
    }
    CHECK(r.Count(Event::Press) == 0 && r.in.stats.heldSlow > 0);
}

// Taps with tracking as the headset has it (live test 2026-10-10: the cursor followed the fingertip,
// taps didn't click). Each tap: the hand 12 cm out, then in along the panel's normal at 0.15-0.6
// m/s to `over` past the surface (negative: short of it), a 0-60 ms stop, and back out at the same
// speed; the finger 0-35 degrees off the normal either way, anywhere on the panel; seen at 30 Hz at
// a random phase. The hand's place carries noise of `sigma` (m, each axis, correlated frame to
// frame: half the last frame's), and each joint `jitter` more on its own.
struct TapResult { int taps = 0, pressed = 0, doubled = 0; };
TapResult NoisyTaps(const Params &k, double sigma, double jitter, double over, int n, uint32_t seed) {
    std::mt19937 rng(seed);
    g_jitter.seed(seed + 1);
    std::uniform_real_distribution<double> U(0, 1);
    std::normal_distribution<double> N(0, 1);
    TapResult res;
    for (int i = 0; i < n; ++i) {
        Rig r;
        r.in.params() = k;
        const double v = 0.15 + 0.45 * U(rng), dwell = 0.06 * U(rng), phase = U(rng) / 30;
        const double pitch = -35 + 70 * U(rng), yaw = -20 + 40 * U(rng), u0 = -0.3 + 0.6 * U(rng), v0 = -0.18 + 0.36 * U(rng);
        const double from = 0.12, tin = (from + over) / v, total = 0.3 + 2 * tin + dwell + 0.3;
        double nz[3] = {0, 0, 0};
        for (double t = phase; t < total; t += 1 / 30.0) {
            double d = from;
            if (t >= 0.3 && t < 0.3 + tin) d = from - v * (t - 0.3);
            else if (t >= 0.3 + tin && t < 0.3 + tin + dwell) d = -over;
            else if (t >= 0.3 + tin + dwell && t < 0.3 + 2 * tin + dwell) d = -over + v * (t - 0.3 - tin - dwell);
            for (double &x : nz) x = 0.5 * x + std::sqrt(0.75) * sigma * N(rng);
            SynthHand h = TipAt(u0 + nz[0], v0 + nz[1], d + nz[2]);
            h.pitchDeg = pitch, h.yawDeg = yaw, h.jitter = jitter;
            r.Frame({h});
        }
        const int p = r.Count(Event::Press);
        ++res.taps, res.pressed += p > 0, res.doubled += p > 1;
    }
    return res;
}

// A finger held still `at` in front of the panel for `seconds`, with the same noise: presses a minute.
double NoisyHover(const Params &k, double sigma, double jitter, double at, double seconds, uint32_t seed) {
    std::mt19937 rng(seed);
    g_jitter.seed(seed + 1);
    std::normal_distribution<double> N(0, 1);
    Rig r;
    r.in.params() = k;
    double nz[3] = {0, 0, 0};
    for (int f = 0; f < int(seconds * 30); ++f) {
        for (double &x : nz) x = 0.5 * x + std::sqrt(0.75) * sigma * N(rng);
        SynthHand h = TipAt(0.1 + nz[0], 0.05 + nz[1], at + nz[2]);
        h.jitter = jitter;
        r.Frame({h});
    }
    if (std::getenv("HANDINPUT_HOVER_LOG"))
        for (const std::string &l : r.logs)
            if (l.find("pressed") != std::string::npos) std::printf("    hover %.1f cm, %.0f mm: %s\n", at * 100, sigma * 1000, l.c_str());
    return r.Count(Event::Press) / seconds * 60;
}

void TestPokeNoisy() {
    Params now, old;
    if (const char *v = std::getenv("HANDINPUT_TRAVEL")) now.travel = std::atof(v);   // trying other values
    if (const char *v = std::getenv("HANDINPUT_QUICK_SPEED")) now.quickSpeed = std::atof(v);
    old.pressFrames = 2, old.travel = 1e9, old.extended = 1.6;   // the rules before 2026-10-10's live test
    std::printf("noisy taps (pressed of 60; old rules: two frames past the surface):\n");
    for (double sigma : {0.003, 0.006, 0.010}) {
        const double jitter = sigma / 2;
        std::printf("  hand noise %.0f mm + %.1f mm per joint:", sigma * 1000, jitter * 1000);
        for (double over : {-0.005, 0.0, 0.005, 0.01, 0.02}) {
            const TapResult a = NoisyTaps(now, sigma, jitter, over, 60, 100), b = NoisyTaps(old, sigma, jitter, over, 60, 100);
            std::printf("  %+.1f cm: %d (old %d)%s", over * 100, a.pressed, b.pressed, a.doubled ? " doubles!" : "");
            if (over >= 0.01) CHECK(a.pressed >= (sigma > 0.008 ? 51 : 57));
            if (over >= 0.005 && sigma < 0.008) CHECK(a.pressed >= 50);
            CHECK(a.doubled <= 1);
        }
        std::printf("\n");
        const double h15 = NoisyHover(now, sigma, jitter, 0.015, 120, 7), h25 = NoisyHover(now, sigma, jitter, 0.025, 120, 8);
        const double o15 = NoisyHover(old, sigma, jitter, 0.015, 120, 7), o25 = NoisyHover(old, sigma, jitter, 0.025, 120, 8);
        std::printf("  ...a finger held still 1.5 cm out: %.1f presses a minute (old %.1f); 2.5 cm out: %.1f (old %.1f)\n",
                    h15, o15, h25, o25);
        // a still finger: no more false presses than the old rules made (and none at 2.5 cm with
        // the noise a still hand has: 3-6 mm, the replays' 12-23 mm range in a second)
        // (10 mm a frame on a still hand is past what ft-hands does: there both rules press from a
        // finger held 1.5-2.5 cm out, the one-frame rule more)
        // (since 2026-10-10's second live test, one frame presses after 1.7 cm in: at 1.5 cm out with
        // 6 mm of noise that's 4 a minute against the old 2; the 2.5 cm hover and 3 mm stay at none)
        if (sigma < 0.008) CHECK(h15 <= o15 + 2.5), CHECK(h25 == 0);
        if (sigma < 0.004) CHECK(h15 == 0);
    }
}

// A panel pitched back by `deg` (its front facing up toward the eyes), centred at c.
Panel Tilted(double deg, double cy, double cz) {
    Panel p = TestPanel();
    const double a = deg * M_PI / 180;
    p.pose = At(0, cy, cz);
    p.pose.m[1][1] = float(std::cos(a)), p.pose.m[1][2] = float(std::sin(a));
    p.pose.m[2][1] = float(-std::sin(a)), p.pose.m[2][2] = float(std::cos(a));
    p.controls.clear();
    return p;
}

// A poke along the panel's normal at u, v: from 6 cm out to 1.5 cm in, 1 cm a frame, and back.
int Poke(Rig &r, double u, double v, double pitch, double yaw, double extraX = 0) {
    const int before = r.Count(Event::Press);
    for (int mm = 60; mm >= -15; mm -= 10) {
        double at[3];
        FromSurface(r.panels[0], u, v, mm / 1000.0, at);
        SynthHand h;
        for (int i = 0; i < 3; ++i) h.tip[i] = at[i];
        h.tip[0] += extraX;
        h.pitchDeg = pitch, h.yawDeg = yaw;
        r.Frame({h});
    }
    for (int k = 0; k < 6; ++k) {
        double at[3];
        FromSurface(r.panels[0], u, v, 0.02 + 0.01 * k, at);
        SynthHand h;
        for (int i = 0; i < 3; ++i) h.tip[i] = at[i];
        h.tip[0] += extraX;
        h.pitchDeg = pitch, h.yawDeg = yaw;
        r.Frame({h});
    }
    return r.Count(Event::Press) - before;
}

void TestApproachFromFront() {
    // straight in: a press; the finger 60 degrees off the normal: none
    {
        Rig r;
        CHECK(Poke(r, 0, 0, 0, 0) == 1);
        CHECK(Poke(r, 0.1, 0, 0, 60) == 0 && r.in.stats.heldAskew > 0);
        CHECK(Poke(r, -0.1, 0, 0, 40) == 1);   // within 50 degrees: still a poke
    }
    // a low panel (0.45 m under the eyes, pitched back 50 degrees to face them):
    // reached for 0.45 m out, a poke; with the hand resting by the body (0.2 m out), none
    {
        Rig r;
        r.panels = {Tilted(50, -0.45, -0.45)};
        CHECK(Poke(r, 0, 0, -50, 0) == 1);
    }
    {
        Rig r;
        r.panels = {Tilted(50, -0.45, -0.22)};
        CHECK(Poke(r, 0, 0, -50, 0) == 0 && r.in.stats.heldResting > 0);
    }
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
    for (int k = 0; k < 45; ++k) r.Frame({gone});   // 1.5 s: held (Params::holdLostMs)
    CHECK(r.in.pressing(1) && r.Count(Event::Release) == 0);
    for (int k = 0; k < 10; ++k) r.Frame({gone});
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
    r.Frame({TipAt(0, 0, 0.09)});
    CHECK(!r.in.near(1));
    r.Frame({TipAt(0, 0, 0.06)});   // within 3 inches (nearFront 0.075)
    CHECK(r.in.near(1));
    for (int k = 0; k < 10; ++k) r.Frame({TipAt(0, 0, 0.095)});
    CHECK(r.in.near(1));   // near stays near up to 2.5 cm farther out (nearLeave)
    r.Frame({TipAt(0, 0, 0.20)});
    CHECK(r.in.near(1));   // lingers 200 ms
    for (int k = 0; k < 10; ++k) r.Frame({TipAt(0, 0, 0.20)});
    CHECK(!r.in.near(1));
    r.Frame({TipAt(0, 0, 0.09)});   // coming back: not near until within nearFront again
    CHECK(!r.in.near(1));
    r.Frame({TipAt(0.7, 0, 0.05)});   // off to the side of the panel
    CHECK(!r.in.near(1));
    // in the zone's margin (7.5-10 cm, after being in it) the ray stays hidden (near: "2" to the
    // helper), but the hand's pinches are the helper's (not claimed: "1" only within the zone)
    Rig m;
    for (int k = 0; k < 5; ++k) m.Frame({PinchAt(0, 0, 0.05)});
    CHECK(m.in.near(1) && m.in.claims(1));
    for (int k = 0; k < 3; ++k) m.Frame({PinchAt(0, 0, 0.09)});
    CHECK(m.in.near(1) && !m.in.claims(1));
    const double mat[3] = {0, 0, -0.41};
    m.synth.Pinch(1, true, m.t, mat, 1);
    m.Frame({PinchAt(0, 0, 0.09)});
    CHECK(m.Count(Event::Press) == 0 && m.Count(Event::GrabStart) == 0);
    // a pinch 10 cm out (beyond the zone) is the ray's or the gaze's: no grab, no click
    Rig f;
    for (int k = 0; k < 5; ++k) f.Frame({PinchAt(0, 0, 0.10)});
    CHECK(!f.in.near(1));
    const double at[3] = {0, 0, -0.40};
    f.synth.Pinch(1, true, f.t, at, 1);
    f.Frame({PinchAt(0, 0, 0.10)});
    CHECK(f.Count(Event::Press) == 0 && f.Count(Event::GrabStart) == 0);
    // a hand resting low by the body (typing under a low panel, its palm 0.35 m under the eyes and
    // 0.15 m out, under a panel 0.30 m out) is near only within restFront (0.04): its pinches stay
    // the helper's
    Rig low;
    low.panels[0].pose = At(0, -0.35, -0.30);
    auto lowTip = [](double d) {
        SynthHand h;
        h.tip[0] = 0, h.tip[1] = -0.35, h.tip[2] = -0.30 + d;
        return h;
    };
    for (int k = 0; k < 5; ++k) low.Frame({lowTip(0.06)});
    CHECK(!low.in.near(1));
    low.Frame({lowTip(0.03)});
    CHECK(low.in.near(1));
    // the same distance with the hand raised out of the resting zone: near
    Rig up;
    up.panels[0].pose = At(0, -0.1, -0.30);
    SynthHand raised;
    raised.tip[0] = 0, raised.tip[1] = -0.1, raised.tip[2] = -0.30 + 0.06;
    for (int k = 0; k < 5; ++k) up.Frame({raised});
    CHECK(up.in.near(1));
}

// Typing on a real keyboard (the helper's "typing"): a hand it holds back neither pokes nor grabs, and
// the cursor doesn't follow its fingertip; the other hand still does; a press or grab already held
// carries on to its release.
void TestTyping() {
    {
        Rig r;
        r.typing[1] = true;
        for (int mm = 100; mm >= -15; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 0 && r.in.stats.heldTyping > 0);
        CHECK(r.Count(Event::Motion) == 0);
    }
    {
        Rig r;
        r.typing[0] = true;   // the left hand typing: the right one pokes as ever
        for (int mm = 100; mm >= -15; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 1);
        r.typing[1] = true;   // typing starts mid-press: it ends as usual, pulled back
        for (int mm = -15; mm <= 50; mm += 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Count(Event::Release) == 1 && !r.Last(Event::Release)->lost);
    }
    {
        Rig r;
        r.typing[1] = true;
        const double barY = r.panels[0].controls[0].v;
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.05, barY, 0.02)});
        const double at[3] = {0.05, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(0.05, barY, 0.02)});
        CHECK(r.Count(Event::GrabStart) == 0 && !r.in.grabbing(1));
    }
}

void TestGrabMove() {
    Rig r;
    const double barY = r.panels[0].controls[0].v;
    // the hand comes near the bar (its fingertip over the panel), then pinches there
    for (int k = 0; k < 5; ++k) r.Frame({KnucklesAt(0.05, barY, 0.02)});
    CHECK(r.in.near(1));
    const double at[3] = {0.05, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({KnucklesAt(0.05, barY, 0.02)});
    CHECK(r.Count(Event::GrabStart) == 1 && r.in.grabbing(1));
    CHECK(r.Last(Event::GrabStart)->zone == Zone::Bar);
    CHECK(r.in.hovered().count(0) && (r.in.hovered().at(0) & 1));   // the bar lights
    // carried 10 cm up and 20 cm closer, slowly (the carry follows the smoothed pinch point)
    const double g0[3] = {0.05 + 0.01, barY, -0.5 + 0.02};   // where it took the panel (PinchAt's hand)
    (void)g0;
    for (int k = 1; k <= 30; ++k) {
        r.panels[0].pose = r.carries.empty() ? r.panels[0].pose : r.carries[0].pose;
        const int j = std::min(k, 20);
        r.Frame({KnucklesAt(0.05, barY + 0.005 * j, 0.02 + 0.01 * j)});
    }
    CHECK(!r.carries.empty() && r.carries[0].kind == Carry::Move);
    if (!r.carries.empty()) {
        const Mat &p = r.carries[0].pose;
        // the grab point (on the bar, 5 cm right of the middle) is under the pinch point, and the
        // panel faces the head
        // (the panel behind it by the grab's dead zone, 1 cm, the way it first moved: up and nearer)
        const Hit g = Locate(PanelAt(p, 1.0), r.hands.seen()[0].pinchPoint);
        NEAR(g.u, 0.05, 0.005);
        CHECK(std::hypot(g.v - barY, g.d - 0.02) <= 0.015 && g.v >= barY - 0.001 && g.d >= 0.02 - 0.001);
        const double c = -(p.m[0][2] * p.m[0][3] + p.m[1][2] * p.m[1][3] + p.m[2][2] * p.m[2][3]) /
                         std::sqrt(p.m[0][3] * p.m[0][3] + p.m[1][3] * p.m[1][3] + p.m[2][3] * p.m[2][3]);
        CHECK(c > 0.995);
        NEAR(r.hands.shown()[0].pinchPoint[2], -0.28, 0.002);
    }
    r.synth.Pinch(1, false, r.t, at, 1);
    {
        SynthHand o = KnucklesAt(0.05, barY + 0.1, 0.22);
        o.open = 1;   // (the fingers open: let go on the second frame, Params::releaseFrames)
        r.Frame({o});
        CHECK(r.in.grabbing(1));
        r.Frame({o});
    }
    CHECK(r.Count(Event::GrabEnd) == 1 && !r.in.grabbing(1));
    // a pinch on the content clicks where the fingers meet (Params::pinchClick), held while it is
    Rig c;
    for (int k = 0; k < 5; ++k) c.Frame({PinchAt(0, 0, 0.01)});
    const double mid[3] = {0, 0, -0.48};
    c.synth.Pinch(1, true, c.t, mid, 1);
    c.Frame({PinchAt(0, 0, 0.01)});
    CHECK(c.Count(Event::GrabStart) == 0 && c.Count(Event::Press) == 1 && c.in.pressing(1));
    if (const auto e = c.Last(Event::Press)) NEAR(e->x, 960, 2), NEAR(e->y, 540, 2);
    for (int k = 0; k < 5; ++k) c.Frame({PinchAt(0, 0, 0.01)});
    CHECK(c.Count(Event::Release) == 0);
    c.synth.Pinch(1, false, c.t, mid, 1);
    {
        SynthHand o = PinchAt(0, 0, 0.01);
        o.open = 1;
        c.Frame({o}), c.Frame({o});
    }
    CHECK(c.Count(Event::Release) == 1 && !c.in.pressing(1));
    // ...off (HANDS_PINCH_CLICK=0): nothing
    Rig o;
    o.in.params().pinchClick = false;
    for (int k = 0; k < 5; ++k) o.Frame({PinchAt(0, 0, 0.01)});
    o.synth.Pinch(1, true, o.t, mid, 1);
    o.Frame({PinchAt(0, 0, 0.01)});
    CHECK(o.Count(Event::GrabStart) == 0 && o.Count(Event::Press) == 0);
    // nor a pinch that began before the hand was near (the helper's)
    Rig f;
    const double far[3] = {0.05, barY, -0.2};
    f.synth.Pinch(1, true, f.t, far, 1);
    f.Frame({KnucklesAt(0.05, barY, 0.3)});
    for (int k = 0; k < 5; ++k) f.Frame({KnucklesAt(0.05, barY, 0.02)});
    CHECK(f.Count(Event::GrabStart) == 0 && f.Count(Event::Press) == 0);
}

// A near grab's smoothing (Params::grab*, fh_smooth.h): a still noisy hand leaves the panel still
// (the dead zone), a steady move carries it smoothly with no jump when it starts, a fast move
// arrives without overshooting, and a new grab starts afresh.
void TestGrabSmooth() {
    // where the panel's grab point is (room), from the carry
    auto grabPoint = [](const Rig &r, const double local[3]) {
        const Mat &p = r.carries[0].pose;
        std::vector<double> out(3);
        for (int i = 0; i < 3; ++i) out[i] = p.m[i][0] * local[0] + p.m[i][1] * local[1] + p.m[i][2] * local[2] + p.m[i][3];
        return out;
    };
    Rig r;
    const double barY = r.panels[0].controls[0].v;
    for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.05, barY, 0.02)});
    const double at[3] = {0.05, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({PinchAt(0.05, barY, 0.02)});
    CHECK(r.in.grabbing(1));
    // the grab point, panel-local: where the pinch point was on the panel
    const Hit g0 = Locate(r.panels[0], r.hands.seen()[0].pinchPoint);
    const double local[3] = {g0.u, g0.v, g0.d};
    // 1 s held, 3 mm of noise on each axis (ft-hands' depth jitter): the panel doesn't move
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0, 0.003);
    double still = 0;
    std::vector<double> first;
    for (int k = 0; k < 30; ++k) {
        r.Frame({PinchAt(0.05 + n(rng), barY + n(rng), 0.02 + n(rng))});
        if (r.carries.empty()) continue;
        const auto gp = grabPoint(r, local);
        if (first.empty()) first = gp;
        still = std::max(still, std::hypot(gp[0] - first[0], gp[1] - first[1], gp[2] - first[2]));
    }
    std::printf("grab smoothing: a still hand with 3 mm of noise moved the panel %.2f mm\n", still * 1000);
    CHECK(!first.empty() && still < 0.0005);
    // a steady move: 30 cm to the right in 1 s; at 90 Hz the grab point moves evenly, no jump as
    // the dead zone lets go, and it never gets ahead of the hand
    std::vector<double> xs;
    for (int k = 1; k <= 45; ++k) {
        const double x = 0.05 + 0.3 * std::min(1.0, k / 30.0);
        r.synth.Write({PinchAt(x, barY, 0.02)}, r.t);
        for (int j = 0; j < 3; ++j) {
            const int64_t now = r.t + 30'000'000 + j * 11'111'111;
            if (j == 0) r.hands.Feed(&r.synth.file, sizeof(fh_hands_t), &r.synth.g, Identity(), now);
            else r.hands.Feed(nullptr, 0, nullptr, Identity(), now);
            Input i;
            i.nowNs = now, i.hands = &r.hands, i.head = Identity(), i.haveHead = true;
            i.allowed[0] = i.allowed[1] = true;
            r.in.Update(i, r.panels);
            if (!r.in.carries().empty()) {
                r.carries = r.in.carries();
                xs.push_back(grabPoint(r, local)[0]);
            }
        }
        r.t += 33'333'333;
    }
    double jump = 0, ahead = 0, back = 0;
    for (size_t k = 1; k < xs.size(); ++k) {
        jump = std::max(jump, xs[k] - xs[k - 1]);
        back = std::max(back, xs[k - 1] - xs[k]);
        ahead = std::max(ahead, xs[k] - (first[0] + 0.3));
    }
    std::printf("grab smoothing: a 0.3 m/s move, at most %.1f mm a tick (the hand: 3.3), %.1f mm back, settled %.1f mm "
                "behind the hand (the dead zone: 10)\n", jump * 1000, back * 1000, (first[0] + 0.3 - xs.back()) * 1000);
    CHECK(jump < 0.006 && back < 1e-6 && ahead <= 1e-6);
    CHECK(std::fabs(xs.back() - (first[0] + 0.3 - 0.01)) < 0.003);   // half a second on: behind by the dead zone
    // let go, then a new grab further along: it starts from the hand, not the old smoothing
    r.panels[0].pose = r.carries[0].pose;
    r.synth.Pinch(1, false, r.t, at, 1);
    {
        SynthHand o = PinchAt(0.35, barY, 0.02);
        o.open = 1;
        r.Frame({o}), r.Frame({o});
    }
    CHECK(!r.in.grabbing(1));
    for (int k = 0; k < 10; ++k) r.Frame({PinchAt(0.35, barY, 0.02)});
    const Hit g1 = Locate(r.panels[0], r.hands.seen()[0].pinchPoint);
    const double at2[3] = {0.35, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at2, 1);
    r.Frame({PinchAt(0.35, barY, 0.02)});
    CHECK(r.in.grabbing(1) && !r.carries.empty());
    if (!r.carries.empty()) {
        const double l1[3] = {g1.u, g1.v, g1.d};
        const auto gp = grabPoint(r, l1);
        const float *pp = r.hands.seen()[0].pinchPoint;
        CHECK(std::hypot(gp[0] - pp[0], gp[1] - pp[1], gp[2] - pp[2]) < 0.002);
    }
}

// A panel's turn from facing the head at the origin: yaw (its front turned toward +x), pitch (its
// front up), roll (counterclockwise as seen), degrees.
void FacingTurn(const Mat &p, double *yaw, double *pitch, double *roll) {
    const double c[3] = {p.m[0][3], p.m[1][3], p.m[2][3]}, l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const double z[3] = {-c[0] / l, -c[1] / l, -c[2] / l}, xl = std::hypot(z[0], z[2]);
    const double x[3] = {z[2] / xl, 0, -z[0] / xl};
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const double *f[3] = {x, y, z};
    double off[3][3];   // F^T P
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) off[i][j] = f[i][0] * p.m[0][j] + f[i][1] * p.m[1][j] + f[i][2] * p.m[2][j];
    *yaw = std::atan2(off[0][2], off[2][2]) * 180 / M_PI;
    *pitch = std::asin(std::clamp(off[1][2], -1.0, 1.0)) * 180 / M_PI;
    *roll = std::atan2(off[1][0], off[0][0]) * 180 / M_PI;
}

// Grabs TestPanel's bar with the right hand and holds still; then each Turn call turns the hand
// (about the pinch point, which stays put) to yaw, pitch and twist over `frames`, and holds `hold`.
struct WristRig {
    Rig r;
    double barY = 0, u = 0.05;
    double yaw = 0, pitch = 0, twist = 0, open = 0;
    explicit WristRig(bool rotate = true, double u0 = 0.05) : u(u0) {
        r.in.params().grabRotate = rotate;
        barY = r.panels[0].controls[0].v;
        for (int k = 0; k < 5; ++k) r.Frame({Hand()});
        const double at[3] = {u, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({Hand()});
        for (int k = 0; k < 10; ++k) Step();
    }
    SynthHand Hand() const {
        SynthHand h = KnucklesAt(u, barY, 0.02);
        h.yawDeg = yaw, h.pitchDeg = pitch, h.twistDeg = twist, h.open = open;
        return h;
    }
    void Step() {
        if (!r.carries.empty() && r.carries[0].kind != Carry::Resize) r.panels[0].pose = r.carries[0].pose;
        r.Frame({Hand()});
    }
    void Turn(double y, double p, double w, int frames = 15, int hold = 45) {
        const double y0 = yaw, p0 = pitch, w0 = twist;
        for (int k = 1; k <= frames; ++k) {
            const double f = double(k) / frames;
            yaw = y0 + (y - y0) * f, pitch = p0 + (p - p0) * f, twist = w0 + (w - w0) * f;
            Step();
        }
        for (int k = 0; k < hold; ++k) Step();
    }
    void Angles(double *y, double *p, double *w) const { FacingTurn(r.panels[0].pose, y, p, w); }
};

// One hand carrying a panel turns it with the wrist (Params::grabRotate): yaw, pitch and roll 1:1
// past the dead zone, the grab point staying under the pinch; a still hand's jitter doesn't turn
// it; carried around the head with the wrist kept, it only faces the head; it snaps level; let go,
// it keeps the turn; a corner grab only resizes.
void TestGrabTurn() {
    const double dead = Params().rotDeadzone;
    double y, p, w;
    {
        WristRig t;
        CHECK(t.r.in.grabbing(1) && !t.r.carries.empty());
        t.Angles(&y, &p, &w);
        NEAR(y, 0, 0.3), NEAR(p, 0, 0.3), NEAR(w, 0, 0.3);
        t.Turn(30, 0, 0);
        t.Angles(&y, &p, &w);
        std::printf("wrist turn: hand yaw 30 -> panel yaw %.1f pitch %.1f roll %.1f (dead zone %.0f)\n", y, p, w, dead);
        NEAR(y, 30 - dead, 1.5), NEAR(p, 0, 1), NEAR(w, 0, 1);
        CHECK(t.r.in.turning(1));
        // the grab point is still under the pinch point
        const Hit g = Locate(PanelAt(t.r.panels[0].pose, 1.0), t.r.hands.seen()[0].pinchPoint);
        CHECK(std::hypot(g.u - 0.05, g.v - t.barY) < 0.015);
        // back: back to facing (5 deg of the dead zone the other way is gone, not jumped)
        t.Turn(5, 0, 0);
        t.Angles(&y, &p, &w);
        NEAR(y, 0, 1.0);
    }
    {
        WristRig t(true, 0.0);   // (straight ahead: the forearm's axis toward the head)
        t.Turn(0, 0, 25);   // a twist about the forearm: roll
        t.Angles(&y, &p, &w);
        std::printf("wrist turn: hand twist 25 -> panel yaw %.1f pitch %.1f roll %.1f\n", y, p, w);
        NEAR(w, 25 - dead, 1.5), NEAR(y, 0, 1.5), NEAR(p, 0, 1.5);
        // nearly level again: it snaps level
        t.Turn(0, 0, dead + 1.5);
        t.Angles(&y, &p, &w);
        NEAR(w, 0, 0.2);
        t.Turn(0, 0, dead + 6);   // past the snap: 6 deg
        t.Angles(&y, &p, &w);
        NEAR(w, 6, 1.0);
    }
    {
        WristRig t;
        t.Turn(0, 20, 0);   // the hand tips up: the panel tips back
        t.Angles(&y, &p, &w);
        std::printf("wrist turn: hand pitch 20 -> panel yaw %.1f pitch %.1f roll %.1f\n", y, p, w);
        NEAR(std::fabs(p), 20 - dead, 1.5), NEAR(y, 0, 1.5), NEAR(w, 0, 1.5);
    }
    {
        // a still hand's jitter (2.5 deg either way on each axis, every frame) doesn't turn it
        WristRig t;
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> n(-2.5, 2.5);
        double most = 0;
        for (int k = 0; k < 90; ++k) {
            t.yaw = n(rng), t.pitch = n(rng), t.twist = n(rng);
            t.Step();
            t.Angles(&y, &p, &w);
            most = std::max({most, std::fabs(y), std::fabs(p), std::fabs(w)});
        }
        std::printf("wrist turn: 2.5 deg of jitter turned the panel at most %.2f deg\n", most);
        CHECK(most < 0.5 && !t.r.in.turning(1));
    }
    {
        // carried 50 degrees around the head to the left, the wrist turning with the arm: it only
        // keeps facing the head
        WristRig t;
        const double r0 = std::hypot(t.u, -0.48);
        double most = 0;
        for (int k = 1; k <= 60; ++k) {
            const double a = 50.0 * std::min(1.0, k / 40.0), ar = a * M_PI / 180;
            const double base = std::atan2(t.u, 0.48);   // where the grab began, from the head
            SynthHand h = KnucklesAt(0, t.barY, 0);
            h.pinch[0] = r0 * std::sin(base - ar), h.pinch[2] = -r0 * std::cos(base - ar);
            h.yawDeg = a;
            if (!t.r.carries.empty()) t.r.panels[0].pose = t.r.carries[0].pose;
            t.r.Frame({h});
            t.Angles(&y, &p, &w);
            if (k > 45) most = std::max({most, std::fabs(y), std::fabs(p), std::fabs(w)});
        }
        std::printf("wrist turn: carried 50 deg round the head, the panel %.2f deg off facing it\n", most);
        CHECK(most < 2 && !t.r.in.turning(1));
    }
    {
        // off (HANDS_GRAB_ROTATE=0): it faces the head whatever the wrist does
        WristRig t(false);
        t.Turn(30, 10, 20);
        t.Angles(&y, &p, &w);
        NEAR(y, 0, 0.5), NEAR(p, 0, 0.5), NEAR(w, 0, 0.5);
    }
    {
        // let go turned, take it again: it keeps the turn (no snap back to facing)
        WristRig t;
        t.Turn(30, 0, 0);
        double y1;
        t.Angles(&y1, &p, &w);
        const double at[3] = {t.u, t.barY, -0.48};
        t.r.synth.Pinch(1, false, t.r.t, at, 1);
        t.open = 1;   // (the fingers open: let go on the second frame, Params::releaseFrames)
        t.Step(), t.Step();
        CHECK(!t.r.in.grabbing(1));
        t.open = 0;
        for (int k = 0; k < 10; ++k) t.Step();
        t.Angles(&y, &p, &w);
        NEAR(y, y1, 0.01);
        // the grab point moved with the turn: pinch where the bar is now
        const Mat &P = t.r.panels[0].pose;
        double bar[3];
        FromSurface(PanelAt(P, 1.0), t.u, t.barY, 0.02, bar);
        SynthHand h = t.Hand();
        h.pinch[0] = bar[0], h.pinch[1] = bar[1], h.pinch[2] = bar[2];
        for (int k = 0; k < 5; ++k) t.r.Frame({h});
        t.r.synth.Pinch(1, true, t.r.t, bar, 1);
        for (int k = 0; k < 30; ++k) {
            if (!t.r.carries.empty()) t.r.panels[0].pose = t.r.carries[0].pose;
            t.r.Frame({h});
        }
        CHECK(t.r.in.grabbing(1));
        t.Angles(&y, &p, &w);
        NEAR(y, y1, 0.5);
    }
    {
        // the corner: twisting the hand there only resizes (and with the pinch still, not even that)
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.5, -0.28, 0.01)});
        const double at[3] = {0.5, -0.28, -0.49};
        r.synth.Pinch(1, true, r.t, at, 1);
        for (int k = 0; k <= 40; ++k) {
            SynthHand h = PinchAt(0.5, -0.28, 0.01);
            h.twistDeg = std::min(k, 20) * 1.5, h.yawDeg = std::min(k, 20);
            r.Frame({h});
        }
        CHECK(r.in.grabbing(1) && !r.carries.empty() && r.carries[0].kind == Carry::Resize);
        if (!r.carries.empty()) NEAR(r.carries[0].width, 1.0, 0.02);
    }
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
    for (int k = 0; k < 5; ++k) r.Frame({KnucklesAt(-0.5, 0, 0.01, false, 1), KnucklesAt(0.5, 0, 0.01, true, 2)});
    const double l[3] = {-0.5, 0, -0.49}, rr[3] = {0.5, 0, -0.49};
    r.synth.Pinch(0, true, r.t, l, 1);
    r.synth.Pinch(1, true, r.t, rr, 2);
    r.Frame({KnucklesAt(-0.5, 0, 0.01, false, 1), KnucklesAt(0.5, 0, 0.01, true, 2)});
    CHECK(r.Count(Event::GrabStart) == 2);
    for (int k = 1; k <= 20; ++k) {
        const int j = std::min(k, 10);
        r.Frame({KnucklesAt(-0.5 - 0.01 * j, 0, 0.01, false, 1), KnucklesAt(0.5 + 0.01 * j, 0, 0.01, true, 2)});
    }
    CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch && r.carries[0].sides == 3);
    if (r.carries.size() != 1) return;
    NEAR(r.carries[0].width, 1.19, 0.01);
    r.panels[0].pose = r.carries[0].pose, r.panels[0].width = r.carries[0].width;
    // the left hand lets go: the right one carries on alone, from where the panel is
    r.synth.Pinch(0, false, r.t, l, 1);
    {
        SynthHand o = KnucklesAt(-0.6, 0, 0.01, false, 1);
        o.open = 1;
        r.Frame({o, KnucklesAt(0.6, 0, 0.01, true, 2)}), r.Frame({o, KnucklesAt(0.6, 0, 0.01, true, 2)});
    }
    CHECK(r.Count(Event::GrabEnd) == 1 && r.in.grabbing(1) && !r.in.grabbing(0));
    for (int k = 1; k <= 20; ++k) {
        const int j = std::min(k, 10);
        r.panels[0].pose = r.carries.empty() ? r.panels[0].pose : r.carries[0].pose;
        r.Frame({KnucklesAt(-0.6, 0, 0.01, false, 1), KnucklesAt(0.6, 0.01 * j, 0.01, true, 2)});
    }
    CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Move && r.carries[0].sides == 2);
    if (r.carries.size() == 1) {   // the grab point stays under the right hand (behind it by the 1 cm dead zone)
        const Hit g = Locate(PanelAt(r.carries[0].pose, 1.19), r.hands.shown()[0].right ? r.hands.shown()[0].pinchPoint : r.hands.shown()[1].pinchPoint);
        NEAR(g.u, 0.595, 0.01), NEAR(g.v, 0, 0.015);
    }
    // the right hand lost: its pinch ends as lost, and it holds on (Params::holdLostMs) until it's
    // been unseen 1.75 s; then it lets go where it is, as lost
    r.synth.Pinch(1, false, r.t, rr, 2);
    r.synth.g.pinch[1].flags |= FH_PINCH_LOST;
    SynthHand left = KnucklesAt(-0.6, 0, 0.01, false, 1);
    left.open = 1;
    for (int k = 0; k < 45; ++k) r.Frame({left});
    CHECK(r.Count(Event::GrabEnd) == 1 && r.in.grabbing(1));
    for (int k = 0; k < 10; ++k) r.Frame({left});
    CHECK(r.Count(Event::GrabEnd) == 2 && r.Last(Event::GrabEnd)->lost);
}

// Holding through tracking loss (Params::holdLostMs, 2026-10-10): a bar grab carried right at
// 0.6 m/s (2 cm a frame) loses its hand for `gap` frames mid-carry (or gets glitch frames, or its
// pinch flickers up), and the hand comes back where it would be. Returns the largest step the panel
// took in one 90 Hz tick, and how far the grab point is from under the hand 0.4 s after.
struct HoldRun { bool held = false; double step = 0, after = 0; int ends = 0; };
HoldRun CarryThrough(int gap, int what, double jitter = 0) {   // what: 0 unseen, 1 glitch, 2 pinch flicker, 3 lost and back closed
    Rig r;
    const double barY = r.panels[0].controls[0].v;
    auto hand = [&](double x) {
        SynthHand h = KnucklesAt(x, barY, 0.02);
        h.jitter = jitter;
        return h;
    };
    for (int k = 0; k < 5; ++k) r.Frame({hand(0)});
    const double at[3] = {0, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({hand(0)});
    HoldRun out;
    double lastX = r.panels[0].pose.m[0][3];
    double x = 0;
    for (int f = 1; f <= 60; ++f) {
        x = std::min(0.6, 0.02 * f);   // then still at 0.6
        const bool off = f >= 15 && f < 15 + gap;
        SynthHand h = hand(x);
        if (off && what == 0) h.present = false;
        if (off && what == 1) h = hand(x + 0.3);   // a glitch: 30 cm off
        if (what == 2 && f == 15) r.synth.Pinch(1, false, r.t, at, 1);   // up for a frame, then down again
        if (what == 2 && f == 16) r.synth.Pinch(1, true, r.t, at, 1);
        if (what == 3 && f == 15) {
            r.synth.Pinch(1, false, r.t, at, 1);
            r.synth.g.pinch[1].flags |= FH_PINCH_LOST;
            h.present = false;
        }
        if (what == 3 && f > 15 && f < 15 + gap) h.present = false;
        r.Frame({h});
        if (!r.carries.empty()) {
            // (per tick: the rig keeps only the last tick's carries, so a frame's largest step is
            // the change over its three ticks, at most 3x a tick's)
            out.step = std::max(out.step, std::fabs(r.carries[0].pose.m[0][3] - lastX) / 3);
            if (std::getenv("HOLD_DEBUG"))
                std::printf("    f %d hand %.3f panel %.3f step %.1f mm%s\n", f, x, r.carries[0].pose.m[0][3],
                            (r.carries[0].pose.m[0][3] - lastX) / 3 * 1000, off ? " (off)" : "");
            lastX = r.carries[0].pose.m[0][3];
            r.panels[0].pose = r.carries[0].pose;
        }
        if (f == 55) {   // 0.4 s or more after it's back, and still
            out.after = r.hands.seen().empty() ? 1 : std::fabs(Locate(r.panels[0], r.hands.seen()[0].pinchPoint).u);
        }
    }
    out.held = r.in.grabbing(1);
    out.ends = r.Count(Event::GrabEnd);
    return out;
}

void TestHoldThroughLoss() {
    for (int gap : {3, 6, 10}) {
        const HoldRun a = CarryThrough(gap, 0), b = CarryThrough(gap, 0, 0.003);
        std::printf("hold through loss: %2d frames unseen at 0.6 m/s: held %d, the panel's largest step %.1f mm a tick "
                    "(6.7 moving), %.1f cm off the hand 0.4 s on; with 3 mm noise: held %d, %.1f mm, %.1f cm\n",
                    gap, a.held, a.step * 1000, a.after * 100, b.held, b.step * 1000, b.after * 100);
        CHECK(a.held && a.ends == 0 && b.held && b.ends == 0);
        // no jump: a tick at 0.6 m/s is 6.7 mm; the panel catches up what the coast missed (the hand
        // went 20 cm in 10 frames, the coast 4.5 cm) at Params::catchUp on average, on top of that
        // (3 mm of noise adds about 3 mm a tick: a noisy carry's steps are up to 9-10 mm without a gap)
        CHECK(a.step < 0.018 && b.step < 0.025);
        CHECK(a.after < 0.02 && b.after < 0.025);   // under the hand again (behind by the 1 cm dead zone)
    }
    {
        const HoldRun g = CarryThrough(2, 1);
        std::printf("hold through loss: 2 glitch frames 30 cm off: held %d, largest step %.1f mm a tick\n", g.held, g.step * 1000);
        CHECK(g.held && g.step < 0.015 && g.after < 0.02);
    }
    {
        const HoldRun f = CarryThrough(1, 2);
        CHECK(f.held && f.ends == 0 && f.step < 0.015);
        const HoldRun l = CarryThrough(8, 3);   // ft-hands ended it as lost; the hand is back, still pinched
        std::printf("hold through loss: the pinch lost for 8 frames, back closed: held %d, %d end(s), largest step %.1f mm\n",
                    l.held, l.ends, l.step * 1000);
        CHECK(l.held && l.ends == 0 && l.step < 0.018);
    }
    {
        // a pinch on the content (a press in KWin) whose pinch flickers up for a frame, the fingers
        // still together: held; then opened: released two frames on
        Rig c;
        for (int k = 0; k < 5; ++k) c.Frame({PinchAt(0, 0, 0.01)});
        const double mid[3] = {0, 0, -0.48};
        c.synth.Pinch(1, true, c.t, mid, 1);
        c.Frame({PinchAt(0, 0, 0.01)});
        CHECK(c.in.pressing(1));
        c.synth.Pinch(1, false, c.t, mid, 1);
        c.Frame({PinchAt(0, 0, 0.01)});
        c.synth.Pinch(1, true, c.t, mid, 1);
        for (int k = 0; k < 3; ++k) c.Frame({PinchAt(0, 0, 0.01)});
        CHECK(c.in.pressing(1) && c.Count(Event::Release) == 0 && c.Count(Event::Press) == 1);
        c.synth.Pinch(1, false, c.t, mid, 1);
        SynthHand o = PinchAt(0, 0, 0.01);
        o.open = 1;
        c.Frame({o});
        CHECK(c.in.pressing(1));
        c.Frame({o});
        CHECK(!c.in.pressing(1) && c.Count(Event::Release) == 1);
    }
    {
        // gone for good: it lets go where it is, as lost, after holdLostMs
        Rig r;
        const double barY = r.panels[0].controls[0].v;
        for (int k = 0; k < 5; ++k) r.Frame({KnucklesAt(0, barY, 0.02)});
        const double at[3] = {0, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({KnucklesAt(0, barY, 0.02)});
        SynthHand gone;
        gone.present = false;
        for (int k = 0; k < 50; ++k) r.Frame({gone});
        CHECK(r.in.grabbing(1));
        for (int k = 0; k < 6; ++k) r.Frame({gone});
        CHECK(!r.in.grabbing(1) && r.Count(Event::GrabEnd) == 1 && r.Last(Event::GrabEnd)->lost);
    }
}

// Near pinches by the border (Params::band*, 2026-10-10: "I still can't pinch to grab displays
// easily"): where the fingers meet, a few cm either side of an edge, in front or behind; corners
// resize; the content clicks; the glow says which before the pinch.
void TestForgivingZones() {
    struct Case { double u, v, d; Zone want; const char *what; };
    const Case cases[] = {
        {0.55, 0.0, 0.01, Zone::Edge, "5 cm outside the right edge"},
        {0.48, 0.1, 0.0, Zone::Edge, "2 cm inside the right edge"},
        {-0.53, -0.1, -0.02, Zone::Edge, "3 cm left of the left edge, 2 cm behind"},
        {0.0, 0.31, 0.03, Zone::Edge, "3 cm over the top, 3 cm in front"},
        {-0.35, -0.29, 0.0, Zone::Edge, "on the bottom edge"},
        {0.53, 0.30, 0.01, Zone::Corner, "by the top right corner"},
        {-0.47, -0.25, 0.0, Zone::Corner, "inside the bottom left corner"},
        {0.2, -0.30, 0.01, Zone::Bar, "by the bar"},
        {0.0, 0.0, 0.01, Zone::None, "the middle (a click)"},
        {0.44, 0.0, 0.01, Zone::None, "6 cm inside the right edge (a click)"},
    };
    for (const Case &c : cases) {
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(c.u, c.v, c.d)});
        const double at[3] = {c.u, c.v, -0.5 + c.d};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(c.u, c.v, c.d)});
        const auto g = r.Last(Event::GrabStart);
        const bool ok = c.want == Zone::None ? !g && r.Count(Event::Press) == 1 : g && g->zone == c.want;
        if (!ok) std::printf("forgiving zones: %s: %s\n", c.what, g ? ZoneName(g->zone) : r.Count(Event::Press) ? "a click" : "nothing");
        CHECK(ok);
    }
    // the glow before the pinch: amber at a corner (resize), blue by an edge (move)
    auto glowColour = [](double u, double v) {
        Rig r;
        SynthHand h = PinchAt(u, v, 0.01);
        h.open = 0.6;
        for (int k = 0; k < 5; ++k) r.Frame({h});
        for (const auto &g : r.in.glows().count(0) ? r.in.glows().at(0) : std::vector<handcut::Glow>{})
            if (g.ring && g.rgb[2] < 0.5f) return 'a';   // amber
        for (const auto &g : r.in.glows().count(0) ? r.in.glows().at(0) : std::vector<handcut::Glow>{})
            if (g.ring && g.rgb[0] < 0.5f) return 'b';   // blue
        return '-';
    };
    CHECK(glowColour(0.52, 0.29) == 'a');
    CHECK(glowColour(0.54, 0.0) == 'b');
}

// The card pinch: the hand under the bottom edge, fingers up, the back of the hand toward you, the
// thumb behind the panel and the index in front, closing on it 1 cm inside the edge (and open: 5 cm
// apart). turn: the hand turned about the vertical by this (degrees), up: raised (m).
SynthHand CardHand(double u, double v, double open, double turnDeg = 0, double up = 0, double rollDeg = 0, int fingers = 1) {
    SynthHand h;
    h.pinching = h.byContact = h.useRot = true, h.open = open, h.fingers = fingers;
    // hand x -> room x, hand y (the back of the hand) -> +z, hand z -> -y (the fingers up); then
    // turned about the vertical, and rolled about the panel's normal
    const double a = turnDeg * M_PI / 180, r = rollDeg * M_PI / 180;
    const double base[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    const double rz[3][3] = {{std::cos(r), -std::sin(r), 0}, {std::sin(r), std::cos(r), 0}, {0, 0, 1}};
    const double ry[3][3] = {{std::cos(a), 0, std::sin(a)}, {0, 1, 0}, {-std::sin(a), 0, std::cos(a)}};
    double t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = rz[i][0] * base[0][j] + rz[i][1] * base[1][j] + rz[i][2] * base[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) h.rot[i][j] = ry[i][0] * t[0][j] + ry[i][1] * t[1][j] + ry[i][2] * t[2][j];
    h.contact[0] = u, h.contact[1] = v + up, h.contact[2] = -0.5;
    return h;
}

void TestCardPinch() {
    const double v = -0.28125 + 0.01;   // 1 cm inside the bottom edge
    {
        Rig r;
        r.panels[0].controls.clear();
        for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({CardHand(0.1, v, o)});
        // the tips either side of the plane: the glow says "card" (green)
        CHECK(r.in.near(1));
        {
            const handcut::Hand &h = r.hands.seen()[0];
            const Hit ti = Locate(r.panels[0], h.p[8]), th = Locate(r.panels[0], h.p[4]);
            std::printf("card pinch: index tip %+.1f cm, thumb tip %+.1f cm from the plane, open\n", ti.d * 100, th.d * 100);
            CHECK(ti.d > 0.003 && th.d < -0.003);
        }
        const double at[3] = {0.1, v, -0.5};
        r.Frame({CardHand(0.1, v, 0)});
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({CardHand(0.1, v, 0)});
        CHECK(r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Card);
        // up 10 cm and turned 30 degrees about the vertical, slowly; then held
        for (int k = 1; k <= 60; ++k) {
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
            const double f = std::min(1.0, k / 30.0);
            r.Frame({CardHand(0.1, v, 0, 30 * f, 0.10 * f)});
        }
        CHECK(!r.carries.empty() && r.carries[0].kind == Carry::Move);
        if (!r.carries.empty()) {
            const Mat &p = r.carries[0].pose;
            const double yaw = std::atan2(p.m[0][2], p.m[2][2]) * 180 / M_PI;
            std::printf("card pinch: the hand up 10 cm and turned 30 deg -> the panel up %.1f cm, turned %.1f deg (dead zone %.0f)\n",
                        (p.m[1][3] - 0) * 100, yaw, Params().cardRotDeadzone);
            NEAR(yaw, 30 - Params().cardRotDeadzone, 1.5);
            NEAR(p.m[1][3], 0.10 - 0.01, 0.015);   // (behind by the grab's dead zone, 1 cm)
            // the pinch is still where it took the panel: on the panel, 1 cm in from its bottom edge
            const Hit c = Locate(PanelAt(p, 1.0), r.hands.seen()[0].contact);
            CHECK(std::fabs(c.u - 0.1) < 0.02 && std::fabs(c.v - v) < 0.02 && std::fabs(c.d) < 0.02);
        }
        // rolled 20 degrees about the panel's normal: it rolls too (no facing, no level snap)
        for (int k = 1; k <= 45; ++k) {
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
            r.Frame({CardHand(0.1, v, 0, 30, 0.10, 20.0 * std::min(1.0, k / 20.0))});
        }
        if (!r.carries.empty()) {
            const Mat &p = r.carries[0].pose;
            const double roll = std::atan2(p.m[1][0], p.m[1][1]) * 180 / M_PI;
            std::printf("card pinch: the hand rolled 20 deg -> the panel %.1f deg\n", roll);
            NEAR(std::fabs(roll), 20, 2.0);
        }
        r.synth.Pinch(1, false, r.t, at, 1);
        r.Frame({CardHand(0.1, v, 1.0, 30, 0.10, 20)}), r.Frame({CardHand(0.1, v, 1.0, 30, 0.10, 20)});
        CHECK(r.Count(Event::GrabEnd) == 1 && !r.in.grabbing(1));
    }
    // which fingers (user, 2026-10-10): the index alone against the thumb is the border's move; the
    // index and middle together, or all four bunched, a card; the middle joining within cardJoinMs
    // turns the move into a card, later it stays a move
    for (int fingers : {0, 2, 3, 4}) {
        Rig r;
        r.panels[0].controls.clear();
        const int first = fingers == 2 ? 2 : fingers == 0 ? 0 : 0;   // 3, 4: the index first, the middle later
        for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({CardHand(0.1, v, o, 0, 0, 0, first)});
        const double at[3] = {0.1, v, -0.5};
        r.Frame({CardHand(0.1, v, 0, 0, 0, 0, first)});
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({CardHand(0.1, v, 0, 0, 0, 0, first)});
        CHECK(r.Count(Event::GrabStart) == 1);
        const Zone z0 = r.Last(Event::GrabStart) ? r.Last(Event::GrabStart)->zone : Zone::None;
        const int join = fingers == 3 ? 4 : fingers == 4 ? 15 : 1000;   // frames on
        for (int k = 1; k <= 20; ++k) r.Frame({CardHand(0.1, v, 0, 0, 0, 0, k >= join ? 1 : first)});
        const bool card = r.in.Describe().find("(card)") != std::string::npos;
        std::printf("card pinch, %s: took it by %s, %s\n",
                    fingers == 0 ? "the index alone" : fingers == 2 ? "four fingertips bunched"
                    : fingers == 3 ? "the middle joining 130 ms on" : "the middle joining 500 ms on",
                    ZoneName(z0), card ? "a card now" : "a border move");
        CHECK(card == (fingers == 2 || fingers == 3));
        CHECK(z0 == (fingers == 2 ? Zone::Card : Zone::Edge));
    }
    {
        // the same edge pinched from the front (both tips in front of it): the border's move, not a card
        Rig r;
        r.panels[0].controls.clear();
        for (double o : {1.0, 0.8, 0.6, 0.4, 0.2, 0.0}) {
            SynthHand h = PinchAt(0.1, v, 0.015);
            h.open = o;
            r.Frame({h});
        }
        const double at[3] = {0.1, v, -0.485};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(0.1, v, 0.015)});
        CHECK(r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Edge);
    }
    {
        // off (HANDS_CARD_PINCH=0): the band takes it as an edge
        Rig r;
        r.panels[0].controls.clear();
        r.in.params().cardPinch = false;
        for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({CardHand(0.1, v, o)});
        const double at[3] = {0.1, v, -0.5};
        r.Frame({CardHand(0.1, v, 0)});
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({CardHand(0.1, v, 0)});
        CHECK(r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Edge);
    }
    {
        // straddling the plane in the middle of the content (a hand reaching through it): not a card
        // (only at the border), a click on the content instead
        Rig r;
        r.panels[0].controls.clear();
        for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({CardHand(0.0, 0.0, o)});
        const double at[3] = {0, 0, -0.5};
        r.Frame({CardHand(0.0, 0.0, 0)});
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({CardHand(0.0, 0.0, 0)});
        CHECK(r.Count(Event::GrabStart) == 0);
    }
}

// A fist (grip) at a panel: its palm forward (toward the panel), fingers up, `palmD` in front of it.
SynthHand FistAt(double u, double v, double palmD, bool right = true, uint32_t id = 1, double tilt = 90) {
    SynthHand h;
    h.palm = true, h.right = right, h.id = id;
    h.palmAt[0] = u, h.palmAt[1] = v, h.palmAt[2] = -0.5 + palmD;
    h.rollDeg = 180, h.tiltDeg = tilt, h.curl = 2, h.curlIndex = 2;
    return h;
}

void TestFistGrab() {
    {
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.in.near(1));
        const double at[3] = {0.0, 0, -0.45};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Fist && r.in.grabbing(1));
        CHECK(r.Count(Event::Press) == 0);   // a fist doesn't poke
        // the fist tips forward 20 degrees (its fingers toward the panel): the panel tips the same way
        for (int k = 1; k <= 45; ++k) {
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
            r.Frame({FistAt(0.0, 0, 0.05, true, 1, 90 - 20 * std::min(1.0, k / 15.0))});
        }
        double y = 0, p = 0, w = 0;
        if (!r.carries.empty()) FacingTurn(r.carries[0].pose, &y, &p, &w);
        std::printf("fist grab: the fist tipped 20 deg -> panel yaw %.1f pitch %.1f roll %.1f\n", y, p, w);
        NEAR(std::fabs(p), 20 - Params().rotDeadzone, 2.0), NEAR(y, 0, 2.0), NEAR(w, 0, 2.0);
        CHECK(r.in.turning(1));
        // carried 15 cm right, slowly: the panel follows
        const double x0 = r.carries.empty() ? 0 : r.carries[0].pose.m[0][3];
        for (int k = 1; k <= 45; ++k) {
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
            r.Frame({FistAt(0.15 * std::min(1.0, k / 30.0), 0, 0.05, true, 1, 70)});
        }
        CHECK(!r.carries.empty() && r.carries[0].kind == Carry::Move);
        if (!r.carries.empty()) NEAR(r.carries[0].pose.m[0][3] - x0, 0.15, 0.015);
        // the fist opens: let go
        r.synth.Grip(1, false, r.t, at, 1);
        SynthHand o = FistAt(0.15, 0, 0.05, true, 1, 70);
        o.curl = o.curlIndex = 0;
        r.Frame({o}), r.Frame({o});
        CHECK(r.Count(Event::GrabEnd) == 1 && !r.in.grabbing(1));
    }
    {
        // two fists on one panel: pulled apart 20 cm, it grows
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(-0.3, 0, 0.05, false, 1), FistAt(0.3, 0, 0.05, true, 2)});
        const double l[3] = {-0.3, 0, -0.45}, rr[3] = {0.3, 0, -0.45};
        r.synth.Grip(0, true, r.t, l, 1), r.synth.Grip(1, true, r.t, rr, 2);
        r.Frame({FistAt(-0.3, 0, 0.05, false, 1), FistAt(0.3, 0, 0.05, true, 2)});
        CHECK(r.Count(Event::GrabStart) == 2);
        for (int k = 1; k <= 45; ++k) {
            const double f = std::min(1.0, k / 30.0);
            r.Frame({FistAt(-0.3 - 0.1 * f, 0, 0.05, false, 1), FistAt(0.3 + 0.1 * f, 0, 0.05, true, 2)});
        }
        CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch && r.carries[0].sides == 3);
        if (r.carries.size() == 1) NEAR(r.carries[0].width, (0.8 - 0.01) / 0.6, 0.02);
    }
    {
        // a fist 20 cm out (not at the panel): the pointer helper's grip, not ours
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.1, 0, 0.20)});
        CHECK(!r.in.near(1));
        const double at[3] = {0.1, 0, -0.30};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.1, 0, 0.20)});
        CHECK(r.Count(Event::GrabStart) == 0);
    }
    {
        // "grab that" has the hand (held out, closing): the fist waits; grab that fires: no grab
        Rig r;
        r.armed[1] = true;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.1, 0, 0.05)});
        const double at[3] = {0.1, 0, -0.45};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 0 && r.in.stats.fistsWaited == 1);
        r.in.GrabThatFired(1);
        r.armed[1] = false;
        for (int k = 0; k < 10; ++k) r.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 0 && r.in.stats.fistsCancelled == 1);
        // ...or it doesn't fire (the hand stays at the panel): the fist grabs once it lets go
        Rig q;
        q.armed[1] = true;
        for (int k = 0; k < 5; ++k) q.Frame({FistAt(0.1, 0, 0.05)});
        q.synth.Grip(1, true, q.t, at, 1);
        q.Frame({FistAt(0.1, 0, 0.05)});
        for (int k = 0; k < 5; ++k) q.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(q.Count(Event::GrabStart) == 0);
        q.armed[1] = false;
        q.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(q.Count(Event::GrabStart) == 1 && q.Last(Event::GrabStart)->zone == Zone::Fist);
        // ...or it stays armed: at most grabThatWaitMs (0.6 s)
        Rig w;
        w.armed[1] = true;
        for (int k = 0; k < 5; ++k) w.Frame({FistAt(0.1, 0, 0.05)});
        w.synth.Grip(1, true, w.t, at, 1);
        for (int k = 0; k < 25; ++k) w.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(w.Count(Event::GrabStart) == 1);
    }
    {
        // off (HANDS_FIST_GRAB=0)
        Rig r;
        r.in.params().fistGrab = false;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.1, 0, 0.05)});
        const double at[3] = {0.1, 0, -0.45};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.1, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 0);
    }
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
    mp.shape = handcut::MatteShape::Mesh;
    handcut::Matte m;
    const double head[3] = {0, 0, 0};
    handcut::BuildMatte(r.hands.shown(), r.hands.capsules(), head, mp, m);
    CHECK(m.caps.size() == 15 && m.hulls.size() == 3 && m.circles.empty());
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
    // the circle (the default): one round hole per hand, around every joint as each eye sees
    // it, and a margin past the outermost
    {
        Rig c;
        c.Frame({TipAt(0, 0, 0.1)});
        handcut::MatteParams cp;
        CHECK(cp.shape == handcut::MatteShape::Circle);
        handcut::BuildMatte(c.hands.shown(), c.hands.capsules(), head, cp, m);
        CHECK(m.circles.size() == 1 && m.caps.empty() && m.hulls.empty());
        CHECK(handcut::Project(p, m, eyes, s));
        const handcut::Hand &h = c.hands.shown()[0];
        for (int e = 0; e < 2; ++e) {
            CHECK(s[e].caps.size() == 1 && s[e].hulls.empty());
            if (s[e].caps.size() != 1) continue;
            const handcut::Capsule2D &k = s[e].caps[0];
            // a panel square to the head: round, but for the eye seeing the disk a little off its axis
            CHECK(std::hypot(k.bx - k.ax, k.by - k.ay) < 0.05 * k.ra);
            double worst = 0;
            for (int j = 0; j < 21; ++j) {
                // where the eye sees joint j on the panel (z -0.5)
                const double f = 0.5 / -double(h.p[j][2]);
                const double x = (eyes[e][0] + (h.p[j][0] - eyes[e][0]) * f + 0.5) * 1920, y = (0.5 - h.p[j][1] * f / 0.5625) * 1080;
                worst = std::max(worst, std::hypot(x - k.ax, y - k.ay));
            }
            const double margin = mp.Margin(0.45, 0) * 1.1 * 1920;   // about the palm's distance
            CHECK(worst + margin < k.ra);
            CHECK(k.ra < worst + 0.06 * 1920);   // ...not much more: a finger's width or so
        }
        // a panel turned 50 degrees: the hole is the disk seen at a slant, longer across
        handcut::Panel q{RotY(50 * M_PI / 180, 0, 0, -0.5), 1.0, 0.5625, 0, 1920, 1080};
        handcut::Spots t[2];
        CHECK(handcut::Project(q, m, eyes, t) && t[0].caps.size() == 1);
        if (t[0].caps.size() == 1) {
            const handcut::Capsule2D &k = t[0].caps[0];
            CHECK(std::fabs(k.bx - k.ax) > 10 && std::fabs(k.by - k.ay) < std::fabs(k.bx - k.ax) * 0.2);
        }
        // the hand pushed through the panel: no hole
        Rig d;
        d.Frame({TipAt(0, 0, -0.30)});
        handcut::BuildMatte(d.hands.shown(), d.hands.capsules(), head, cp, m);
        CHECK(m.circles.size() == 1 && !handcut::Project(p, m, eyes, s));
        // the setting's names
        handcut::MatteShape shape;
        CHECK(handcut::ParseShape("mesh", &shape) && shape == handcut::MatteShape::Mesh);
        CHECK(handcut::ParseShape("circle", &shape) && shape == handcut::MatteShape::Circle && !handcut::ParseShape("round", &shape));
    }
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
    int64_t dq = 0;          // the set's dequeue time, CLOCK_MONOTONIC (FHDUMP02)
    Mat head = Identity();   // the head then, from a take's poses.jsonl (else at the origin)
};

// Where the head was, from the dump's poses (identity without them): panels are placed
// around it.
Mat g_ref = Identity();
bool g_poses = false;

Mat Level(const Mat &h) {   // the head turned only about the vertical, at its place
    const double fx = -h.m[0][2], fz = -h.m[2][2];   // where its -z points, across the floor
    return RotY(std::atan2(-fx, -fz), h.m[0][3], h.m[1][3], h.m[2][3]);
}

Mat Mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    return r;
}

// A take's poses.jsonl: CLOCK_MONOTONIC time, head -> room.
std::vector<std::pair<int64_t, Mat>> LoadPoses(const std::string &path) {
    std::vector<std::pair<int64_t, Mat>> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        long long t;
        const size_t m = line.find("\"m\": [");
        if (std::sscanf(line.c_str(), "{\"t\": %lld", &t) != 1 || m == std::string::npos || line.find("\"ok\": true") == std::string::npos)
            continue;
        Mat h{};
        float v[12];
        if (std::sscanf(line.c_str() + m + 6, "%f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f", &v[0], &v[1], &v[2], &v[3],
                        &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) != 12)
            continue;
        for (int k = 0; k < 12; ++k) h.m[k / 4][k % 4] = v[k];
        out.push_back({t, h});
    }
    return out;
}

// The touch take's targets (prompts.jsonl): a dot in the room, shown at `show`, held until `done`.
struct Target { int64_t show = 0, done = 0; double room[3]; std::string id; };
std::vector<Target> LoadTargets(const std::string &path) {
    std::vector<Target> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("\"event\": \"target\"") == std::string::npos) continue;
        long long t;
        const size_t r = line.find("\"room\": [");
        double x, y, z;
        if (std::sscanf(line.c_str(), "{\"t\": %lld", &t) != 1 || r == std::string::npos ||
            std::sscanf(line.c_str() + r + 9, "%lf, %lf, %lf", &x, &y, &z) != 3)
            continue;
        if (line.find("\"state\": \"show\"") != std::string::npos) out.push_back({t, 0, {x, y, z}, ""});
        else if (line.find("\"state\": \"done\"") != std::string::npos && !out.empty()) out.back().done = t;
    }
    return out;
}

bool Load(const char *path, std::vector<Record> &out) {
    std::ifstream f(path, std::ios::binary);
    char magic[8];
    uint32_t sizes[2];
    if (!f.read(magic, 8) || (std::memcmp(magic, "FHDUMP01", 8) && std::memcmp(magic, "FHDUMP02", 8)) ||
        !f.read(reinterpret_cast<char *>(sizes), 8) ||
        sizes[0] < sizeof(fh_hands_t) || sizes[1] < 64 + 4 * 64 || sizes[1] > 4096)
        return false;
    // the gestures file as the dump's ft-hands wrote it: version 2 (grips) or later
    std::vector<char> g(sizes[1]);
    while (true) {
        Record r;
        r.hands.resize(sizes[0]);
        if (!f.read(r.hands.data(), sizes[0]) || !f.read(g.data(), sizes[1])) break;
        std::memset(&r.g, 0, sizeof r.g);
        std::memcpy(&r.g, g.data(), std::min(sizeof r.g, size_t(sizes[1])));
        if (!std::memcmp(magic, "FHDUMP02", 8) && !f.read(reinterpret_cast<char *>(&r.dq), sizeof r.dq)) break;
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
Run Play(const std::vector<Record> &recs, size_t from, size_t to, const std::vector<Panel> &panels, int sides = 3,
         const Params &given = Params{}) {
    handcut::Hands hands;
    Params params = given;
    if (const char *v = std::getenv("HANDINPUT_REST_NEAR")) params.restNear = std::atof(v);   // trying other values
    if (const char *v = std::getenv("HANDINPUT_TRAVEL")) params.travel = std::atof(v);
    if (const char *v = std::getenv("HANDINPUT_EXTENDED")) params.extended = std::atof(v);
    if (const char *v = std::getenv("HANDINPUT_NEAREST")) params.nearest = std::atof(v);
    if (const char *v = std::getenv("HANDINPUT_ANGLE")) params.fingerAngle = std::atof(v);
    if (const char *v = std::getenv("HANDINPUT_PINCH_CLICK")) params.pinchClick = std::atoi(v) != 0;
    HandInput in(params);
    Run run;
    for (size_t k = from; k < to; ++k) {
        const Record &r = recs[k];
        const int64_t next = k + 1 < to ? recs[k + 1].publish : r.publish + 33'000'000;
        for (int64_t t = r.publish; t < next; t += 11'111'111) {
            // the head pose at capture time goes in with the capture time (Hands looks it up by time)
            if (t == r.publish) hands.Feed(nullptr, 0, nullptr, r.head, r.capture), hands.Feed(r.hands.data(), r.hands.size(), &r.g, r.head, t);
            else hands.Feed(nullptr, 0, nullptr, r.head, t);
            Input i;
            i.nowNs = t, i.hands = &hands, i.head = r.head, i.haveHead = true, i.allowed[0] = sides & 1, i.allowed[1] = sides & 2;
            in.Update(i, panels);
            if (std::getenv("HANDINPUT_LOG"))
                for (const std::string &l : in.logs()) std::printf("    [%.3f] %s\n", (r.capture - recs.front().capture) / 1e9, l.c_str());
            for (const Event &e : in.events()) {
                if (e.type == Event::Press && std::getenv("HANDINPUT_VERBOSE")) {
                    for (const handcut::Hand &h : hands.seen())
                        if (h.right == (e.side == 1)) {
                            double palm[3];
                            h.Palm(palm);
                            std::printf("    press %s at %.1f s: palm %.2f m under the eyes, %.2f out; tip d %.3f\n",
                                        e.side ? "R" : "L", (r.capture - recs.front().capture) / 1e9, r.head.m[1][3] - palm[1],
                                        std::hypot(palm[0] - r.head.m[0][3], palm[2] - r.head.m[2][3]),
                                        Locate(panels[0], h.p[8]).d);
                        }
                }
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
Panel Facing(const double pt[3], double offset, const double head[3] = nullptr) {
    const double o[3] = {head ? head[0] : 0, head ? head[1] : 0, head ? head[2] : 0};
    const double p[3] = {pt[0] - o[0], pt[1] - o[1], pt[2] - o[2]};
    const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const double c[3] = {o[0] + p[0] / n * (n + offset), o[1] + p[1] / n * (n + offset), o[2] + p[2] / n * (n + offset)};
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

// A near grab's input at the display's rate, on recorded hands: the pinch point moved ahead
// (shown(), what a grab carried before fh_smooth.h) against the smoothed one (Params::grab*, the
// dead zone left out), read at 90 Hz ticks between the published frames. Judder: its second
// difference per tick (steps and noise show there); wobble: a still hand's spread over 0.5 s;
// lag: the delay that best fits each to the pinch point as seen (capture time), while it moves
// faster than 0.15 m/s (negative: ahead of it, by the prediction).
void SmoothReport(const std::vector<Record> &recs) {
    Params k;
    if (const char *v = std::getenv("HANDINPUT_GRAB_MINCUTOFF")) k.grabMinCutoff = std::atof(v);   // trying other values
    if (const char *v = std::getenv("HANDINPUT_GRAB_BETA")) k.grabBeta = std::atof(v);
    struct Tick {
        int64_t t;
        int run;
        double old[3], now[3];
        bool still;
    };
    struct Raw {
        int64_t t;
        int run;
        double p[3];
    };
    struct Side {
        fhsmooth::Point3 f;
        uint32_t id = 0;
        int run = 0;
        int64_t seen = 0;
        std::vector<Tick> ticks;
        std::vector<Raw> raw;
    } sd[2];
    handcut::Hands hs;
    for (size_t r = 0; r < recs.size(); ++r) {
        const Record &rec = recs[r];
        const int64_t next = r + 1 < recs.size() ? std::min(recs[r + 1].publish, rec.publish + 100'000'000) : rec.publish + 33'000'000;
        for (int64_t t = rec.publish; t < next; t += 11'111'111) {
            if (t == rec.publish) {
                hs.Feed(nullptr, 0, nullptr, rec.head, rec.capture);
                hs.Feed(rec.hands.data(), rec.hands.size(), &rec.g, rec.head, t);
                for (int s = 0; s < 2; ++s) {
                    Side &q = sd[s];
                    const handcut::Hand *h = nullptr;
                    for (const handcut::Hand &x : hs.seen())
                        if (x.right == (s == 1)) h = &x;
                    if (!h) continue;
                    if (h->id != q.id || rec.capture - q.seen > 250'000'000) {
                        q.f.Set({k.grabMinCutoff, k.grabBeta, 1.0});
                        q.f.Reset(), q.id = h->id, ++q.run;
                    }
                    q.seen = rec.capture;
                    q.f.Sample({h->pinchPoint[0], h->pinchPoint[1], h->pinchPoint[2]}, rec.capture / 1e9, t / 1e9);
                    q.raw.push_back({rec.capture, q.run, {h->pinchPoint[0], h->pinchPoint[1], h->pinchPoint[2]}});
                }
            } else {
                hs.Feed(nullptr, 0, nullptr, rec.head, t);
            }
            for (int s = 0; s < 2; ++s) {
                Side &q = sd[s];
                const handcut::Hand *h = nullptr;
                for (const handcut::Hand &x : hs.shown())
                    if (x.right == (s == 1) && x.id == q.id) h = &x;
                if (!h || !q.f.Ready() || t - q.seen > 250'000'000) continue;
                const fhsmooth::V3 v = q.f.At(t / 1e9);
                Tick tk{t, q.run, {h->pinchPoint[0], h->pinchPoint[1], h->pinchPoint[2]}, {v.x, v.y, v.z}, false};
                q.ticks.push_back(tk);
            }
        }
    }
    for (int s = 0; s < 2; ++s) {
        Side &q = sd[s];
        if (q.ticks.size() < 100) continue;
        auto rawAt = [&](int64_t t, int run, double out[3]) {
            auto it = std::lower_bound(q.raw.begin(), q.raw.end(), t, [](const Raw &r, int64_t v) { return r.t < v; });
            if (it == q.raw.begin() || it == q.raw.end()) return false;
            const Raw &b = *it, &a = *(it - 1);
            if (a.run != run || b.run != run || b.t - a.t > 200'000'000) return false;
            const double u = double(t - a.t) / double(b.t - a.t);
            for (int i = 0; i < 3; ++i) out[i] = a.p[i] + u * (b.p[i] - a.p[i]);
            return true;
        };
        auto dist = [](const double a[3], const double b[3]) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); };
        // moving (the seen point, 50 ms either side) and still (under 2 cm/s over 0.3 s)
        std::vector<char> moving(q.ticks.size()), still(q.ticks.size());
        for (size_t i = 0; i < q.ticks.size(); ++i) {
            double a[3], b[3], c[3];
            const Tick &x = q.ticks[i];
            moving[i] = rawAt(x.t - 50'000'000, x.run, a) && rawAt(x.t + 50'000'000, x.run, b) && dist(a, b) / 0.1 > 0.15;
            still[i] = rawAt(x.t - 300'000'000, x.run, c) && rawAt(x.t, x.run, b) && dist(c, b) / 0.3 < 0.02;
        }
        std::vector<double> jo, jn;
        for (size_t i = 1; i + 1 < q.ticks.size(); ++i) {
            const Tick &a = q.ticks[i - 1], &b = q.ticks[i], &c = q.ticks[i + 1];
            if (a.run != b.run || b.run != c.run || c.t - a.t > 30'000'000) continue;
            double o2 = 0, n2 = 0;
            for (int j = 0; j < 3; ++j) {
                const double o = a.old[j] - 2 * b.old[j] + c.old[j], n = a.now[j] - 2 * b.now[j] + c.now[j];
                o2 += o * o, n2 += n * n;
            }
            jo.push_back(std::sqrt(o2) * 1000), jn.push_back(std::sqrt(n2) * 1000);
        }
        auto rms = [](const std::vector<double> &v) {
            double a = 0;
            for (double x : v) a += x * x;
            return v.empty() ? 0.0 : std::sqrt(a / double(v.size()));
        };
        auto pct = [](std::vector<double> v, double p) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[size_t(p * double(v.size() - 1))];
        };
        std::vector<double> wo, wn;
        for (size_t i = 0; i + 45 <= q.ticks.size(); i += 45) {
            bool ok = q.ticks[i + 44].t - q.ticks[i].t < 600'000'000;
            for (size_t j = i; j < i + 45 && ok; ++j) ok = still[j] && q.ticks[j].run == q.ticks[i].run;
            if (!ok) continue;
            for (int which = 0; which < 2; ++which) {
                double m[3] = {0, 0, 0}, v = 0;
                for (size_t j = i; j < i + 45; ++j)
                    for (int c = 0; c < 3; ++c) m[c] += (which ? q.ticks[j].now[c] : q.ticks[j].old[c]) / 45;
                for (size_t j = i; j < i + 45; ++j) {
                    const double d = dist(which ? q.ticks[j].now : q.ticks[j].old, m);
                    v += d * d / 45;
                }
                (which ? wn : wo).push_back(std::sqrt(v) * 1000);
            }
        }
        struct Fit { double ms, mm; };
        auto lag = [&](bool now) {
            double best = 0, bestErr = 1e9;
            for (int d = -150; d <= 300; d += 5) {
                double e = 0;
                int n = 0;
                for (size_t i = 0; i < q.ticks.size(); ++i) {
                    double r[3];
                    if (!moving[i] || !rawAt(q.ticks[i].t - d * 1'000'000, q.ticks[i].run, r)) continue;
                    e += dist(now ? q.ticks[i].now : q.ticks[i].old, r), ++n;
                }
                if (n > 50 && e / n < bestErr) bestErr = e / n, best = d;
            }
            return Fit{best, bestErr * 1000};
        };
        const auto lo = lag(false), ln = lag(true);
        std::printf("  grab input, %s hand (%zu ticks at 90 Hz): judder rms %.2f p99 %.2f mm, before (moved ahead) rms %.2f p99 %.2f; "
                    "still wobble p50 %.2f p90 %.2f mm, before %.2f %.2f (%zu windows); lag moving %.0f ms (%.1f mm off), "
                    "before %.0f ms (%.1f mm off), after the capture\n",
                    s ? "right" : "left", q.ticks.size(), rms(jn), pct(jn, 0.99), rms(jo), pct(jo, 0.99), pct(wn, 0.5),
                    pct(wn, 0.9), pct(wo, 0.5), pct(wo, 0.9), wn.size(), ln.ms, ln.mm, lo.ms, lo.mm);
    }
}

int Replay(const char *path) {
    std::vector<Record> recs;
    if (!Load(path, recs)) return std::printf("%s: not a publish dump\n", path), 1;
    // a hand-recorder take's head poses and targets, next to the dump (NAME.poses.jsonl, NAME.prompts.jsonl)
    std::string base = path;
    if (base.size() > 5 && base.substr(base.size() - 5) == ".dump") base.resize(base.size() - 5);
    const auto poses = LoadPoses(base + ".poses.jsonl");
    g_poses = !poses.empty() && recs.front().dq;
    g_ref = Identity();
    if (g_poses) {
        size_t j = 0;
        Mat last = poses.front().second;
        for (Record &r : recs) {   // the pose nearest the capture (about 20 ms before the dequeue)
            const int64_t at = r.dq - 20'000'000;
            while (j + 1 < poses.size() && std::llabs(poses[j + 1].first - at) <= std::llabs(poses[j].first - at)) ++j;
            if (std::llabs(poses[j].first - at) < 60'000'000) last = poses[j].second;
            r.head = last;
        }
        g_ref = Level(recs.front().head);
        std::printf("(head poses from %s.poses.jsonl; panels placed around the head at the start)\n", base.c_str());
    }
    const double secs = (recs.back().capture - recs.front().capture) / 1e9;
    std::printf("== %s: %zu frames, %.0f s\n", path, recs.size(), secs);
    SmoothReport(recs);
    {   // grab that (grabthat.h): every fire here is a false one (none of the takes has the gesture)
        handcut::Hands hands;
        grabthat::Detector det;
        const bool allowed[2] = {true, true};
        int fires = 0, claimed = 0, arms = 0, pinches = 0, grips = 0, takenPinches = 0, takenGrips = 0;
        uint32_t begins[2][2] = {{0, 0}, {0, 0}};
        bool first = true;
        for (const Record &r : recs) {
            hands.Feed(r.hands.data(), r.hands.size(), &r.g, r.head, r.publish);
            det.Frame(hands.captureNs(), hands.seen(), hands.captureHead(), allowed);
            for (const auto &f : det.fired())
                std::printf("grab that: fired, %s hand, at %.2f s\n", f.side ? "right" : "left", (f.t - recs.front().capture) / 1e9), ++fires;
            for (int s = 0; s < 2; ++s) {
                const bool mine = det.claims(s, hands.captureNs());
                claimed += mine;
                // a pinch or grip that began on a claimed hand: the pointer helper leaves it (lost)
                const uint32_t b[2] = {r.g.pinch[s].begins, r.g.grip[s].begins};
                if (!first) {
                    pinches += b[0] != begins[s][0], grips += b[1] != begins[s][1];
                    takenPinches += mine && b[0] != begins[s][0], takenGrips += mine && b[1] != begins[s][1];
                }
                begins[s][0] = b[0], begins[s][1] = b[1];
            }
            first = false;
        }
        arms = det.stats.arms;
        std::printf("grab that: %d fire(s), armed %d times, claimed a hand on %d frames: %d of %d pinches and %d of %d grips "
                    "began on a claimed hand\n", fires, arms, claimed, takenPinches, pinches, takenGrips, grips);
    }

    // 1. Hovering: stretches of 1 s and more where a hand holds still (its palm within 2 cm)
    // pointing (index straight), a panel facing the head 0.5-3 cm beyond the fingertip's
    // median place (along the line from the head). The fingertip's own wobble is all that
    // moves it toward the panel, so every press is a false one.
    {
        handcut::Hands hands;
        struct Win { size_t from, to; int side; double tip[3]; double spread; double dir[3]; bool resting; };
        std::vector<Win> wins;
        for (int s = 0; s < 2; ++s) {
            size_t start = 0;
            bool open = false;
            double anchor[3] = {};
            std::vector<std::array<double, 3>> tips, dirs;
            int pointing = 0, resting = 0;
            auto close = [&](size_t k) {
                open = false;
                if (tips.size() < 15 || recs[k - 1].capture - recs[start].capture < 1'000'000'000 || pointing < int(tips.size() * 0.8))
                    return;
                Win w{start, k, s, {0, 0, 0}, 0, {0, 0, 0}, resting * 2 > int(tips.size())};
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
                for (auto &d : dirs)
                    for (int i = 0; i < 3; ++i) w.dir[i] += d[i];
                const double dl = std::sqrt(w.dir[0] * w.dir[0] + w.dir[1] * w.dir[1] + w.dir[2] * w.dir[2]);
                for (double &x : w.dir) x /= dl;
                wins.push_back(w);
            };
            handcut::Hands hs;
            for (size_t k = 0; k < recs.size(); ++k) {
                hs.Feed(nullptr, 0, nullptr, recs[k].head, recs[k].capture);
                hs.Feed(recs[k].hands.data(), recs[k].hands.size(), &recs[k].g, recs[k].head, recs[k].publish);
                const double hx = recs[k].head.m[0][3], hy = recs[k].head.m[1][3], hz = recs[k].head.m[2][3];
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
                    open = true, start = k, tips.clear(), dirs.clear(), pointing = 0, resting = 0;
                    std::copy(palm, palm + 3, anchor);
                }
                tips.push_back({h->p[8][0], h->p[8][1], h->p[8][2]});
                {
                    const double d[3] = {double(h->p[8][0]) - h->p[7][0], double(h->p[8][1]) - h->p[7][1], double(h->p[8][2]) - h->p[7][2]};
                    const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-9;
                    dirs.push_back({d[0] / n, d[1] / n, d[2] / n});
                    resting += hy - palm[1] > 0.35 && std::hypot(palm[0] - hx, palm[2] - hz) < 0.30;   // as handinput's
                }
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
                const double hp[3] = {recs[w.from].head.m[0][3], recs[w.from].head.m[1][3], recs[w.from].head.m[2][3]};
                const Run r = Play(recs, w.from, w.to, {Facing(w.tip, off, hp)}, 1 << w.side);
                presses += r.presses;
                held += r.stats.heldSlow, nearest += r.stats.heldNotNearest;
            }
            std::printf("  panel %.1f cm beyond the fingertip: %d presses (%.2f per minute); frames at d <= 0 held back: %d slow, %d not nearest\n",
                        off * 100, presses, total > 0 ? presses / total * 60 : 0.0, held, nearest);
        }
        // Deliberate pokes, made from these stretches: a panel across the finger 2 cm beyond its
        // tip, and the hand moved 4 cm into it along the finger (0.2 s), held 0.15 s, pulled back
        // (0.25 s), half a second in; the hand's own tracking noise stays. A poke should press.
        int found = 0, tried = 0, restingTried = 0, restingFound = 0;
        HandInput::Stats why;
        for (const Win &w : wins) {
            std::vector<Record> mod(recs.begin() + long(w.from), recs.begin() + long(w.to));
            const int64_t t0 = mod.front().capture + 500'000'000;
            if (mod.back().capture < t0 + 600'000'000) continue;
            for (Record &r : mod) {
                const double t = (r.capture - t0) / 1e9;
                const double a = t < 0 ? 0 : t < 0.2 ? t / 0.2 : t < 0.35 ? 1 : t < 0.6 ? 1 - (t - 0.35) / 0.25 : 0;
                double room[3], off[3];   // the file's points are in the head frame
                for (int i = 0; i < 3; ++i) room[i] = w.dir[i] * 0.04 * a;
                for (int i = 0; i < 3; ++i) off[i] = r.head.m[0][i] * room[0] + r.head.m[1][i] * room[1] + r.head.m[2][i] * room[2];
                auto *v1 = reinterpret_cast<fh_hands_t *>(r.hands.data());
                auto *fits = r.hands.size() >= sizeof(fh_hands_v2_t) ? reinterpret_cast<fh_hands_v2_t *>(r.hands.data())->fits : nullptr;
                uint32_t cap = 0;
                for (uint32_t k = 0; k < v1->nhands && k < FH_HANDS_MAX_HANDS; ++k) {
                    fh_hand_t &h = v1->hands[k];
                    const bool mine = bool(h.flags & FH_HAND_RIGHT) == (w.side == 1);
                    if (mine) {
                        for (auto &q : h.pts)
                            for (int i = 0; i < 3; ++i) q[i] += float(off[i]);
                        for (uint32_t c = cap; c < cap + h.ncapsules && c < FH_HANDS_MAX_CAPSULES; ++c)
                            for (int i = 0; i < 3; ++i) v1->capsules[c].a[i] += float(off[i]), v1->capsules[c].b[i] += float(off[i]);
                        if (fits) {
                            for (auto &j : fits[k].joints)
                                for (int i = 0; i < 3; ++i) j.p[i] += float(off[i]);
                            for (int i = 0; i < 3; ++i)
                                fits[k].elbow[i] += float(off[i]), fits[k].pinch_point[i] += float(off[i]), fits[k].contact[i] += float(off[i]);
                        }
                    }
                    cap += h.ncapsules;
                }
            }
            double c[3], z[3];
            for (int i = 0; i < 3; ++i) c[i] = w.tip[i] + w.dir[i] * 0.02, z[i] = -w.dir[i];
            Panel P = TestPanel();
            double x[3] = {z[2], 0, -z[0]};
            const double xl = std::hypot(x[0], x[2]);
            if (xl < 1e-3) continue;
            x[0] /= xl, x[2] /= xl;
            const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
            for (int i = 0; i < 3; ++i) P.pose.m[i][0] = float(x[i]), P.pose.m[i][1] = float(y[i]), P.pose.m[i][2] = float(z[i]), P.pose.m[i][3] = float(c[i]);
            P.controls.clear();
            const Run r = Play(mod, 0, mod.size(), {P}, 1 << w.side);
            ++tried, found += r.presses > 0;
            restingTried += w.resting, restingFound += w.resting && r.presses > 0;
            if (!r.presses) {
                const auto &q = r.stats;
                why.heldNotStereo += q.heldNotStereo, why.heldNotExtended += q.heldNotExtended, why.heldNotNearest += q.heldNotNearest;
                why.heldResting += q.heldResting, why.heldAskew += q.heldAskew, why.heldSlow += q.heldSlow, why.heldBusy += q.heldBusy;
            }
        }
        std::printf("  made-up pokes along the finger: %d of %d pressed (%d of %d by a hand resting low by the body); the "
                    "missed ones' frames held back: %d one view, %d bent, %d not nearest, %d resting, %d askew, %d slow, %d busy\n",
                    found, tried, restingFound, restingTried, why.heldNotStereo, why.heldNotExtended, why.heldNotNearest,
                    why.heldResting, why.heldAskew, why.heldSlow, why.heldBusy);
    }
    // 2. Fixed panels where ft-handtest put its own (0.15 m below the eyes) at a range of
    // distances: the hands cross some of them. Presses, the near claim, grabs.
    // Then low ones, where a keyboard or a lap is: 0.45 m under the eyes, pitched back 50
    // degrees to face them (typing under a low panel should poke nothing).
    for (int low = 0; low < 2; ++low)
    for (double z : {0.30, 0.35, 0.40, 0.45, 0.50, 0.60}) {
        Panel P = low ? Tilted(50, -0.45, -z) : TestPanel(-z);
        if (!low) P.pose.m[1][3] = -0.15f;
        P.width = 1.2, P.height = 0.675;
        P.controls.clear();
        P.pose = Mul(g_ref, P.pose);
        const Run r = Play(recs, 0, recs.size(), {P});
        const auto &s = r.stats;
        std::printf("%s panel %.2f m ahead: near %4.1f%% of frames, %3d presses (%.1f/min), %d grabs; held back: %d one view, "
                    "%d bent, %d not nearest, %d pinching, %d busy, %d resting, %d askew, %d slow\n",
                    low ? "low " : "eye ", z, 100.0 * r.nearFrames / std::max(1, r.frames), r.presses, r.presses / secs * 60, r.grabs,
                    s.heldNotStereo, s.heldNotExtended, s.heldNotNearest, s.heldPinching, s.heldBusy, s.heldResting,
                    s.heldAskew, s.heldSlow);
    }
    // 2b. The hand recorder's touch-the-dot take: deliberate touches of a dot in the room, with
    // nothing to push against (the tip stops at the dot, give or take the tracking error). A
    // panel facing the head through each dot (30 cm square), or 3 cm nearer: does each touch press?
    // At the default finger angle (50 deg) and at 65.
    {
        const auto targets = LoadTargets(base + ".prompts.jsonl");
        for (double ahead : {0.0, 0.03})
            for (double angle : {50.0, 65.0}) {
                Params k;
                k.fingerAngle = angle;
                int hit = 0, tried = 0;
                HandInput::Stats why;
                for (const Target &tg : targets) {
                    if (!g_poses || !tg.done) continue;
                    size_t from = recs.size(), to = 0;
                    for (size_t j = 0; j < recs.size(); ++j)
                        if (recs[j].dq >= tg.show - 500'000'000 && recs[j].dq <= tg.done + 300'000'000) from = std::min(from, j), to = j + 1;
                    if (from >= to) continue;
                    const Mat &h = recs[from].head;
                    const double hp[3] = {h.m[0][3], h.m[1][3], h.m[2][3]};
                    Panel P = Facing(tg.room, -ahead, hp);
                    P.width = P.height = 0.3;
                    P.pxW = P.pxH = 600;
                    const Run r = Play(recs, from, to, {P}, 3, k);
                    ++tried, hit += r.presses > 0;
                    const auto &q = r.stats;
                    why.heldNotStereo += q.heldNotStereo, why.heldNotExtended += q.heldNotExtended, why.heldNotNearest += q.heldNotNearest;
                    why.heldResting += q.heldResting, why.heldAskew += q.heldAskew, why.heldSlow += q.heldSlow, why.heldBusy += q.heldBusy;
                    if (ahead > 0 || angle > 50) continue;
                    // how close the nearest index tip came, and at what angle to the normal
                    handcut::Hands hs;
                    double best = 1e9, ang = 0, u = 0, v = 0;
                    for (size_t j = from; j < to; ++j) {
                        hs.Feed(nullptr, 0, nullptr, recs[j].head, recs[j].capture);
                        hs.Feed(recs[j].hands.data(), recs[j].hands.size(), &recs[j].g, recs[j].head, recs[j].publish);
                        for (const handcut::Hand &x : hs.seen()) {
                            const Hit t = Locate(P, x.p[8]);
                            if (std::hypot(t.u, t.v) < 0.1 && t.d < best) {
                                best = t.d, u = t.u, v = t.v;

                                double d[3], l[3];
                                for (int i = 0; i < 3; ++i) d[i] = double(x.p[8][i]) - x.p[7][i];
                                for (int i = 0; i < 3; ++i) l[i] = P.pose.m[0][i] * d[0] + P.pose.m[1][i] * d[1] + P.pose.m[2][i] * d[2];
                                ang = std::acos(std::clamp(-l[2] / std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]), -1.0, 1.0)) * 180 / M_PI;
                            }
                        }
                    }
                    std::printf("  touch %d: the index tip came to %+.1f cm of the dot's plane (%.1f, %.1f cm off the dot), its last "
                                "bone %.0f deg off the normal\n", tried, best * 100, u * 100, v * 100, ang);
                }
                if (tried)
                    std::printf("touch the dot, panel %.0f cm nearer than the dot, finger within %.0f deg: %d of %d pressed; frames "
                                "held back: %d one view, %d bent, %d not nearest, %d resting, %d askew, %d slow, %d busy\n",
                                ahead * 100, angle, hit, tried, why.heldNotStereo, why.heldNotExtended, why.heldNotNearest,
                                why.heldResting, why.heldAskew, why.heldSlow, why.heldBusy);
            }
    }
    // 3. The matte: every frame's hands against a panel 0.6 m ahead, CPU cost of building and
    // projecting it, circle, mesh and capsules.
    {
        handcut::Hands hands;
        handcut::Panel P{At(0, -0.15, -0.6), 1.2, 0.675, 0, 3440, 1935};
        const double eyes[2][3] = {{-0.032, 0, 0}, {0.032, 0, 0}}, head[3] = {0, 0, 0};
        for (handcut::MatteShape shape : {handcut::MatteShape::Circle, handcut::MatteShape::Mesh, handcut::MatteShape::Capsules}) {
            handcut::MatteParams mp;
            mp.shape = shape;
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
                        handcut::ShapeName(shape), withHands, withHands ? pieces / 2.0 / withHands : 0.0, bad,
                        us[us.size() / 2], us[us.size() * 9 / 10], us.back());
        }
    }
    return 0;
}


// ------------------------------------------------------------------------------ the palm menu

SynthHand PalmUpHand(bool right = true, uint32_t id = 1) {
    SynthHand h;
    h.palm = true, h.right = right, h.id = id;
    return h;
}

handcut::Hand Seen(const SynthHand &sh) {
    Synth s;
    s.Write({sh}, 1'000'000'000);
    handcut::Hands h;
    h.Feed(&s.file, sizeof(fh_hands_t), nullptr, Identity(), 1'010'000'000);
    return h.seen().empty() ? handcut::Hand{} : h.seen()[0];
}

void TestPalmUp() {
    const palmmenu::Params k;
    for (bool right : {false, true}) {
        SynthHand sh = PalmUpHand(right);
        palmmenu::Pose pose;
        // palm up at chest height, tilted a little toward the face, looked at: up
        CHECK(palmmenu::PalmUp(Seen(sh), Identity(), k, false, &pose));
        CHECK(pose.facingDeg < 45 && pose.lookDeg < 40 && pose.open > 1.8);
        double n[3];
        palmmenu::PalmNormal(Seen(sh), n);
        CHECK(n[1] > 0.9);   // the normal comes out of the palm, up, for either hand
        // palm down (typing, resting): not
        sh.rollDeg = 180;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, false, &pose) && pose.facingDeg > 120);
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        // on its side (thumb up): not
        sh.rollDeg = right ? 90 : -90;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        // a fist: not; half curled: only once up (hysteresis)
        sh.rollDeg = 0, sh.curl = 2;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        sh.curl = 1;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, false, &pose) && palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        CHECK(pose.open > k.openLeave && pose.open < k.open);
        // turned a little away from the face: only once up
        sh.curl = 0, sh.tiltDeg = -10;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, false, &pose) && palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        CHECK(pose.facingDeg > k.facing && pose.facingDeg < k.facingLeave);
        // not looked at (out to the side and low), or out of reach: not
        sh.tiltDeg = 20, sh.palmAt[0] = right ? 0.4 : -0.4, sh.palmAt[1] = -0.15, sh.palmAt[2] = -0.2;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, true, &pose) && pose.lookDeg > k.lookLeave);
        sh.palmAt[0] = 0, sh.palmAt[1] = -0.3, sh.palmAt[2] = -0.8;
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, true));
        // the head turned to look at it there: up again (the head's pose counts, not the room's axes)
        sh.palmAt[0] = right ? 0.4 : -0.4, sh.palmAt[1] = -0.25, sh.palmAt[2] = 0;
        sh.tiltDeg = 0;
        sh.rollDeg = right ? 35 : -35;   // the palm turned toward the head
        CHECK(!palmmenu::PalmUp(Seen(sh), Identity(), k, false));
        CHECK(palmmenu::PalmUp(Seen(sh), RotY(right ? -M_PI / 2 : M_PI / 2, 0, 0, 0), k, false));
    }
}

struct PalmRig {
    handcut::Hands hands;
    palmmenu::PalmMenu menu;
    Synth synth;
    int64_t t = 1'000'000'000;
    bool allowed[2] = {true, true}, busy[2] = {false, false};
    std::vector<int> toggles;   // the side of each toggle
    void Frame(const std::vector<SynthHand> &h, int ticks = 3) {
        synth.Write(h, t);
        for (int k = 0; k < ticks; ++k) {
            const int64_t now = t + 30'000'000 + k * 11'111'111;
            if (k == 0) hands.Feed(&synth.file, sizeof(fh_hands_t), &synth.g, Identity(), now);
            else hands.Feed(nullptr, 0, nullptr, Identity(), now);
            palmmenu::Input i;
            i.nowNs = now, i.hands = &hands, i.head = Identity(), i.haveHead = true;
            for (int s = 0; s < 2; ++s) i.allowed[s] = allowed[s], i.busy[s] = busy[s];
            menu.Update(i);
            if (menu.toggled() >= 0) toggles.push_back(menu.toggled());
        }
        t += 33'333'333;
    }
    void Frames(int n, const std::vector<SynthHand> &h) {
        for (int k = 0; k < n; ++k) Frame(h);
    }
    void Pinch(int side, bool down, uint32_t id = 1) {
        const double p[3] = {0, -0.2, -0.35};
        synth.Pinch(side, down, t, p, id);
    }
};

void TestPalmMenuShows() {
    PalmRig r;
    const SynthHand up = PalmUpHand();
    SynthHand down = up;
    down.rollDeg = 180;
    r.Frames(5, {up});   // 167 ms
    CHECK(!r.menu.shown(1));
    r.Frames(6, {up});   // 367 ms
    CHECK(r.menu.shown(1) && !r.menu.shown(0) && !r.menu.pinched(1));
    double at[3];
    CHECK(r.menu.Ring(1, r.hands.shown(), at));
    const handcut::Hand h = Seen(up);
    double palm[3];
    h.Palm(palm);
    NEAR(at[1] - palm[1], r.menu.params().lift * std::cos(20 * M_PI / 180), 0.003);   // above the palm, toward the face
    CHECK(!r.menu.Ring(0, r.hands.shown(), at));
    // turned away for one frame: stays; for 150 ms: goes
    r.Frame({down});
    CHECK(r.menu.shown(1));
    r.Frame({up});
    r.Frames(6, {down});
    CHECK(!r.menu.shown(1));
    // a palm up for less than showMs, twice: never shows
    r.Frames(5, {up});
    r.Frame({down});
    r.Frames(5, {up});
    CHECK(!r.menu.shown(1));
    // the hand gone: the ring goes after lostMs
    r.Frames(6, {up});
    CHECK(r.menu.shown(1));
    r.Frames(10, {});
    CHECK(!r.menu.shown(1));
    // the left hand alone
    r.Frames(11, {PalmUpHand(false, 2)});
    CHECK(r.menu.shown(0) && !r.menu.shown(1));
    // not allowed (a controller on that side, a game without the dashboard), or near input busy
    // with that hand: no ring
    r.allowed[0] = false;
    r.Frame({PalmUpHand(false, 2)});
    CHECK(!r.menu.shown(0));
    r.allowed[0] = true, r.busy[0] = true;
    r.Frames(11, {PalmUpHand(false, 2)});
    CHECK(!r.menu.shown(0));
    r.busy[0] = false;
    r.menu.params().on = false;   // HANDS_PALM_MENU=0
    r.Frames(11, {PalmUpHand(false, 2)});
    CHECK(!r.menu.shown(0) && r.toggles.empty());
}

void TestPalmMenuPinch() {
    const SynthHand up = PalmUpHand();
    {   // a pinch tap on the ring: lit while held, the release toggles the Steam menu
        PalmRig r;
        r.Frames(11, {up});
        r.Pinch(1, true);
        r.Frame({up});
        CHECK(r.menu.pinched(1) && r.menu.shown(1) && r.toggles.empty());
        r.Frames(3, {up});
        r.Pinch(1, false);
        r.Frame({up});
        CHECK(!r.menu.pinched(1) && r.menu.shown(1) && r.toggles == std::vector<int>{1});
        CHECK(r.menu.stats.toggles == 1 && r.menu.stats.pinches == 1);
        // again within againMs (one pinch read as two): nothing; later: toggles again (closes it)
        r.Pinch(1, true);
        r.Frame({up});
        r.Pinch(1, false);
        r.Frame({up});
        CHECK(r.toggles.size() == 1);
        r.Frames(15, {up});
        r.Pinch(1, true);
        r.Frame({up});
        r.Pinch(1, false);
        r.Frame({up});
        CHECK(r.toggles.size() == 2);
    }
    {   // a pinch that began and ended between two reads (a quick tap): toggles too
        PalmRig r;
        r.Frames(11, {up});
        r.Pinch(1, true);
        r.Pinch(1, false);
        r.Frame({up});
        CHECK(r.toggles == std::vector<int>{1});
    }
    {   // a pinch that began before the ring showed is the helper's: its release does nothing
        PalmRig r;
        r.Frames(3, {up});
        r.Pinch(1, true);
        r.Frames(10, {up});
        CHECK(r.menu.shown(1) && !r.menu.pinched(1));
        r.Pinch(1, false);
        r.Frame({up});
        CHECK(r.toggles.empty());
    }
    {   // lost (the hand went mid-pinch): nothing
        PalmRig r;
        r.Frames(11, {up});
        r.Pinch(1, true);
        r.Frame({up});
        r.Pinch(1, false);
        r.synth.g.pinch[1].flags |= FH_PINCH_LOST;
        r.Frame({up});
        CHECK(r.toggles.empty() && r.menu.stats.cancels == 1);
    }
    {   // the hand turned over while pinching: the ring stays until the release, which does nothing
        PalmRig r;
        SynthHand down = up;
        down.rollDeg = 180;
        r.Frames(11, {up});
        r.Pinch(1, true);
        r.Frame({up});
        r.Frames(8, {down});
        CHECK(r.menu.pinched(1) && r.menu.shown(1));
        r.Pinch(1, false);
        r.Frame({down});
        CHECK(r.toggles.empty() && r.menu.stats.cancels == 1);
        r.Frames(6, {down});
        CHECK(!r.menu.shown(1));
    }
    {   // a fist pinch (curled fingers) once the ring shows: still the ring's (the leave thresholds)
        PalmRig r;
        SynthHand half = up;
        half.curl = 1;
        r.Frames(11, {up});
        r.Frames(3, {half});
        r.Pinch(1, true);
        r.Frame({half});
        r.Pinch(1, false);
        r.Frame({half});
        CHECK(r.toggles == std::vector<int>{1});
    }
    {   // hand input taken away mid-pinch: cancelled
        PalmRig r;
        r.Frames(11, {up});
        r.Pinch(1, true);
        r.Frame({up});
        r.allowed[1] = false;
        r.Frame({up});
        r.Pinch(1, false);
        r.allowed[1] = true;
        r.Frame({up});
        CHECK(r.toggles.empty() && !r.menu.pinched(1) && r.menu.stats.cancels == 1);
    }
    {   // the other hand's pinch: not the ring's
        PalmRig r;
        SynthHand other = PalmUpHand(false, 2);
        other.rollDeg = 180;
        r.Frames(11, {up, other});
        r.Pinch(0, true, 2);
        r.Frame({up, other});
        r.Pinch(0, false, 2);
        r.Frame({up, other});
        CHECK(r.toggles.empty() && r.menu.shown(1) && !r.menu.shown(0));
    }
}


// ------------------------------------------------------------------------------ grab that

// A hand held out, palm forward, fingers up (grabthat.h), the palm's centre at (x, y, z) (head at
// the origin, -z ahead); `flip` 0..1 turns it over toward you about the fingers' axis, `fist`
// 0 open, 1 the three fingers half curled, 2 all four curled.
SynthHand StopHand(double x, double y, double z, double flip = 0, int fist = 0, bool right = true) {
    SynthHand h;
    h.palm = true, h.right = right, h.id = right ? 1 : 2;
    h.palmAt[0] = x, h.palmAt[1] = y, h.palmAt[2] = z;
    h.rollDeg = 180 * (1 - flip), h.tiltDeg = 90;
    h.curl = fist >= 2 ? 2 : fist, h.curlIndex = fist >= 2 ? 2 : 0;
    return h;
}

// Plays synthetic hands frames (30 Hz) through a detector, head at the origin; returns the fires'
// times (s from the start) and how many frames it claimed the hand on.
struct GrabRig {
    handcut::Hands hands;
    Synth synth;
    grabthat::Detector det;
    int64_t t0 = 1'000'000'000, t = 1'000'000'000;
    std::vector<grabthat::Fire> fires;
    int claimed[2] = {0, 0};
    bool allowed[2] = {true, true};
    void Frame(const std::vector<SynthHand> &h) {
        synth.Write(h, t);
        hands.Feed(&synth.file, sizeof(fh_hands_t), &synth.g, Identity(), t + 30'000'000);
        det.Frame(hands.captureNs(), hands.seen(), hands.captureHead(), allowed);
        for (const auto &f : det.fired()) fires.push_back(f);
        for (int s = 0; s < 2; ++s) claimed[s] += det.claims(s, t);
        t += 33'333'333;
    }
    double At(const grabthat::Fire &f) const { return (f.t - t0) / 1e9; }
};

void TestGrabThatGesture() {
    {   // the user's gesture: held out 0.5 s, then a fist turning over and pulled back 15 cm in 0.2 s
        GrabRig r;
        for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
        CHECK(r.det.armed(1) && !r.det.claims(1, r.t));
        const grabthat::Features open = grabthat::Measure(r.hands.seen()[0], Identity());
        for (int k = 1; k <= 6; ++k) {
            const double f = k / 6.0;
            r.Frame({StopHand(0.1, -0.15, -0.42 + 0.15 * f, f, k >= 2 ? 2 : 1)});
        }
        const grabthat::Features fist = grabthat::Measure(r.hands.seen()[0], Identity());
        for (int k = 0; k < 15; ++k) r.Frame({StopHand(0.1, -0.15, -0.27, 1, 2)});
        std::printf("grab that, synthetic: open %.2f palm %.2f fwd %.2f -> fist %.2f palm %.2f fwd %.2f; %zu fire(s), "
                    "claimed %d frames\n", open.open, open.palmFwd, open.fwd, fist.open, fist.palmFwd, fist.fwd,
                    r.fires.size(), r.claimed[1]);
        CHECK(r.fires.size() == 1 && r.fires[0].side == 1);
        CHECK(r.claimed[1] >= 2 && r.claimed[0] == 0);
        if (!r.fires.empty()) {   // aimed from the eyes through the palm as it was held out
            NEAR(r.fires[0].palm[2], -0.42, 0.01), NEAR(r.fires[0].eye[2], 0, 1e-6);
            CHECK(r.fires[0].fist[2] > r.fires[0].palm[2] + 0.05);   // pulled in by then
        }
        // the left hand, the same way
        GrabRig l;
        for (int k = 0; k < 10; ++k) l.Frame({StopHand(-0.1, -0.15, -0.42, 0, 0, false)});
        for (int k = 1; k <= 6; ++k) l.Frame({StopHand(-0.1, -0.15, -0.42 + 0.025 * k, k / 6.0, k >= 2 ? 2 : 1, false)});
        for (int k = 0; k < 5; ++k) l.Frame({StopHand(-0.1, -0.15, -0.27, 1, 2, false)});
        CHECK(l.fires.size() == 1 && l.fires[0].side == 0);
        // not allowed (a controller on that side, hand input off): nothing
        GrabRig n;
        n.allowed[1] = false;
        for (int k = 0; k < 10; ++k) n.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 6; ++k) n.Frame({StopHand(0.1, -0.15, -0.42 + 0.025 * k, k / 6.0, 2)});
        CHECK(n.fires.empty() && n.claimed[1] == 0);
        // held out too briefly (under armMs): nothing
        GrabRig q;
        for (int k = 0; k < 4; ++k) q.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 6; ++k) q.Frame({StopHand(0.1, -0.15, -0.42 + 0.025 * k, k / 6.0, 2)});
        CHECK(q.fires.empty());
    }
    {   // a wave: held out open, palm forward, swinging 15 cm either way at 1.5 Hz for 3 s
        GrabRig r;
        for (int k = 0; k < 90; ++k) {
            const double a = std::sin(2 * M_PI * 1.5 * k / 30.0);
            SynthHand h = StopHand(0.1 + 0.15 * a, -0.1, -0.40);
            h.rollDeg += 20 * a;
            r.Frame({h});
        }
        CHECK(r.fires.empty() && r.claimed[1] == 0);
    }
    {   // a push: palm forward from 25 to 50 cm out, then back to 30 cm, relaxed
        GrabRig r;
        for (int k = 0; k <= 12; ++k) r.Frame({StopHand(0.1, -0.15, -0.25 - 0.25 * k / 12.0)});
        for (int k = 1; k <= 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.50 + 0.02 * k, 0.3 * k / 10, 1)});
        for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.30, 0.3, 1)});
        CHECK(r.fires.empty());
    }
    {   // a fist made without pulling back, turning over: no fire, and the claim lets go
        GrabRig r;
        for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 6; ++k) r.Frame({StopHand(0.1, -0.15, -0.42 + 0.003 * k, k / 6.0, 2)});
        for (int k = 0; k < 30; ++k) r.Frame({StopHand(0.1, -0.15, -0.40, 1, 2)});
        CHECK(r.fires.empty() && !r.det.claims(1, r.t));
        // ...nor a fist pulled back slowly, over a second
        GrabRig w;
        for (int k = 0; k < 10; ++k) w.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 3; ++k) w.Frame({StopHand(0.1, -0.15, -0.42, k / 3.0, 2)});
        for (int k = 1; k <= 30; ++k) w.Frame({StopHand(0.1, -0.15, -0.42 + 0.15 * k / 30.0, 1, 2)});
        CHECK(w.fires.empty());
    }
    {   // held out, turned over first (palm toward you), then closed and pulled in: not closing on
        // the window (the one fire on the recordings before the palm rule)
        GrabRig r;
        for (int k = 0; k < 15; ++k) r.Frame({StopHand(0.1, -0.15, -0.36)});
        for (int k = 1; k <= 8; ++k) r.Frame({StopHand(0.1, -0.15, -0.36, k / 8.0)});   // over 0.27 s
        for (int k = 1; k <= 6; ++k) r.Frame({StopHand(0.1, -0.15, -0.36 + 0.02 * k, 1, k >= 2 ? 2 : 1)});
        CHECK(r.fires.empty());
    }
    {   // a pinch with the hand held out, pulled back (a ray carry): the other fingers stay open
        GrabRig r;
        for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 8; ++k) {
            SynthHand h = StopHand(0.1, -0.15, -0.42 + 0.02 * k, 0.5 * k / 8);
            h.curlIndex = 2;
            r.Frame({h});
        }
        CHECK(r.fires.empty() && r.claimed[1] == 0);
        // a pinch-grab near a panel: pointing hand, palm down, pulled in: never held out
        GrabRig g;
        for (int k = 0; k < 20; ++k) g.Frame({PinchAt(0.05, -0.3, 0.1 + 0.01 * k)});
        CHECK(g.fires.empty() && g.claimed[1] == 0);
    }
    {   // held out, then dropped to the lap, the fingers relaxing and the palm turning down
        GrabRig r;
        for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
        for (int k = 1; k <= 9; ++k) {
            const double f = k / 9.0;
            SynthHand h = StopHand(0.1, -0.15 - 0.30 * f, -0.42 + 0.22 * f, 0, 1);
            h.tiltDeg = 90 - 90 * f;   // fingers forward, palm down
            r.Frame({h});
        }
        for (int k = 0; k < 15; ++k) {
            SynthHand h = StopHand(0.1, -0.45, -0.20, 0, 1);
            h.tiltDeg = 0;
            r.Frame({h});
        }
        CHECK(r.fires.empty());
    }
    {   // typing: both hands low and close, palms down, fingers half curled, tapping
        GrabRig r;
        for (int k = 0; k < 90; ++k) {
            SynthHand a = StopHand(0.12, -0.40, -0.25 - 0.01 * std::sin(k), 0, 1), b = StopHand(-0.12, -0.40, -0.25, 0, k % 7 ? 1 : 2, false);
            a.tiltDeg = b.tiltDeg = 0;
            r.Frame({a, b});
        }
        CHECK(r.fires.empty() && r.claimed[0] == 0 && r.claimed[1] == 0);
    }
    {   // twice in a row: the second needs the hand held out again (and againMs after the first)
        GrabRig r;
        for (int rep = 0; rep < 2; ++rep) {
            for (int k = 0; k < 30; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
            for (int k = 1; k <= 6; ++k) r.Frame({StopHand(0.1, -0.15, -0.42 + 0.025 * k, k / 6.0, 2)});
            for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.27, 1, 2)});
        }
        CHECK(r.fires.size() == 2);
    }
}

// The target, where it goes, and the move.
void TestGrabThatPlace() {
    using grabthat::Target;
    std::vector<Target> panels(3);
    panels[0].index = 0, panels[0].pose = At(0, 0, -2), panels[0].width = 1.6, panels[0].height = 0.9;
    panels[1].index = 1, panels[1].pose = RotY(M_PI / 4, -1.4, 0, -1.4), panels[1].width = 1.0, panels[1].height = 0.6;
    panels[2].index = 2, panels[2].pose = At(1.5, 0.2, -1.5), panels[2].width = 1.0, panels[2].height = 0.6,
    panels[2].curve = 1.5;
    grabthat::Params k;
    grabthat::Fire f;
    f.head = Identity();
    auto aim = [&](double x, double y, double z) {
        f.palm[0] = x, f.palm[1] = y, f.palm[2] = z;
        return grabthat::Pick(panels, f, k);
    };
    CHECK(aim(0.05, -0.02, -0.4) == 0);
    CHECK(aim(-0.28, 0, -0.28) == 1);    // 45 degrees left
    CHECK(aim(0.28, 0.04, -0.28) == 2);  // the curved one, 45 degrees right
    CHECK(aim(0.5, 0.3, -0.05) == 0);    // nothing that way (off to the right and up): where the head points
    f.head = RotY(-M_PI / 2, 0, 0, 0);    // looking right, palm at nothing: nothing within 20 deg
    CHECK(aim(0, -0.4, 0.3) == -1);
    // the curved panel's surface: from its axis straight ahead, it's met r away at u = 0
    double u, v, t;
    const double from[3] = {1.5, 0.2, -1.5 + 1.5}, dir[3] = {0, 0, -1};
    CHECK(grabthat::RayHit(panels[2], from, dir, &u, &v, &t));
    NEAR(u, 0, 1e-6), NEAR(t, 1.5, 1e-6);
    const double d2[3] = {std::sin(0.3), 0, -std::cos(0.3)};   // 0.3 rad round: u = r * 0.3 along the arc
    CHECK(grabthat::RayHit(panels[2], from, d2, &u, &v, &t));
    NEAR(u, 0.45, 1e-4), NEAR(t, 1.5, 1e-4);
    // where it comes: 45 cm ahead across the floor, at the fist's height, facing the eyes
    f.head = Identity(), f.fist[0] = 0.1, f.fist[1] = -0.15, f.fist[2] = -0.25;
    Mat m = grabthat::Arrival(f, k);
    NEAR(m.m[0][3], 0, 1e-6), NEAR(m.m[1][3], -0.15, 1e-6), NEAR(m.m[2][3], -0.45, 1e-6);
    const double c = -(m.m[0][2] * m.m[0][3] + m.m[1][2] * m.m[1][3] + m.m[2][2] * m.m[2][3]) /
                     std::sqrt(m.m[0][3] * m.m[0][3] + m.m[1][3] * m.m[1][3] + m.m[2][3] * m.m[2][3]);
    CHECK(c > 0.9999);
    f.head = RotY(-M_PI / 2, 0, 1.6, 0), f.fist[1] = 0.5;   // turned right; a fist low: no lower than 45 cm under the eyes
    m = grabthat::Arrival(f, k);
    NEAR(m.m[0][3], 0.45, 1e-5), NEAR(m.m[1][3], 1.6 - 0.45, 1e-5), NEAR(m.m[2][3], 0, 1e-5);
    // the move: the ends, and halfway turned half
    const Mat a = At(0, 0, -2), b = RotY(M_PI / 2, 1, 0, 0);
    Mat h = grabthat::Blend(a, b, 0);
    NEAR(h.m[2][3], -2, 1e-6), NEAR(h.m[0][0], 1, 1e-6);
    h = grabthat::Blend(a, b, 1);
    NEAR(h.m[0][3], 1, 1e-6), NEAR(h.m[0][2], 1, 1e-5);
    h = grabthat::Blend(a, b, 0.5);
    NEAR(h.m[0][3], 0.5, 1e-6), NEAR(std::atan2(h.m[0][2], h.m[2][2]), M_PI / 4, 1e-5);
    NEAR(grabthat::Ease(0.5), 0.5, 1e-9), NEAR(grabthat::Ease(0), 0, 1e-9), NEAR(grabthat::Ease(2), 1, 1e-9);
    // at a panel near you: back if it came by the gesture (wherever about arm's reach it is now),
    // else centred at arm's reach; a far one comes (2026-10-10: it fired 3 times at near displays and
    // never sent one back)
    using grabthat::Outcome;
    const Mat head = At(0, 1.6, 0);
    CHECK(grabthat::Decide(true, At(0, 1.45, -0.45), head, k) == Outcome::Back);
    CHECK(grabthat::Decide(true, At(0.3, 1.3, -0.5), head, k) == Outcome::Back);   // moved about since
    CHECK(grabthat::Decide(false, At(0, 1.45, -0.45), head, k) == Outcome::Centre);
    CHECK(grabthat::Decide(false, At(0.2, 1.5, -0.6), head, k) == Outcome::Centre);
    CHECK(grabthat::Decide(true, At(0, 1.6, -2), head, k) == Outcome::Bring);       // taken far away since
    CHECK(grabthat::Decide(false, At(0, 1.6, -2), head, k) == Outcome::Bring);
}

// --- the user's flags (frame-hands captures/flags/<flag>: sets.jsonl, hmd.jsonl, manifest.json)

// A small JSON reader for the flags' lines: numbers, strings, arrays, objects.
struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json *operator[](const char *key) const {
        for (const auto &[k, v] : fields)
            if (k == key) return &v;
        return nullptr;
    }
};
bool ParseJson(const char *&p, Json &out) {
    while (*p && std::isspace((unsigned char)*p)) ++p;
    if (*p == '{' || *p == '[') {
        const bool obj = *p++ == '{';
        out.kind = obj ? Json::Obj : Json::Arr;
        for (;;) {
            while (*p && std::isspace((unsigned char)*p)) ++p;
            if (*p == (obj ? '}' : ']')) return ++p, true;
            if (*p == ',') {
                ++p;
                continue;
            }
            if (obj) {
                Json key;
                if (!ParseJson(p, key) || key.kind != Json::Str) return false;
                while (*p && std::isspace((unsigned char)*p)) ++p;
                if (*p++ != ':') return false;
                out.fields.push_back({key.str, Json{}});
                if (!ParseJson(p, out.fields.back().second)) return false;
            } else {
                out.items.emplace_back();
                if (!ParseJson(p, out.items.back())) return false;
            }
        }
    }
    if (*p == '"') {
        out.kind = Json::Str;
        for (++p; *p && *p != '"'; ++p) {
            if (*p == '\\' && p[1]) ++p;
            out.str += *p;
        }
        return *p ? (++p, true) : false;
    }
    if (!std::strncmp(p, "null", 4) || !std::strncmp(p, "true", 4)) return out.num = *p == 't', p += 4, true;
    if (!std::strncmp(p, "false", 5)) return p += 5, true;
    char *end;
    out.kind = Json::Num, out.num = std::strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
}
bool ReadJsonLines(const std::string &path, std::vector<Json> &out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const char *p = line.c_str();
        Json j;
        if (ParseJson(p, j)) out.push_back(std::move(j));
    }
    return true;
}

// Replays a flag's hands (as ft-hands published them, "pub") through the detector, with the head
// poses it recorded; the fires' times are seconds from the flag. False if it can't be read.
bool ReplayFlag(const std::string &dir, std::vector<grabthat::Fire> &fires, double *flagT, double *seconds,
                int *claimed = nullptr, int *arms = nullptr, const grabthat::Params &k = {}) {
    std::vector<Json> sets, poses, manifest;
    if (!ReadJsonLines(dir + "/sets.jsonl", sets) || !ReadJsonLines(dir + "/hmd.jsonl", poses)) return false;
    std::ifstream mf(dir + "/manifest.json");
    std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    const char *mp = text.c_str();
    Json m;
    if (!ParseJson(mp, m) || !m["flag_t_raw_ns"]) return false;
    *flagT = m["flag_t_raw_ns"]->num;
    std::vector<std::pair<double, Mat>> heads;
    for (const Json &j : poses) {
        if (!j["t_ns"] || !j["m"] || j["m"]->items.size() != 12) continue;
        Mat h{};
        for (int i = 0; i < 12; ++i) h.m[i / 4][i % 4] = float(j["m"]->items[i].num);
        heads.push_back({j["t_ns"]->num, h});
    }
    grabthat::Detector det(k);
    const bool allowed[2] = {true, true};
    fires.clear();
    double first = 0, last = 0;
    for (const Json &j : sets) {
        if (!j["t_raw_ns"] || !j["hands"]) continue;
        const double t = j["t_raw_ns"]->num;
        if (first == 0) first = t;
        last = t;
        Mat head = Identity();
        double best = 1e18;
        for (const auto &[ht, hm] : heads)
            if (std::fabs(ht - t) < best) best = std::fabs(ht - t), head = hm;
        std::vector<handcut::Hand> hands;
        for (const Json &hj : j["hands"]->items) {
            const Json *pts = hj["pub"] ? hj["pub"] : hj["pts"];
            if (!pts || pts->items.size() != 21 || !hj["side"]) continue;
            handcut::Hand h;
            h.id = hj["id"] ? uint32_t(hj["id"]->num) : 0, h.right = hj["side"]->str == "R";
            h.stereo = hj["nviews"] && hj["nviews"]->num >= 2, h.confidence = 1, h.captureNs = int64_t(t);
            for (int q = 0; q < 21; ++q) {
                const double x = pts->items[q].items[0].num, y = pts->items[q].items[1].num, z = pts->items[q].items[2].num;
                for (int i = 0; i < 3; ++i) h.p[q][i] = float(head.m[i][0] * x + head.m[i][1] * y + head.m[i][2] * z + head.m[i][3]);
            }
            hands.push_back(h);
        }
        det.Frame(int64_t(t), hands, head, allowed);
        for (const auto &f : det.fired()) fires.push_back(f);
        if (claimed) *claimed += det.claims(0, int64_t(t)) + det.claims(1, int64_t(t));
    }
    if (arms) *arms = det.stats.arms;
    *seconds = (last - first) / 1e9;
    return true;
}

std::string FlagsDir() {
    const char *env = std::getenv("FRAME_HANDS_FLAGS");
    if (env && *env) return env;
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Desktop/Projects/frame-hands/captures/flags";
}

// A flag's hands (as ft-hands published them) and gestures through near hand input and "grab
// that" together, as vr.cpp runs them (90 Hz ticks, a fire cancels a fist waiting on it), with
// `panels` in the room. Fires, the hand input's events, and its log.
struct FlagRun {
    std::vector<grabthat::Fire> fires;
    std::vector<Event> events;
    std::vector<std::string> logs;
    HandInput::Stats stats;
};
bool FlagThroughHandInput(const std::string &dir, const std::vector<Panel> &panels, FlagRun &out, const Params &k = {}) {
    std::vector<Json> sets, poses;
    if (!ReadJsonLines(dir + "/sets.jsonl", sets) || !ReadJsonLines(dir + "/hmd.jsonl", poses)) return false;
    std::vector<std::pair<double, Mat>> heads;
    for (const Json &j : poses) {
        if (!j["t_ns"] || !j["m"] || j["m"]->items.size() != 12) continue;
        Mat h{};
        for (int i = 0; i < 12; ++i) h.m[i / 4][i % 4] = float(j["m"]->items[i].num);
        heads.push_back({j["t_ns"]->num, h});
    }
    handcut::Hands hands;
    HandInput in(k);
    grabthat::Detector det;
    fh_hands_v2_t file{};
    fh_gestures_t g{};
    std::memcpy(g.magic, FH_GESTURES_MAGIC, 8);
    g.version = FH_GESTURES_VERSION;
    bool was[2][2] = {{false, false}, {false, false}};   // [pinch, grip][side]
    uint64_t seq = 0;
    for (size_t n = 0; n < sets.size(); ++n) {
        const Json &j = sets[n];
        if (!j["t_raw_ns"] || !j["hands"]) continue;
        const int64_t t = int64_t(j["t_raw_ns"]->num);
        Mat head = Identity();
        double best = 1e18;
        for (const auto &[ht, hm] : heads)
            if (std::fabs(ht - double(t)) < best) best = std::fabs(ht - double(t)), head = hm;
        std::memset(&file, 0, sizeof file);
        std::memcpy(file.v1.magic, FH_HANDS_MAGIC, 8);
        file.v1.version = 1, file.v1.size = sizeof(fh_hands_t), file.v1.seq = 2 * ++seq;
        file.v1.capture_ns = uint64_t(t), file.v1.publish_ns = uint64_t(t + 30'000'000);
        uint32_t nh = 0, ids[2] = {0, 0};
        float knuck[2][3] = {}, palm[2][3] = {};
        for (const Json &hj : j["hands"]->items) {
            const Json *pts = hj["pub"] ? hj["pub"] : hj["pts"];
            if (!pts || pts->items.size() != 21 || !hj["side"] || nh >= FH_HANDS_MAX_HANDS) continue;
            fh_hand_t &o = file.v1.hands[nh++];
            const bool right = hj["side"]->str == "R";
            o.id = hj["id"] ? uint32_t(hj["id"]->num) : 0, o.confidence = 1;
            o.flags = (right ? FH_HAND_RIGHT : 0) | (hj["nviews"] && hj["nviews"]->num >= 2 ? FH_HAND_STEREO : 0);
            for (int q = 0; q < 21; ++q)
                for (int i = 0; i < 3; ++i) o.pts[q][i] = float(pts->items[q].items[i].num);
            ids[right] = o.id;
            for (int i = 0; i < 3; ++i) {
                knuck[right][i] = (o.pts[5][i] + o.pts[9][i]) / 2;
                palm[right][i] = (o.pts[0][i] + o.pts[5][i] + o.pts[9][i] + o.pts[13][i] + o.pts[17][i]) / 5;
            }
        }
        file.v1.nhands = nh;
        // the gestures: down or not per frame; the counters and points as ft-hands keeps them
        const Json *gj = j["gestures"];
        for (int kind = 0; kind < 2; ++kind)
            for (int side = 0; side < 2; ++side) {
                const Json *arr = gj ? (*gj)[kind ? "grip" : "pinch"] : nullptr;
                const bool down = arr && arr->items.size() == 2 && arr->items[side]["flags"] &&
                                  (int(arr->items[side]["flags"]->num) & FH_PINCH_DOWN);
                fh_pinch_t &o = kind ? g.grip[side] : g.pinch[side];
                o.flags = FH_PINCH_TRACKED | (down ? FH_PINCH_DOWN : 0), o.hand_id = ids[side];
                for (int i = 0; i < 3; ++i) o.point[i] = kind ? palm[side][i] : knuck[side][i];
                if (down && !was[kind][side]) {
                    ++o.begins, o.begin_ns = uint64_t(t);
                    std::memcpy(o.begin_point, o.point, sizeof o.point);
                }
                if (!down && was[kind][side]) ++o.ends, o.end_ns = uint64_t(t);
                was[kind][side] = down;
            }
        g.capture_ns = uint64_t(t), g.publish_ns = uint64_t(t + 30'000'000), g.seq += 2;
        const int64_t next = n + 1 < sets.size() && sets[n + 1]["t_raw_ns"] ? int64_t(sets[n + 1]["t_raw_ns"]->num) : t + 33'000'000;
        bool first = true;
        hands.Feed(nullptr, 0, nullptr, head, t);
        for (int64_t now = t + 30'000'000; now < next + 30'000'000; now += 11'111'111) {
            if (first) hands.Feed(&file, sizeof(fh_hands_t), &g, head, now);
            else hands.Feed(nullptr, 0, nullptr, head, now);
            Input i;
            i.nowNs = now, i.hands = &hands, i.head = head, i.haveHead = true, i.allowed[0] = i.allowed[1] = true;
            for (int s2 = 0; s2 < 2; ++s2) i.grabThatArmed[s2] = det.armed(s2);
            in.Update(i, panels);
            for (const Event &e : in.events()) out.events.push_back(e);
            for (const std::string &l : in.logs()) out.logs.push_back(l);
            if (first) {
                bool allowed[2];
                for (int s2 = 0; s2 < 2; ++s2) allowed[s2] = !in.pressing(s2) && !in.grabbing(s2);
                det.Frame(hands.captureNs(), hands.seen(), hands.captureHead(), allowed);
                for (const auto &f : det.fired()) in.GrabThatFired(f.side), out.fires.push_back(f);
            }
            first = false;
        }
    }
    out.stats = in.stats;
    return true;
}

// The user's "grab that" at a panel already at arm's reach (where the gesture would bring one): it
// still fires, twice; the open hand doesn't poke it, nor the fist grab it.
void TestGrabThatAtNearPanel() {
    const std::string dir = FlagsDir() + "/flag-20261010-150115";
    std::vector<grabthat::Fire> fires;
    double flagT = 0, secs = 0;
    if (!ReplayFlag(dir, fires, &flagT, &secs) || fires.empty()) {
        std::printf("grab that at a near panel: %s not here, skipped\n", dir.c_str());
        return;
    }
    for (double distance : {0.45, 0.40, 0.35}) {
        grabthat::Params gk;
        gk.distance = distance;
        Panel P = TestPanel();
        P.pose = grabthat::Arrival(fires[0], gk), P.width = 0.6, P.height = 0.6 * 0.5625, P.controls.clear();
        FlagRun run;
        FlagThroughHandInput(dir, {P}, run);
        int presses = 0, grabs = 0;
        for (const Event &e : run.events) presses += e.type == Event::Press, grabs += e.type == Event::GrabStart;
        std::printf("grab that at a panel %.2f m ahead (the user's flag): %zu fire(s), %d presses, %d grabs, %d fist(s) "
                    "waited, %d were grab that, near %d of %d frames\n", distance, run.fires.size(), presses, grabs,
                    run.stats.fistsWaited, run.stats.fistsCancelled, run.stats.nearFrames, run.stats.frames);
        if (std::getenv("HANDINPUT_LOG"))
            for (const std::string &l : run.logs) std::printf("    %s\n", l.c_str());
        CHECK(run.fires.size() == 2);
        CHECK(presses == 0 && grabs == 0);
    }
}

// The user's example (2026-10-10 15:01, right hand, twice): fires twice, at the right times.
void TestGrabThatFlag() {
    const std::string dir = FlagsDir() + "/flag-20261010-150115";
    std::vector<grabthat::Fire> fires;
    double flagT = 0, secs = 0;
    if (!ReplayFlag(dir, fires, &flagT, &secs)) {
        std::printf("grab that: %s not here, its replay skipped\n", dir.c_str());
        return;
    }
    std::printf("grab that, the user's flag (%.0f s): %zu fire(s):", secs, fires.size());
    for (const auto &f : fires) std::printf(" %s at %+.2f s", f.side ? "right" : "left", (f.t - flagT) / 1e9);
    std::printf("\n");
    CHECK(fires.size() == 2);
    if (fires.size() == 2) {
        CHECK(fires[0].side == 1 && fires[1].side == 1);
        NEAR((fires[0].t - flagT) / 1e9, -11.4, 0.15);
        NEAR((fires[1].t - flagT) / 1e9, -5.9, 0.15);
    }
}

// --grabthat FLAG_DIR...: the fires in each flag (false fires: flags taken for something else).
int ScanFlags(int n, char **dirs) {
    int total = 0;
    double all = 0;
    for (int i = 0; i < n; ++i) {
        std::vector<grabthat::Fire> fires;
        double flagT = 0, secs = 0;
        int claimed = 0, arms = 0;
        if (!ReplayFlag(dirs[i], fires, &flagT, &secs, &claimed, &arms)) {
            std::printf("%s: can't read\n", dirs[i]);
            continue;
        }
        std::printf("%s: %.0f s, armed %d, claimed %d frames, %zu fire(s)", dirs[i], secs, arms, claimed, fires.size());
        for (const auto &f : fires) std::printf(" %s at %+.2f s", f.side ? "right" : "left", (f.t - flagT) / 1e9);
        std::printf("\n");
        // ...and with near hand input, a panel at arm's reach (0.45 m ahead of the head's first heading,
        // 0.15 m under the eyes): what the hands did to it
        std::vector<Json> poses;
        if (ReadJsonLines(std::string(dirs[i]) + "/hmd.jsonl", poses) && !poses.empty() && poses[0]["m"] &&
            poses[0]["m"]->items.size() == 12) {
            Mat h{};
            for (int q = 0; q < 12; ++q) h.m[q / 4][q % 4] = float(poses[0]["m"]->items[q].num);
            grabthat::Fire f;
            f.head = h, f.fist[1] = h.m[1][3] - 0.15;
            Panel P = TestPanel();
            P.pose = grabthat::Arrival(f, grabthat::Params{}), P.width = 0.6, P.height = 0.6 * 0.5625, P.controls.clear();
            FlagRun run;
            if (FlagThroughHandInput(dirs[i], {P}, run)) {
                int presses = 0, grabs = 0;
                for (const Event &e : run.events) presses += e.type == Event::Press, grabs += e.type == Event::GrabStart;
                std::printf("  with a panel at arm's reach: near %d of %d frames, %d presses (%d by pinch), %d grabs, %zu fire(s)\n",
                            run.stats.nearFrames, run.stats.frames, presses, run.stats.pinchClicks, grabs, run.fires.size());
                for (const std::string &l : run.logs)
                    if (l.find("pressed") != std::string::npos || l.find("took") != std::string::npos) std::printf("    %s\n", l.c_str());
            }
        }
        total += int(fires.size()), all += secs;
    }
    std::printf("%d fire(s) in %.0f s\n", total, all);
    return 0;
}


}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--grabthat")) return ScanFlags(argc - 2, argv + 2);
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
    TestPokeRefusals();
    TestSlowDrift();
    TestPokeNoisy();
    TestApproachFromFront();
    TestGuards();
    TestTyping();
    TestRelease();
    TestDeep();
    TestDrag();
    TestLost();
    TestControlPoke();
    TestNearClaim();
    TestGrabMove();
    TestGrabSmooth();
    TestGrabTurn();
    TestGrabResize();
    TestStretchHands();
    TestHoldThroughLoss();
    TestForgivingZones();
    TestCardPinch();
    TestFistGrab();
    TestV2Ingest();
    TestMatte();
    TestPalmUp();
    TestPalmMenuShows();
    TestPalmMenuPinch();
    TestGrabThatGesture();
    TestGrabThatPlace();
    TestGrabThatFlag();
    TestGrabThatAtNearPanel();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
