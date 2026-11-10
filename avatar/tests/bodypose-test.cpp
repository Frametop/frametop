// The pose solver (avatar/pose/bodypose.h) on a made-up skeleton: the body stands where its
// wearer does at their size, every bone keeps its length whatever is asked of it, the hands
// reach the wrists, the trunk leans, sits and turns after the head, tracking that comes and
// goes moves an arm across without a jump, and nothing that isn't a number gets in or out.
#include "../pose/bodypose.h"
#include "person.h"

#include <cstdio>
#include <random>

using namespace bodypose;
using namespace bodyfile;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static Vec at(const Body &b, int role) { return {b.at[role][0], b.at[role][1], b.at[role][2]}; }
static double far(const Vec &a, const Vec &b) { return (a - b).norm(); }
static Turn yaw(double degrees) { return Turn::About({0, 1, 0}, degrees * kPi / 180); }
static Turn nod(double degrees) { return Turn::About({1, 0, 0}, -degrees * kPi / 180); }   // down

// Someone standing where the body stands at rest.
static Input standing(const Body &b) {
    Input in;
    in.eyes = {b.eyes[0], b.eyes[1], b.eyes[2]};
    in.eyeHeight = b.eyes[1];
    return in;
}

// Every bone as far from the one that carries it as at rest (times the scale), and every number
// a number.
static bool sound(const Body &b, const Pose &p) {
    for (int r = 0; r < kRoles; ++r) {
        if (!p.at[r].finite() || !p.turn[r].finite() || std::fabs(p.turn[r].dot(p.turn[r]) - 1) > 1e-9) return false;
        const int up = b.Parent(r);
        if (up >= 0 && b.present[r] && std::fabs(far(p.at[r], p.at[up]) - far(at(b, r), at(b, up)) * p.scale) > 1e-9) return false;
    }
    return true;
}

static void stands() {
    const Body b = person();
    Solver solver(b);
    Pose p;
    check("someone standing is posed", solver.Solve(standing(b), p) && sound(b, p));
    bool trunk = true;
    for (int r = kHips; r <= kJaw; ++r) trunk = trunk && far(p.at[r], at(b, r)) < 1e-9 && p.turn[r].AngleTo(Turn{}) < 1e-9;
    check("the trunk and head stand as at rest", trunk);
    bool legs = true;
    for (int leg : {kLeftUpperLeg, kRightUpperLeg}) {
        legs = legs && far(p.at[leg], at(b, leg)) < 1e-9 && far(p.at[leg + 2], at(b, leg + 2)) < 2e-3 && far(p.at[leg + 3], at(b, leg + 3)) < 2e-3;
        legs = legs && p.at[leg + 1].z < at(b, leg + 1).z && far(p.at[leg + 1], at(b, leg + 1)) < 0.03;   // a soft knee, forward
    }
    check("the legs stand on the floor, the knees not locked", legs);
    bool arms = true;
    for (int side = 0; side < 2; ++side) {
        const int upper = (side ? kRightShoulder : kLeftShoulder) + 1;
        const Vec hang = p.at[upper + 2] - p.at[upper];
        arms = arms && hang.y < -0.5 && std::fabs(hang.x) < 0.1 && hang.x * (side ? 1 : -1) > 0 && hang.z < 0;
        // the palm faces the body: the hand's down at rest now points in
        arms = arms && (p.turn[upper + 2] * Vec{0, -1, 0}).x * (side ? 1 : -1) < -0.8;
        arms = arms && p.turn[(side ? kRightFingers : kLeftFingers) + 4].AngleTo(p.turn[upper + 2]) < 1e-9;
    }
    check("the arms hang by the sides, palms in", arms);

    Input in = standing(b);
    in.eyes = {2 + 0.75 * b.eyes[0], 0.3 + 0.75 * b.eyes[1], -1 + 0.75 * b.eyes[2]};
    in.floor = 0.3, in.eyeHeight = 0.75 * b.eyes[1];
    check("someone shorter, elsewhere, on a higher floor", solver.Solve(in, p) && sound(b, p) && std::fabs(p.scale - 0.75) < 1e-9 &&
          far(p.at[kHips], Vec{2, 0.3, -1} + at(b, kHips) * 0.75) < 1e-9 && far(p.at[kLeftFoot], Vec{2, 0.3, -1} + at(b, kLeftFoot) * 0.75) < 2e-3);

    float m[kRoles][12];
    solver.Matrices(p, m);
    bool carried = true;
    for (int r = 0; r < kRoles; ++r) {
        const Vec rest = at(b, r);
        const Vec moved = {m[r][0] * rest.x + m[r][1] * rest.y + m[r][2] * rest.z + m[r][3], m[r][4] * rest.x + m[r][5] * rest.y + m[r][6] * rest.z + m[r][7],
                           m[r][8] * rest.x + m[r][9] * rest.y + m[r][10] * rest.z + m[r][11]};
        carried = carried && far(moved, p.at[r]) < 1e-5;
        const Vec x = {m[r][0], m[r][4], m[r][8]};
        carried = carried && std::fabs(x.norm() - 0.75) < 1e-5 && far(x, p.turn[r] * Vec{0.75, 0, 0}) < 1e-5;
    }
    check("a bone's matrix takes its rest place to its place, at the body's scale", carried);
}

