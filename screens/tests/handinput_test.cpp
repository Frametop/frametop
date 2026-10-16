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
#include "../softglow.h"
#include "../steampanels.h"

#include <dirent.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
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
    bool thumbOnMiddle = false;        // ...the thumb tip on the middle's (a middle pinch)
    double curlT = -1, curlIndexT = -1;   // ...or in between (0..2: open, half, fist), a hand closing
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
    for (int f = 0; f < 3 && h.curl && h.curlT < 0; ++f) std::memcpy(out[10 + 4 * f], h.curl == 1 ? half[f] : fist[f], sizeof half[f]);
    static const float halfIndex[3][3] = {{0.027f, 0.02f, -0.13f}, {0.027f, 0.035f, -0.135f}, {0.027f, 0.04f, -0.13f}};
    static const float fistIndex[3][3] = {{0.027f, 0.03f, -0.11f}, {0.027f, 0.04f, -0.09f}, {0.027f, 0.03f, -0.07f}};
    if (h.curlIndex && h.curlIndexT < 0) std::memcpy(out[6], h.curlIndex == 1 ? halfIndex : fistIndex, sizeof halfIndex);
    // in between: open (as base) to half curled to a fist
    auto mix = [](const float open[3][3], const float a[3][3], const float b[3][3], double t, float dst[][3]) {
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const double x = t <= 1 ? open[j][i] + (a[j][i] - open[j][i]) * t : a[j][i] + (b[j][i] - a[j][i]) * (t - 1);
                dst[j][i] = float(x);
            }
    };
    if (h.curlT >= 0)
        for (int f = 0; f < 3; ++f) mix(&base[10 + 4 * f], half[f], fist[f], std::clamp(h.curlT, 0.0, 2.0), &out[10 + 4 * f]);
    if (h.curlIndexT >= 0) mix(&base[6], halfIndex, fistIndex, std::clamp(h.curlIndexT, 0.0, 2.0), &out[6]);
    if (h.thumbOnMiddle) out[4][0] = out[12][0] + 0.008f, out[4][1] = out[12][1], out[4][2] = out[12][2];
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
    std::vector<HandInput::Act> acts;
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
            for (const HandInput::Act &a : in.acts()) acts.push_back(a);
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
    // a floating window: the title bar, and every corner resizes (2026-10-10; the bottom right only before)
    Panel f = TestPanel();
    f.floating = true, f.cropW = 1600, f.cropH = 900, f.titleH = 60;
    CHECK(GrabZone(f, {0.1, 0.27, 0.0}, k, &sx, &sy, &e) == Zone::Title && e <= 0);
    CHECK(GrabZone(f, {-0.5, 0.28, 0.0}, k, &sx, &sy, &e) == Zone::Corner && sx == -1 && sy == 1);
    CHECK(GrabZone(f, {-0.4, 0.27, 0.0}, k, &sx, &sy, &e) == Zone::Title);   // the title bar next to it
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
    // a panel turned 20 degrees off facing keeps that turn (seen from the point that faces the head:
    // held this near by its corner, between the corner and its centre, CarryAnchor)
    const Mat turned = RotY(20 * M_PI / 180, 0, 0, -0.5);
    double a0[3], a1[3];
    CarryAnchor(turned, grab, head, a0);
    const double c1 = (turned.m[0][2] * -a0[0] + turned.m[1][2] * -a0[1] + turned.m[2][2] * -a0[2]) / std::hypot(a0[0], a0[1], a0[2]);
    q = CarryPose(turned, head, grab, side, head, turned);
    for (int k = 0; k < 4; ++k) q = CarryPose(turned, head, grab, side, head, q);
    CarryAnchor(q, grab, head, a1);
    const double c2 = (q.m[0][2] * -a1[0] + q.m[1][2] * -a1[1] + q.m[2][2] * -a1[2]) / std::hypot(a1[0], a1[1], a1[2]);
    NEAR(std::acos(std::clamp(c2, -1.0, 1.0)) * 180 / M_PI, std::acos(std::clamp(c1, -1.0, 1.0)) * 180 / M_PI, 0.5);
    std::printf("carry: a panel 20 deg off facing (%.1f from where it faces the head from), held by its corner 0.5 m away, carried 90 deg round: %.1f deg off "
                "(from its centre %.1f)\n", std::acos(std::clamp(c1, -1.0, 1.0)) * 180 / M_PI, std::acos(std::clamp(c2, -1.0, 1.0)) * 180 / M_PI,
                std::acos(std::clamp(double(q.m[0][2] * -q.m[0][3] + q.m[2][2] * -q.m[2][3]) / std::hypot(q.m[0][3], q.m[2][3]), -1.0, 1.0)) * 180 / M_PI);
    // ...and from 2 m, the centre faces (the anchor is the centre there)
    {
        const Mat far0 = RotY(20 * M_PI / 180, 0, 0, -2.0);
        const double farGrab[3] = {0.3, -0.25, 0}, farSide[3] = {2.0, -0.25, 0.0};
        Mat f = far0;
        for (int k = 0; k < 5; ++k) f = CarryPose(far0, head, farGrab, farSide, head, f);
        const double c3 = (f.m[0][2] * -f.m[0][3] + f.m[2][2] * -f.m[2][3]) / std::hypot(f.m[0][3], f.m[2][3]);
        NEAR(std::acos(std::clamp(c3, -1.0, 1.0)) * 180 / M_PI, 20, 0.5);
    }
    // held by its edge near the eyes, moved about a little each tick: no spin, no flip (the third live
    // test's replays: 0.6 m panels held by an edge at 0.3-0.45 m turned 50-180 deg a tick)
    {
        const double edge[3] = {-0.3, 0, 0};
        const Mat st = At(0.3, -0.15, -0.35);
        Mat c = st;
        double worst = 0;
        for (int k = 0; k < 270; ++k) {
            const double tt = k / 90.0;
            const double hp[3] = {0.0 + 0.05 * std::sin(tt * 3), -0.15 + 0.04 * std::sin(tt * 5), -0.35 + 0.1 * std::sin(tt * 2)};
            const Mat n = CarryPose(st, head, edge, hp, head, c);
            double R[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) R[i][j] = n.m[i][0] * c.m[j][0] + n.m[i][1] * c.m[j][1] + n.m[i][2] * c.m[j][2];
            worst = std::max(worst, std::acos(std::clamp((R[0][0] + R[1][1] + R[2][2] - 1) / 2, -1.0, 1.0)) * 180 / M_PI);
            c = n;
        }
        std::printf("carry: a 0.6 m panel held by its edge 0.35 m ahead, the hand wandering: at most %.2f deg a tick\n", worst);
        CHECK(worst < 2);
    }
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

