// FrameHands shared-file reader and 3D hand geometry for the SteamVR overlay.
// The tracker publishes positions in the head frame at capture time. Hands turns
// them into standing space with the HMD pose and predicts their display position.
// BuildMatte supplies the palm, fingers, and forearm mesh used by handmesh.cpp.
#pragma once

#include "../hands/include/fh_gestures.h"
#include "../hands/include/fh_hands.h"
#include "../hands/include/fh_shm.hpp"

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
#include <cstddef>
#include <cstdint>
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

// A round hole: a disk around c, radius r (metres, the margin in it), facing whichever eye
// looks at it.
struct Circle {
    float c[3];
    float r;
};

struct Matte {
    std::vector<Circle> circles;
    std::vector<Capsule> caps;
    std::vector<Hull> hulls;
    // How much wider than the hand each piece is drawn (metres at the hand), per piece: the
    // capsules have it in their radii already; a hull grows by it on the panel.
    std::vector<float> hullMargin;
    void clear() { circles.clear(), caps.clear(), hulls.clear(), hullMargin.clear(); }
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
// agree on it (see hands/cutouts.md).
struct Shift {
    double dist;            // metres from the head to the palm
    double yawDeg, pitchDeg;  // positive: to the right, up
};

enum class MatteShape {
    Circle,     // a round hole per hand
    Mesh,       // the hand-shaped matte
    Capsules,   // ft-hands' capsules
};
const char *ShapeName(MatteShape s);
bool ParseShape(const std::string &name, MatteShape *out);   // circle|mesh|capsules

struct MatteParams {
    MatteShape shape = MatteShape::Circle;
    // Margin around the hand, as an angle seen from the eyes: the probe measured the cutout
    // points 1.8-2.4 degrees (median) off the hand Room View shows with the hand still (p90
    // 4.1-4.5), 2.8-3.1 moving (p90 5.7-6.7); part of that is the video landmark model's own
    // error. A still margin of 1.5 and up to 1.5 more as the hand moves covers the median
    // spread without making the hand a blob (see hands/cutouts.md).
    double marginDeg = 1.5;
    double motionMs = 25;     // ...plus how far the hand moves in this long
    double motionMaxDeg = 1.5;   // ...up to this much more
    double minMarginM = 0.003;
    std::vector<Shift> shift;    // empty: off
    double Margin(double dist, double speed) const;
};

// Parses settings from ~/.config/framehands/config (FH_<NAME> in the environment overrides):
// HANDS_MATTE (circle|mesh|capsules), HANDS_MATTE_MARGIN (deg), HANDS_MATTE_MOTION (ms),
// HANDS_MATTE_MOTION_MAX (deg), HANDS_MATTE_SHIFT ("dist:yaw,pitch dist:yaw,pitch ...", deg).
// Returns what it read as "NAME=value" lines, for the log.
std::string ReadMatteSettings(MatteParams &p);
// "dist:yaw,pitch ..." -> p.shift. False if it doesn't parse.
bool ParseShift(const std::string &text, std::vector<Shift> &out);

// A framehands/config value (FH_<name> in the environment first), or def.
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
    const Mat &captureHead() const { return captureHead_; }   // the head pose seen() was turned into the room with
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
    // the hands file (version 1 or later) and the gestures file (with the grips: version 2)
    fh::ShmFile file_{"hands", FH_HANDS_MAGIC, sizeof(fh_hands_t)};
    fh::ShmFile gfile_{"gestures", FH_GESTURES_MAGIC, offsetof(fh_gestures_t, grip) + sizeof(fh_pinch_t) * 2};
    uint64_t seq_ = 0, gseq_ = 0, frames_ = 0;
    int opens_ = 0, gopens_ = 0;
    int version_ = 0;
    Mat captureHead_{};
    int64_t captureNs_ = 0, publishNs_ = 0, nowNs_ = 0;
};

// Geometry shared with the standalone capsule hand renderer.
void BuildMatte(const std::vector<Hand> &hands, const std::vector<Capsule> &fileCapsules, const double headPos[3],
                const MatteParams &p, Matte &out);

// The eye positions in the room for a head pose (asks SteamVR).
void EyePositions(const Mat &head, double out[2][3]);

}  // namespace handcut
