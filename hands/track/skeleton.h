// A per-user hand skeleton fitted to the tracker's 21 landmarks (hands/README.md, "Skeleton").
//
// Each side (left, right) learns its bone lengths and palm shape from clean stereo frames.
// Every processed frame then fits a skeleton with those lengths to the measured points: the
// palm as a rigid body, each finger as a chain of hinges with joint limits. The fitted pose
// (palm pose and joint angles) is filtered, not the points, so fingers keep their length.
// An arm model puts an elbow behind the wrist. All 3D is metres in the head frame.
//
// Frames follow OpenXR's hand joints: a joint's -Z points along the bone that starts there
// (toward the fingertip), +Y out of the back of the hand, +X = Y x Z. The wrist's frame is
// the palm's: -Z toward the middle knuckle. Both hands use the same axes, so +X points to
// the little finger on a right hand and to the thumb on a left one.
#pragma once

#include "geom.h"

#include <cstdint>
#include <vector>

namespace skel {

// 3x3 rotation; columns are the frame's axes in the head frame.
struct Rot {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 col(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
    V3 operator*(V3 v) const;
    Rot operator*(const Rot &o) const;
    Rot t() const;
};
Rot exp_rot(V3 w);        // rotation vector -> rotation
V3 log_rot(const Rot &r); // rotation -> rotation vector
void to_quat(const Rot &r, double q[4]);   // x, y, z, w

constexpr int kAngles = 26;   // thumb 6, each finger 5
constexpr int kParams = kAngles + 15;   // and each chain's base offset from the rigid palm (m)
constexpr int kQuantities = 30;   // what calibration learns: 5 palm points x 3, 15 phalanges

// One user's hand, in the right hand's palm frame (a left hand is its mirror image in x).
struct Shape {
    V3 palm[21]{};        // landmarks 1, 5, 9, 13 and 17 from the wrist (others unused)
    double len[20]{};     // bone b ends at landmark b + 1; the 5 from the wrist are |palm[k]|
    void from_quantities(const double *q);
};

// Learns one side's Shape from clean stereo frames: medians of the first kSettle samples of
// each quantity, then a running median that moves each by at most 0.02 mm a sample.
class Calibration {
public:
    static constexpr int kSettle = 45;   // samples: 3 s of a clean hand at 15 Hz
    // A frame's raw landmarks (2+ views, small residual), in this side's palm frame already
    // mirrored to a right hand (Skeleton::update does that). Fingers bent a lot are skipped.
    // ok: landmarks triangulated cleanly (both ends of a bone must be).
    void add(const V3 local[21], const bool ok[21], const bool finger_ok[5]);
    // Estimates, blended with prior (a Shape) while few samples are in.
    Shape shape(const Shape &prior) const;
    double settled() const;   // 0..1: the least-sampled quantity's share of kSettle
    int samples(int k) const { return n_[k]; }

private:
    std::vector<float> buf_[kQuantities];
    double est_[kQuantities]{};
    int n_[kQuantities]{};
};

// Per hand, kept with the tracker's Hand.
struct Fit {
    bool valid = false;       // fitted this frame
    int side = 1;             // +1 right, -1 left: the mirror the fit used
    int64_t ns = 0;           // last fit
    // this frame's fit, before filtering
    Rot R;                    // palm frame -> head
    V3 t{};                   // wrist
    double q[kParams]{};       // angles (rad), then each chain base's offset (m)
    double rms = 0;           // fitted landmarks against measured, m (all 21)
    double chain_rms[5]{};    // each chain's (thumb, index, ...: its base and 3 landmarks)
    double us = 0;            // fit time, microseconds
    bool flipped = false;     // fitted as the other side than the model's call (it fitted much better)
    int votes = 0;            // frames in a row the other side fitted much better
    // filtered
    Rot Rf;
    V3 cf{}, vel{}, angvel{}; // palm centre, its velocity (m/s), the palm's angular velocity (rad/s)
    double qf[kParams]{}, dq[kParams]{};
    // results, from the filtered pose
    V3 raw[21]{};             // this frame's fit, unfiltered
    V3 pts[21]{};             // fitted landmarks
    Rot frames[21];           // each joint's frame (see the top)
    double radius[21]{};      // rough joint radii, m, for colliders
    double len[20]{};         // bone lengths used
    V3 elbow{};
    double settled = 0;       // the side's calibration, 0..1
};

struct Params {
    // the filter on the fitted pose: like the One Euro on the points (tracker.cpp), cutoffs in Hz
    double pos_min = 2.0, pos_beta = 30.0;   // palm centre: + beta per m/s
    double rot_min = 2.0, rot_beta = 10.0;   // palm rotation: + beta per rad/s
    double ang_min = 2.0, ang_beta = 3.0;    // a finger's joint angles: + beta per rad/s of that finger
    double d_cutoff = 1.5;                   // for the speeds themselves
    double warm = 1;                         // 1: also start each chain from last frame's pose
    double glitch = 0.015;                   // m: a chain fitting worse than this moves the filter less
    int iterations = 4;                      // Gauss-Newton steps per chain
    double depth_ratio = 3.5;                // stereo: how much less sure depth is than across the view
    double limits = 1;                       // scales the joint limits (for tests: 10 is about none)
    double forearm_ratio = 2.8;              // forearm / palm length (notes/forearm.md)
};

class Skeleton {
public:
    explicit Skeleton(const Params &p = {}) : p_(p) {}
    // After the tracker's 3D: pts (raw, this frame), the model's side, views (1: one camera),
    // residual (m, stereo: the landmarks' median) and res_pt (each landmark's, or null), eye (the views' camera centre, for which way depth is), the
    // hand's size against the model's (Hand::scale).
    void update(Fit &f, const V3 pts[21], bool right, double right_score, int nviews, double residual,
                const double *res_pt, V3 eye, double scale, int64_t t_ns);
    // The head's up direction, for the arm model (default: the head frame's +y).
    void set_up(V3 up) { up_ = unit(up); }
    const Calibration &calibration(int side) const { return cal_[side > 0]; }
    Shape shape(int side, double scale) const;   // the side's current estimate
    static Shape canonical(double scale);        // the model's average hand, times scale
    const Params &params() const { return p_; }
    void set_params(const Params &p) { p_ = p; }

    // diagnostics
    int fits = 0, flips = 0;
    double us_total = 0;

    // Forward kinematics: landmarks and frames for palm pose (R, t), parameters q (kParams), a
    // shape and side.
    static void pose(const Shape &s, int side, const Rot &R, V3 t, const double *q, V3 out[21], Rot frames[21]);
    // Fits (R, t, q) to pts; weights from views and residual (see skeleton.cpp). Returns the
    // weighted cost; rms gets the plain rms distance. prev: last frame's angles for the prior, or null.
    static double solve(const Shape &s, int side, const V3 pts[21], const double sigma_l[21], const double sigma_d[21],
                        V3 eye, const double *prev, int iterations, Rot &R, V3 &t, double *q, double *rms,
                        double limits = 1, double chain_rms[5] = nullptr, bool warm = true);
    // The arm model: an elbow for a wrist pose, side, palm length (m) and the up direction.
    static V3 elbow(V3 wrist, V3 mid_knuckle, int side, double forearm, V3 up);

private:
    Params p_;
    Calibration cal_[2];   // left, right
    V3 up_{0, 1, 0};
};

}  // namespace skel