// A testing session's events (hands/include/fh_events.h): a press and its release, and a poke the
// middle finger held back, as what they were and how they went, on the screen the log names.
void TestActs() {
    auto has = [](const Rig &r, const char *kind, const char *outcome) {
        return std::any_of(r.acts.begin(), r.acts.end(), [&](const HandInput::Act &a) {
            return !std::strcmp(a.kind, kind) && !std::strcmp(a.outcome, outcome) && a.side == 1 && a.panel == 1 && !a.text.empty();
        });
    };
    Rig r;
    for (int mm = 100; mm >= -15; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
    for (int mm = -15; mm <= 50; mm += 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
    CHECK(has(r, "poke", "done") && has(r, "poke", "end") && !has(r, "poke", "refused"));
    CHECK(r.acts.size() <= r.logs.size());   // each one is also a log line
    Rig n;
    for (int mm = 60; mm >= -15; mm -= 10) {
        SynthHand h = TipAt(0.1, 0.05, mm / 1000.0);
        h.middleTip = 0.012, h.pitchDeg = 25;
        n.Frame({h});
    }
    for (int mm = -5; mm <= 45; mm += 10) {
        SynthHand h = TipAt(0.1, 0.05, mm / 1000.0);
        h.middleTip = 0.012, h.pitchDeg = 25;
        n.Frame({h});
    }
    CHECK(has(n, "poke", "refused") && !has(n, "poke", "done"));
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
        for (int mm = -5; mm <= 45; mm += 10) {   // and off again (two fingers about level click on the lift)
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
// middleTip: the middle finger nearly straight beside the index (SynthHand::middleTip), NaN curled.
int Poke(Rig &r, double u, double v, double pitch, double yaw, double extraX = 0, double middleTip = NAN) {
    const int before = r.Count(Event::Press);
    for (int mm = 60; mm >= -15; mm -= 10) {
        double at[3];
        FromSurface(r.panels[0], u, v, mm / 1000.0, at);
        SynthHand h;
        for (int i = 0; i < 3; ++i) h.tip[i] = at[i];
        h.tip[0] += extraX;
        h.pitchDeg = pitch, h.yawDeg = yaw, h.middleTip = middleTip;
        r.Frame({h});
    }
    for (int k = 0; k < 6; ++k) {
        double at[3];
        FromSurface(r.panels[0], u, v, 0.02 + 0.01 * k, at);
        SynthHand h;
        for (int i = 0; i < 3; ++i) h.tip[i] = at[i];
        h.tip[0] += extraX;
        h.pitchDeg = pitch, h.yawDeg = yaw, h.middleTip = middleTip;
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
    // reached for 0.45 m out, a poke; with the hand resting by the body (0.2 m out), none from a
    // typing hand (its middle finger about as straight as the index), but a pointing hand pokes it
    // (a panel pulled in close and low: its bar's buttons, 2026-10-14)
    {
        Rig r;
        r.panels = {Tilted(50, -0.45, -0.45)};
        CHECK(Poke(r, 0, 0, -50, 0) == 1);
    }
    {
        Rig r;
        r.panels = {Tilted(50, -0.45, -0.22)};
        r.in.params().twoFingerScroll = false;   // (two fingers level: their own tap, not a poke)
        CHECK(Poke(r, 0, 0, -50, 0, 0, 0.0) == 0 && r.in.stats.heldResting > 0);
        CHECK(Poke(r, 0.1, 0, -50, 0) == 1);
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
    r.Frame({TipAt(0.03, 0, -0.012)});   // 3 cm at once: still a tap (Params::tapMs), locked
    CHECK(r.Count(Event::Motion) == motions);
    for (int k = 0; k < 8; ++k) r.Frame({TipAt(0.01, 0, -0.012)});   // held a while, 1 cm: still locked
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
    for (int k = 0; k < 5; ++k) r.Frame({gone});   // a short gap: still pressed (Params::lostMs)
    CHECK(r.in.pressing(1) && r.Count(Event::Release) == 0);
    for (int k = 0; k < 10; ++k) r.Frame({gone});   // a press isn't held through a loss (2026-10-11: stuck taps)
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
        double q[3];   // (from the point that faces the head: CarryAnchor; the head is at the origin)
        const double gl[3] = {0.05, barY, 0}, origin[3] = {0, 0, 0};
        CarryAnchor(p, gl, origin, q);
        const double c = -(p.m[0][2] * q[0] + p.m[1][2] * q[1] + p.m[2][2] * q[2]) / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
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

// Each corner, pulled 10 cm out along the diagonal (the panel following what the carries say, as
// vr.cpp applies them): 1.1 m wide, the opposite corner where it was (2026-10-10: it stayed centred
// before, and only the bottom right corner worked by laser).
void TestGrabResizeCorners() {
    for (int sx : {1, -1})
        for (int sy : {-1, 1}) {
            Rig r;
            r.panels[0].controls.clear();
            Panel &P = r.panels[0];
            const double a = P.height / P.width;
            double opp0[3];
            FromSurface(P, -sx * P.width / 2, -sy * P.height / 2, 0, opp0);
            const double cu = sx * 0.5, cv = sy * 0.28125;
            for (int k = 0; k < 5; ++k) r.Frame({PinchAt(cu, cv, 0.01)});
            const double at[3] = {cu, cv, -0.49};
            r.synth.Pinch(1, true, r.t, at, 1);
            r.Frame({PinchAt(cu, cv, 0.01)});
            CHECK(r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Corner &&
                  r.Last(Event::GrabStart)->sx == sx && r.Last(Event::GrabStart)->sy == sy);
            for (int k = 1; k <= 30; ++k) {
                const double f = std::min(1.0, k / 10.0) * 0.1 / std::sqrt(1 + a * a);   // 10 cm along the diagonal
                r.Frame({PinchAt(cu + sx * f, cv + sy * a * f, 0.01)});
                if (!r.carries.empty() && r.carries[0].kind == Carry::Resize && r.carries[0].width > 0)
                    P.pose = r.carries[0].pose, P.width = r.carries[0].width, P.height = P.width * a;
            }
            double opp[3];
            FromSurface(P, -sx * P.width / 2, -sy * P.height / 2, 0, opp);
            const double moved = std::hypot(std::hypot(opp[0] - opp0[0], opp[1] - opp0[1]), opp[2] - opp0[2]);
            std::printf("resize by the %s %s corner, 10 cm out: %.3f m wide (1.0 + 0.1 along the diagonal, %.3f), the "
                        "opposite corner moved %.1f mm\n", sy > 0 ? "top" : "bottom", sx > 0 ? "right" : "left", P.width,
                        1.0 + 0.1 / std::sqrt(1 + a * a), moved * 1000);
            NEAR(P.width, 1.0 + 0.1 / std::sqrt(1 + a * a), 0.02);
            CHECK(moved < 0.005);
            // the handle there shows held
            CHECK(r.in.corners().count(0) && r.in.corners().at(0).held && r.in.corners().at(0).sx == sx && r.in.corners().at(0).sy == sy);
        }
    {
        // the same math vr.cpp's laser resize uses, on a curved screen (2 m radius): from the top left
        // corner, the bottom right stays on the cylinder where it was; clamped at the smallest width
        Panel P = TestPanel(-2.0);
        P.curve = 2.0, P.width = 2.0, P.height = 1.125;
        const double a = P.height / P.width;
        double opp0[3], opp[3], u, v;
        FromSurface(P, P.width / 2, -P.height / 2, 0, opp0);
        const double w = ResizeFromCorner(P.width, a, -1, 1, -1.2, 0.65, 0.15, 12, &u, &v);
        Panel Q = P;
        Q.pose = SurfacePose(P.pose, P.curve, u, v), Q.width = w, Q.height = w * a;
        FromSurface(Q, Q.width / 2, -Q.height / 2, 0, opp);
        NEAR(std::hypot(std::hypot(opp[0] - opp0[0], opp[1] - opp0[1]), opp[2] - opp0[2]), 0, 0.002);
        CHECK(w > 2.1 && w < 2.3);
        const double small = ResizeFromCorner(P.width, a, -1, 1, 0.9, -0.5, 0.15, 12, &u, &v);
        NEAR(small, 0.15, 1e-9);
        NEAR(u, 1.0 - 0.075, 1e-9);   // the bottom right corner kept: the centre half the new width left of it
    }
    {
        // the resize handle moved to the top left (vr.cpp put it where a laser aimed): a pinch on it
        // resizes from the top left
        Panel P = TestPanel();
        for (Spot &c : P.controls)
            if (c.kind == Control::Tab) c.u = -c.u, c.v = -c.v;
        int sx = 0, sy = 0;
        double e = 1;
        CHECK(GrabZone(P, {-0.52, 0.30, 0.0}, Params(), &sx, &sy, &e) == Zone::Tab && sx == -1 && sy == 1 && e <= 0);
    }
    {
        // a pinch near a corner (not yet in it): the handle there, dimmer
        Rig r;
        r.panels[0].controls.clear();
        r.Frame({PinchAt(-0.5 - 0.05, 0.28125 + 0.05, 0.01)});
        r.Frame({PinchAt(-0.5 - 0.05, 0.28125 + 0.05, 0.01)});
        const auto &c = r.in.corners();
        CHECK(c.count(0) && c.at(0).sx == -1 && c.at(0).sy == 1 && !c.at(0).held && c.at(0).closeness > 0 && c.at(0).closeness < 1);
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
    // (the panel isn't moved here: the corner 10 cm out from where it was, the opposite one kept)
    if (!r.carries.empty()) NEAR(r.carries[0].width, 1.1, 0.03);
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

// Letting go without a nudge (Params::openFreeze, rewindMs): a carry by the bar, still; then the fingers
// open over a few frames while the hand sags 2 cm, and the pinch ends: the panel stays where it was held.
void TestLetGoStill() {
    for (bool freeze : {true, false}) {
        Rig r;
        if (!freeze) r.in.params().openFreeze = 0, r.in.params().rewindMs = 0;
        r.in.params().grabRotate = false;
        const double barY = r.panels[0].controls[0].v;
        auto hand = [&](double x, double y, double open) {
            SynthHand h = KnucklesAt(x, barY + y, 0.02);
            h.open = open;
            return h;
        };
        for (int k = 0; k < 5; ++k) r.Frame({hand(0, 0, 0)});
        const double at[3] = {0, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({hand(0, 0, 0)});
        for (int k = 1; k <= 30; ++k) {   // carried 10 cm right, then held still
            r.Frame({hand(0.1 * std::min(1.0, k / 20.0), 0, 0)});
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
        }
        const double held[2] = {r.panels[0].pose.m[0][3], r.panels[0].pose.m[1][3]};
        for (int k = 1; k <= 6; ++k) {    // the let-go: fingers opening, the hand sagging 2 cm
            r.Frame({hand(0.1, -0.02 * k / 6.0, k / 6.0)});
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
        }
        r.synth.Pinch(1, false, r.t, at, 1);
        for (int k = 0; k < 3; ++k) {
            r.Frame({hand(0.1, -0.02, 1)});
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
        }
        if (const auto e = r.Last(Event::Settle)) r.panels[0].pose = e->to;
        const double off = std::hypot(r.panels[0].pose.m[0][3] - held[0], r.panels[0].pose.m[1][3] - held[1]);
        std::printf("let go %s: the panel %.1f mm from where it was held (%d freezes, %d rewinds)\n",
                    freeze ? "with the freeze and rewind" : "without", off * 1000, r.in.stats.releaseFreezes, r.in.stats.rewinds);
        if (freeze) CHECK(off < 0.004);
        CHECK(r.Count(Event::GrabEnd) == 1);
    }
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
        c.Frame({o});   // the fingers seen apart: let go at once (Params::pinchApart, 2026-10-11: stuck clicks)
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
        {0.1, -0.30, 0.01, Zone::Bar, "by the bar"},
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
    // the cutout buffer's glow before the pinch (HANDS_EDGE_GLOW=0): amber at a corner (resize), blue by
    // an edge (move)
    auto glowColour = [](double u, double v) {
        Rig r;
        r.in.params().edgeGlow = false;
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
        r.in.params().fistResizeOnly = false;   // (the fist's carry; resize-only: TestFistResizeOnly)
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

// Throws (2026-10-10). An open hand, palm toward the panel and fingers up, at x, y, z (its palm).
SynthHand OpenAt(double x, double y, double z, double jitter = 0) {
    SynthHand h;
    h.palm = true, h.palmAt[0] = x, h.palmAt[1] = y, h.palmAt[2] = z, h.rollDeg = 180, h.tiltDeg = 90, h.jitter = jitter;
    return h;
}

// A hand moving from `a` to `b` (palm or tip, as made) at `speed` m/s, frames at 30 Hz; then held.
template <typename Make>
void Sweep(Rig &r, const double a[3], const double b[3], double speed, Make make, int hold = 10) {
    const double d = std::hypot(std::hypot(b[0] - a[0], b[1] - a[1]), b[2] - a[2]);
    const int n = std::max(1, int(std::ceil(d / (speed / 30))));
    for (int k = 0; k <= n + hold; ++k) {
        const double f = std::min(1.0, double(k) / n);
        r.Frame({make(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f)});
    }
}

void TestFlick() {
    struct Case { const char *what; int hand; double from[3], to[3], speed; double jitter; bool throws; };
    // the panel 0.5 m ahead (z -0.5), the head at the origin; hand 0: curled, opening as it goes out
    // (a flick), 2: open throughout; 1: pointing (the tip)
    const Case cases[] = {
        {"a hand flicked out through it, the fingers opening, 1.5 m/s", 0, {0.05, 0, -0.44}, {0.05, 0, -0.76}, 1.5, 0, true},
        {"...with 3 mm of noise", 0, {0.05, 0, -0.44}, {0.05, 0, -0.76}, 1.5, 0.003, true},
        {"...at 0.8 m/s (a push)", 0, {0.05, 0, -0.44}, {0.05, 0, -0.76}, 0.8, 0, false},
        // (a push since the third live test: TestPush; here with push off)
        {"an open hand pushed out through it, 1.5 m/s (open throughout)", 2, {0.05, 0, -0.44}, {0.05, 0, -0.76}, 1.5, 0, false},
        {"a hand waved across it, the fingers opening, 1.5 m/s", 0, {-0.3, 0, -0.45}, {0.3, 0, -0.45}, 1.5, 0, false},
        {"a hand pulled back from it, the fingers opening, 1.5 m/s", 0, {0.05, 0, -0.46}, {0.05, 0, -0.16}, 1.5, 0, false},
        {"a hand flicked out 30 cm away from it", 0, {0.05, 0, -0.14}, {0.05, 0, -0.30}, 1.5, 0, false},
        {"a pointing hand jabbed through it, 1.5 m/s", 1, {0.05, 0, -0.42}, {0.05, 0, -0.62}, 1.5, 0, false},
        {"a poke, 0.3 m/s", 1, {0.05, 0, -0.42}, {0.05, 0, -0.515}, 0.3, 0, false},
    };
    for (const Case &c : cases) {
        Rig r;
        r.panels[0].controls.clear();
        r.in.params().push = false;   // (the flick alone)
        auto make = [&](double x, double y, double z) {
            if (c.hand == 1) {
                SynthHand h = TipAt(x, y, z + 0.5);
                h.jitter = c.jitter;
                return h;
            }
            SynthHand h = OpenAt(x, y, z, c.jitter);
            // curled until a third of the way out, then open
            const double gone = std::hypot(std::hypot(x - c.from[0], y - c.from[1]), z - c.from[2]);
            const double all = std::hypot(std::hypot(c.to[0] - c.from[0], c.to[1] - c.from[1]), c.to[2] - c.from[2]);
            if (c.hand == 0 && gone < all / 3) h.curl = h.curlIndex = 1;
            return h;
        };
        for (int k = 0; k < 8; ++k) r.Frame({make(c.from[0], c.from[1], c.from[2])});
        Sweep(r, c.from, c.to, c.speed, make);
        const auto t = r.Last(Event::Throw);
        std::printf("flick: %s: %s%s\n", c.what, t ? "thrown" : "stays", r.Count(Event::Press) ? ", a press" : "");
        CHECK(bool(t) == c.throws);
        if (c.throws) {
            CHECK(r.Count(Event::Press) == 0 && t->flick);
            if (t) NEAR(t->to.m[2][3], -3.0, 0.01), NEAR(t->to.m[0][3], 0.0, 0.01), NEAR(t->to.m[2][2], 1.0, 0.01);
        }
        if (c.hand == 1 && c.speed < 1) CHECK(r.Count(Event::Press) == 1);
    }
    {
        // the hand lost for 6 frames at the panel, back 25 cm out and moving on out (opening): its
        // velocity and travel don't reach back over the gap: no flick until it has really flicked
        Rig r;
        r.in.params().push = false;   // (the flick alone)
        SynthHand gone;
        gone.present = false;
        auto curled = [](double z) {
            SynthHand h = OpenAt(0.05, 0, z);
            h.curl = h.curlIndex = 1;
            return h;
        };
        for (int k = 0; k < 8; ++k) r.Frame({curled(-0.44)});
        for (int k = 0; k < 3; ++k) r.Frame({gone});
        for (int k = 0; k < 3; ++k) r.Frame({OpenAt(0.05, 0, -0.60 - 0.05 * k)});   // 1.5 m/s, 10 cm since it's back
        CHECK(r.Count(Event::Throw) == 0);
        r.in.params().flickTravel = 0.05;   // (it is a flick from where it came back, once that's far enough)
        r.Frame({OpenAt(0.05, 0, -0.75)});
        CHECK(r.Count(Event::Throw) == 0);   // ...but it didn't start from the panel (it came back 10 cm behind it)
    }
    {
        // HANDS_FLICK_OPENING=0: an open hand pushed out flicks too; HANDS_FLICK=0: nothing
        const double a[3] = {0.05, 0, -0.44}, b[3] = {0.05, 0, -0.76};
        Rig r;
        r.in.params().flickOpening = 0, r.in.params().push = false;
        for (int k = 0; k < 8; ++k) r.Frame({OpenAt(a[0], a[1], a[2])});
        Sweep(r, a, b, 1.5, [](double x, double y, double z) { return OpenAt(x, y, z); });
        CHECK(r.Count(Event::Throw) == 1);
        Rig q;
        q.in.params().flick = false, q.in.params().flickOpening = 0, q.in.params().push = false;
        for (int k = 0; k < 8; ++k) q.Frame({OpenAt(a[0], a[1], a[2])});
        Sweep(q, a, b, 1.5, [](double x, double y, double z) { return OpenAt(x, y, z); });
        CHECK(q.Count(Event::Throw) == 0);
    }
}

// The card throw: a card carried, let go moving out fast (or slowly, or sideways, or just after the
// hand came back from a gap).
void TestCardThrow() {
    const double v = -0.28125 + 0.01;
    // slow: the last frames before the let-go at this speed instead (a hand slowing as it lets go, or
    // one that came to rest), how many
    struct Case { const char *what; double dir[3], speed; int gap; bool throws; double slow = 0; int slowFrames = 0; };
    const Case cases[] = {
        {"let go moving straight out at 1.5 m/s", {0, 0, -1}, 1.5, 0, true},
        {"let go moving out and up at 1.5 m/s", {0, 0.5, -0.866}, 1.5, 0, true},
        {"let go moving out at 0.5 m/s", {0, 0, -1}, 0.5, 0, true},
        {"let go moving out at 0.3 m/s", {0, 0, -1}, 0.3, 0, false},
        {"let go moving sideways at 1.5 m/s", {-1, 0, 0}, 1.5, 0, false},
        {"let go moving in at 1.5 m/s", {0, 0, 1}, 1.5, 0, false},
        {"let go the frame it came back from a 6-frame gap", {0, 0, -1}, 1.5, 6, false},
        {"out at 1 m/s, slowing to 0.15 over the last 3 frames", {0, 0, -1}, 1.0, 0, true, 0.15, 3},
        {"out at 1 m/s, then at rest 8 frames before the let-go", {0, 0, -1}, 1.0, 0, false, 0, 8},
    };
    for (const Case &c : cases) {
        Rig r;
        r.panels[0].controls.clear();
        double x = 0.1, y = v, z = 0;
        auto hand = [&](double open) {
            SynthHand h = CardHand(x, y, open);
            h.contact[2] += z;
            return h;
        };
        for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({hand(o)});
        const double at[3] = {0.1, v, -0.5};
        r.Frame({hand(0)});
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({hand(0)});
        CHECK(r.Last(Event::GrabStart) && r.Last(Event::GrabStart)->zone == Zone::Card);
        for (int k = 0; k < 10; ++k) r.Frame({hand(0)});
        // the throw: 8 frames along dir at speed (the last ones with the hand back after a gap), and
        // slowFrames more at slow
        for (int k = 1; k <= 8 + c.slowFrames; ++k) {
            const double sp = k > 8 ? c.slow : c.speed;
            x += c.dir[0] * sp / 30, y += c.dir[1] * sp / 30, z += c.dir[2] * sp / 30;
            SynthHand h = hand(0);
            if (c.gap && k > 8 - 1 - c.gap && k < 8) h.present = false;
            if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
            r.Frame({h});
        }
        r.synth.Pinch(1, false, r.t, at, 1);
        for (int k = 0; k < 4; ++k) {
            const double sp = c.slowFrames ? c.slow : c.speed;
            x += c.dir[0] * sp / 30, y += c.dir[1] * sp / 30, z += c.dir[2] * sp / 30;
            r.Frame({hand(1.0)});
        }
        const auto t = r.Last(Event::Throw);
        std::printf("card throw: %s: %s\n", c.what, t ? "thrown" : "stays");
        CHECK(r.Count(Event::GrabEnd) == 1);
        CHECK(bool(t) == c.throws);
        if (t) {
            const double d = std::hypot(std::hypot(t->to.m[0][3], t->to.m[1][3]), t->to.m[2][3]);
            NEAR(d, 3.0, 0.01);
            CHECK(!t->flick);
            if (c.dir[1] > 0) CHECK(t->to.m[1][3] > 0.5);   // up along the throw
        }
    }
}

// A remote display (vr.cpp's index 101 and up, "frametop.remote.N") is a panel like any other here:
// pokes, the card, corner resize and throws all name it.
void TestRemoteDisplay() {
    {
        Rig r;
        r.panels[0].index = 102;
        for (int mm = 60; mm >= -15; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Last(Event::Press) && r.Last(Event::Press)->panel == 102);
    }
    {
        Rig r;
        r.panels[0].index = 102, r.panels[0].controls.clear();
        const double cu = -0.5, cv = 0.28125;
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(cu, cv, 0.01)});
        const double at[3] = {cu, cv, -0.49};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(cu, cv, 0.01)});
        CHECK(r.Last(Event::GrabStart) && r.Last(Event::GrabStart)->panel == 102 && r.Last(Event::GrabStart)->zone == Zone::Corner);
        for (int k = 1; k <= 10; ++k) r.Frame({PinchAt(cu - 0.005 * k, cv + 0.003 * k, 0.01)});
        CHECK(!r.carries.empty() && r.carries[0].panel == 102 && r.carries[0].kind == Carry::Resize && r.carries[0].width > 1.0);
        CHECK(r.in.corners().count(102) && r.in.corners().at(102).held);
    }
    {
        Rig r;
        r.panels[0].index = 102, r.panels[0].controls.clear();
        const double a[3] = {0.05, 0, -0.44}, b[3] = {0.05, 0, -0.76};
        for (int k = 0; k < 8; ++k) {
            SynthHand h = OpenAt(a[0], a[1], a[2]);
            h.curl = h.curlIndex = 1;
            r.Frame({h});
        }
        Sweep(r, a, b, 1.5, [](double x, double y, double z) { return OpenAt(x, y, z); });
        CHECK(r.Last(Event::Throw) && r.Last(Event::Throw)->panel == 102);
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
        std::printf("%s panel %.2f m ahead: near %4.1f%% of frames, %3d presses (%.1f/min), %d grabs, %d flicks, %d pushes; held back: %d one view, "
                    "%d bent, %d not nearest, %d pinching, %d busy, %d resting, %d askew, %d slow\n",
                    low ? "low " : "eye ", z, 100.0 * r.nearFrames / std::max(1, r.frames), r.presses, r.presses / secs * 60, r.grabs,
                    r.stats.flicks, r.stats.pushes,
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
        // turned a little away from the face (about 50 deg off it): only once up
        sh.curl = 0, sh.tiltDeg = 5;
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
    bool allowed[2] = {true, true}, busy[2] = {false, false}, atPanel[2] = {false, false};
    std::vector<int> toggles;   // the side of each toggle
    void Frame(const std::vector<SynthHand> &h, int ticks = 3) {
        synth.Write(h, t);
        for (int k = 0; k < ticks; ++k) {
            const int64_t now = t + 30'000'000 + k * 11'111'111;
            if (k == 0) hands.Feed(&synth.file, sizeof(fh_hands_t), &synth.g, Identity(), now);
            else hands.Feed(nullptr, 0, nullptr, Identity(), now);
            palmmenu::Input i;
            i.nowNs = now, i.hands = &hands, i.head = Identity(), i.haveHead = true;
            for (int s = 0; s < 2; ++s) i.allowed[s] = allowed[s], i.busy[s] = busy[s], i.atPanel[s] = atPanel[s];
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
    {   // a middle pinch from a hand held out (a ray's right click, the other two curled with it): the
        // pointer helper's, not claimed (2026-10-15: the user's right clicks were "left to ft-screens")
        auto closing = [](bool middlePinch) {
            GrabRig r;
            for (int k = 0; k < 10; ++k) r.Frame({StopHand(0.1, -0.15, -0.42)});
            for (int k = 0; k < 6; ++k) {
                SynthHand h = StopHand(0.1, -0.15, -0.42, 0, 1);
                h.thumbOnMiddle = middlePinch;
                r.Frame({h});
            }
            return r.claimed[1];
        };
        const int fist = closing(false), pinch = closing(true);
        std::printf("grab that: the three curled from a hand held out claim %d frames, with the thumb on the middle %d\n",
                    fist, pinch);
        CHECK(fist >= 2 && pinch == 0);
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
    k.aimPalm = false;   // (these are the line from the eyes through the palm; the palm's own is below)
    grabthat::Fire f;
    f.head = Identity();
    auto aim = [&](double x, double y, double z) {
        f.palm[0] = x, f.palm[1] = y, f.palm[2] = z;
        return grabthat::Pick(panels, f, k);
    };
    CHECK(aim(0.05, -0.02, -0.4) == 0);
    CHECK(aim(-0.28, 0, -0.28) == 1);    // 45 degrees left
    CHECK(aim(0.28, 0.04, -0.28) == 2);  // the curved one, 45 degrees right
    CHECK(aim(0.5, 0.3, -0.05) == -1);   // nothing that way (off to the right and up): nothing (the fourth live test)
    {
        grabthat::Params cone = k;        // ...with the head's cone (before it): where the head points
        cone.headCone = 20;
        f.palm[0] = 0.5, f.palm[1] = 0.3, f.palm[2] = -0.05;
        CHECK(grabthat::Pick(panels, f, cone) == 0);
        f.head = RotY(-M_PI / 2, 0, 0, 0);    // looking right, palm at nothing: nothing within 20 deg
        f.palm[0] = 0, f.palm[1] = -0.4, f.palm[2] = 0.3;
        CHECK(grabthat::Pick(panels, f, cone) == -1);
        f.head = Identity();
    }
    // past the big one's left edge: 2 cm (0.6 deg) takes it (6 deg since the fifth live test); with no
    // margin (the fourth) nothing; 10 deg past it, nothing
    CHECK(aim(-(0.8 + 0.02) * 0.4 / 2, 0, -0.4) == 0);
    CHECK(aim(-(0.8 - 0.02) * 0.4 / 2, 0, -0.4) == 0);
    {
        grabthat::Params exact = k;
        exact.aimMarginDeg = 0;
        f.palm[0] = -(0.8 + 0.02) * 0.4 / 2, f.palm[1] = 0, f.palm[2] = -0.4;
        grabthat::Closest c;
        CHECK(grabthat::Pick(panels, f, exact, &c) == -1 && c.index == 0);
        NEAR(c.deg, (std::atan(0.82 / 2) - std::atan(0.8 / 2)) * 180 / M_PI, 0.05);
        const double a = std::atan(0.45 / 2) + 10 * M_PI / 180;   // 10 deg over its top edge
        CHECK(aim(0, 0.4 * std::tan(a), -0.4) == -1);
    }
    {   // a panel within arm's reach (0.8 m of the eyes) isn't taken: the one behind it is
        std::vector<Target> two = panels;
        two.push_back(Target{});
        two.back().index = 7, two.back().pose = At(0, -0.1, -0.45), two.back().width = 0.6, two.back().height = 0.34;
        f.palm[0] = 0.02, f.palm[1] = -0.02, f.palm[2] = -0.4;
        CHECK(grabthat::Pick(two, f, k) == 0);
        grabthat::Params old = k;
        old.nearest = 0;   // (before: the near one)
        CHECK(grabthat::Pick(two, f, old) == 7);
    }
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
    // a panel thrown 3 m out (handinput's throws, 2026-10-10), 0.6 m wide: the open palm's line passing
    // 14 cm off its edge still finds it (3 deg seen from the eyes; 10 cm alone missed it), and it comes
    {
        std::vector<Target> far(1);
        far[0].index = 5, far[0].pose = At(0, 0, -3), far[0].width = 0.6, far[0].height = 0.34;
        grabthat::Fire g;
        g.head = Identity();
        const double off = 0.30 + 0.14;   // at 3 m
        g.palm[0] = off * 0.4 / 3, g.palm[1] = 0, g.palm[2] = -0.4;
        g.head = RotY(M_PI / 2, 0, 0, 0);   // looking left: the head's cone can't pick it, only the palm's line
        grabthat::Params wide = k;          // (with the margin it had before the fourth live test)
        wide.aimMargin = 0.10, wide.aimMarginDeg = 3;
        CHECK(grabthat::Pick(far, g, wide) == 5);
        CHECK(grabthat::Pick(far, g, k) == 5);    // (2.7 deg: within 6 since the fifth live test)
        grabthat::Params exact = k;
        exact.aimMarginDeg = 0, exact.smallDeg = 0;
        CHECK(grabthat::Pick(far, g, exact) == -1);   // the fourth live test's: the line has to meet it
        g.palm[0] = 0.25 * 0.4 / 3;                   // ...on it: it comes
        CHECK(grabthat::Pick(far, g, exact) == 5);
        CHECK(grabthat::Decide(false, far[0].pose, Identity(), k) == Outcome::Bring);
    }
    {   // the palm's own line (2026-10-13, "the glow should be where the palm is facing"): a palm held out
        // ahead takes the panel it faces, not the one the line from the eyes through it meets
        grabthat::Params pk;
        grabthat::Fire g;
        g.head = Identity(), g.palm[0] = 0.05, g.palm[1] = -0.05, g.palm[2] = -0.4;
        auto facing = [&](double x, double y, double z) {   // the palm facing toward x, y, z
            const double d[3] = {x - g.palm[0], y - g.palm[1], z - g.palm[2]};
            const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int i = 0; i < 3; ++i) g.normal[i] = d[i] / l;
            return grabthat::Pick(panels, g, pk);
        };
        CHECK(facing(-1.4, 0, -1.4) == 1);     // turned to the one 45 degrees left
        CHECK(facing(0.05, -0.05, -2) == 0);   // straight out: the one ahead
        CHECK(facing(1.5, 0.2, -1.5) == 2);    // the curved one on the right
        CHECK(facing(0.05, -2, -0.6) == -1);   // palm down: nothing
        CHECK(facing(-2, 0, 0.5) == -1);       // to the left, past them all: nothing
        g.normal[0] = g.normal[1] = g.normal[2] = 0;
        CHECK(grabthat::Pick(panels, g, pk) == -1);
        grabthat::Params ek = pk;              // ...with the eyes' line, the one ahead whatever it faces
        ek.aimPalm = false;
        CHECK(facing(-1.4, 0, -1.4) == 1 && grabthat::Pick(panels, g, ek) == 0);
    }
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

// --- the user's flags, frame by frame: loaded once, played through hand input (and "grab that")

// A flag's frames as ft-hands published them: the hands file, the gestures (down or not per frame,
// the counters and points as ft-hands keeps them), the head at capture.
struct FlagFrame {
    int64_t t = 0;
    Mat head{};
    std::shared_ptr<fh_hands_v2_t> file;
    fh_gestures_t g{};
};
bool LoadFlags(const std::vector<std::string> &dirs, std::vector<FlagFrame> &frames);
bool LoadFlag(const std::string &dir, std::vector<FlagFrame> &frames) { return LoadFlags({dir}, frames); }
// ...flags one after the other as one (their frames in time order, each once)
bool LoadFlags(const std::vector<std::string> &dirs, std::vector<FlagFrame> &frames) {
    std::vector<Json> sets, poses;
    for (const std::string &dir : dirs)
        if (!ReadJsonLines(dir + "/sets.jsonl", sets) || !ReadJsonLines(dir + "/hmd.jsonl", poses)) return false;
    auto t = [](const Json &j, const char *k) { return j[k] ? j[k]->num : 0.0; };
    std::stable_sort(sets.begin(), sets.end(), [&](const Json &x, const Json &y) { return t(x, "t_raw_ns") < t(y, "t_raw_ns"); });
    sets.erase(std::unique(sets.begin(), sets.end(), [&](const Json &x, const Json &y) { return t(x, "t_raw_ns") == t(y, "t_raw_ns"); }),
               sets.end());
    std::stable_sort(poses.begin(), poses.end(), [&](const Json &x, const Json &y) { return t(x, "t_ns") < t(y, "t_ns"); });
    std::vector<std::pair<double, Mat>> heads;
    for (const Json &j : poses) {
        if (!j["t_ns"] || !j["m"] || j["m"]->items.size() != 12) continue;
        Mat h{};
        for (int i = 0; i < 12; ++i) h.m[i / 4][i % 4] = float(j["m"]->items[i].num);
        heads.push_back({j["t_ns"]->num, h});
    }
    fh_gestures_t g{};
    std::memcpy(g.magic, FH_GESTURES_MAGIC, 8);
    g.version = FH_GESTURES_VERSION;
    bool was[2][2] = {{false, false}, {false, false}};   // [pinch, grip][side]
    uint64_t seq = 0;
    size_t hi = 0;
    for (const Json &j : sets) {
        if (!j["t_raw_ns"] || !j["hands"]) continue;
        FlagFrame f;
        f.t = int64_t(j["t_raw_ns"]->num);
        while (hi + 1 < heads.size() && std::fabs(heads[hi + 1].first - double(f.t)) <= std::fabs(heads[hi].first - double(f.t))) ++hi;
        f.head = heads.empty() ? Identity() : heads[hi].second;
        f.file = std::make_shared<fh_hands_v2_t>();
        fh_hands_v2_t &file = *f.file;
        std::memset(&file, 0, sizeof file);
        std::memcpy(file.v1.magic, FH_HANDS_MAGIC, 8);
        file.v1.version = 1, file.v1.size = sizeof(fh_hands_t), file.v1.seq = 2 * ++seq;
        file.v1.capture_ns = uint64_t(f.t), file.v1.publish_ns = uint64_t(f.t + 30'000'000);
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
        const Json *gj = j["gestures"];
        for (int kind = 0; kind < 2; ++kind)
            for (int side = 0; side < 2; ++side) {
                const Json *arr = gj ? (*gj)[kind ? "grip" : "pinch"] : nullptr;
                const bool down = arr && arr->items.size() == 2 && arr->items[side]["flags"] &&
                                  (int(arr->items[side]["flags"]->num) & FH_PINCH_DOWN);
                const bool lost = arr && arr->items.size() == 2 && arr->items[side]["flags"] &&
                                  (int(arr->items[side]["flags"]->num) & FH_PINCH_LOST);
                fh_pinch_t &o = kind ? g.grip[side] : g.pinch[side];
                o.flags = FH_PINCH_TRACKED | (down ? FH_PINCH_DOWN : 0) | (lost ? FH_PINCH_LOST : 0), o.hand_id = ids[side];
                for (int i = 0; i < 3; ++i) o.point[i] = kind ? palm[side][i] : knuck[side][i];
                if (down && !was[kind][side]) {
                    ++o.begins, o.begin_ns = uint64_t(f.t);
                    std::memcpy(o.begin_point, o.point, sizeof o.point);
                }
                if (!down && was[kind][side]) ++o.ends, o.end_ns = uint64_t(f.t);
                was[kind][side] = down;
            }
        g.capture_ns = uint64_t(f.t), g.publish_ns = uint64_t(f.t + 30'000'000), g.seq += 2;
        f.g = g;
        frames.push_back(std::move(f));
    }
    return true;
}

// Plays frames [from, to) through hand input (and "grab that", if det; the palm menu, if palm), as
// vr.cpp runs them: 90 Hz ticks, the hands frame 30 ms after its capture, a fire cancels a fist
// waiting on it. tick(now, first) after each Update (first: the tick the frame came on).
template <class Tick>
void PlayFrames(const std::vector<FlagFrame> &fr, size_t from, size_t to, handcut::Hands &hands, HandInput &in,
                grabthat::Detector *det, std::vector<Panel> &panels, Tick tick, palmmenu::PalmMenu *palm = nullptr) {
    to = std::min(to, fr.size());
    for (size_t n = from; n < to; ++n) {
        const FlagFrame &f = fr[n];
        const int64_t next = n + 1 < fr.size() ? fr[n + 1].t : f.t + 33'000'000;
        bool first = true;
        hands.Feed(nullptr, 0, nullptr, f.head, f.t);
        for (int64_t now = f.t + 30'000'000; now < next + 30'000'000; now += 11'111'111) {
            if (first) hands.Feed(f.file.get(), sizeof(fh_hands_t), &f.g, f.head, now);
            else hands.Feed(nullptr, 0, nullptr, f.head, now);
            Input i;
            i.nowNs = now, i.hands = &hands, i.head = f.head, i.haveHead = true, i.allowed[0] = i.allowed[1] = true;
            if (palm) {
                palmmenu::Input pin;
                pin.nowNs = now, pin.hands = &hands, pin.head = f.head, pin.haveHead = true;
                for (int s2 = 0; s2 < 2; ++s2) {
                    pin.allowed[s2] = true, pin.busy[s2] = in.pressing(s2) || in.grabbing(s2);
                    pin.atPanel[s2] = in.near(s2) || HandAtPanel(hands, s2, panels, in.params());
                }
                palm->Update(pin);
                for (int s2 = 0; s2 < 2; ++s2) i.allowed[s2] = !palm->shown(s2);
            }
            // ("grab that" has the hand only while it aims at a panel it would take: vr.cpp's GrabThatAims)
            for (int s2 = 0; s2 < 2; ++s2) {
                grabthat::Fire aim;
                double progress = 0;
                std::vector<grabthat::Target> targets;
                for (const Panel &P : panels) targets.push_back({P.index, P.pose, P.width, P.height, P.curve});
                i.grabThatArmed[s2] = det && det->armed(s2) && det->Aim(s2, &aim, &progress) && grabthat::Pick(targets, aim, det->params()) >= 0;
            }
            in.Update(i, panels);
            if (first && det) {
                bool allowed[2];
                for (int s2 = 0; s2 < 2; ++s2) allowed[s2] = !in.pressing(s2) && !in.grabbing(s2);
                det->Frame(hands.captureNs(), hands.seen(), hands.captureHead(), allowed);
                for (const auto &f2 : det->fired()) in.GrabThatFired(f2.side);
            }
            tick(now, first);
            first = false;
        }
    }
}

// Panels that move as vr.cpp moves them: a carry's pose and width, a throw's place at once.
void ApplyCarries(const HandInput &in, std::vector<Panel> &panels) {
    for (Panel &P : panels) {
        for (const Carry &c : in.carries()) {
            if (c.panel != P.index || c.kind == Carry::Resize) continue;
            if (c.width > 0) P.height *= c.width / P.width, P.width = c.width;
            P.pose = c.pose;
        }
        for (const Event &e : in.events())
            if (e.type == Event::Throw && e.panel == P.index) P.pose = e.to;
    }
}

// How steady a carried panel is, per 90 Hz tick: the second difference of its centre (mm; the
// shake, the steps of a moving one), and of its turn (deg).
struct Steady {
    std::vector<double> pos, rot;
    int segments = 0, grabbed = 0, spikes = 0, turnSpikes = 0;
    HandInput::Stats stats;
    void Add(const std::vector<Mat> &poses) {
        for (size_t i = 2; i < poses.size(); ++i) {
            double d2 = 0;
            for (int k = 0; k < 3; ++k) {
                const double a = poses[i].m[k][3] - 2 * poses[i - 1].m[k][3] + poses[i - 2].m[k][3];
                d2 += a * a;
            }
            pos.push_back(std::sqrt(d2) * 1000);
            spikes += std::sqrt(d2) > 0.005;
            // the turn each tick (axis * angle, room), and its change
            auto turn = [](const Mat &a, const Mat &b, double out[3]) {   // b a^T
                double r[3][3];
                for (int x = 0; x < 3; ++x)
                    for (int y = 0; y < 3; ++y) r[x][y] = b.m[x][0] * a.m[y][0] + b.m[x][1] * a.m[y][1] + b.m[x][2] * a.m[y][2];
                const double ax[3] = {r[2][1] - r[1][2], r[0][2] - r[2][0], r[1][0] - r[0][1]};
                const double s = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), c = r[0][0] + r[1][1] + r[2][2] - 1;
                const double ang = std::atan2(s, c);
                for (int k = 0; k < 3; ++k) out[k] = s > 1e-12 ? ax[k] / s * ang : 0;
            };
            double w0[3], w1[3];
            turn(poses[i - 2], poses[i - 1], w0), turn(poses[i - 1], poses[i], w1);
            const double dw = std::sqrt((w1[0] - w0[0]) * (w1[0] - w0[0]) + (w1[1] - w0[1]) * (w1[1] - w0[1]) + (w1[2] - w0[2]) * (w1[2] - w0[2]));
            rot.push_back(dw * 180 / M_PI);
            turnSpikes += dw * 180 / M_PI > 1.5;
        }
    }
};
double Pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}
double Rms(const std::vector<double> &v) {
    double s = 0;
    for (double x : v) s += x * x;
    return v.empty() ? 0 : std::sqrt(s / v.size());
}

// Each pinch or grip held 0.4 s or more in the flag, against a panel 0.6 m wide placed where it
// began (facing the eyes, the pinch's contact or the fist's palm on its edge, the hand's side of
// it): the panel's steadiness while the hand carries it, moving as vr.cpp moves it.
void CarrySegments(const std::vector<FlagFrame> &fr, const Params &k, Steady &out, const char *name) {
    for (int s = 0; s < 2; ++s) {
        size_t n = 0;
        while (n < fr.size()) {
            auto down = [&](size_t i) { return (fr[i].g.pinch[s].flags & FH_PINCH_DOWN) || (fr[i].g.grip[s].flags & FH_PINCH_DOWN); };
            if (!down(n)) {
                ++n;
                continue;
            }
            size_t b = n, e = n, gap = 0;
            for (size_t i = n; i < fr.size(); ++i) {
                if (down(i)) e = i, gap = 0;
                else if (++gap > 3) break;
            }
            n = e + 1;
            if (fr[e].t - fr[b].t < 400'000'000) continue;
            // the hand at the begin, in the room
            const fh_hands_t &hf = fr[b].file->v1;
            const fh_hand_t *h = nullptr;
            for (uint32_t q = 0; q < hf.nhands; ++q)
                if (bool(hf.hands[q].flags & FH_HAND_RIGHT) == (s == 1)) h = &hf.hands[q];
            if (!h) continue;
            const bool grip = fr[b].g.grip[s].flags & FH_PINCH_DOWN;
            double local[3] = {0, 0, 0}, at[3];
            if (grip)
                for (int q : {0, 5, 9, 13, 17})
                    for (int i = 0; i < 3; ++i) local[i] += h->pts[q][i] / 5.0;
            else
                for (int i = 0; i < 3; ++i) local[i] = (h->pts[4][i] + h->pts[8][i]) / 2.0;
            const Mat &H = fr[b].head;
            for (int i = 0; i < 3; ++i) at[i] = H.m[i][0] * local[0] + H.m[i][1] * local[1] + H.m[i][2] * local[2] + H.m[i][3];
            const double hp[3] = {H.m[0][3], H.m[1][3], H.m[2][3]};
            // where a panel can be: ahead of the eyes, not in the lap (a hand resting there pinches too)
            const double rel[3] = {at[0] - hp[0], at[1] - hp[1], at[2] - hp[2]};
            const double ahead = -(rel[0] * H.m[0][2] + rel[1] * H.m[1][2] + rel[2] * H.m[2][2]);
            if (ahead < 0.25 || hp[1] - at[1] > 0.35) continue;
            Panel P = Facing(at, 0, hp);
            P.index = 0, P.width = 0.6, P.height = 0.25, P.pxW = 1920, P.pxH = 800, P.glows = false;
            const double toward = s == 0 ? 0.29 : -0.29;   // the hand on its own side's edge
            for (int i = 0; i < 3; ++i) P.pose.m[i][3] += float(P.pose.m[i][0] * toward);
            {   // (and its centre ahead too)
                const double c[3] = {P.pose.m[0][3] - hp[0], P.pose.m[1][3] - hp[1], P.pose.m[2][3] - hp[2]};
                if (-(c[0] * H.m[0][2] + c[1] * H.m[1][2] + c[2] * H.m[2][2]) < 0.3) continue;
            }
            std::vector<Panel> panels{P};
            handcut::Hands hands;
            HandInput in(k);
            grabthat::Detector det;   // (a fist pulled back to the face is "grab that", as live)
            std::vector<Mat> poses;
            std::vector<std::string> logs;
            bool took = false;
            const bool trace = std::getenv("HANDINPUT_LOG") && std::atoi(std::getenv("HANDINPUT_LOG")) >= 2;
            PlayFrames(fr, b >= 30 ? b - 30 : 0, e + 12, hands, in, &det, panels, [&](int64_t now, bool first) {
                ApplyCarries(in, panels);
                for (const std::string &l : in.logs()) logs.push_back(l);
                if (in.grabbing(s) && in.grabPanel(s) == 0) {
                    took = true, poses.push_back(panels[0].pose);
                    if (trace) {
                        char b2[200];
                        const Mat &m = panels[0].pose;
                        std::snprintf(b2, sizeof b2, "      %+.3f%s centre %+.3f %+.3f %+.3f  z %+.2f %+.2f %+.2f  %s", (now - fr[b].t) / 1e9,
                                      first ? "*" : " ", m.m[0][3], m.m[1][3], m.m[2][3], m.m[0][2], m.m[1][2], m.m[2][2], in.Describe().c_str());
                        logs.push_back(b2);
                    }
                } else if (!poses.empty()) {
                    out.Add(poses), poses.clear();
                }
            });
            out.Add(poses);
            ++out.segments, out.grabbed += took;
            const HandInput::Stats &st = in.stats;
            out.stats.coasts += st.coasts, out.stats.glitches += st.glitches, out.stats.reacquired += st.reacquired;
            out.stats.throws += st.throws, out.stats.cards += st.cards, out.stats.fists += st.fists, out.stats.grabs += st.grabs;
            if (std::getenv("HANDINPUT_LOG")) {
                std::printf("  %s %s %s %.2f s: %s\n", name, s ? "right" : "left", grip ? "grip" : "pinch", (fr[e].t - fr[b].t) / 1e9,
                            took ? "carried" : "not taken");
                for (const std::string &l : logs) std::printf("    %s\n", l.c_str());
            }
        }
    }
}

void PrintSteady(const char *what, const Steady &s) {
    std::printf("%s: %d holds, %d carried; centre's 2nd difference per tick rms %.2f mm, 99%% %.1f, max %.1f, %d over 5 mm; "
                "turn's rms %.2f deg, 99%% %.2f, max %.1f, %d over 1.5 deg; %d coasts, %d glitches, %d back, %d thrown\n",
                what, s.segments, s.grabbed, Rms(s.pos), Pct(s.pos, 0.99), Pct(s.pos, 1), s.spikes, Rms(s.rot), Pct(s.rot, 0.99),
                Pct(s.rot, 1), s.turnSpikes, s.stats.coasts, s.stats.glitches, s.stats.reacquired, s.stats.throws);
}

// --carries FLAG_DIR...: the steadiness of carried panels in each flag, with the tracking gates (and
// the confidence smoothing) on and off.
int ScanCarries(int n, char **dirs) {
    Steady allOld, allNew;
    for (int i = 0; i < n; ++i) {
        std::vector<FlagFrame> fr;
        if (!LoadFlag(dirs[i], fr)) {
            std::printf("%s: can't read\n", dirs[i]);
            continue;
        }
        const char *name = std::strrchr(dirs[i], '/') ? std::strrchr(dirs[i], '/') + 1 : dirs[i];
        Params old = Params{};
        old.gate = false;
        Steady a, b;
        CarrySegments(fr, old, a, name);
        CarrySegments(fr, Params{}, b, name);
        std::printf("%s\n", name);
        PrintSteady("  before (no gates)", a);
        PrintSteady("  now              ", b);
        for (const Steady *src : {&a, &b}) {
            Steady &dst = src == &a ? allOld : allNew;
            dst.pos.insert(dst.pos.end(), src->pos.begin(), src->pos.end());
            dst.rot.insert(dst.rot.end(), src->rot.begin(), src->rot.end());
            dst.segments += src->segments, dst.grabbed += src->grabbed, dst.spikes += src->spikes, dst.turnSpikes += src->turnSpikes;
            dst.stats.coasts += src->stats.coasts, dst.stats.glitches += src->stats.glitches;
            dst.stats.reacquired += src->stats.reacquired, dst.stats.throws += src->stats.throws;
        }
    }
    PrintSteady("all, before", allOld);
    PrintSteady("all, now   ", allNew);
    return 0;
}

// A flag's hands (as ft-hands published them) and gestures through near hand input and "grab
// that" together, as vr.cpp runs them (PlayFrames), with `panels` in the room (still, unless live:
// then they move as carries and throws move them). Fires, the hand input's events, and its log.
struct FlagRun {
    std::vector<grabthat::Fire> fires;
    std::vector<Event> events;
    std::vector<std::string> logs;
    HandInput::Stats stats;
    palmmenu::PalmMenu::Stats palm;
};
bool FlagThroughHandInput(const std::string &dir, const std::vector<Panel> &panels0, FlagRun &out, const Params &k = {},
                          bool live = false, palmmenu::PalmMenu *palm = nullptr) {
    std::vector<FlagFrame> fr;
    if (!LoadFlag(dir, fr)) return false;
    handcut::Hands hands;
    HandInput in(k);
    grabthat::Detector det;
    std::vector<Panel> panels = panels0;
    PlayFrames(fr, 0, fr.size(), hands, in, &det, panels, [&](int64_t, bool first) {
        for (const Event &e : in.events()) out.events.push_back(e);
        for (const std::string &l : in.logs()) out.logs.push_back(l);
        if (first)
            for (const auto &f : det.fired()) out.fires.push_back(f);
        if (live) ApplyCarries(in, panels);
    }, palm);
    out.stats = in.stats;
    if (palm) out.palm = palm->stats;
    return true;
}

// ------------------------------------------------------------------- the third live test (2026-10-10)

// A held hand's impossible frames (Params::gate): a bar grab held still while every other frame reads
// the hand 9 cm off (under jumpMax, the old glitch rule) (the side-grab flag's two-place misread), then a real 1.5 m/s move, then the hand
// read 20 cm away for good (it did move there), then past the edge of view (as unseen).
struct GateRun { double still = 0, step = 0, moveErr = 0, after = 0; int gated = 0, moved = 0; };
GateRun RunGate(bool gate) {
    Rig r;
    r.in.params().gate = gate;
    const double barY = r.panels[0].controls[0].v;
    auto hand = [&](double x, double z = 0.02) { return KnucklesAt(x, barY, z); };
    for (int k = 0; k < 5; ++k) r.Frame({hand(0)});
    const double at[3] = {0, barY, -0.48};
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({hand(0)});
    GateRun out;
    double lastX = r.panels[0].pose.m[0][3], x0 = lastX;
    auto step = [&](const SynthHand &h) {
        r.Frame({h});
        if (r.carries.empty()) return;
        out.step = std::max(out.step, std::fabs(r.carries[0].pose.m[0][3] - lastX) / 3);
        lastX = r.carries[0].pose.m[0][3];
        r.panels[0].pose = r.carries[0].pose;
    };
    for (int k = 0; k < 10; ++k) step(hand(0));
    for (int k = 0; k < 30; ++k) {   // the misread every other frame
        step(hand(k % 2 ? 0.09 : 0.0));
        out.still = std::max(out.still, std::fabs(lastX - x0));
    }
    out.step = 0;
    for (int k = 1; k <= 10; ++k) step(hand(0.05 * k));   // 1.5 m/s for a third of a second
    for (int k = 0; k < 15; ++k) step(hand(0.5));
    out.moveErr = std::fabs(lastX - (x0 + 0.5));
    for (int k = 0; k < 20; ++k) step(hand(0.7));          // read 20 cm on, and staying
    out.after = std::fabs(lastX - (x0 + 0.7));
    out.gated = r.in.stats.gated, out.moved = r.in.stats.moved;
    CHECK(r.in.grabbing(1));
    return out;
}

void TestTrackGate() {
    const GateRun on = RunGate(true), off = RunGate(false);
    std::printf("gate: a still hold read 9 cm off every other frame: the panel %.1f cm off at most (%.1f without the gate); "
                "a 1.5 m/s move: %.1f mm a tick at most, %.1f cm short after it (%.1f); a real 20 cm jump: %.1f cm off 0.6 s "
                "later (%.1f); %d frames dropped, %d new places taken\n", on.still * 100, off.still * 100, on.step * 1000,
                on.moveErr * 100, off.moveErr * 100, on.after * 100, off.after * 100, on.gated, on.moved);
    CHECK(on.still < 0.01 && off.still > 0.02);
    CHECK(on.moveErr < 0.02);
    CHECK(on.after < 0.02 && on.moved >= 1);
    CHECK(on.gated >= 15);
    // past the edge of view (the hand 80 deg or more off the head's forward): as unseen, the panel
    // holds still where it was; inside it again, it follows
    {
        Rig r;
        const double barY = r.panels[0].controls[0].v;
        for (int k = 0; k < 5; ++k) r.Frame({KnucklesAt(-0.1, barY, 0.02)});
        const double at[3] = {-0.1, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({KnucklesAt(-0.1, barY, 0.02)});
        auto apply = [&] { if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose; };
        // carried out to the left (1 m/s), on past the edge of view: 85 deg left at 0.5 m
        const double ex = -0.5 * std::sin(85 * M_PI / 180), ez = -0.5 * std::cos(85 * M_PI / 180);
        for (int k = 1; k <= 12; ++k) {
            SynthHand h = KnucklesAt(-0.1, barY, 0.02);
            const double f = k / 12.0;
            h.pinch[0] = -0.1 + (ex + 0.1) * f, h.pinch[2] = -0.48 + (ez + 0.48) * f;
            r.Frame({h}), apply();
        }
        const double x0 = r.panels[0].pose.m[0][3];
        std::vector<double> xs;
        for (int k = 0; k < 20; ++k) {
            SynthHand h = KnucklesAt(0, barY, 0);
            h.pinch[0] = ex + 0.01 * (k % 2), h.pinch[2] = ez;   // jittering out there
            r.Frame({h}), apply();
            xs.push_back(r.panels[0].pose.m[0][3]);
        }
        CHECK(x0 < -0.2);   // (it followed the hand out, until the edge)
        const double spread = *std::max_element(xs.begin() + 5, xs.end()) - *std::min_element(xs.begin() + 5, xs.end());
        std::printf("gate: a hand carried out of view at the side (85 deg off the head's forward): the panel still within %.1f mm "
                    "(%.0f cm on from where it was), %d frames counted unseen\n", spread * 1000, (xs.back() - x0) * 100,
                    r.in.stats.edgeFrames);
        CHECK(spread < 0.002 && r.in.grabbing(1) && r.in.stats.edgeFrames >= 15);
    }
    // the hand's frame flipping 150 deg every other frame (the side-grab flag: 100-180 deg between
    // frames): the wrist turn doesn't follow; one camera's frames don't turn it either
    {
        WristRig t;
        double y, p, w;
        for (int k = 0; k < 30; ++k) {
            t.yaw = k % 2 ? 150 : 0;
            t.Step();
        }
        t.yaw = 0;
        for (int k = 0; k < 10; ++k) t.Step();
        t.Angles(&y, &p, &w);
        std::printf("gate: the hand's frame flipping 150 deg every other frame: the panel turned yaw %.1f pitch %.1f roll %.1f, "
                    "%d turns dropped\n", y, p, w, t.r.in.stats.turnsGated);
        NEAR(y, 0, 1.0), NEAR(p, 0, 1.0), NEAR(w, 0, 1.0);
        CHECK(t.r.in.stats.turnsGated >= 10);
    }
    // one camera's frames are smoothed harder: 1 cm of landmark noise moves a held panel less (the
    // wrist turn off: its dead zone takes the frame's noise, and one camera's frames don't turn it)
    for (bool stereo : {true, false}) {
        Rig r;
        r.in.params().grabRotate = false, r.in.params().grabDeadzone = 0;
        const double barY = r.panels[0].controls[0].v;
        auto hand = [&](double j) {
            SynthHand h = KnucklesAt(0, barY, 0.02);
            h.jitter = j, h.stereo = stereo;
            return h;
        };
        for (int k = 0; k < 5; ++k) r.Frame({hand(0)});
        const double at[3] = {0, barY, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({hand(0)});
        std::vector<double> xs;
        for (int k = 0; k < 90; ++k) {
            r.Frame({hand(0.01)});
            if (!r.carries.empty()) xs.push_back(r.carries[0].pose.m[0][3]), r.panels[0].pose = r.carries[0].pose;
        }
        CHECK(xs.size() > 80);
        double m = 0, v = 0;
        for (double x : xs) m += x / xs.size();
        for (double x : xs) v += (x - m) * (x - m) / xs.size();
        std::printf("gate: 1 cm landmark noise on a held still hand, %s: the panel's sd %.2f mm\n", stereo ? "two cameras" : "one camera",
                    std::sqrt(v) * 1000);
        static double stereoSd = 0;
        if (stereo) stereoSd = std::sqrt(v);
        else CHECK(std::sqrt(v) < stereoSd);
    }
}

// A fist ft-hands calls a pinch (the thumb on the index of a closed hand): a fist grab, not a pinch
// click; two of them stretch, and the pair turns the panel on every axis (Params::stretchTurn).
// Fists only resize (Params::fistResizeOnly, user 2026-10-11): one fist holds the panel still; two fists
// pulled apart and moved and turned only scale it, about its centre where it was.
// Two fists near a panel resize it by their shape alone, no grip from ft-hands (Params::fistPair).
// The lazy susan by hand (Params::sweep): an open hand on its edge swept 30 cm sideways in 0.6 s spins one
// panel; a flat hand, a slow sweep, a short one don't; back and forth, each stroke counts.
void TestSweep() {
    auto run = [](double rollDeg, std::vector<double> xs, double seconds, bool right = true) {
        Rig r;
        const int frames = std::max(2, int(seconds * 30));
        auto at = [&](double x) {
            SynthHand h;
            h.palm = true, h.rollDeg = rollDeg, h.tiltDeg = 0, h.right = right;
            h.palmAt[0] = x, h.palmAt[1] = -0.2, h.palmAt[2] = -0.30;
            return h;
        };
        for (int k = 0; k < 5; ++k) r.Frame({at(xs[0])});
        for (size_t leg = 0; leg + 1 < xs.size(); ++leg) {
            for (int k = 1; k <= frames; ++k) {
                const double f = double(k) / frames;
                r.Frame({at(xs[leg] + (xs[leg + 1] - xs[leg]) * (3 * f * f - 2 * f * f * f))});
            }
            for (int k = 0; k < 4; ++k) r.Frame({at(xs[leg + 1])});   // (a pause at each end)
        }
        std::vector<int> spins;
        for (const Event &e : r.events) if (e.type == Event::Spin) spins.push_back(int(e.x));
        if (std::getenv("SWEEP_DEBUG"))
            for (const std::string &l : r.logs) std::printf("    %s\n", l.c_str());
        return spins;
    };
    auto show = [](const char *what, const std::vector<int> &v) {
        std::printf("sweep: %s: %zu spin(s)", what, v.size());
        for (int d : v) std::printf(" %+d", d);
        std::printf("\n");
    };
    auto left = run(90, {0.25, -0.15}, 0.4);   // on its edge, 40 cm left in 0.4 s
    show("edge-on, 40 cm left", left);
    CHECK(left.size() == 1 && left[0] == -1);
    auto right = run(-90, {-0.15, 0.25}, 0.4, false);  // the left hand, right (its palm leads that way)
    show("edge-on, 40 cm right, the left hand", right);
    CHECK(right.size() == 1 && right[0] == 1);
    // the right hand to the right is its return stroke, whichever way the tracker reads its palm (on its
    // edge the palm's normal flips sign from frame to frame: 2026-10-14)
    auto outward = run(-90, {-0.15, 0.25}, 0.4);
    show("edge-on, 40 cm right, the right hand", outward);
    CHECK(outward.empty());
    auto flat = run(180, {0.25, -0.15}, 0.4);    // palm down
    show("flat, palm down", flat);
    CHECK(flat.empty());
    auto slow = run(90, {0.25, -0.15}, 1.5);
    show("edge-on, 40 cm in 1.5 s", slow);
    CHECK(slow.empty());
    auto shortOne = run(90, {0.1, -0.05}, 0.3);
    show("edge-on, 15 cm", shortOne);
    CHECK(shortOne.empty());
    // one hand on its own (the other unseen): 22 cm counts (20 asked: the frames land just inside the ends), and
    // a hand turned a little toward flat
    auto solo20 = run(90, {0.11, -0.11}, 0.3);
    show("edge-on, 22 cm, one hand", solo20);
    CHECK(solo20.size() == 1 && solo20[0] == -1);
    auto tilted53 = run(143, {0.25, -0.15}, 0.4);
    show("tilted 53 deg, 40 cm, one hand", tilted53);
    CHECK(tilted53.size() == 1 && tilted53[0] == -1);
    // back and forth with one hand: only the palm-led strokes (user, 2026-10-11: the return stroke, the
    // back of the hand leading, does nothing; the other hand palms the other way)
    auto paw = run(90, {0.25, -0.15, 0.25, -0.15}, 0.4);
    show("back and forth", paw);
    CHECK(paw.size() == 2 && paw[0] == -1 && paw[1] == -1);
    // easier since the second live test: 28 cm counts, a hand tilted 30 deg off its edge too
    auto shorter = run(90, {0.16, -0.12}, 0.35);
    show("edge-on, 28 cm", shorter);
    CHECK(shorter.size() == 1 && shorter[0] == -1);
    auto tilted = run(120, {0.25, -0.15}, 0.4);
    show("tilted 30 deg, 40 cm", tilted);
    CHECK(tilted.size() == 1);
    auto halfFlat = run(160, {0.25, -0.15}, 0.4);   // 70 deg off its edge: mostly flat (60 counts since 2026-10-13)
    show("tilted 70 deg (mostly flat)", halfFlat);
    CHECK(halfFlat.empty());
    auto sixty = run(150, {0.25, -0.15}, 0.4);
    show("tilted 60 deg, one hand", sixty);
    CHECK(sixty.size() == 1);
}

void TestSusanPick() {
    using grabthat::Bearing;
    // panels at -90 (left), -20, +5 (ahead), +40, +120 (right, behind)
    const std::vector<Bearing> p = {{0, -90}, {1, -20}, {2, 5}, {3, 40}, {4, 120}};
    CHECK(grabthat::SusanPick(p, -1, 0, true, 8) == 3);    // none brought: the next to the right, not the one ahead
    CHECK(grabthat::SusanPick(p, -1, 0, false, 8) == 1);   // ...or to the left
    CHECK(grabthat::SusanPick(p, 3, 40, true, 8) == 4);    // brought 3 (it lives at +40): the next round
    CHECK(grabthat::SusanPick(p, 4, 120, true, 8) == 0);   // past the last: round to the far left
    CHECK(grabthat::SusanPick(p, 0, -90, false, 8) == 4);  // and the other way
    CHECK(grabthat::SusanPick(p, 3, 40, false, 8) == 2);   // back: the one ahead comes too once one is brought
    CHECK(grabthat::SusanPick({{2, 5}}, -1, 0, true, 8) == -1);   // only the one ahead: nothing
    CHECK(grabthat::SusanPick({{2, 5}}, 2, 5, true, 8) == -1);    // only the brought one: nothing
}

void TestReadableSize() {
    const grabthat::ReadParams k;   // 26 px/deg, at most 100 x 55 deg, at least 40 wide, flat to 60
    // a 1920x1080 window: 74 x 42 deg, curved round you 1 m out
    grabthat::Readable r = grabthat::ReadableSize(1920, 1080, 1.0, k);
    NEAR(r.degW, 1920 / 26.0, 1e-9);
    NEAR(r.degH, 1080 / 26.0, 1e-9);
    NEAR(r.curve, 1.0, 1e-9);
    NEAR(r.width, r.degW * M_PI / 180, 1e-9);
    // the native screen: 3440x1440 at 125% = 2752x1152 logical: capped at 100 deg wide, its shape kept
    r = grabthat::ReadableSize(2752, 1152, 1.0, k);
    NEAR(r.degW, 100, 1e-9);
    NEAR(r.degH, 100 * 1152 / 2752.0, 1e-9);
    NEAR(r.ppd, 27.52, 1e-9);
    // the Mac (3024x1964): 55 deg tall at most, so 85 wide
    r = grabthat::ReadableSize(3024, 1964, 1.0, k);
    NEAR(r.degH, 55, 1e-9);
    NEAR(r.degW, 55 * 3024 / 1964.0, 1e-9);
    // the 5120x1440 remote, an ultrawide: up to 150 deg wide, so 42 tall at 34 px/deg (at 100: 51, too small to read)
    r = grabthat::ReadableSize(5120, 1440, 1.0, k);
    NEAR(r.degW, 150, 1e-9);
    NEAR(r.degH, 150 * 1440 / 5120.0, 1e-9);
    NEAR(r.ppd, 5120 / 150.0, 1e-9);
    // 3840x1080 (32:9) too; the 21:9 3440x1440 isn't one: 100 deg at most
    NEAR(grabthat::ReadableSize(3840, 1080, 1.0, k).degW, 3840 / 26.0 > 150 ? 150 : 3840 / 26.0, 1e-9);
    NEAR(grabthat::ReadableSize(3440, 1440, 1.0, k).degW, 100, 1e-9);
    // a small window (800x600): grown to 40 deg wide, flat
    r = grabthat::ReadableSize(800, 600, 1.0, k);
    NEAR(r.degW, 40, 1e-9);
    NEAR(r.curve, 0, 1e-9);
    NEAR(r.width, 2 * std::tan(20 * M_PI / 180), 1e-9);
}

void TestViewDistance() {
    double w = 0;
    // 0.6 m at arm's reach (0.45) spans 67 deg: stays
    NEAR(grabthat::ViewDistance(0.6, 75, 0.45, 1.5, &w), 0.45, 1e-9);
    NEAR(w, 0.6, 1e-9);
    // the flagged 1.62 m: out to where it spans 75 deg (~1.06 m), its width kept
    const double d = grabthat::ViewDistance(1.62, 75, 0.45, 1.5, &w);
    NEAR(d, 0.81 / std::tan(37.5 * M_PI / 180), 1e-6);
    NEAR(w, 1.62, 1e-9);
    // a 2.5 m screen at most 1.5 m out: narrowed to span 75 deg there
    NEAR(grabthat::ViewDistance(2.5, 75, 0.45, 1.5, &w), 1.5, 1e-9);
    NEAR(w, 2 * 1.5 * std::tan(37.5 * M_PI / 180), 1e-9);
}

void TestFistPair() {
    Rig r;
    const Mat start = r.panels[0].pose;
    // two fists 20 cm in front of the panel (beyond the fist grab's own reach), no grip or pinch events
    for (int k = 0; k < 6; ++k) r.Frame({FistAt(-0.3, 0, 0.20, false, 1), FistAt(0.3, 0, 0.20, true, 2)});
    CHECK(r.in.grabbing(0) && r.in.grabbing(1) && r.in.stats.fistPairs == 1);
    for (int k = 1; k <= 30; ++k) {
        const double f = k / 30.0;
        r.Frame({FistAt(-0.3 - 0.1 * f, 0, 0.20, false, 1), FistAt(0.3 + 0.1 * f, 0, 0.20, true, 2)});
    }
    CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch && r.carries[0].width > 1.15);
    if (r.carries.size() == 1) NEAR(r.carries[0].pose.m[0][3], start.m[0][3], 1e-4);
    // the right hand opens: let go
    SynthHand o = FistAt(0.4, 0, 0.20, true, 2);
    o.curl = o.curlIndex = 0;
    for (int k = 0; k < 3; ++k) r.Frame({FistAt(-0.4, 0, 0.20, false, 1), o});
    CHECK(!r.in.grabbing(1));
    // one fist alone, or two fists far from any panel: nothing
    Rig q;
    for (int k = 0; k < 6; ++k) q.Frame({FistAt(-0.3, 0, 0.60, false, 1), FistAt(0.3, 0, 0.60, true, 2)});
    CHECK(!q.in.grabbing(0) && !q.in.grabbing(1));
}

void TestFistResizeOnly() {
    {
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        const double at[3] = {0.0, 0, -0.45};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 1 && r.in.grabbing(1));
        for (int k = 1; k <= 45; ++k) r.Frame({FistAt(0.15 * std::min(1.0, k / 30.0), 0, 0.05, true, 1, 70)});
        CHECK(r.carries.empty());   // the panel stays
        r.synth.Grip(1, false, r.t, at, 1);
        SynthHand o = FistAt(0.15, 0, 0.05, true, 1, 70);
        o.curl = o.curlIndex = 0;
        r.Frame({o}), r.Frame({o});
        CHECK(r.Count(Event::GrabEnd) == 1 && r.Count(Event::Throw) == 0);
    }
    {
        Rig r;
        const Mat start = r.panels[0].pose;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(-0.3, 0, 0.05, false, 1), FistAt(0.3, 0, 0.05, true, 2)});
        const double l[3] = {-0.3, 0, -0.45}, rr[3] = {0.3, 0, -0.45};
        r.synth.Grip(0, true, r.t, l, 1), r.synth.Grip(1, true, r.t, rr, 2);
        r.Frame({FistAt(-0.3, 0, 0.05, false, 1), FistAt(0.3, 0, 0.05, true, 2)});
        for (int k = 1; k <= 45; ++k) {
            const double f = std::min(1.0, k / 30.0);   // apart 20 cm, both 10 cm right, the right one 10 cm up
            r.Frame({FistAt(-0.3 + 0.1 * f - 0.1 * f, 0, 0.05, false, 1), FistAt(0.3 + 0.1 * f + 0.1 * f, 0.1 * f, 0.05, true, 2)});
        }
        CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch);
        if (r.carries.size() == 1) {
            const Mat &c = r.carries[0].pose;
            std::printf("two fists, resize only: %.2f m wide (from 1.00), centre moved %.1f cm\n", r.carries[0].width,
                        100 * std::hypot(c.m[0][3] - start.m[0][3], c.m[1][3] - start.m[1][3], c.m[2][3] - start.m[2][3]));
            CHECK(r.carries[0].width > 1.15);
            NEAR(c.m[0][3], start.m[0][3], 1e-4), NEAR(c.m[1][3], start.m[1][3], 1e-4), NEAR(c.m[0][0], start.m[0][0], 1e-4);
        }
    }
}

void TestFistPinch() {
    {
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        const double at[3] = {0.0, 0, -0.45};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.Count(Event::Press) == 0 && r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Fist);
        CHECK(r.in.stats.fistPinches == 1);
        // the pinch flickers up, the hand still a fist: held; the hand opens: let go
        r.synth.Pinch(1, false, r.t, at, 1);
        for (int k = 0; k < 3; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.in.grabbing(1));
        SynthHand o = FistAt(0.0, 0, 0.05);
        o.curl = o.curlIndex = 0;
        r.Frame({o}), r.Frame({o});
        CHECK(!r.in.grabbing(1) && r.Count(Event::Press) == 0);
        // with HANDS_FIST_GRAB=0 it's a pinch again (on the content: a click)
        Rig q;
        q.in.params().fistGrab = false;
        for (int k = 0; k < 5; ++k) q.Frame({FistAt(0.0, 0, 0.05)});
        q.synth.Pinch(1, true, q.t, at, 1);
        q.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(q.Count(Event::GrabStart) == 0);
    }
    // two fists that ft-hands calls pinches, at the panel's sides: stretch (wider), the right one up
    // 10 cm (the panel rolls), both wrists tipped 25 deg about the line between them (it pitches)
    {
        Rig r;
        r.in.params().fistResizeOnly = false, r.in.params().fistPair = false;   // (ft-hands' pinches; the shape: TestFistPair)
        auto both = [&](double sep, double lift, double tilt) {
            return std::vector<SynthHand>{FistAt(-0.3 - sep, 0, 0.05, false, 1, 90 - tilt), FistAt(0.3 + sep, lift, 0.05, true, 2, 90 - tilt)};
        };
        for (int k = 0; k < 5; ++k) r.Frame(both(0, 0, 0));
        const double l[3] = {-0.3, 0, -0.45}, rr[3] = {0.3, 0, -0.45};
        r.synth.Pinch(0, true, r.t, l, 1), r.synth.Pinch(1, true, r.t, rr, 2);
        r.Frame(both(0, 0, 0));
        // (the left one takes it, the right one joins it: Params::joinFront)
        CHECK(r.Count(Event::GrabStart) == 2 && r.in.stats.fistPinches + r.in.stats.joins == 2 && r.Count(Event::Press) == 0);
        auto run = [&](double sep, double lift, double tilt) {
            for (int k = 1; k <= 45; ++k) {
                const double f = std::min(1.0, k / 30.0);
                r.Frame(both(sep * f, lift * f, tilt * f));
            }
        };
        run(0.1, 0, 0);
        CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch);
        const double w1 = r.carries.empty() ? 0 : r.carries[0].width;
        run(0.1, 0.1, 0);
        double y = 0, p = 0, w = 0;
        if (!r.carries.empty()) FacingTurn(r.carries[0].pose, &y, &p, &w);
        const double roll = w;
        run(0.1, 0.1, 25);
        if (!r.carries.empty()) FacingTurn(r.carries[0].pose, &y, &p, &w);
        std::printf("two fists (pinches to ft-hands): 20 cm apart -> %.2f m wide; the right one 10 cm up -> roll %.1f; both "
                    "wrists tipped 25 deg -> pitch %.1f (yaw %.1f, roll %.1f)\n", w1, roll, p, y, w);
        NEAR(w1, (0.8 - 0.01) / 0.6, 0.02);
        NEAR(std::fabs(roll), std::atan2(0.1, 0.8) * 180 / M_PI, 1.5);
        NEAR(std::fabs(p), 25 - Params().rotDeadzone, 3.0);
        // HANDS_STRETCH_TURN=0: about the vertical only (no roll)
        Rig q;
        q.in.params().stretchTurn = false;
        for (int k = 0; k < 5; ++k) q.Frame(both(0, 0, 0));
        q.synth.Pinch(0, true, q.t, l, 1), q.synth.Pinch(1, true, q.t, rr, 2);
        q.Frame(both(0, 0, 0));
        for (int k = 1; k <= 45; ++k) q.Frame(both(0.1, 0.1 * std::min(1.0, k / 30.0), 0));
        if (!q.carries.empty()) FacingTurn(q.carries[0].pose, &y, &p, &w);
        NEAR(w, 0, 0.5);
    }
    // StretchTurn itself: a quarter turn of the pair about the vertical turns the panel with it
    {
        const Mat start = At(0, 0, -0.5);
        const double l0[3] = {-0.3, 0, -0.5}, r0[3] = {0.3, 0, -0.5}, l1[3] = {0, 0, -0.2}, r1[3] = {0, 0, -0.8};
        double width;
        const Mat m = StretchTurn(start, 0.6, l0, r0, l1, r1, 0, 0.01, 0.15, 12, true, 0, &width);
        NEAR(m.m[0][2], 1, 1e-3), NEAR(m.m[0][3], 0, 1e-4), NEAR(m.m[2][3], -0.5, 1e-4), NEAR(width, 0.6, 1e-6);
    }
}

// The push (Params::push): an open hand, its palm to the panel, pushed into it (or through it slowly
// for 10 cm or more) throws it 3 m out; a resting touch, a poke, the back of the hand, a wave across
// it, and a hand that just let go of it don't.
void TestPush() {
    struct Case { const char *what; double from[3], to[3], speed; int hand; bool throws; };   // hand: 0 open palm, 1 pointing, 2 back of the hand
    const Case cases[] = {
        {"an open hand pushed into it, 0.6 m/s", {0.05, 0, -0.42}, {0.05, 0, -0.58}, 0.6, 0, true},
        {"an open hand pushed slowly 17 cm into it, 0.15 m/s", {0.05, 0, -0.46}, {0.05, 0, -0.67}, 0.15, 0, true},
        {"an open hand held out 10 cm into it, slowly (grab that's hold to a near panel)", {0.05, 0, -0.46}, {0.05, 0, -0.60}, 0.15, 0, false},
        {"an open hand pushed 7 cm into it at 0.4 m/s (grab that's reach to a near panel)", {0.05, 0, -0.42}, {0.05, 0, -0.57}, 0.4, 0, false},
        {"an open hand resting on it, then pushed in, 0.7 m/s", {0.05, 0, -0.495}, {0.05, 0, -0.62}, 0.7, 0, true},
        {"an open hand brought to it, resting there", {0.05, 0, -0.40}, {0.05, 0, -0.49}, 0.6, 0, false},
        {"an open hand resting on it, sinking 1.5 cm in", {0.05, 0, -0.49}, {0.05, 0, -0.515}, 0.03, 0, false},
        {"a poke, 0.4 m/s", {0.05, 0, -0.42}, {0.05, 0, -0.52}, 0.4, 1, false},
        {"the back of an open hand pushed into it", {0.05, 0, -0.42}, {0.05, 0, -0.58}, 0.6, 2, false},
        {"an open hand waved across it", {-0.3, 0, -0.48}, {0.3, 0, -0.48}, 1.0, 0, false},
    };
    for (const Case &c : cases) {
        Rig r;
        r.panels[0].controls.clear();
        auto make = [&](double x, double y, double z) {
            if (c.hand == 1) return TipAt(x, y, z + 0.5);
            SynthHand h = OpenAt(x, y, z);
            if (c.hand == 2) h.rollDeg = 0;   // the palm toward the head
            return h;
        };
        for (int k = 0; k < 8; ++k) r.Frame({make(c.from[0], c.from[1], c.from[2])});
        Sweep(r, c.from, c.to, c.speed, make);
        const auto t = r.Last(Event::Throw);
        std::printf("push: %s: %s%s\n", c.what, t ? "thrown" : "stays", r.Count(Event::Press) ? ", a press" : "");
        CHECK(bool(t) == c.throws);
        if (t) CHECK(t->push && t->flick), NEAR(t->to.m[2][3], -3.0, 0.01);
        if (c.throws) CHECK(r.Count(Event::Press) == 0);
        if (c.hand == 1) CHECK(r.Count(Event::Press) == 1);
    }
    {
        // the throw aim lights a panel an open palm faces from close in front; not the back of the hand
        Rig r;
        r.Frame({OpenAt(0.05, 0, -0.45)}), r.Frame({OpenAt(0.05, 0, -0.45)});
        CHECK(r.in.throwAims().count(0) && r.in.throwAims().at(0) > 0.5);
        SynthHand b = OpenAt(0.05, 0, -0.45);
        b.rollDeg = 0;
        r.Frame({b}), r.Frame({b});
        CHECK(!r.in.throwAims().count(0));
        // HANDS_PUSH=0: an open hand pushed in doesn't throw it (and doesn't poke)
        Rig q;
        q.in.params().push = false, q.in.params().flick = false;
        q.panels[0].controls.clear();
        const double a[3] = {0.05, 0, -0.42}, z[3] = {0.05, 0, -0.58};
        for (int k = 0; k < 8; ++k) q.Frame({OpenAt(a[0], a[1], a[2])});
        Sweep(q, a, z, 0.6, [](double x, double y, double zz) { return OpenAt(x, y, zz); });
        CHECK(q.Count(Event::Throw) == 0);
    }
}

// A pinch the touch zone claimed whose fingers met a little beyond the band's depth (the third live
// test: "the fingers +9.0 cm from it, out of the touch zone: nothing") still takes the border.
void TestPinchSlack() {
    for (double slack : {0.03, 0.0}) {
        Rig r;
        r.in.params().nearFront = 0.10, r.in.params().pinchSlack = slack;   // (claimed at 9 cm: a wider zone)
        const double v = -0.28125 - 0.01;   // just under the bottom edge
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.1, v, 0.09)});
        const double at[3] = {0.1, v, -0.41};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(0.1, v, 0.09)});
        std::printf("pinch slack %.0f cm: a pinch 9 cm out by the border: %s\n", slack * 100, r.Count(Event::GrabStart) ? "grabs" : "nothing");
        CHECK((r.Count(Event::GrabStart) == 1) == (slack > 0));
    }
}

// The stricter palm menu (2026-10-10): no ring at a panel or for a second after; nor while the
// palm moves; a pinch with the middle finger in it (a card, the bunched fingers) on the ring does
// nothing; a plain one toggles.
void TestPalmMenuStrict() {
    {
        // HandAtPanel: a palm up just under a panel's bottom edge (a card pinch from below) is at it;
        // one 25 cm in front of it (its fingertips 15 cm) isn't
        const std::vector<Panel> panels{TestPanel()};
        SynthHand under = PalmUpHand(), before = PalmUpHand();
        under.palmAt[0] = 0, under.palmAt[1] = -0.33, under.palmAt[2] = -0.49;
        before.palmAt[2] = -0.25;
        CHECK(HandAtPanel(Seen(under), panels, Params{}, 0.12, 0.08));
        CHECK(!HandAtPanel(Seen(before), panels, Params{}, 0.12, 0.08));
    }
    {
        PalmRig r;
        r.atPanel[1] = true;
        r.Frames(15, {PalmUpHand()});
        CHECK(!r.menu.shown(1));
        r.atPanel[1] = false;
        r.Frames(20, {PalmUpHand()});   // 0.67 s: quiet still
        CHECK(!r.menu.shown(1));
        r.Frames(25, {PalmUpHand()});   // 1.5 s after: up long enough since
        CHECK(r.menu.shown(1));
        r.atPanel[1] = true;            // brought to a panel: gone at once
        r.Frame({PalmUpHand()});
        CHECK(!r.menu.shown(1));
    }
    {
        PalmRig r;   // busy (a grab with that hand) then free: quiet for a second
        r.busy[1] = true;
        r.Frames(10, {PalmUpHand()});
        r.busy[1] = false;
        r.Frames(20, {PalmUpHand()});
        CHECK(!r.menu.shown(1));
        r.Frames(25, {PalmUpHand()});
        CHECK(r.menu.shown(1));
    }
    {
        PalmRig r;   // the palm moving at 0.45 m/s: no ring
        for (int k = 0; k < 20; ++k) {
            SynthHand h = PalmUpHand();
            h.palmAt[0] = -0.15 + 0.015 * k;
            r.Frame({h});
        }
        CHECK(!r.menu.shown(1));
    }
    {
        PalmRig r;   // shown; a pinch with the middle curled into it: nothing; a plain one: the menu
        r.Frames(12, {PalmUpHand()});
        CHECK(r.menu.shown(1));
        SynthHand bunched = PalmUpHand();   // (the middle curled in to the thumb, as a card's or bunched fingers)
        bunched.curl = 2;
        r.Pinch(1, true);
        r.Frame({bunched});
        r.Pinch(1, false);
        r.Frames(3, {bunched});
        CHECK(r.toggles.empty() && r.menu.stats.notPlain == 1);
        r.Frames(12, {PalmUpHand()});
        r.Pinch(1, true);
        r.Frame({PalmUpHand()});
        r.Pinch(1, false);
        r.Frames(3, {PalmUpHand()});
        CHECK(r.toggles == std::vector<int>{1});
    }
}

// The user's "grab that" at a panel already at arm's reach (where the gesture would bring one): it
// still fires, twice; the open hand doesn't poke it, nor the fist grab it.
// The fifth live test (2026-10-10 22:55): "grab that" brought the 3.22 m desktop in (0.6 m wide), the
// user carried it by the bar and threw it (3.0 m out, 3.22 m wide again), then "grab that" at it found
// "no panel there" five times, the palm 36-38 deg off its line each time. The open palm is held out
// under the line of sight, not over the window: from the eyes through it is 12-15 deg under where the
// head points and 10-15 right (the user's flag 150115), so at a panel the user looks at the middle of,
// thrown 3 m out and 1.35 m tall (12.7 deg each way), it passes just under its bottom edge.
void TestGrabThatAfterThrow() {
    using grabthat::Target;
    const double W = 3.22, H = 3.22 * 1440 / 3440, R = 2.888;   // the live desktop: 3440x1440, curved
    GrabRig g;   // head at the origin, looking along -z
    const double palm[3] = {0.1, -0.11, -0.45};   // as the flag holds it: 12 deg right, 14 under
    auto gesture = [&]() -> const grabthat::Fire * {
        const size_t n = g.fires.size();
        for (int k = 0; k < 10; ++k) g.Frame({StopHand(palm[0], palm[1], palm[2])});
        for (int k = 1; k <= 6; ++k) {
            const double f = k / 6.0;
            g.Frame({StopHand(palm[0], palm[1], palm[2] + 0.15 * f, f, k >= 2 ? 2 : 1)});
        }
        for (int k = 0; k < 40; ++k) g.Frame({StopHand(palm[0], palm[1], palm[2] + 0.15, 1, 2)});   // (past againMs)
        return g.fires.size() == n + 1 ? &g.fires.back() : nullptr;
    };
    grabthat::Params k;
    k.aimPalm = false;   // (the line from the eyes through the palm, as these live tests had; the palm's own: below)
    grabthat::Params exact = k;
    exact.aimMarginDeg = 0;   // (the fourth live test's: the line has to meet it)
    // 1. the desktop 2.5 m ahead at eye level (15 deg each way): it comes, as it did live
    std::vector<Target> t(1);
    t[0].index = 0, t[0].pose = At(0, 0, -2.5), t[0].width = W, t[0].height = H, t[0].curve = R;
    const grabthat::Fire *f = gesture();
    CHECK(f && grabthat::Pick(t, *f, exact) == 0 && grabthat::Pick(t, *f, k) == 0);
    if (!f) return;
    t[0].pose = grabthat::Arrival(*f, k), t[0].width = 0.6, t[0].height = 0.6 * 1440 / 3440;
    // 2. carried by the bar and let go moving out and up (the live one: 1.30 m/s, 26 deg off straight out)
    Rig r;
    r.panels[0].controls.clear();
    const double v = -0.28125 + 0.01;
    double x = 0.1, y = v, z = 0;
    auto hand = [&](double open) {
        SynthHand h = CardHand(x, y, open);
        h.contact[2] += z;
        return h;
    };
    for (double o : {1.0, 1.0, 0.8, 0.6, 0.4, 0.2}) r.Frame({hand(o)});
    const double at[3] = {0.1, v, -0.5};
    r.Frame({hand(0)});
    r.synth.Pinch(1, true, r.t, at, 1);
    r.Frame({hand(0)});
    for (int q = 0; q < 10; ++q) r.Frame({hand(0)});
    const double dir[3] = {0, std::sin(26 * M_PI / 180), -std::cos(26 * M_PI / 180)};
    for (int q = 1; q <= 8; ++q) {
        x += dir[0] * 1.3 / 30, y += dir[1] * 1.3 / 30, z += dir[2] * 1.3 / 30;
        if (!r.carries.empty()) r.panels[0].pose = r.carries[0].pose;
        r.Frame({hand(0)});
    }
    r.synth.Pinch(1, false, r.t, at, 1);
    for (int q = 0; q < 4; ++q) {
        x += dir[0] * 1.3 / 30, y += dir[1] * 1.3 / 30, z += dir[2] * 1.3 / 30;
        r.Frame({hand(1.0)});
    }
    const auto thrown = r.Last(Event::Throw);
    CHECK(thrown.has_value());
    if (!thrown) return;
    const double out = std::hypot(std::hypot(thrown->to.m[0][3], thrown->to.m[1][3]), thrown->to.m[2][3]);
    NEAR(out, 3.0, 0.01);
    CHECK(thrown->to.m[1][3] > 0.3);   // up along the throw: above the eyes
    t[0].pose = thrown->to, t[0].width = W, t[0].height = H;   // (its width back: vr.cpp's SendAwayWidth)
    // 3. "grab that" at it again, the head turned to its middle: seen from there it's 3 m straight ahead,
    // facing the eyes (the throw faces it at them). The line passes under it: the fourth live test's rule
    // found nothing there (as live); 6 deg takes it.
    std::vector<Target> seen = t;
    seen[0].pose = At(0, 0, -out);
    f = gesture();
    CHECK(f != nullptr);
    if (!f) return;
    grabthat::Closest c;
    const int before = grabthat::Pick(seen, *f, exact, &c), now = grabthat::Pick(seen, *f, k);
    std::printf("grab that after a throw (the fifth live test): the line passes %.1f deg under the thrown desktop; "
                "it %s (with no margin: %s)\n", c.deg, now == 0 ? "comes" : "doesn't come", before == 0 ? "comes" : "nothing there");
    CHECK(before == -1 && c.index == 0 && c.deg > 0.5 && c.deg < 6);
    CHECK(now == 0);
    // 4. ...or at a remote display after that (live 103: 4.85 x 1.36 m, curved 2.11 m, 2.1 m to the
    // right), the head turned to it: it comes, not the thrown desktop (90 deg to the left now)
    std::vector<Target> two = seen;
    two[0].pose = RotY(M_PI / 2, -out, 0, 0);
    two.push_back(Target{102, At(0, 0, -2.1), 4.85, 1.36, 2.11});
    CHECK(grabthat::Pick(two, *f, k) == 102);
    // 5. still precise: a line meeting a panel beats one passing near another nearer; one passing 10
    // deg off every panel takes none; a near one (at arm's reach) isn't taken
    const double pitch = std::atan2(f->palm[1] - f->eye[1], -(f->palm[2] - f->eye[2])),
                 yaw = std::atan2(f->palm[0] - f->eye[0], -(f->palm[2] - f->eye[2]));
    std::vector<Target> pair = seen;
    pair[0].pose = At(3 * std::tan(yaw), 3 * std::tan(pitch), -3);   // the line meets it in its middle now
    pair.push_back(Target{7, At(1.5 * std::tan(yaw), 1.5 * std::tan(pitch + 3 * M_PI / 180) + 0.17, -1.5), 0.6, 0.34, 0});   // nearer, 3 deg over the line
    CHECK(grabthat::Pick(pair, *f, k) == 0);
    pair.erase(pair.begin());
    CHECK(grabthat::Pick(pair, *f, k) == 7);   // (alone, it's taken)
    std::vector<Target> off = seen;
    off[0].pose = At(0, 3 * std::tan(pitch + 14 * M_PI / 180) + H / 2, -3);   // its bottom edge 14 deg over the line (the margin's 10)
    CHECK(grabthat::Pick(off, *f, k) == -1);
    std::vector<Target> nearOne = seen;
    nearOne[0].pose = grabthat::Arrival(*f, k), nearOne[0].width = 0.6, nearOne[0].height = 0.25;
    CHECK(grabthat::Pick(nearOne, *f, k) == -1);
    // the user's own example: both fires at a desktop like the live one, 3 m out where the head pointed
    // as each fired
    const std::string dir2 = FlagsDir() + "/flag-20261010-150115";
    std::vector<grabthat::Fire> fires;
    double flagT = 0, secs = 0;
    if (!ReplayFlag(dir2, fires, &flagT, &secs) || fires.size() != 2) {
        std::printf("grab that after a throw: %s not here, its fires skipped\n", dir2.c_str());
        return;
    }
    int took = 0, tookExact = 0, tookPalm = 0;
    for (const auto &x2 : fires) {
        const double fw[3] = {-x2.head.m[0][2], -x2.head.m[1][2], -x2.head.m[2][2]};
        const double ctr[3] = {x2.eye[0] + 3 * fw[0], x2.eye[1] + 3 * fw[1], x2.eye[2] + 3 * fw[2]};
        const Panel P = Facing(ctr, 0, x2.eye);
        const std::vector<Target> one{{0, P.pose, W, H, R}};
        took += grabthat::Pick(one, x2, k) == 0, tookExact += grabthat::Pick(one, x2, exact) == 0;
        tookPalm += grabthat::Pick(one, x2, grabthat::Params{}) == 0;
    }
    // (the palm's own line, since 2026-10-13: this flag's palm was held 41 and 51 deg off the line from the
    // eyes, half down, so it faced under the desktop: the palm now has to face what it takes)
    std::printf("grab that after a throw, the user's flag: %d of 2 fires take a 3.22 m desktop 3 m out where the head points "
                "(%d with no margin, %d along the palm's own line)\n", took, tookExact, tookPalm);
    CHECK(took == 2);
}

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
        std::printf("grab that at a panel %.2f m ahead (the user's flag): %zu fire(s), %d presses, %d grabs, %d flicks, %d pushes, %d fist(s) "
                    "waited, %d were grab that, near %d of %d frames\n", distance, run.fires.size(), presses, grabs, run.stats.flicks, run.stats.pushes,
                    run.stats.fistsWaited, run.stats.fistsCancelled, run.stats.nearFrames, run.stats.frames);
        if (std::getenv("HANDINPUT_LOG"))
            for (const std::string &l : run.logs) std::printf("    %s\n", l.c_str());
        // since the fourth live test grab that doesn't take a panel at arm's reach: the closing hand's
        // fist takes it instead (it doesn't wait on grab that), and nothing clicks, flicks or pushes it
        const std::vector<grabthat::Target> t{{P.index, P.pose, P.width, P.height, P.curve}};
        CHECK(grabthat::Pick(t, fires[0], gk) == -1);
        CHECK(presses == 0 && run.stats.flicks == 0);   // (grab that's pull back isn't a flick)
        CHECK(run.stats.pushes == 0);   // (nor its open hand held out to the panel a push)
    }
    {   // the same at a panel 1.6 m out along the aim: both fire at it, and no hand input
        const grabthat::Fire &f = fires[0];
        const double aim[3] = {f.palm[0] - f.eye[0], f.palm[1] - f.eye[1], f.palm[2] - f.eye[2]};
        const double l = std::sqrt(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
        const double at[3] = {f.eye[0] + aim[0] / l * 1.6, f.eye[1] + aim[1] / l * 1.6, f.eye[2] + aim[2] / l * 1.6};
        Panel P = Facing(at, 0, f.eye);
        P.width = 1.0, P.height = 0.5625;
        FlagRun run;
        FlagThroughHandInput(dir, {P}, run);
        int presses = 0, grabs = 0;
        for (const Event &e : run.events) presses += e.type == Event::Press, grabs += e.type == Event::GrabStart;
        const std::vector<grabthat::Target> t{{P.index, P.pose, P.width, P.height, P.curve}};
        int picked = 0;
        grabthat::Params eyes;
        eyes.aimPalm = false;   // (the panel is on the line from the eyes through the palm)
        for (const auto &x : run.fires) picked += grabthat::Pick(t, x, eyes) == P.index;
        std::printf("grab that at a panel 1.6 m out along the aim (the user's flag): %zu fire(s), %d at it, %d presses, %d grabs\n",
                    run.fires.size(), picked, presses, grabs);
        CHECK(run.fires.size() == 2 && picked == 2 && presses == 0 && grabs == 0);
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

// ------------------------------------------------------------------- the fourth live test (2026-10-10)

// A panel as the live test had it after "grab that": 0.6 m wide, 3440x1440, half a metre ahead (as
// TestPanel), with vr.cpp's controls for its distance (0.45 m: chrome 5.7 cm, buttons 8 mm).
Panel LivePanel() {
    Panel p = TestPanel();
    p.width = 0.6, p.height = 0.6 * 1440 / 3440, p.pxW = 3440, p.pxH = 1440;
    const double chrome = 0.057, grip = 0.0081, gap = chrome * 0.06, barY = -(p.height / 2 + gap + chrome * 12 / 256);
    p.controls = {{Control::Bar, 0, barY, chrome / 2, chrome * 12 / 256},
                  {Control::Curve, chrome / 2 + gap + grip / 2, barY, grip / 2, grip / 2},
                  {Control::Roll, chrome / 2 + gap * 2 + grip * 1.5, barY, grip / 2, grip / 2},
                  {Control::Tab, p.width / 2 + grip / 2, -(p.height / 2 + grip / 2), grip / 2, grip / 2},
                  {Control::Reset, -(chrome / 2 + gap + grip / 2), barY, grip / 2, grip / 2}};
    return p;
}

// Buttons for hands: the nearest control wins, round ones have a bigger hit area, a pinch presses them.
void TestButtons() {
    const Panel p = LivePanel();
    const Spot &bar = p.controls[0], &curve = p.controls[1];
    const double v = curve.v;
    // the live log's pokes at the curve button: 3-5 mm left of its centre, which the bar took
    CHECK(ControlAt(p, curve.u - 0.005, v, 0.01, 0.015) == Control::Curve);
    CHECK(ControlAt(p, curve.u - 0.004, v + 0.002, 0.01) == Control::Curve);   // (nearest: even without the bigger area)
    CHECK(ControlAt(p, bar.u + 0.02, v, 0.01, 0.015) == Control::Bar);        // on the bar: the bar
    CHECK(ControlAt(p, curve.u, v - 0.016, 0.005, 0.015) == Control::Curve);  // 1.6 cm under it: the bigger area
    CHECK(ControlAt(p, curve.u, v - 0.016, 0.005) == Control::None);
    CHECK(ControlAt(p, p.controls[2].u + 0.002, v, 0.005, 0.015) == Control::Roll);   // the roll button next to it
    // a poke 4 mm left of the curve button's centre presses it
    {
        Rig r;
        r.panels = {LivePanel()};
        for (int mm = 50; mm >= -15; mm -= 10) r.Frame({TipAt(curve.u - 0.004, v, mm / 1000.0)});
        CHECK(r.Count(Event::Control) == 1 && r.Last(Event::Control)->control == Control::Curve && r.Count(Event::Press) == 0);
        std::printf("buttons: a poke 4 mm left of the 8 mm curve button: %s\n", r.Count(Event::Control) ? "pressed it" : "nothing");
    }
    // a pinch on it: lit before (hovered, its guide), pressed once, held with the pinch, no grab
    {
        Rig r;
        r.panels = {LivePanel()};
        SynthHand h = PinchAt(curve.u - 0.003, v, 0.01);
        h.open = 0.6;
        for (int k = 0; k < 5; ++k) r.Frame({h});
        CHECK(r.in.hovered().count(0) && (r.in.hovered().at(0) >> int(Control::Curve) & 1));
        CHECK(r.in.guide(1).kind == Guide::Button && r.in.guide(1).control == Control::Curve && !r.in.guide(1).selected);
        const double at[3] = {curve.u, v, -0.49};
        h.open = 0;
        r.synth.Pinch(1, true, r.t, at, 1);
        for (int k = 0; k < 5; ++k) r.Frame({h});
        CHECK(r.Count(Event::Control) == 1 && r.Last(Event::Control)->control == Control::Curve);
        CHECK(r.Count(Event::GrabStart) == 0 && r.Count(Event::Press) == 0 && r.in.pressing(1) && r.in.stats.buttonPinches == 1);
        CHECK(r.in.guide(1).kind == Guide::Button && r.in.guide(1).selected);
        r.synth.Pinch(1, false, r.t, at, 1);
        h.open = 1;
        for (int k = 0; k < 4; ++k) r.Frame({h});
        CHECK(!r.in.pressing(1) && r.Count(Event::Release) == 0 && r.Count(Event::Control) == 1);
        // HANDS_PINCH_BUTTONS=0: the bar's grab zone takes it (before the fourth live test)
        Rig o;
        o.panels = {LivePanel()};
        o.in.params().pinchButtons = false;
        h.open = 0.6;
        for (int k = 0; k < 5; ++k) o.Frame({h});
        o.synth.Pinch(1, true, o.t, at, 1);
        h.open = 0;
        o.Frame({h});
        CHECK(o.Count(Event::Control) == 0 && o.Count(Event::GrabStart) == 1 && o.Last(Event::GrabStart)->zone == Zone::Bar);
    }
    // a pinch on the bar still takes the bar
    {
        Rig r;
        r.panels = {LivePanel()};
        SynthHand h = PinchAt(bar.u - 0.01, v, 0.01);
        for (int k = 0; k < 5; ++k) r.Frame({h});
        const double at[3] = {bar.u, v, -0.49};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({h});
        CHECK(r.Count(Event::Control) == 0 && r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Bar);
    }
    // a bar pinch ft-hands doesn't call (Params::barPinch; the 2026-10-14 testing session: the tips at
    // 2.0-2.5 cm): the tips closing to 2.3 cm at the bar take it, held while they stay within 3.5 cm
    // (PinchingHand's tips: open 0.3 2.3 cm, 0.5 3.3 cm, 0.6 3.9 cm, 1 5.9 cm apart)
    {
        auto run = [&](double u, double startOpen, bool on, double *heldTo = nullptr) {
            Rig r;
            r.panels = {LivePanel()};
            if (!on) r.in.params().barPinch = 0;
            SynthHand h = PinchAt(u, v, 0.01);
            h.open = startOpen;
            for (int k = 0; k < 5; ++k) r.Frame({h});
            h.open = 0.3;
            for (int k = 0; k < 3; ++k) r.Frame({h});
            const int took = r.Count(Event::GrabStart);
            if (heldTo) {
                h.open = 0.5;
                for (int k = 0; k < 4; ++k) r.Frame({h});
                *heldTo = r.Count(Event::GrabEnd) == 0;
                h.open = 1;
                for (int k = 0; k < 4; ++k) r.Frame({h});
                CHECK(r.Count(Event::GrabEnd) == 1);
            }
            return took && r.Last(Event::GrabStart)->zone == Zone::Bar && r.in.stats.barPinches == 1;
        };
        double held = 0;
        CHECK(run(bar.u - 0.01, 0.6, true, &held));          // closing at the bar: a bar grab
        CHECK(held == 1);                                     // ...held at 3.3 cm, let go at 5.9
        CHECK(!run(bar.u - 0.01, 0.3, true));                 // resting at 2.3 cm, never opened: nothing
        CHECK(!run(bar.u - 0.01, 0.6, false));                // HANDS_BAR_PINCH=0: ft-hands' pinch only
        CHECK(!run(curve.u - 0.003, 0.6, true));              // at the curve button: left to the pinch
        Rig c;                                                // on the content, not the bar: nothing
        c.panels = {LivePanel()};
        SynthHand h = PinchAt(0, 0, 0.01);
        h.open = 0.6;
        for (int k = 0; k < 5; ++k) c.Frame({h});
        h.open = 0.3;
        for (int k = 0; k < 3; ++k) c.Frame({h});
        CHECK(c.Count(Event::GrabStart) == 0 && c.Count(Event::Press) == 0 && c.in.stats.barPinches == 0);
        std::printf("buttons: a bar pinch with the tips 2.3 cm apart, ft-hands calling none: %s\n", held ? "took the bar, held" : "nothing");
    }
}

// The soft glow per hand (Guide, Params::edgeGlow): what a pinch would take, brighter closer.
// The fingertip's dot (Guide::Touch): a pointing hand over the content shows where a tap lands; it
// holds still once the tip is within lockOut (the aim), and the press lands there.
void TestTouchDot() {
    Rig r;
    for (int mm = 60; mm >= 20; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
    Guide g = r.in.guide(1);
    CHECK(g.kind == Guide::Touch && std::fabs(g.u - 0.1) < 0.005 && std::fabs(g.v - 0.05) < 0.005 && !g.selected);
    // within lockOut the tip slides 1.5 cm sideways as it goes in: the aim (and the press) stay
    r.Frame({TipAt(0.1, 0.05, 0.012)});
    r.Frame({TipAt(0.107, 0.05, 0.005)});
    r.Frame({TipAt(0.115, 0.05, -0.005)});
    r.Frame({TipAt(0.115, 0.05, -0.012)});
    const auto p = r.Last(Event::Press);
    CHECK(p && std::fabs(p->x - (0.5 + 0.1 / 1.0) * 1920) < 4);   // (TestPanel: 1 m wide, 1920 px)
    g = r.in.guide(1);
    CHECK(g.kind == Guide::Touch && g.selected);
}

void TestGuides() {
    auto at = [](double u, double v, double d, int pinchFrames = 0) {
        Rig r;
        SynthHand h = PinchAt(u, v, d);
        h.open = 0.6;
        for (int k = 0; k < 5; ++k) r.Frame({h});
        if (pinchFrames) {
            const double p[3] = {u, v, -0.5 + d};
            r.synth.Pinch(1, true, r.t, p, 1);
            h.open = 0;
            for (int k = 0; k < pinchFrames; ++k) r.Frame({h});
        }
        return r.in.guide(1);
    };
    const double W = 0.5, H = 0.5625 / 2;
    Guide g = at(0.0, 0.0, 0.02);   // the middle, a hand shaping a pinch: nothing (no edge, corner or fingertip dot)
    CHECK(g.kind == Guide::None);
    g = at(W + 0.01, H + 0.01, 0.20);   // 20 cm in front of the top right corner: nothing yet
    CHECK(g.kind == Guide::None);
    // (a hand shows one once it's near the panel: in the touch zone, or just out of it)
    const Guide outside = at(W + 0.06, H + 0.04, 0.02), inside = at(W - 0.005, H - 0.005, 0.01), atCorner = at(W, H, 0.0);
    std::printf("guides: the top right corner, the fingers 7 cm off it %.2f, 1 cm inside it %.2f, at it %.2f\n", outside.strength,
                inside.strength, atCorner.strength);
    CHECK(outside.kind == Guide::Corner && outside.sx == 1 && outside.sy == 1 && !outside.selected);
    CHECK(inside.kind == Guide::Corner && atCorner.kind == Guide::Corner);
    CHECK(outside.strength > 0 && outside.strength < 0.5 && inside.strength > outside.strength && atCorner.strength >= inside.strength &&
          atCorner.strength > 0.9);
    g = at(-W - 0.02, -H + 0.01, 0.01);   // the bottom left
    CHECK(g.kind == Guide::Corner && g.sx == -1 && g.sy == -1);
    g = at(W + 0.02, 0.05, 0.01);         // by the right edge, 5 cm above the middle: a segment there
    CHECK(g.kind == Guide::Edge && g.sx == 1 && g.sy == 0);
    NEAR(g.u, W, 1e-6), NEAR(g.v, 0.05, 0.01);
    g = at(-0.2, H + 0.02, 0.01);         // over the top edge
    CHECK(g.kind == Guide::Edge && g.sy == 1 && g.sx == 0);
    NEAR(g.u, -0.2, 0.01);
    // resizing from that corner: lit
    g = at(W - 0.01, -H + 0.01, 0.01, 3);
    CHECK(g.kind == Guide::Corner && g.selected && g.strength == 1 && g.sx == 1 && g.sy == -1);
    // the cutout buffer's corner and edge rings give way to it (HANDS_EDGE_GLOW)
    Rig r;
    SynthHand h = PinchAt(W + 0.01, H + 0.01, 0.01);
    h.open = 0.6;
    for (int k = 0; k < 5; ++k) r.Frame({h});
    bool ring = false;
    if (r.in.glows().count(0))
        for (const auto &x : r.in.glows().at(0)) ring = ring || x.ring;
    CHECK(!ring);
}

// A fist made at a panel: the hand closing (the thumb meets the index first: a pinch to ft-hands)
// waits to become a fist; a still pinch clicks at once; fists reach 11 cm in front, 10 cm behind.
SynthHand Closing(double u, double v, double palmD, double t, bool right = true, uint32_t id = 1) {
    SynthHand h = FistAt(u, v, palmD, right, id);
    h.curl = h.curlIndex = 0, h.curlT = t, h.curlIndexT = t;
    return h;
}
void TestFistSettle() {
    {   // closing: open -> half (the pinch begins, the others 1.3) -> a fist 67 ms later: a fist grab
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({Closing(0.1, 0, 0.05, 0)});
        r.Frame({Closing(0.1, 0, 0.05, 0.6)});
        const double at[3] = {0.1, 0, -0.45};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({Closing(0.1, 0, 0.05, 1.2)});
        CHECK(r.Count(Event::Press) == 0 && r.Count(Event::GrabStart) == 0 && r.in.stats.settles == 1);
        r.Frame({Closing(0.1, 0, 0.05, 1.7)});
        r.Frame({Closing(0.1, 0, 0.05, 2.0)});
        CHECK(r.Count(Event::Press) == 0 && r.Count(Event::GrabStart) == 1 && r.Last(Event::GrabStart)->zone == Zone::Fist);
        CHECK(r.in.stats.settledFists == 1);
        // before (no wait): the click on the content, as in the live test
        Rig o;
        o.in.params().fistSettleMs = 0;
        for (int k = 0; k < 5; ++k) o.Frame({Closing(0.1, 0, 0.05, 0)});
        o.Frame({Closing(0.1, 0, 0.05, 0.6)});
        o.synth.Pinch(1, true, o.t, at, 1);
        o.Frame({Closing(0.1, 0, 0.05, 1.2)});
        o.Frame({Closing(0.1, 0, 0.05, 2.0)});
        CHECK(o.Count(Event::Press) == 1 && o.Count(Event::GrabStart) == 0);
    }
    {   // a still pinch (the other fingers where they were): a click at once
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0.1, 0, 0.02)});
        const double at[3] = {0.1, 0, -0.48};
        r.synth.Pinch(1, true, r.t, at, 1);
        r.Frame({PinchAt(0.1, 0, 0.02)});
        CHECK(r.Count(Event::Press) == 1 && r.in.stats.settles == 0);
    }
    {   // closing, but it stays a pinch: the click 150 ms on, where it began
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({Closing(0.1, 0, 0.05, 0)});
        r.Frame({Closing(0.1, 0, 0.05, 0.6)});
        const double at[3] = {0.1, 0, -0.45};
        r.synth.Pinch(1, true, r.t, at, 1);
        int frames = 0;
        for (; frames < 10 && !r.Count(Event::Press); ++frames) r.Frame({Closing(0.1, 0, 0.05, 1.2)});
        std::printf("fist settle: a closing hand's pinch that stays a pinch clicks %d frame(s) on\n", frames - 1);
        CHECK(r.Count(Event::Press) == 1 && frames >= 4 && frames <= 7 && r.Count(Event::GrabStart) == 0);
    }
    // the fist's reach: 10 cm in front, 8.5 cm behind (the live log's "not at it"): it takes it; 14 cm: not
    for (double d : {0.10, -0.085, 0.14}) {
        Rig r;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.1, 0, d)});
        const double at[3] = {0.1, 0, -0.5 + d};
        r.synth.Grip(1, true, r.t, at, 1);
        r.Frame({FistAt(0.1, 0, d)});
        CHECK((r.Count(Event::GrabStart) == 1) == (d < 0.12));
        CHECK(r.in.claims(1) == (d < 0.12));   // (claimed: the pointer helper leaves its grip)
    }
}

