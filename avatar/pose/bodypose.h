// An avatar's body posed from what a headset knows of its wearer: the head, and each hand while
// it is tracked (the wrist, the hand's turn, the fingers' and, when the tracker has one, the
// elbow). The rest is made up to fit: the trunk hangs from the head and turns after it, the
// spine leans when the head comes down looking down, the arms reach the wrists with two bones
// each, an arm with no tracked hand hangs, and the legs stand under the hips with the knees
// bending as the head comes down (and the feet out in front the lower it gets, as on a chair).
//
// A bone has a place and a turn from its rest (bodyfile.h: a body's bones have no rest turn).
// The body is scaled so its eyes stand as high as its wearer's.
//
// The sums only: no tracking, GPU or SteamVR calls here. The same solver poses your own body
// and the people you share a room with.
#pragma once

#include "../bodyfile.h"
#include "turn.h"

namespace bodypose {

constexpr int kFingerBones = 15;   // a hand's, in bodyfile's order

struct Hand {
    bool tracked = false;
    Vec wrist;                   // the room, metres
    Turn turn;                   // the hand bone's turn from its rest
    bool fingers = false;        // finger[] is known
    Turn finger[kFingerBones];   // each finger bone's turn from its rest
    bool hasElbow = false;
    Vec elbow;                   // the room
};

struct Input {
    Vec eyes;                // between the wearer's eyes, the room, metres
    Turn head;               // none when the head is upright and faces -z
    Hand hand[2];            // left, right
    double floor = 0;        // its height in the room
    double eyeHeight = 1.6;  // the wearer's eyes above the floor when they stand (Stature)
    double dt = 0;           // seconds since the pose before; 0: this pose stands alone
};

struct Pose {
    double scale = 1;
    Vec at[bodyfile::kRoles];      // every role's, the absent ones' too (as their parents carry them)
    Turn turn[bodyfile::kRoles];
};

// Tuning, for the tests to name.
constexpr double kTrunkDeadZone = 15 * kPi / 180;   // the head turns this far before the trunk follows
constexpr double kTrunkMaxTwist = 75 * kPi / 180;   // ...and the trunk is never further behind it
constexpr double kReach = 0.999;                    // of a limb's length: it never locks straight

class Solver {
public:
    // The body's skeleton is copied: the body needn't outlive the solver.
    explicit Solver(const bodyfile::Body &body);

    // The body posed. False, and `out` untouched, when the input isn't numbers.
    bool Solve(const Input &in, Pose &out);
    // Forget the poses before: the next stands alone.
    void Reset();

    // The skinning matrices of a pose: each takes a point of the body at rest (metres) to the
    // room. Row-major 3x4, as the hands' (handskin.h).
    void Matrices(const Pose &pose, float out[bodyfile::kRoles][12]) const;

    double trunkYaw() const { return yaw_; }

private:
    struct Arm {
        double held = 0;   // 0: hanging, 1: at the tracked hand
        bool seen = false, fingers = false;
        Vec wrist;
        Turn turn, finger[kFingerBones];
    };
    void Trunk(const Input &in, Pose &out, double headYaw);
    void SolveArm(int side, const Input &in, Pose &out);
    void SolveLeg(int side, const Input &in, Pose &out);
    void Place(int role, Pose &out) const;
    Vec Along(int from, int to) const;

    Vec at_[bodyfile::kRoles], eyes_;
    bool present_[bodyfile::kRoles];
    int parent_[bodyfile::kRoles];
    bool fresh_ = true;
    double yaw_ = 0;
    Arm arm_[2];
};

// The wearer's standing eye height, told from where their head has been: the highest it has
// stayed, and never less than a short adult's, so someone who has only sat gets a body that
// sits. A jump barely moves it.
class Stature {
public:
    void See(double eyesAboveFloor, double dt);
    double eyeHeight() const { return high_; }

private:
    double high_ = 1.45;
};

}  // namespace bodypose