static void reaches() {
    const Body b = person();
    Solver solver(b);
    Pose p;
    Input in = standing(b);
    Hand &left = in.hand[0], &right = in.hand[1];
    right.tracked = true, right.wrist = {0.25, 1.2, -0.35}, right.turn = yaw(90);
    check("a hand in reach is at the wrist, turned as tracked", solver.Solve(in, p) && sound(b, p) && far(p.at[kRightHand], right.wrist) < 1e-9 &&
          p.turn[kRightHand].AngleTo(right.turn) < 1e-9 && far(p.at[kLeftHand], right.wrist) > 0.3);
    const Vec shoulder = p.at[kRightUpperArm], elbow = p.at[kRightLowerArm];
    const Vec middle = shoulder + (right.wrist - shoulder) * 0.5;
    check("the elbow bends down and out when nothing says where it is", elbow.y < middle.y && elbow.x > middle.x);

    right.hasElbow = true, right.elbow = middle + Vec{0, 0.3, 0};
    check("...and where the tracker has it when it does", solver.Solve(in, p) && sound(b, p) && p.at[kRightLowerArm].y > middle.y + 0.05 &&
          far(p.at[kRightHand], right.wrist) < 1e-9);
    right.hasElbow = false;

    right.wrist = {0.6, 1.5, -3};
    const double arm = far(at(b, kRightUpperArm), at(b, kRightLowerArm)) + far(at(b, kRightLowerArm), at(b, kRightHand));
    solver.Solve(in, p);
    const Vec root = p.at[kRightUpperArm];
    check("a wrist out of reach is reached toward, the arm all but straight", sound(b, p) && std::fabs(far(p.at[kRightHand], root) - arm * kReach) < 1e-9 &&
          far((p.at[kRightHand] - root).unit(), (right.wrist - root).unit()) < 1e-9);

    // the forearm takes half the hand's twist: a hand held out ahead and rolled a quarter turn
    left.tracked = true, left.wrist = {-0.2, 1.3, -0.4};
    solver.Solve(in, p);
    const Vec forearm = p.at[kLeftHand] - p.at[kLeftLowerArm];
    const Turn level = p.turn[kLeftLowerArm];   // the hand's turn is none: the forearm's twist toward it
    left.turn = Turn::About(forearm, 0.8) * p.turn[kLeftHand];
    solver.Solve(in, p);
    check("the forearm takes half the hand's twist", sound(b, p) && far(p.at[kLeftHand], left.wrist) < 1e-9 &&
          std::fabs((p.turn[kLeftLowerArm] * level.inverse()).TwistAbout(forearm) - 0.4) < 1e-6);

    left.fingers = true;
    for (int i = 0; i < kFingerBones; ++i) left.finger[i] = Turn::About({0, 0, 1}, 0.1 * (i + 1)) * left.turn;
    solver.Solve(in, p);
    bool fingers = true;
    for (int i = 0; i < kFingerBones; ++i) fingers = fingers && p.turn[kLeftFingers + i].AngleTo(left.finger[i]) < 1e-9;
    check("the fingers turn as tracked, the other hand's with their hand", fingers && sound(b, p) &&
          p.turn[kRightFingers + 7].AngleTo(p.turn[kRightHand]) < 1e-9);
}