// Two anchors: a second hand's pinch or fist anywhere on a panel the other hand holds joins it.
void TestJoin() {
    auto leftHolds = [](Rig &r) {
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(-0.45, 0, 0.05, false, 1)});
        const double l[3] = {-0.45, 0, -0.45};
        r.synth.Grip(0, true, r.t, l, 1);
        r.Frame({FistAt(-0.45, 0, 0.05, false, 1)});
        CHECK(r.in.grabbing(0));
    };
    {   // a pinch on the content: joins (no click), and they stretch it
        Rig r;
        r.in.params().fistResizeOnly = false;
        leftHolds(r);
        SynthHand p = PinchAt(0.2, 0.05, 0.03, true, 2);
        p.open = 0.6;
        for (int k = 0; k < 4; ++k) r.Frame({FistAt(-0.45, 0, 0.05, false, 1), p});
        const double at[3] = {0.2, 0.05, -0.47};
        r.synth.Pinch(1, true, r.t, at, 2);
        p.open = 0;
        r.Frame({FistAt(-0.45, 0, 0.05, false, 1), p});
        CHECK(r.Count(Event::Press) == 0 && r.in.grabbing(1) && r.in.stats.joins == 1);
        CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch && r.carries[0].sides == 3);
        // HANDS_JOIN_REACH=0: the pinch clicks the content (the live test's second hand)
        Rig o;
        o.in.params().joinFront = 0;
        leftHolds(o);
        p.open = 0.6;
        for (int k = 0; k < 4; ++k) o.Frame({FistAt(-0.45, 0, 0.05, false, 1), p});
        o.synth.Pinch(1, true, o.t, at, 2);
        p.open = 0;
        o.Frame({FistAt(-0.45, 0, 0.05, false, 1), p});
        CHECK(o.Count(Event::Press) == 1 && !o.in.grabbing(1));
    }
    {   // a fist 13 cm in front of the held panel (out of the fist's own reach): joins; then the pair moves
        // it (midpoint 10 cm right), scales it (20 cm further apart) and rolls it (the right one 10 cm up)
        Rig r;
        r.in.params().fistResizeOnly = false;
        leftHolds(r);
        for (int k = 0; k < 4; ++k) r.Frame({FistAt(-0.45, 0, 0.05, false, 1), FistAt(0.45, 0, 0.13, true, 2)});
        const double at[3] = {0.45, 0, -0.37};
        r.synth.Grip(1, true, r.t, at, 2);
        r.Frame({FistAt(-0.45, 0, 0.05, false, 1), FistAt(0.45, 0, 0.13, true, 2)});
        CHECK(r.in.grabbing(1) && r.in.stats.joins == 1 && r.Last(Event::GrabStart)->zone == Zone::Fist);
        const Mat start = r.panels[0].pose;
        for (int k = 1; k <= 45; ++k) {
            const double f = std::min(1.0, k / 30.0);
            r.Frame({FistAt(-0.45 + 0.1 * f - 0.1 * f, 0, 0.05, false, 1), FistAt(0.45 + 0.1 * f + 0.1 * f, 0.1 * f, 0.13, true, 2)});
        }
        CHECK(r.carries.size() == 1 && r.carries[0].kind == Carry::Stretch);
        if (r.carries.size() == 1) {
            double y = 0, p = 0, w = 0;
            FacingTurn(r.carries[0].pose, &y, &p, &w);
            const double moved = r.carries[0].pose.m[0][3] - start.m[0][3];
            std::printf("two anchors (a fist joining 13 cm out): the pair 10 cm right, 20 cm apart, the right 10 cm up -> moved "
                        "%.1f cm, %.2f m wide (from 1.00), roll %.1f deg\n", moved * 100, r.carries[0].width, w);
            CHECK(r.carries[0].width > 1.15 && r.carries[0].width < 1.30);
            NEAR(std::fabs(w), std::atan2(0.1, 1.1) * 180 / M_PI, 2.0);
            CHECK(moved > 0.06 && moved < 0.16);
        }
    }
    {   // the other hand's poke on it: not a press; its open hand pushed into it: not thrown
        Rig r;
        leftHolds(r);
        for (int mm = 50; mm >= -15; mm -= 10) {
            SynthHand tip = TipAt(0.2, 0.05, mm / 1000.0);
            tip.id = 2;
            r.Frame({FistAt(-0.45, 0, 0.05, false, 1), tip});
        }
        CHECK(r.Count(Event::Press) == 0 && r.in.stats.heldByOther >= 1);
        Rig q;
        leftHolds(q);
        for (int k = 0; k < 8; ++k) q.Frame({FistAt(-0.45, 0, 0.05, false, 1), OpenAt(0.2, 0, -0.42)});
        for (int k = 1; k <= 12; ++k) q.Frame({FistAt(-0.45, 0, 0.05, false, 1), OpenAt(0.2, 0, -0.42 - 0.02 * k)});
        CHECK(q.Count(Event::Throw) == 0 && q.in.grabbing(0));
    }
}

