// Hand cutouts: where a tracked hand is between an eye and a screen, that eye sees the
// room (Room View) through the screen instead of the screen drawn over the hand.
//
// ft-hands (hands/) publishes the hands it sees with the headset's cameras to
// /run/user/UID/frametop-hands/hands (hands/include/fh_hands.h), in the head frame at capture
// time: version 1 has 21 points and capsules per hand, version 2 adds the fitted skeleton
// (joints with radii, bone lengths, an elbow, the pinch point). Hands turns them into the room
// with the head pose at that time, and reads the gestures file next to it (pinches, grips) for
// hand input (handinput.h).
//
// The matte (what's cut out) is built from the hands each tick (BuildMatte): either the
// capsules ft-hands publishes, or a hand-shaped one of convex pieces: a tapered capsule per
// finger bone, the palm as a slab through the wrist and knuckles, the thumb's web, and the
// forearm toward the elbow as a tapered tube. Project() finds where each eye sees them on a
// panel, and Renderer draws the panel's client buffer into a side-by-side buffer (left eye |
// right eye) with those spots transparent and the glows of hand input (fingertip, pinch point)
// on top. A panel shows that buffer, with the overlay's SideBySide_Parallel flag, only while a
// hand is in front of it.
//
// The hands arrive some 30-60 ms after the cameras saw them, and show up on the displays
// later still, so Hands moves each hand ahead along its velocity in the room to where it
// will be when the frame reaches the eyes.
//
// This file and handcut.cpp have no GPU or SteamVR calls (tests use them); the Renderer and
// EyePositions are in handrender.cpp.
#pragma once

#include "vr.h"

#include "../hands/include/fh_gestures.h"
#include "../hands/include/fh_hands.h"