static void leans_and_sits() {
    const Body b = person();
    Solver solver(b);
    Pose p;
    Input in = standing(b);
    in.eyes.y -= 0.4;
    solver.Solve(in, p);
    bool sat = sound(b, p) && std::fabs(p.at[kHips].y - (at(b, kHips).y - 0.4)) < 1e-9 && std::fabs(p.at[kHips].z) < 1e-9;
    for (int leg : {kLeftUpperLeg, kRightUpperLeg}) {
        sat = sat && std::fabs(p.at[leg + 2].y - at(b, leg + 2).y) < 1e-9;       // the feet on the floor
        sat = sat && p.at[leg + 2].z < -0.2 && p.at[leg + 1].z < p.at[leg].z - 0.2;   // out in front, the knees too
        sat = sat && std::fabs(p.at[leg + 1].x) > std::fabs(p.at[leg].x) - 1e-9;      // the knees not crossed
    }
    check("a head that comes straight down sits the body: trunk upright, knees and feet ahead", sat);

    in = standing(b);
    in.head = nod(60);
    in.eyes = {0, 1.35, -0.35};
    solver.Solve(in, p);
    const Vec up = p.turn[kChest] * Vec{0, 1, 0};
    check("a head that comes down looking down leans the trunk: hips behind, chest tipped", sound(b, p) && p.at[kHips].z > p.at[kHead].z + 0.25 &&
          up.z < -0.3 && up.y > 0.5 && p.at[kHips].y > at(b, kHips).y - 0.15 && std::fabs(p.at[kLeftFoot].y - at(b, kLeftFoot).y) < 2e-3);

    in = standing(b);
    in.head = nod(40);
    solver.Solve(in, p);
    check("a nod alone leaves the trunk upright", sound(b, p) && p.turn[kHips].AngleTo(Turn{}) < 1e-9 && p.turn[kUpperChest].AngleTo(Turn{}) < 1e-9 &&
          p.turn[kNeck].AngleTo(Turn{}) > 0.2);

    in = standing(b);
    in.eyes.y += 0.3;
    solver.Solve(in, p);
    check("a jump lifts the feet off the floor, the legs straight", sound(b, p) && p.at[kLeftFoot].y > at(b, kLeftFoot).y + 0.25 &&
          std::fabs(p.at[kLeftFoot].x - at(b, kLeftFoot).x) < 1e-3);
}