// The user's two-fist flags (2026-10-10 21:59:29-21:59:57, flags 215945 and 215955, one after the
// other): the right hand closes on the panel (ft-hands: a pinch, the thumb on the index first) at
// +0.81 s, the left grips at +1.21 s, both held 9 s. The panel's place isn't recorded: it's put where
// the live log's click had the right hand's fingers (on its content at 2600,654 of 3440x1440, 3.3 cm
// past its surface), 0.6 m wide, facing the eyes; and moved by a few cm either way. Played from half
// a second before that pinch.
bool LoadFlags(const std::vector<std::string> &dirs, std::vector<FlagFrame> &frames);
void TestFlagTwoFists() {
    const std::string a = FlagsDir() + "/flag-20261010-215945", b = FlagsDir() + "/flag-20261010-215955";
    std::vector<FlagFrame> fr;
    if (!LoadFlags({a, b}, fr) || fr.size() < 100) {
        std::printf("two fists: the flags aren't here, skipped\n");
        return;
    }
    std::vector<Json> man;
    std::ifstream mf(a + "/manifest.json");
    std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    const char *mp = text.c_str();
    Json m;
    if (!ParseJson(mp, m) || !m["flag_t_raw_ns"]) return;
    const int64_t flagT = int64_t(m["flag_t_raw_ns"]->num);
    // the right pinch's first frame after +0.7 s: its contact (room) and the eyes
    size_t at = 0;
    for (size_t i = 0; i < fr.size(); ++i)
        if (fr[i].t >= flagT + 700'000'000 && (fr[i].g.pinch[1].flags & FH_PINCH_DOWN)) {
            at = i;
            break;
        }
    double contact[3] = {0, 0, 0}, eye[3] = {fr[at].head.m[0][3], fr[at].head.m[1][3], fr[at].head.m[2][3]};
    for (uint32_t k = 0; k < fr[at].file->v1.nhands; ++k) {
        const fh_hand_t &h = fr[at].file->v1.hands[k];
        if (!(h.flags & FH_HAND_RIGHT)) continue;
        for (int i = 0; i < 3; ++i) {
            const double q[3] = {(h.pts[4][0] + h.pts[8][0]) / 2.0, (h.pts[4][1] + h.pts[8][1]) / 2.0, (h.pts[4][2] + h.pts[8][2]) / 2.0};
            contact[i] = fr[at].head.m[i][0] * q[0] + fr[at].head.m[i][1] * q[1] + fr[at].head.m[i][2] * q[2] + fr[at].head.m[i][3];
        }
    }
    const double W = 0.6, H = 0.6 * 1440 / 3440, u = (2600.0 / 3440 - 0.5) * W, v = (0.5 - 654.0 / 1440) * H, d = -0.033;
    if (std::getenv("HANDINPUT_LOG"))
        std::printf("two fists: the right pinch at %+.2f s, its fingers at %.3f %.3f %.3f (room), the eyes at %.3f %.3f %.3f\n",
                    (fr[at].t - flagT) / 1e9, contact[0], contact[1], contact[2], eye[0], eye[1], eye[2]);
    // (presses and grabs from the right hand's pinch to the end of the two-hand hold)
    struct Out { int presses = 0, grabs = 0, joins = 0, stretches = 0, settled = 0; double both = 0, w0 = 0, w1 = 0; };
    auto run = [&](const Params &k, double shift) {
        double c[3] = {contact[0], contact[1], contact[2]};
        Mat pose{};
        for (int it = 0; it < 8; ++it) {   // facing the eyes, the fingers at (u, v, d) on it
            pose = Facing(c, 0, eye).pose;
            for (int i = 0; i < 3; ++i) c[i] = contact[i] - (pose.m[i][0] * u + pose.m[i][1] * v + pose.m[i][2] * (d + shift));
        }
        pose = Facing(c, 0, eye).pose;
        Panel P = LivePanel();
        P.pose = pose;
        if (std::getenv("HANDINPUT_LOG")) {
            const Hit h = Locate(P, contact);
            std::printf("two fists: the panel at %.3f %.3f %.3f, the right pinch's fingers on it at %+.3f %+.3f %+.3f\n", pose.m[0][3],
                        pose.m[1][3], pose.m[2][3], h.u, h.v, h.d);
        }
        std::vector<Panel> panels{P};
        handcut::Hands hands;
        HandInput in(k);
        grabthat::Detector det;
        Out o;
        o.w0 = W;
        int64_t bothSince = -1;
        // (from half a second before the right hand's pinch: earlier grabs would have moved the panel)
        const size_t from = at > 15 ? at - 15 : 0;
        bool held = false;   // (the two-hand hold has begun; presses after it ends are another gesture)
        PlayFrames(fr, from, fr.size(), hands, in, &det, panels, [&](int64_t now, bool) {
            // (from the right hand's pinch: with the panel 4 cm nearer, the left index pokes it on the way in)
            const bool over = (held && bothSince < 0) || now < fr[at].t;
            for (const Event &e : in.events()) o.presses += e.type == Event::Press && !over, o.grabs += e.type == Event::GrabStart && !over;
            if (std::getenv("HANDINPUT_LOG"))
                for (const std::string &l : in.logs()) std::printf("    [%+.2f] %s\n", (now - flagT) / 1e9, l.c_str());
            if (std::getenv("TWOFISTS_TRACE") && in.newFrame() && (now - flagT) / 1e9 > 0.6 && (now - flagT) / 1e9 < 1.5) {
                std::printf("    [%+.2f] %s; pinch %d/%d grip %d/%d (begins %u %u)", (now - flagT) / 1e9, in.Describe().c_str(),
                            hands.pinch(0).down, hands.pinch(1).down, hands.grip(0).down, hands.grip(1).down, hands.pinch(1).begins,
                            hands.grip(0).begins);
                for (const handcut::Hand &h : hands.seen()) {
                    double pc[3];
                    h.Palm(pc);
                    const Hit c = Locate(panels[0], h.contact), q = Locate(panels[0], pc);
                    std::printf("; %s fingers %+.3f %+.3f %+.3f palm %+.3f", h.right ? "R" : "L", c.u, c.v, c.d, q.d);
                }
                std::printf("\n");
            }
            const bool both = in.grabbing(0) && in.grabbing(1) && in.grabPanel(0) == in.grabPanel(1);
            if (both && bothSince < 0) bothSince = now, held = true;
            if (!both && bothSince >= 0) o.both = std::max(o.both, (now - bothSince) / 1e9), bothSince = -1;
            ApplyCarries(in, panels);
        });
        if (bothSince >= 0) o.both = std::max(o.both, (fr.back().t + 30'000'000 - bothSince) / 1e9);
        o.joins = in.stats.joins, o.stretches = in.stats.stretches, o.settled = in.stats.settledFists, o.w1 = panels[0].width;
        return o;
    };
    Params before;   // as the live test ran
    before.fistSettleMs = 0, before.joinFront = 0, before.fistFront = 0, before.fistBack = 0, before.fistClaimCurl = 0;
    before.fistPair = false;
    const Out was = run(before, 0);
    std::printf("two fists, the user's flags (before): %d press(es), %d grab(s), %d stretch(es), both hands %.1f s\n", was.presses,
                was.grabs, was.stretches, was.both);
    CHECK(was.presses >= 1 && was.both < 1);   // the live failure: a click, no two-hand hold
    for (double shift : {0.0, -0.04, 0.04, 0.08}) {
        if (std::getenv("TWOFISTS_SHIFT") && std::fabs(std::atof(std::getenv("TWOFISTS_SHIFT")) - shift) > 1e-6) continue;
        const Out now = run(Params{}, shift);
        std::printf("two fists, the user's flags (now, the panel %+.0f cm): %d press(es), %d grab(s) (%d joined, %d settled to a "
                    "fist), %d stretch(es), both hands %.1f s, %.2f m wide after\n", shift * 100, now.presses, now.grabs, now.joins,
                    now.settled, now.stretches, now.both, now.w1);
        CHECK(now.presses == 0 && now.stretches >= 1 && now.both >= 5);
    }
}