#if __has_include(<openvr.h>)
#include <openvr.h>
#else
// Offline tests build without OpenVR's header: only its matrix type is used here.
namespace vr {
struct HmdMatrix34_t {
    float m[3][4];
};
}  // namespace vr
#endif

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace handcut {

using Mat = vr::HmdMatrix34_t;

struct Capsule {         // in the room (standing universe), metres
    float a[3], b[3];
    float ra, rb;
};

// A convex piece of the matte (the palm, the thumb's web, the forearm): the convex hull of
// these points, in the room, metres.
struct Hull {
    std::vector<std::array<float, 3>> v;
};

struct Matte {
    std::vector<Capsule> caps;
    std::vector<Hull> hulls;
    // How much wider than the hand each piece is drawn (metres at the hand), per piece: the
    // capsules have it in their radii already; a hull grows by it on the panel.
    std::vector<float> hullMargin;
    void clear() { caps.clear(), hulls.clear(), hullMargin.clear(); }
};

struct Capsule2D {       // on a panel's texture, pixels from the top left
    float ax, ay, bx, by;
    float ra, rb;
};

// A convex polygon on a panel's texture: inside where every edge's n . p - c <= 0, grown
// by r pixels (a soft edge either side of that).
constexpr int kMaxHullEdges = 16;
struct Hull2D {
    int n = 0;
    float e[kMaxHullEdges][3];   // nx, ny, c (n points out)
    float r = 0;
    float box[4];                // x0 y0 x1 y1 around it, r and the feather included
    float fan[kMaxHullEdges][2]; // the polygon grown by r and the feather: what's drawn (n corners)
};

// A glow of hand input (handinput.cpp), drawn on top of the panel at a texture point: a soft
// ring (ring true) or a filled dot, radius in pixels, colour and opacity.
struct Glow {
    float x, y, r, alpha;
    bool ring;
    float rgb[3];
};

// What's drawn on one eye's half of a panel's cutout buffer.
struct Spots {
    std::vector<Capsule2D> caps;
    std::vector<Hull2D> hulls;
    std::vector<Glow> glows;
    bool empty() const { return caps.empty() && hulls.empty() && glows.empty(); }
    void clear() { caps.clear(), hulls.clear(), glows.clear(); }
};

// A panel in the room: pose of its centre (+x right, +y up, +z out of its front),
// size in metres, cylinder radius (0: flat), and its texture size in pixels.
struct Panel {
    Mat pose;
    double width, height, curve;
    int pxWidth, pxHeight;
};

// One tracked hand, in the room (standing universe), metres.
struct Hand {
    uint32_t id = 0;
    bool right = false;       // fh_hand_t's side (the gestures file's slots follow it)
    bool stereo = false;      // seen by two or more cameras (one view's depth is 2-6 cm off)
    bool fit = false;         // the fitted skeleton (fh_hands version 2); else the landmarks
    float confidence = 0;
    int64_t captureNs = 0;    // CLOCK_MONOTONIC: when the cameras saw it
    float p[21][3];           // the joints (MediaPipe's landmark order)
    float radius[21];         // rough radii (version 1: the skeleton's defaults)
    float bone[20];           // bone b ends at joint b + 1 (version 1: from the points)
    float elbow[3];           // the arm model's elbow (version 1: on the hand's axis)
    float elbowConf = 0;
    bool hasElbow = false;
    float pinchPoint[3];      // between the index and middle knuckles
    float contact[3];         // between the thumb and index tips
    float pinchD[2];          // thumb to index tip, thumb to middle tip
    float v[3];               // palm velocity in the room, m/s (differenced across frames)
    double speed() const;
    void Palm(double out[3]) const;   // the palm's centre: wrist and knuckles
};

// One gesture slot of the gestures file (hands/include/fh_gestures.h), in the room.
struct Gesture {
    bool tracked = false, down = false, lost = false;
    uint32_t handId = 0, begins = 0, ends = 0;
    int64_t beginNs = 0, endNs = 0;
    float distance = 0, strength = 0;
    float point[3] = {}, beginPoint[3] = {};
};

// A shift of the matte's hands, as seen from the head, by their distance from it (the
// cutout probe found Room View's picture of a hand a little off the tracked one, by an
// amount that depended on the distance). Off (empty) by default: the probe runs so far didn't
// agree on it (see screens/README.md, "Hand cutouts").
struct Shift {
    double dist;            // metres from the head to the palm
    double yawDeg, pitchDeg;  // positive: to the right, up
};

struct MatteParams {
    bool mesh = true;         // the hand-shaped matte (else ft-hands' capsules)
    // Margin around the hand, as an angle seen from the eyes: the probe measured the cutout
    // points 1.8-2.4 degrees (median) off the hand Room View shows with the hand still (p90
    // 4.1-4.5), 2.8-3.1 moving (p90 5.7-6.7); part of that is the video landmark model's own
    // error. A still margin of 1.5 and up to 1.5 more as the hand moves covers the median
    // spread without making the hand a blob (see screens/README.md).
    double marginDeg = 1.5;
    double motionMs = 25;     // ...plus how far the hand moves in this long
    double motionMaxDeg = 1.5;   // ...up to this much more
    double minMarginM = 0.003;
    std::vector<Shift> shift;    // empty: off
    double Margin(double dist, double speed) const;
};

// Parses settings from ~/.config/frametop.conf (FT_<NAME> in the environment overrides):
// HANDS_MATTE (mesh|capsules), HANDS_MATTE_MARGIN (deg), HANDS_MATTE_MOTION (ms),
// HANDS_MATTE_MOTION_MAX (deg), HANDS_MATTE_SHIFT ("dist:yaw,pitch dist:yaw,pitch ...", deg).
// Returns what it read as "NAME=value" lines, for the log.
std::string ReadMatteSettings(MatteParams &p);
// "dist:yaw,pitch ..." -> p.shift. False if it doesn't parse.
bool ParseShift(const std::string &text, std::vector<Shift> &out);

// A frametop.conf value (FT_<name> in the environment first), or def.
std::string Setting(const char *name, const char *def);

class Hands {
public:
    ~Hands();
    // Once per tick: the head pose now, CLOCK_MONOTONIC ns. Re-reads the hands and gestures
    // files when they changed. Returns true while fresh hands are known.
    bool Update(const Mat &head, int64_t nowNs);
    // The same from given file contents instead of the files (tests: ft-handreplay
    // --publish-dump). hands: the hands file's bytes (size of them), gestures may be null.
    bool Feed(const void *hands, size_t size, const fh_gestures_t *gestures, const Mat &head, int64_t nowNs);

    // The hands where the cameras saw them (capture time), in the room: for input.
    const std::vector<Hand> &seen() const { return seen_; }
    // ...moved ahead to now + lead (see SetPrediction), shifted by MatteParams::shift: where
    // the eyes will see them, for the matte and the glows.
    const std::vector<Hand> &shown() const { return shown_; }
    // ft-hands' capsules, moved ahead like shown() (the capsule matte).
    const std::vector<Capsule> &capsules() const { return world_; }
    const Gesture &pinch(int side) const { return pinch_[side]; }   // 0 left, 1 right
    const Gesture &grip(int side) const { return grip_[side]; }
    // Counts the hands file's updates read: a new one is a new processed camera frame.
    uint64_t frame() const { return frames_; }
    int64_t captureNs() const { return captureNs_; }
    int64_t publishNs() const { return publishNs_; }
    int version() const { return version_; }
    bool fresh() const { return fresh_; }
    // Predict the hands' motion (on by default) to now + leadMs: about how long a frame
    // takes from here to the displays.
    void SetPrediction(bool on, double leadMs);
    bool predicting() const { return predict_; }
    double leadMs() const { return leadNs_ / 1e6; }
    void SetShift(const std::vector<Shift> &s) { shift_ = s; }

    // Each hand's 21 landmarks in the room, for ft-handtest --probe: as the cameras saw them
    // (predict false), or moved ahead along the hand's velocity to now + leadMs, as the
    // capsules are. Empty while no fresh hands are known (as capsules()).
    struct HandPoints {
        uint32_t id;
        bool right;
        float p[21][3];
    };
    void Points(int64_t nowNs, bool predict, double leadMs, std::vector<HandPoints> &out) const;

private:
    bool ReadFiles();
    void Ingest(const fh_hands_t &v1, const fh_hand_fit_t *fits, const fh_gestures_t *g);
    void Advance(int64_t nowNs);
    Mat HeadAt(int64_t ns) const;
    struct Past { int64_t ns; Mat head; };
    struct Motion { int64_t ns = 0; double palm[3]{}, v[3]{}; };   // a hand's palm, in the room
    std::vector<Past> history_;   // the last second of head poses
    std::vector<Capsule> base_;   // the capsules at capture time, in the room
    std::vector<int> owner_;      // each capsule's hand (index into seen_), or -1
    std::vector<Hand> seen_, shown_;
    std::map<uint32_t, Motion> motion_;
    std::vector<Capsule> world_;  // base_, moved ahead
    Gesture pinch_[2], grip_[2];
    std::vector<Shift> shift_;
    bool predict_ = true, fresh_ = false;
    int64_t leadNs_ = 36'000'000;   // 25 ms to the displays, plus the tick a cutout buffer waits for its fence
    int fd_ = -1, gfd_ = -1;
    const void *map_ = nullptr, *gmap_ = nullptr;
    size_t mapLen_ = 0, gmapLen_ = 0;
    uint64_t seq_ = 0, gseq_ = 0, frames_ = 0;
    int version_ = 0;
    int64_t captureNs_ = 0, publishNs_ = 0, lastOpenTry_ = 0, lastGOpenTry_ = 0, nowNs_ = 0;
};

// The matte for these hands (Hands::shown()), seen from a head at headPos.
void BuildMatte(const std::vector<Hand> &hands, const std::vector<Capsule> &fileCapsules, const double headPos[3],
                const MatteParams &p, Matte &out);

// Where each eye sees the matte on the panel, for the parts in front of it. Eyes are
// positions in the room. Clears out[e] first; leaves glows to the caller. False if nothing
// reaches the panel for either eye.
bool Project(const Panel &p, const Matte &m, const double eyes[2][3], Spots out[2]);
// Capsules only (ft-handtest --probe's dots).
bool Project(const Panel &p, const std::vector<Capsule> &caps, const double eyes[2][3], std::vector<Capsule2D> out[2]);

// The eye positions in the room for a head pose (handrender.cpp: asks SteamVR).
void EyePositions(const Mat &head, double out[2][3]);

// An output buffer: the dmabuf SteamVR imports (see vr.cpp), stable while it exists.
struct Output {
    ft_dmabuf buf{};
    void *bo = nullptr;
    unsigned fbo = 0, rb = 0;
    void *image = nullptr;
    // What's drawn in it: the client buffer and its frame, and the cutouts and glows (a later
    // draw with the same frame only redraws around the old and new ones).
    void *fence = nullptr;        // the GPU is still drawing it
    const void *key = nullptr;
    uint64_t serial = 0;
    bool drawn = false;
    Spots spots[2];
};

// A dot for ft-handtest --probe: centre and radius in pixels of the eye's half, colour.
struct Mark {
    float x, y, r;
    float rgb[3];
};

// Composite's counts since the last TakeStats.
struct CutStats {
    int draws = 0, partial = 0;   // buffers drawn, of them only around the cutouts
    int same = 0;                 // nothing changed: the newest buffer stays
    int busy = 0;                 // the GPU hadn't finished the last one: drawn next tick
    int waits = 0;                // a panel's first buffer, waited for
    double cpuMs = 0, worstMs = 0;
};

class Renderer {
public:
    ~Renderer();
    // modifiers: what SteamVR takes for DRM_FORMAT_ABGR8888, the outputs' format.
    // released: an output is about to be freed (drop its SteamVR import).
    bool Init(const std::vector<uint64_t> &modifiers, std::function<void(const Output *)> released);
    // Draw client buffer `src` (identified by `key`; `serial` counts its frames) into a
    // buffer of panel `panel`, both eyes, cutting out and glowing `eyes`. Returns the newest
    // buffer the GPU has finished, or null.
    //
    // It doesn't wait for the GPU: a buffer is drawn, fenced, and returned from a later call
    // once the fence has passed, so what SteamVR shows is a tick behind. Each panel has three
    // buffers: the one shown, the one shown before it (SteamVR may still be reading it), and
    // the one being drawn. Nothing is drawn when the frame and the spots are what the newest
    // buffer has, and when only the spots moved, a buffer that holds the same client frame
    // is drawn again only around them. The first call after a pause (no call for 30 ms, about
    // 3 ticks: the panel showed its client buffer meanwhile) waits for its buffer, so a stale
    // one never shows.
    const Output *Composite(int panel, const void *key, uint64_t serial, const ft_dmabuf &src, const Spots eyes[2]);
    // ft-handtest --probe: a transparent w x h buffer per eye (side by side) with only the
    // dots in it, drawn every call, fenced as Composite's. Returns the newest finished one.
    const Output *Marks(int panel, int w, int h, const std::vector<Mark> eyes[2]);
    // A client buffer is going away.
    void Forget(const void *key);
    // A panel is gone: drop its outputs.
    void DropPanel(int panel);
    // How long the last Composite took on the CPU, ms.
    double lastMs() const { return lastMs_; }
    CutStats TakeStats() { CutStats s = stats_; stats_ = {}; return s; }
    // Waits for everything drawn so far and returns how long that took, ms (offline benches).
    double Finish();

private:
    unsigned Texture(const void *key, const ft_dmabuf &src);
    bool MakeOutput(Output &o, int w, int h);
    void FreeOutput(Output &o);
    bool Passed(Output &o, int64_t timeoutNs);
    void Draw(Output &o, unsigned tex, int w, int h, const Spots eyes[2], bool partial);
    bool ready_ = false;
    int drm_ = -1;
    void *gbm_ = nullptr, *dpy_ = nullptr, *ctx_ = nullptr;
    unsigned copyProg_ = 0, cutProg_ = 0, hullProg_ = 0, glowProg_ = 0, markProg_ = 0, vbo_ = 0;
    std::vector<uint64_t> modifiers_;
    std::function<void(const Output *)> released_;
    struct Imported { void *image; unsigned tex; };
    std::map<const void *, Imported> imported_;
    struct Ring {
        Output out[3];
        int shown = -1, before = -1, drawing = -1;   // indices into out
        int w = 0, h = 0;
        int64_t lastCall = 0;                        // steady clock ns
    };
    std::map<int, Ring> rings_;
    double lastMs_ = 0;
    CutStats stats_;
};

}  // namespace handcut