static void turns() {
    const Body b = person();
    Solver solver(b);
    Pose p;
    Input in = standing(b);
    in.head = yaw(90);
    in.eyes = at(b, kHead) + in.head * (Vec{b.eyes[0], b.eyes[1], b.eyes[2]} - at(b, kHead));
    solver.Solve(in, p);
    check("a pose on its own faces the way the head does", sound(b, p) && std::fabs(solver.trunkYaw() - kPi / 2) < 1e-9 &&
          far(p.at[kRightFoot], yaw(90) * at(b, kRightFoot)) < 2e-3 && p.turn[kHips].AngleTo(yaw(90)) < 1e-9);

    // looking straight down and up, the head's facing is still told
    for (double pitch : {89.0, -89.0}) {
        in.head = yaw(90) * nod(pitch);
        solver.Reset();
        solver.Solve(in, p);
        check(pitch > 0 ? "...looking straight down" : "...looking straight up", sound(b, p) && std::fabs(solver.trunkYaw() - kPi / 2) < 1e-6);
    }

    // over time the trunk waits for the head to turn far enough, then follows, never far behind
    solver.Reset();
    in = standing(b);
    in.dt = 1.0 / 90;
    solver.Solve(in, p);
    in.head = yaw(10);
    for (int i = 0; i < 180; ++i) solver.Solve(in, p);
    check("a glance aside leaves the trunk where it was", std::fabs(solver.trunkYaw()) < 1e-9 && p.turn[kHips].AngleTo(Turn{}) < 1e-9 &&
          p.turn[kUpperChest].AngleTo(yaw(5)) < 1e-9);
    in.head = yaw(60);
    solver.Solve(in, p);
    const double first = solver.trunkYaw();
    for (int i = 0; i < 270; ++i) solver.Solve(in, p);
    check("a turn of the head is followed, not at once", first > 0 && first < 3 * kPi / 180 &&
          std::fabs(solver.trunkYaw() - (60 * kPi / 180 - kTrunkDeadZone)) < 0.01);
    in.head = yaw(-170);
    solver.Solve(in, p);
    check("the trunk is never twisted far from the head", sound(b, p) && std::fabs(std::fabs(Wrap(-170 * kPi / 180 - solver.trunkYaw())) - kTrunkMaxTwist) < 1e-9);
    bool kept = true;
    for (int i = 0; i < 2000; ++i) {   // round and round, through the back
        const double a = i * 0.9;
        in.head = yaw(a);
        solver.Solve(in, p);
        const double behind = Wrap(a * kPi / 180 - solver.trunkYaw());   // 81 degrees a second: well behind, never too far
        kept = kept && sound(b, p) && std::fabs(behind) < kTrunkMaxTwist + 1e-9 && (i < 200 || behind > 0.3);
    }
    check("the trunk follows a head turning round and round", kept);
}

static void comes_and_goes() {
    const Body b = person();
    Solver solver(b);
    Pose p;
    Input in = standing(b);
    in.dt = 1.0 / 90;
    solver.Solve(in, p);
    const Vec hung = p.at[kRightHand];
    in.hand[1].tracked = true, in.hand[1].wrist = {0.3, 1.3, -0.4}, in.hand[1].turn = yaw(40);
    Vec last = hung;
    bool smooth = true;
    for (int i = 0; i < 90; ++i) {
        solver.Solve(in, p);
        smooth = smooth && sound(b, p) && far(p.at[kRightHand], last) < 0.06;
        last = p.at[kRightHand];
    }
    check("a hand that starts being tracked is reached over a moment", smooth && far(last, in.hand[1].wrist) < 1e-3 && far(hung, in.hand[1].wrist) > 0.5);
    in.hand[1].tracked = false;
    for (int i = 0; i < 90; ++i) {
        solver.Solve(in, p);
        smooth = smooth && sound(b, p) && far(p.at[kRightHand], last) < 0.06;
        last = p.at[kRightHand];
    }
    check("...and let go of the same way when it stops", smooth && far(last, hung) < 1e-3);
    solver.Reset();
    in.dt = 0;
    solver.Solve(in, p);
    check("a solver that has been reset remembers no hand", far(p.at[kRightHand], hung) < 1e-9);
}