// The soft white glow's textures (softglow.h): clear over the panel, at most half opaque at its edge,
// gone a halo's width out; a corner's L, an edge's segment, a button's ring.
void TestSoftGlow() {
    auto alpha = [](const std::vector<uint8_t> &px, int w, int x, int y) { return px[(size_t(y) * w + x) * 4 + 3] / 255.0; };
    auto dump = [](const char *name, const std::vector<uint8_t> &px, int w, int h) {   // (GLOW_DUMP=dir: PGMs to look at)
        const char *dir = std::getenv("GLOW_DUMP");
        if (!dir) return;
        std::FILE *f = std::fopen((std::string(dir) + "/" + name + ".pgm").c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "P5 %d %d 255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) std::fputc(px[i * 4 + 3], f);
        std::fclose(f);
    };
    {   // the target: 512 wide, the halo 9 px (1 cm round a 0.6 m panel at arm's reach)
        const int w = 512, m = 9, h = 230;
        const auto px = softglow::TargetTexture(w, h, m);
        dump("target", px, w, h);
        double inside = 0;
        for (int y = m; y < h - m; ++y)
            for (int x = m; x < w - m; ++x) inside = std::max(inside, alpha(px, w, x, y));
        CHECK(inside == 0);                                   // nothing over the content
        CHECK(alpha(px, w, w / 2, m - 1) > 0.35 && alpha(px, w, w / 2, m - 1) <= 0.5);   // at its edge: soft, at most half
        CHECK(alpha(px, w, w / 2, 0) < 0.02 && alpha(px, w, 0, h / 2) < 0.02);         // gone at the halo's outer edge
        CHECK(alpha(px, w, m - 1, m - 1) > 0.2);              // round the corners too
        NEAR(softglow::Width(0.45), 0.0099, 1e-4), NEAR(softglow::Width(0.1), 0.008, 1e-9), NEAR(softglow::Width(5), 0.05, 1e-9);
    }
    {   // a hand's guide at the top right corner: an L outside it, clear inside, fading along the edges
        Guide g;
        g.kind = Guide::Corner, g.sx = 1, g.sy = 1;
        const int n = 128;
        const double size = 0.105, halo = 0.01, arm = 0.05, perM = n / size;
        const auto px = softglow::GuideTexture(g, n, n, size, halo, arm, 0, 1e9);
        dump("corner", px, n, n);
        auto at = [&](double X, double Y) { return alpha(px, n, int(n / 2.0 + X * perM), int(n / 2.0 - Y * perM)); };
        CHECK(at(-0.01, -0.01) == 0 && at(-0.04, -0.002) == 0);    // inside the panel: clear
        CHECK(at(0.002, -0.01) > 0.3 && at(-0.01, 0.002) > 0.3);   // just past the right and top edges by the corner
        CHECK(at(0.003, 0.003) > 0.12);                            // round the corner itself
        CHECK(at(0.002, -0.052) < 0.02 && at(-0.052, 0.002) < 0.02);   // gone an arm's length along each edge
        CHECK(at(0.012, -0.01) == 0);                              // a halo's width out: gone
        g.selected = true;
        const auto lit = softglow::GuideTexture(g, n, n, size, halo, arm, 0, 1e9);
        CHECK(alpha(lit, n, int(n / 2.0 + 0.002 * perM), int(n / 2.0 + 0.01 * perM)) > at(0.002, -0.01) + 0.15);   // brighter held
    }
    {   // the bottom edge's segment: outside (under) it only, fading toward its ends
        Guide g;
        g.kind = Guide::Edge, g.sy = -1;
        const int w = 128, h = 16;
        const double len = 0.12, perM = w / len;
        const auto px = softglow::GuideTexture(g, w, h, len, 0.01, len, 0, 1e9);
        dump("edge", px, w, h);
        CHECK(alpha(px, w, w / 2, h / 2 - 2) == 0);                // above the edge (the panel): clear
        CHECK(alpha(px, w, w / 2, h / 2 + 1) > 0.3);               // just under it
        CHECK(alpha(px, w, 1, h / 2 + 1) < 0.02);                  // its ends faded
        (void)perM;
    }
    {   // a button's ring: clear over the button and above the panel's edge
        Guide g;
        g.kind = Guide::Button;
        const int n = 64;
        const double r = 0.004, hb = 0.004, size = 2 * (r + hb), perM = n / size;
        const auto px = softglow::GuideTexture(g, n, n, size, hb, 0, r, 0.006);
        dump("button", px, n, n);
        CHECK(alpha(px, n, n / 2, n / 2) == 0);                                       // over the button
        CHECK(alpha(px, n, int(n / 2.0 + (r + 0.0005) * perM), n / 2) > 0.3);         // just round it
        CHECK(alpha(px, n, n / 2, int(n / 2.0 - 0.0065 * perM)) == 0);                 // above the panel's edge (6 mm up): masked
    }
}

// --grabthat FLAG_DIR...: the fires in each flag (false fires: flags taken for something else).
int ScanFlags(int n, char **dirs) {
    int total = 0, totals[4] = {0, 0, 0, 0};   // pushes, palm menu toggles, flicks with the panel at arm's reach; the old palm menu's toggles
    double all = 0;
    grabthat::Params gk;   // (GRABTHAT_FACING, GRABTHAT_SPEED: other thresholds, for tuning)
    if (const char *e = std::getenv("GRABTHAT_FACING")) gk.aimFacing = std::atof(e);
    if (const char *e = std::getenv("GRABTHAT_SPEED")) gk.holdSpeed = std::atof(e);
    for (int i = 0; i < n; ++i) {
        std::vector<grabthat::Fire> fires;
        double flagT = 0, secs = 0;
        int claimed = 0, arms = 0;
        if (!ReplayFlag(dirs[i], fires, &flagT, &secs, &claimed, &arms, gk)) {
            std::printf("%s: can't read\n", dirs[i]);
            continue;
        }
        std::printf("%s: %.0f s, armed %d, claimed %d frames, %zu fire(s)", dirs[i], secs, arms, claimed, fires.size());
        for (const auto &f : fires)
        {
            std::printf(" %s at %+.2f s (palm %.0f deg off the aim, %.2f m/s at most", f.side ? "right" : "left", (f.t - flagT) / 1e9,
                        f.facing, f.speed);
            // the line from the eyes through the palm, seen from the head: degrees right and up of straight ahead
            double d[3], h[3];
            for (int q = 0; q < 3; ++q) d[q] = f.palm[q] - f.eye[q];
            for (int q = 0; q < 3; ++q) h[q] = f.head.m[0][q] * d[0] + f.head.m[1][q] * d[1] + f.head.m[2][q] * d[2];
            std::printf("; the eyes' line through it %+.0f deg right, %+.0f deg up, %.2f m out", std::atan2(h[0], -h[2]) * 180 / M_PI,
                        std::atan2(h[1], std::hypot(h[0], h[2])) * 180 / M_PI, std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]));
            for (int q = 0; q < 3; ++q) h[q] = f.head.m[0][q] * f.normal[0] + f.head.m[1][q] * f.normal[1] + f.head.m[2][q] * f.normal[2];
            std::printf("; the palm faces %+.0f deg right, %+.0f deg up)", std::atan2(h[0], -h[2]) * 180 / M_PI,
                        std::atan2(h[1], std::hypot(h[0], h[2])) * 180 / M_PI);
        }
        {   // ...as before the fourth live test (no facing or stillness rule)
            grabthat::Params loose;
            loose.aimFacing = 0, loose.holdSpeed = 0;
            std::vector<grabthat::Fire> was;
            double t2 = 0, s2 = 0;
            if (ReplayFlag(dirs[i], was, &t2, &s2, nullptr, nullptr, loose) && was.size() != fires.size()) {
                std::printf("; without the facing and stillness rules %zu:", was.size());
                for (const auto &f : was)
                    std::printf(" %s at %+.2f s (%.0f deg, %.2f m/s)", f.side ? "right" : "left", (f.t - t2) / 1e9, f.facing, f.speed);
            }
        }
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
            palmmenu::PalmMenu palm;
            if (FlagThroughHandInput(dirs[i], {P}, run, Params{}, false, &palm)) {
                int presses = 0, grabs = 0;
                for (const Event &e : run.events) presses += e.type == Event::Press, grabs += e.type == Event::GrabStart;
                std::printf("  with a panel at arm's reach: near %d of %d frames, %d presses (%d by pinch), %d grabs (%d fists, %d of them "
                            "pinches to ft-hands; %d two-hand), %d flicks, %d pushes, %zu fire(s); palm menu: %d shown, %d toggles, %d "
                            "pinches not plain\n", run.stats.nearFrames, run.stats.frames, presses, run.stats.pinchClicks, grabs,
                            run.stats.fists, run.stats.fistPinches, run.stats.stretches, run.stats.flicks, run.stats.pushes,
                            run.fires.size(), run.palm.shows, run.palm.toggles, run.palm.notPlain);
                for (const std::string &l : run.logs)
                    if (l.find("pressed") != std::string::npos || l.find("took") != std::string::npos || l.find("flick") != std::string::npos ||
                        l.find("two fingers") != std::string::npos || l.find("hand opening") != std::string::npos ||
                        l.find("lazy susan") != std::string::npos ||
                        l.find("closed again") != std::string::npos || l.find("let go of") != std::string::npos ||
                        l.find("push") != std::string::npos || l.find("both hands") != std::string::npos)
                        std::printf("    %s\n", l.c_str());
                totals[0] += run.stats.pushes, totals[1] += run.palm.toggles, totals[2] += run.stats.flicks;
                // ...and the palm menu as it was before the third live test (palm 60/75 deg, no stillness,
                // panel or quiet rule, any pinch), no panels: what it did then
                palmmenu::PalmMenu was;
                palmmenu::Params &q = was.params();
                q.facing = 60, q.facingLeave = 75, q.still = 1e9, q.quietMs = 0, q.plainMiddle = 0, q.plainExtended = 0, q.pinchStill = 1e9;
                FlagRun old;
                if (FlagThroughHandInput(dirs[i], {}, old, Params{}, false, &was) && (old.palm.shows || old.palm.toggles))
                    std::printf("  the palm menu before the third live test (no panels): %d shown, %d toggles\n", old.palm.shows, old.palm.toggles);
                totals[3] += old.palm.toggles;
            }
        }
        total += int(fires.size()), all += secs;
    }
    std::printf("%d fire(s) in %.0f s; with a panel at arm's reach: %d pushes, %d flicks, %d palm menu toggles (%d with the old "
                "palm menu and no panels)\n", total, all, totals[0], totals[2], totals[1], totals[3]);
    return 0;
}


