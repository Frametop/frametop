// The body's pose from the head and the hands (bodypose.h).
//
// The order: the head bone goes where the wearer's eyes put it. The trunk's turns are chosen
// (it faces where the head has been facing, leans by how far the head has come down while
// looking down, and twists a share of the way to the head), then its bones are placed down
// from the head, each from the one it carries, so the hips end wherever that leaves them: lower
// than they stand when the head is lower than the lean accounts for. Arms and legs are each a
// limb of two bones bent toward a pole. Every place below the trunk then follows from its
// parent's place and turn, so a bone keeps its length whatever the limbs were asked to reach.
#include "bodypose.h"

#include <algorithm>

namespace bodypose {

using namespace bodyfile;

namespace {

double Clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
Turn Yaw(double a) { return Turn::About({0, 1, 0}, a); }
Vec Mixed(const Vec &a, const Vec &b, double t) { return a + (b - a) * t; }
// The share of a gap closed in dt seconds by something that closes it in about three times tau.
double Closed(double dt, double tau) { return 1 - std::exp(-dt / tau); }
Vec Of(const float v[3]) { return {v[0], v[1], v[2]}; }

struct Limb {
    Vec mid, end;
};

// A limb of two bones, a then b long, from `root` toward `target`, its middle joint out toward
// `pole`. The end stops short of a target out of reach.
Limb TwoBones(const Vec &root, const Vec &target, double a, double b, const Vec &pole) {
    Vec to = target - root;
    double far = to.norm();
    if (far < 1e-9) to = {0, -1, 0}, far = 1e-9;
    const Vec n = to.unit();
    const double most = (a + b) * kReach, reach = Clamp(far, std::min(std::fabs(a - b), most), most);
    const double bend = a > 1e-9 && reach > 1e-9 ? Clamp((a * a + reach * reach - b * b) / (2 * a * reach), -1, 1) : 1;
    Vec side = pole - n * pole.dot(n);
    if (side.norm() < 1e-9) side = n.cross(std::fabs(n.y) < 0.9 ? Vec{0, 1, 0} : Vec{1, 0, 0});
    return {root + n * (a * bend) + side.unit() * (a * std::sqrt(1 - bend * bend)), root + n * reach};
}

bool Numbers(const Hand &h) {
    if (!h.tracked) return true;
    if (!h.wrist.finite() || !h.turn.finite() || (h.hasElbow && !h.elbow.finite())) return false;
    for (int i = 0; h.fingers && i < kFingerBones; ++i)
        if (!h.finger[i].finite()) return false;
    return true;
}

}  // namespace

Solver::Solver(const bodyfile::Body &body) : eyes_(Of(body.eyes)) {
    for (int r = 0; r < kRoles; ++r) at_[r] = Of(body.at[r]), present_[r] = body.present[r], parent_[r] = body.Parent(r);
}

void Solver::Reset() {
    fresh_ = true, yaw_ = 0;
    arm_[0] = arm_[1] = Arm();
}

Vec Solver::Along(int from, int to) const { return (at_[to] - at_[from]).unit(); }

void Solver::Place(int role, Pose &p) const {
    const int up = parent_[role];
    if (up >= 0) p.at[role] = p.at[up] + p.turn[up] * ((at_[role] - at_[up]) * p.scale);
}

bool Solver::Solve(const Input &in, Pose &out) {
    if (!in.eyes.finite() || !in.head.finite() || !std::isfinite(in.floor) || !std::isfinite(in.eyeHeight) || !std::isfinite(in.dt)) return false;
    if (!Numbers(in.hand[0]) || !Numbers(in.hand[1])) return false;
    Pose p;
    p.scale = eyes_.y > 0.2 ? Clamp(in.eyeHeight / eyes_.y, 0.2, 5) : 1;
    const Turn head = in.head.unit();

    // The way the head faces along the floor, from its right side: that stays level while the
    // head nods, where the way it looks swings through straight up and down.
    const Vec right = head * Vec{1, 0, 0}, flat = {right.z, 0, -right.x};
    const double headYaw = flat.norm() > 0.2 ? std::atan2(-flat.x, -flat.z) : yaw_;
    if (fresh_ || !(in.dt > 0)) {
        yaw_ = headYaw;
    } else {
        // the trunk turns after the head once the head has turned far enough, and never trails it far
        const double behind = Wrap(headYaw - yaw_), over = std::fabs(behind) - kTrunkDeadZone;
        if (over > 0) yaw_ += (behind < 0 ? -over : over) * Closed(in.dt, 0.35);
        const double left = Wrap(headYaw - yaw_);
        if (std::fabs(left) > kTrunkMaxTwist) yaw_ = headYaw - (left < 0 ? -kTrunkMaxTwist : kTrunkMaxTwist);
        yaw_ = Wrap(yaw_);
    }

    Trunk(in, p, Wrap(headYaw - yaw_));
    for (int side = 0; side < 2; ++side) SolveArm(side, in, p), SolveLeg(side, in, p);
    for (int r : {kLeftEye, kRightEye, kJaw}) p.turn[r] = head, Place(r, p);
    for (int r = 0; r < kHead; ++r)   // a trunk bone the body lacks goes where its parent carries it
        if (!present_[r]) Place(r, p);
    fresh_ = false;
    out = p;
    return true;
}

void Solver::Trunk(const Input &in, Pose &p, double twist) {
    const double s = p.scale;
    const Turn head = in.head.unit();
    p.turn[kHead] = head;
    p.at[kHead] = in.eyes - head * ((eyes_ - at_[kHead]) * s);

    // How far the head is under where it stands (a nod's worth doesn't count), and how far down
    // it looks: together a lean. A head that is low and looks ahead leaves the trunk upright,
    // and the legs take it.
    const double low = std::max(0.0, in.floor + (at_[kHead].y - 0.03) * s - p.at[kHead].y);
    const double down = std::asin(Clamp(-(head * Vec{0, 0, -1}).y, -1, 1));
    const double spine = (at_[kHead] - at_[kHips]).norm() * s;
    const double lean = spine > 1e-6 && down > 0 ? std::min(0.6 * down, std::acos(Clamp(1 - low / spine, 0.5, 1))) : 0;

    // up from the hips each bone leans and twists toward the head a little more
    struct Share {
        int role;
        double lean, twist;
    };
    static const Share shares[] = {{kHips, 0.5, 0}, {kSpine, 0.9, 0.15}, {kChest, 1.1, 0.35}, {kUpperChest, 1.2, 0.5}};
    for (const Share &sh : shares) p.turn[sh.role] = Yaw(yaw_ + twist * sh.twist) * Turn::About({1, 0, 0}, -lean * sh.lean);
    p.turn[kNeck] = Turn::Mix(parent_[kNeck] >= 0 ? p.turn[parent_[kNeck]] : head, head, 0.5);

    for (int child = kHead, up = parent_[kHead]; up >= 0; child = up, up = parent_[up])
        p.at[up] = p.at[child] - p.turn[up] * ((at_[child] - at_[up]) * s);
}

void Solver::SolveArm(int side, const Input &in, Pose &p) {
    const int shoulder = side ? kRightShoulder : kLeftShoulder, upper = shoulder + 1, lower = shoulder + 2, hand = shoulder + 3;
    const int fingers = side ? kRightFingers : kLeftFingers;
    const double outward = side ? 1 : -1;
    const Turn chest = parent_[shoulder] >= 0 ? p.turn[parent_[shoulder]] : Turn{};
    p.turn[shoulder] = chest;
    Place(shoulder, p), Place(upper, p);
    const Vec root = p.at[upper];
    const double a = (at_[lower] - at_[upper]).norm() * p.scale, b = (at_[hand] - at_[lower]).norm() * p.scale;

    // The wrist goes to the tracked hand, or hangs by the side, and crosses between the two
    // over a moment when tracking comes and goes.
    Arm &arm = arm_[side];
    const Hand &h = in.hand[side];
    if (h.tracked) {
        arm.seen = true, arm.wrist = h.wrist, arm.turn = h.turn.unit(), arm.fingers = h.fingers;
        for (int i = 0; h.fingers && i < kFingerBones; ++i) arm.finger[i] = h.finger[i].unit();
    }
    const double want = h.tracked ? 1 : 0;
    arm.held = fresh_ || !(in.dt > 0) ? want : arm.held + (want - arm.held) * Closed(in.dt, 0.12);
    const double held = arm.seen ? arm.held : 0;
    const Vec hang = root + Yaw(yaw_) * (Vec{outward * 0.10, -0.96, -0.08} * (a + b));   // straight down, whatever the lean
    const Vec pole = h.tracked && h.hasElbow ? h.elbow - root : chest * Vec{outward * 0.55, -0.75, 0.35};
    const Limb limb = TwoBones(root, Mixed(hang, arm.wrist, held), a, b, pole);

    p.turn[upper] = Turn::Between(chest * Along(upper, lower), limb.mid - root) * chest;
    const Vec forearm = limb.end - limb.mid;
    const Turn fore = Turn::Between(p.turn[upper] * Along(lower, hand), forearm) * p.turn[upper];
    const Turn palm = Turn::Mix(fore, arm.turn, held);
    // the forearm takes half of the hand's twist about it, as the two bones in one do
    p.turn[lower] = Turn::About(forearm, Clamp(0.5 * (palm * fore.inverse()).TwistAbout(forearm), -1.2, 1.2)) * fore;
    p.turn[hand] = palm;
    Place(lower, p), Place(hand, p);
    for (int i = 0; i < kFingerBones; ++i) {
        p.turn[fingers + i] = arm.fingers ? Turn::Mix(palm, arm.finger[i], held) : palm;
        Place(fingers + i, p);
    }
}

void Solver::SolveLeg(int side, const Input &in, Pose &p) {
    const int upper = side ? kRightUpperLeg : kLeftUpperLeg, lower = upper + 1, foot = upper + 2, toes = upper + 3;
    const double outward = side ? 1 : -1, s = p.scale;
    const Turn flat = Yaw(yaw_);
    Place(upper, p);
    const Vec root = p.at[upper], hips = p.at[kHips];
    const double a = (at_[lower] - at_[upper]).norm() * s, b = (at_[foot] - at_[lower]).norm() * s;

    // The foot stands under the hips as it does at rest, and further out in front the lower
    // the hips are: by a thigh's length down, someone is sitting.
    const double sunk = std::max(0.0, in.floor + at_[kHips].y * s - hips.y), sit = a > 1e-9 ? Clamp(sunk / a, 0, 1) : 0;
    const Vec stance = flat * ((at_[foot] - at_[kHips]) * s), ahead = flat * Vec{0, 0, -1};
    const Vec target = Vec{hips.x + stance.x, in.floor + at_[foot].y * s, hips.z + stance.z} + ahead * (0.85 * a * sit);
    const Limb limb = TwoBones(root, target, a, b, flat * Vec{outward * 0.15, 0, -1});

    p.turn[upper] = Turn::Between(flat * Along(upper, lower), limb.mid - root) * flat;
    p.turn[lower] = Turn::Between(p.turn[upper] * Along(lower, foot), limb.end - limb.mid) * p.turn[upper];
    p.turn[foot] = p.turn[toes] = flat;
    Place(lower, p), Place(foot, p), Place(toes, p);
}

void Solver::Matrices(const Pose &pose, float out[kRoles][12]) const {
    for (int r = 0; r < kRoles; ++r) {
        const Vec x = pose.turn[r] * Vec{pose.scale, 0, 0}, y = pose.turn[r] * Vec{0, pose.scale, 0}, z = pose.turn[r] * Vec{0, 0, pose.scale};
        const Vec t = pose.at[r] - (x * at_[r].x + y * at_[r].y + z * at_[r].z);
        const double m[12] = {x.x, y.x, z.x, t.x, x.y, y.y, z.y, t.y, x.z, y.z, z.z, t.z};
        for (int k = 0; k < 12; ++k) out[r][k] = float(m[k]);
    }
}

void Stature::See(double eyesAboveFloor, double dt) {
    if (!std::isfinite(eyesAboveFloor) || !(dt > 0)) return;
    const double seen = std::min(eyesAboveFloor, 2.2);
    if (seen > high_) high_ += (seen - high_) * Closed(dt, 1.0);
}

}  // namespace bodypose