static void holds_up() {
    const Body b = person();
    Solver solver(b);
    Pose p, before;
    solver.Solve(standing(b), before);
    p = before;
    const double bad[] = {std::nan(""), HUGE_VAL};
    bool refused = true;
    for (double v : bad) {
        Input in = standing(b);
        in.eyes.y = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.head.w = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.floor = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.eyeHeight = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.dt = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.hand[0].tracked = true, in.hand[0].wrist.x = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.hand[1].tracked = true, in.hand[1].turn.z = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.hand[1].tracked = true, in.hand[1].hasElbow = true, in.hand[1].elbow.z = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.hand[0].tracked = true, in.hand[0].fingers = true, in.hand[0].finger[14].x = v;
        refused = refused && !solver.Solve(in, p);
        in = standing(b), in.hand[0].wrist.x = v;   // not tracked: not read
        refused = refused && solver.Solve(in, p);
    }
    check("what isn't a number is refused, the pose left alone", refused && far(p.at[kHead], before.at[kHead]) < 1e-9);

    // anything at all that is numbers gives a sound pose
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1, 1);
    for (const bool all : {true, false}) {
        const Body some = person(all);
        Solver s(some);
        bool ok = true;
        for (int i = 0; i < 4000; ++i) {
            Input in = standing(some);
            in.eyes = {u(rng) * 2, 0.2 + (u(rng) + 1), u(rng) * 2};
            in.head = Turn{u(rng), u(rng), u(rng), u(rng)};   // any length: the solver brings it to one
            in.floor = u(rng) * 0.3, in.eyeHeight = 1.5 + u(rng) * 0.5, in.dt = i % 7 ? 0.011 : 0;
            for (Hand &h : in.hand) {
                h.tracked = u(rng) > -0.5, h.hasElbow = u(rng) > 0, h.fingers = u(rng) > 0;
                h.wrist = in.eyes + Vec{u(rng), u(rng), u(rng)} * (i % 5 ? 0.7 : 0), h.elbow = h.wrist + Vec{u(rng), u(rng), u(rng)} * (i % 3 ? 0.3 : 0);
                h.turn = Turn{u(rng), u(rng), u(rng), u(rng)};
                for (Turn &f : h.finger) f = Turn{u(rng), u(rng), u(rng), u(rng)};
            }
            ok = ok && s.Solve(in, p) && sound(some, p);
        }
        check(all ? "any head and hands give a sound pose" : "...on a body with only the bones it must have", ok);
    }

    // a body whose bones are all in one place, or nowhere a person's are: numbers still
    Body odd = person();
    for (int r = 0; r < kRoles; ++r) odd.at[r][0] = odd.at[r][1] = odd.at[r][2] = 0.5f;
    Body flat = person();
    for (int r = 0; r < kRoles; ++r) flat.at[r][1] = 0, flat.at[r][0] = float(r) * 1e4f;
    flat.eyes[1] = 0;
    bool finite = true;
    for (const Body *body : {&odd, &flat}) {
        Solver s(*body);
        Input in = standing(person());
        in.hand[0].tracked = in.hand[1].tracked = true, in.dt = 0.011;
        for (int i = 0; i < 3; ++i) finite = finite && s.Solve(in, p);
        float m[kRoles][12];
        s.Matrices(p, m);
        for (int r = 0; r < kRoles; ++r) {
            finite = finite && p.at[r].finite() && p.turn[r].finite();
            for (float v : m[r]) finite = finite && std::isfinite(v);
        }
    }
    check("a body with its bones in one place, or far apart, still gives numbers", finite);
}

static void stature() {
    Stature s;
    check("someone who has only sat stands a short adult's height", s.eyeHeight() == 1.45);
    for (int i = 0; i < 900; ++i) s.See(1.2, 1.0 / 90);
    check("...however long they sit", s.eyeHeight() == 1.45);
    for (int i = 0; i < 27; ++i) s.See(2.0, 1.0 / 90);   // a jump: a third of a second up there
    check("a jump barely counts", s.eyeHeight() < 1.62);
    for (int i = 0; i < 900; ++i) s.See(1.71, 1.0 / 90);
    check("standing does", std::fabs(s.eyeHeight() - 1.71) < 1e-3);
    for (int i = 0; i < 900; ++i) s.See(1.1, 1.0 / 90), s.See(std::nan(""), 1.0 / 90), s.See(5, 0), s.See(1.9, -1);
    check("it never comes down, and takes no nonsense", std::fabs(s.eyeHeight() - 1.71) < 1e-3);
    for (int i = 0; i < 2000; ++i) s.See(9, 1.0 / 90);
    check("nobody is taller than 2.2 m at the eyes", s.eyeHeight() <= 2.2 && s.eyeHeight() > 2.19);
}

int main() {
    stands();
    reaches();
    leans_and_sits();
    turns();
    comes_and_goes();
    holds_up();
    stature();
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed != 0;
}