// SteamVR's own panels (steampanels.h, vr.cpp's "SteamVR's panels by hand"): the fit from a cone's
// hits, and the laser's touch, grab point and carry.
void TestSteamPanels() {
    using namespace steampanels;
    // a 0.8 x 0.5 m panel 0.6 m ahead, 1.1 m up, turned 0.4 rad: the hits on a patch of it
    const Mat P = RotY(0.4, 0.2, 1.1, -0.6);
    const double W = 0.8, H = 0.5;
    auto pointAt = [&](double u, double v, double out[3]) {   // texture u, v (v up) to the room
        for (int i = 0; i < 3; ++i) out[i] = P.m[i][3] + P.m[i][0] * (u - 0.5) * W + P.m[i][1] * (v - 0.5) * H;
    };
    std::vector<Sample> s;
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 7; ++j) {
            Sample x;
            x.u = 0.3 + 0.05 * i, x.v = 0.6 + 0.04 * j;
            pointAt(x.u, x.v, x.p);
            s.push_back(x);
        }
    const Fit f = FitPanel(s);
    CHECK(f.ok && f.hits == 49);
    NEAR(f.width, W, 1e-4), NEAR(f.height, H, 1e-4);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) NEAR(f.pose.m[i][j], P.m[i][j], 1e-4);
    // ...a line of hits fits no plane; too few hits fit nothing
    std::vector<Sample> line;
    for (const Sample &x : s)
        if (std::fabs(x.v - 0.6) < 1e-9) line.push_back(x);
    CHECK(!FitPanel(line).ok && !FitPanel(std::vector<Sample>(s.begin(), s.begin() + 5)).ok);
    // ...a speck (2 cm across) isn't a panel
    std::vector<Sample> speck = s;
    for (Sample &x : speck) x.p[0] = P.m[0][3] + (x.u - 0.5) * 0.02, x.p[1] = P.m[1][3] + (x.v - 0.5) * 0.02, x.p[2] = P.m[2][3];
    CHECK(!FitPanel(speck).ok);

    // the cone: n x n unit rays, the middle one along the line, the corners halfDeg off each way
    const double eye[3] = {0, 1.6, 0}, tip[3] = {0.1, 1.3, -0.5};
    const auto rays = Cone(eye, tip, 12, 7);
    CHECK(rays.size() == 49);
    double ax[3] = {tip[0] - eye[0], tip[1] - eye[1], tip[2] - eye[2]};
    const double al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    double most = 0;
    for (const auto &r : rays) {
        NEAR(std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), 1.0, 1e-9);
        most = std::max(most, std::acos(std::min(1.0, (r[0] * ax[0] + r[1] * ax[1] + r[2] * ax[2]) / al)) * 180 / M_PI);
    }
    NEAR((rays[24][0] * ax[0] + rays[24][1] * ax[1] + rays[24][2] * ax[2]) / al, 1.0, 1e-9);
    NEAR(most, std::atan(std::tan(12 * M_PI / 180) * std::sqrt(2.0)) * 180 / M_PI, 0.01);

    // pixels (a millimetre each) to the point on it
    double u = 0, v = 0;
    FromPixels(W, H, 400, 250, &u, &v);
    NEAR(u, 0, 1e-9), NEAR(v, 0, 1e-9);
    FromPixels(W, H, 0, 0, &u, &v);
    NEAR(u, -0.4, 1e-9), NEAR(v, 0.25, 1e-9);
    // the touch: 10 cm out in front of the point, turned as the panel is (its -z aims in)
    const Mat t = Touch(P, 0.1, -0.05, 0.1);
    for (int i = 0; i < 3; ++i) {
        NEAR(t.m[i][3], P.m[i][3] + P.m[i][0] * 0.1 - P.m[i][1] * 0.05 + P.m[i][2] * 0.1, 1e-5);
        for (int j = 0; j < 3; ++j) NEAR(t.m[i][j], P.m[i][j], 1e-6);
    }
    // where a move and a resize press
    GrabPoint(W, H, false, 0, 0, 0.075, 0.01, &u, &v);
    NEAR(u, 0, 1e-9), NEAR(v, -0.325, 1e-9);
    GrabPoint(W, H, true, 1, -1, 0.075, 0.01, &u, &v);
    NEAR(u, 0.41, 1e-9), NEAR(v, -0.26, 1e-9);
    GrabPoint(W, H, true, -1, 1, 0.075, 0.01, &u, &v);
    NEAR(u, -0.41, 1e-9), NEAR(v, 0.26, 1e-9);
    // the laser follows a carried panel: where it touched it, it still does
    const Mat Q = RotY(-0.3, -0.4, 1.3, -0.5);
    const Mat moved = Follow(P, Q, t), want = Touch(Q, 0.1, -0.05, 0.1);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) NEAR(moved.m[i][j], want.m[i][j], 1e-5);
    // the quaternion: about y by 0.4, and one past 90 degrees (the trace below zero), unit with w >= 0
    double q[4];
    Quat(P, q);
    NEAR(q[0], std::cos(0.2), 1e-6), NEAR(q[1], 0, 1e-6), NEAR(q[2], std::sin(0.2), 1e-6), NEAR(q[3], 0, 1e-6);
    Quat(RotY(3.0, 0, 0, 0), q);
    NEAR(q[0], std::cos(1.5), 1e-5), NEAR(q[2], std::sin(1.5), 1e-5);
    Quat(RotY(-3.0, 0, 0, 0), q);
    CHECK(q[0] >= 0), NEAR(std::fabs(q[2]), std::sin(1.5), 1e-5), NEAR(q[0] * q[0] + q[2] * q[2], 1.0, 1e-5);
}

// SteamVR's panels in near input (Panel::foreign): a poke and a pinch click, the border and corners
// take them (vr.cpp moves and resizes them with the laser), but nothing throws, flicks, pushes or
// rewinds them, a fist doesn't take them, and a second hand doesn't join one.
void TestForeignPanel() {
    auto rig = [](Rig &r) {
        r.panels[0].index = 1000, r.panels[0].foreign = true, r.panels[0].controls.clear();
        r.panels[0].pxW = 1000, r.panels[0].pxH = 563;
    };
    {   // a poke presses where it touches
        Rig r;
        rig(r);
        for (int mm = 100; mm >= -15; mm -= 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Count(Event::Press) == 1);
        if (const auto p = r.Last(Event::Press)) CHECK(p->panel == 1000), NEAR(p->x, 600, 3), NEAR(p->y, 563 * (0.5 - 0.05 / 0.5625), 3);
        for (int mm = -15; mm <= 50; mm += 10) r.Frame({TipAt(0.1, 0.05, mm / 1000.0)});
        CHECK(r.Count(Event::Release) == 1);
    }
    {   // a pinch on the content clicks
        Rig r;
        rig(r);
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(0, 0, 0.01)});
        const double mid[3] = {0, 0, -0.49};
        r.synth.Pinch(1, true, r.t, mid, 1);
        r.Frame({PinchAt(0, 0, 0.01)});
        CHECK(r.Count(Event::GrabStart) == 0 && r.Count(Event::Press) == 1);
    }
    {   // its edge and its corner take nothing (no grabs on SteamVR's panels: GrabZone), nor glow as if they would
        for (const double at : {0.0, 1.0}) {
            Rig r;
            rig(r);
            const double x = 0.55, y = at > 0 ? 0.30 : 0.0;   // the right edge, then the top right corner
            for (int k = 0; k < 5; ++k) r.Frame({PinchAt(x - (at > 0 ? 0.02 : 0), y, 0.01)});
            const double p[3] = {x - (at > 0 ? 0.02 : 0), y, -0.49};
            r.synth.Pinch(1, true, r.t, p, 1);
            for (int k = 0; k < 5; ++k) r.Frame({PinchAt(x - (at > 0 ? 0.02 : 0), y, 0.01)});
            CHECK(r.Count(Event::GrabStart) == 0 && !r.in.grabbing(1));
            CHECK(r.carries.empty());
        }
    }
    {   // a fist doesn't take it
        Rig r;
        rig(r);
        r.in.params().fistResizeOnly = false;
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        const double at[3] = {0.0, 0, -0.45};
        r.synth.Grip(1, true, r.t, at, 1);
        for (int k = 0; k < 5; ++k) r.Frame({FistAt(0.0, 0, 0.05)});
        CHECK(r.Count(Event::GrabStart) == 0 && !r.in.grabbing(1));
    }
    {   // a flick out through it and an open hand pushed into it: no throw, no throw aim
        const double a[3] = {0.05, 0, -0.44}, b[3] = {0.05, 0, -0.76};
        Rig r;
        rig(r);
        auto flick = [&](double x, double y, double z) {
            SynthHand h = OpenAt(x, y, z);
            if (z > -0.55) h.curl = h.curlIndex = 1;
            return h;
        };
        for (int k = 0; k < 8; ++k) r.Frame({flick(a[0], a[1], a[2])});
        Sweep(r, a, b, 1.5, flick);
        CHECK(r.Count(Event::Throw) == 0);
        Rig p;
        rig(p);
        const double c[3] = {0.05, 0, -0.42}, d[3] = {0.05, 0, -0.58};
        p.Frame({OpenAt(0.05, 0, -0.45)}), p.Frame({OpenAt(0.05, 0, -0.45)});
        CHECK(!p.in.throwAims().count(1000));
        for (int k = 0; k < 8; ++k) p.Frame({OpenAt(c[0], c[1], c[2])});
        Sweep(p, c, d, 0.6, [](double x, double y, double z) { return OpenAt(x, y, z); });
        CHECK(p.Count(Event::Throw) == 0);
    }
    {   // the left hand's pinch at its left edge takes nothing; the right hand's pinch on the content clicks
        Rig r;
        rig(r);
        for (int k = 0; k < 5; ++k) r.Frame({PinchAt(-0.55, 0, 0.01, false, 1)});
        const double l[3] = {-0.55, 0, -0.49};
        r.synth.Pinch(0, true, r.t, l, 1);
        r.Frame({PinchAt(-0.55, 0, 0.01, false, 1)});
        CHECK(!r.in.grabbing(0));
        SynthHand p = PinchAt(0.2, 0.05, 0.01, true, 2);
        p.open = 0.6;
        for (int k = 0; k < 4; ++k) r.Frame({PinchAt(-0.55, 0, 0.01, false, 1), p});
        const double at[3] = {0.2, 0.05, -0.49};
        r.synth.Pinch(1, true, r.t, at, 2);
        p.open = 0;
        r.Frame({PinchAt(-0.55, 0, 0.01, false, 1), p});
        CHECK(r.in.stats.joins == 0 && !r.in.grabbing(1) && r.Count(Event::Press) == 1);
    }
}

}  // namespace

// A testing session's take (hands/tools/live_dump.py: its live.bin as a dump, poses beside it) played
// through hand input with no panels, every log line with its time: seconds from t0 (CLOCK_MONOTONIC ns,
// the take's started_ns, so the times read as the take's events.jsonl and marks), from..to only.
// TAKE_SWEEP=1 traces the lazy susan's strokes frame by frame (Params::sweepTrace).
int Take(const char *path, int64_t t0, double from, double to) {
    std::vector<Record> recs;
    if (!Load(path, recs)) return std::printf("%s: not a publish dump\n", path), 1;
    std::string base = path;
    if (base.size() > 5 && base.substr(base.size() - 5) == ".dump") base.resize(base.size() - 5);
    const auto poses = LoadPoses(base + ".poses.jsonl");
    if (poses.empty() || !recs.front().dq) return std::printf("%s: no head poses beside it\n", path), 1;
    size_t j = 0;
    for (Record &r : recs) {
        const int64_t at = r.dq - 20'000'000;
        while (j + 1 < poses.size() && std::llabs(poses[j + 1].first - at) <= std::llabs(poses[j].first - at)) ++j;
        r.head = poses[j].second;
    }
    if (!t0) t0 = recs.front().capture;
    Params params;
    params.sweepTrace = std::getenv("TAKE_SWEEP") != nullptr;
    if (const char *v = std::getenv("SWEEP_OPEN")) params.sweepOpen = std::atof(v);
    if (const char *v = std::getenv("SWEEP_SOLO_OPEN")) params.sweepSoloOpen = std::atof(v);
    if (const char *v = std::getenv("SWEEP_BUSY_OPEN")) params.sweepBusyOpen = std::atof(v);
    handcut::Hands hands;
    HandInput in(params);
    const std::vector<Panel> none;
    for (size_t k = 0; k < recs.size(); ++k) {
        const Record &r = recs[k];
        const int64_t next = k + 1 < recs.size() ? recs[k + 1].publish : r.publish + 33'000'000;
        const double at = (r.capture - t0) / 1e9;
        for (int64_t t = r.publish; t < next; t += 11'111'111) {
            if (t == r.publish) hands.Feed(nullptr, 0, nullptr, r.head, r.capture), hands.Feed(r.hands.data(), r.hands.size(), &r.g, r.head, t);
            else hands.Feed(nullptr, 0, nullptr, r.head, t);
            Input i;
            i.nowNs = t, i.hands = &hands, i.head = r.head, i.haveHead = true, i.allowed[0] = i.allowed[1] = true;
            in.Update(i, none);
            if (at >= from && at <= to)
                for (const std::string &l : in.logs()) std::printf("[%7.3f] %s\n", at, l.c_str());
            if (at >= from && at <= to && t == r.publish && std::getenv("TAKE_REST"))   // where the palms are (Params::restBelow)
                for (const handcut::Hand &h : hands.seen()) {
                    double palm[3];
                    h.Palm(palm);
                    auto ext = [&](int mcp) {   // handinput.cpp's FingerExtension
                        auto d = [&](int a) { return double(std::hypot(std::hypot(h.p[a][0] - h.p[0][0], h.p[a][1] - h.p[0][1]), h.p[a][2] - h.p[0][2])); };
                        return d(mcp + 3) / std::max(1e-6, d(mcp));
                    };
                    std::printf("[%7.3f] %s palm %.0f cm under the eyes, %.0f cm out, index tip %.0f cm under, index %.2f, others %.2f %.2f %.2f%s\n", at,
                                h.right ? "right" : "left", (r.head.m[1][3] - palm[1]) * 100,
                                std::hypot(palm[0] - r.head.m[0][3], palm[2] - r.head.m[2][3]) * 100, (r.head.m[1][3] - h.p[8][1]) * 100, ext(5),
                                ext(9), ext(13), ext(17), h.stereo ? "" : " (one camera)");
                }
        }
    }
    return 0;
}

// The lazy susan over the user's flags (each dir, or every flag under FlagsDir() without any), no
// panels: each flag's strokes counted and refused, with their log lines (SWEEPS_LOG=1). For trying a
// change to Params::sweep against what the flags held.
int Sweeps(int n, char **dirs) {
    std::vector<std::string> list;
    for (int i = 0; i < n; ++i) list.push_back(dirs[i]);
    if (list.empty()) {
        std::vector<std::string> names;
        if (DIR *d = opendir(FlagsDir().c_str())) {
            while (const dirent *e = readdir(d))
                if (!std::strncmp(e->d_name, "flag-", 5)) names.push_back(e->d_name);
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &s : names) list.push_back(FlagsDir() + "/" + s);
    }
    Params k;
    if (const char *v = std::getenv("SWEEP_OPEN")) k.sweepOpen = std::atof(v);   // trying other values
    if (const char *v = std::getenv("SWEEP_SOLO_OPEN")) k.sweepSoloOpen = std::atof(v);
    if (const char *v = std::getenv("SWEEP_BUSY_OPEN")) k.sweepBusyOpen = std::atof(v);
    int spins = 0, refused = 0;
    for (const std::string &dir : list) {
        FlagRun r;
        if (!FlagThroughHandInput(dir, {}, r, k)) continue;
        int s = 0;
        for (const Event &e : r.events) s += e.type == Event::Spin;
        spins += s, refused += r.stats.sweepsRefused;
        std::printf("%s: %d stroke(s), %d refused\n", dir.substr(dir.rfind('/') + 1).c_str(), s, r.stats.sweepsRefused);
        if (std::getenv("SWEEPS_LOG"))
            for (const std::string &l : r.logs)
                if (l.find("swept") != std::string::npos) std::printf("    %s\n", l.c_str());
    }
    std::printf("== %zu flags: %d strokes, %d refused\n", list.size(), spins, refused);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--sweeps")) return Sweeps(argc - 2, argv + 2);
    if (argc > 2 && !std::strcmp(argv[1], "--take"))
        return Take(argv[2], argc > 3 ? std::atoll(argv[3]) : 0, argc > 4 ? std::atof(argv[4]) : -1e9, argc > 5 ? std::atof(argv[5]) : 1e9);
    if (argc > 1 && !std::strcmp(argv[1], "--grabthat")) return ScanFlags(argc - 2, argv + 2);
    if (argc > 1 && !std::strcmp(argv[1], "--carries")) return ScanCarries(argc - 2, argv + 2);
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
    TestActs();
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
    TestGrabResizeCorners();
    TestStretchHands();
    TestLetGoStill();
    TestHoldThroughLoss();
    TestForgivingZones();
    TestCardPinch();
    TestFistGrab();
    TestFlick();
    TestCardThrow();
    TestTrackGate();
    TestSweep();
    TestSusanPick();
    TestReadableSize();
    TestViewDistance();
    TestFistPair();
    TestFistResizeOnly();
    TestFistPinch();
    TestPush();
    TestPinchSlack();
    TestRemoteDisplay();
    TestV2Ingest();
    TestMatte();
    TestPalmUp();
    TestPalmMenuShows();
    TestPalmMenuPinch();
    TestPalmMenuStrict();
    TestGrabThatGesture();
    TestGrabThatPlace();
    TestGrabThatFlag();
    TestGrabThatAtNearPanel();
    TestGrabThatAfterThrow();
    TestButtons();
    TestTouchDot();
    TestGuides();
    TestFistSettle();
    TestJoin();
    TestFlagTwoFists();
    TestSoftGlow();
    TestSteamPanels();
    TestForeignPanel();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
