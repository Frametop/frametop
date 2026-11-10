// The CPU side of ft-avatars (avatar/view): a body's mesh for the vertex buffers and its face
// shapes (bodymesh.h), a tracked hand as the pose solver takes it (handfit.h), the demo wearers
// (demo.h) and the views (viewmath.h). No GPU, OpenVR or headset.
#include "../view/bodymesh.h"
#include "../view/demo.h"
#include "../view/handfit.h"
#include "../view/viewmath.h"
#include "person.h"

#include <cstdio>
#include <cstring>

using namespace bodyfile;
using avatar::Vec;
using bodypose::Turn;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// The made-up person with a little mesh: nine vertices up the body, each on a bone or two, and
// two face shapes that move a few of them.
static Body dressed() {
    Body b = person();
    b.unit = 5e-4f;
    const int on[9][2] = {{kLeftFoot, kLeftFoot}, {kHips, kSpine}, {kChest, kChest}, {kRightLowerArm, kRightHand}, {kNeck, kHead},
                          {kHead, kHead}, {kHead, kHead}, {kHead, kNeck}, {kHead, kHead}};
    for (int v = 0; v < 9; ++v) {
        for (int k = 0; k < 3; ++k) {
            b.pos.push_back(int16_t(std::lround((b.at[on[v][0]][k] + 0.01f * float(k)) / b.unit)));
            b.nrm.push_back(int8_t(k == 2 ? -127 : 0));
        }
        b.uv.push_back(0.1f * float(v)), b.uv.push_back(1 - 0.1f * float(v));
        b.bones.insert(b.bones.end(), {uint8_t(on[v][0]), uint8_t(on[v][1]), 0, 0});
        b.weights.insert(b.weights.end(), {155, 100, 0, 0});
    }
    for (int v = 0; v + 2 < 9; ++v) b.idx.insert(b.idx.end(), {uint16_t(v), uint16_t(v + 1), uint16_t(v + 2)});
    b.parts = {{0, 7, 0}};
    b.materials.resize(1);
    Shape blink, aa;
    blink.id = Shape::kBlink, blink.verts = {5, 6}, blink.delta = {0, -20, 0, 0, -22, 2};
    aa.id = Shape::kAa, aa.verts = {6, 7}, aa.delta = {0, 4, 0, 0, -30, 0};
    b.shapes = {blink, aa};
    return b;
}

static void mesh() {
    const Body b = dressed();
    std::string why;
    check("the made-up body keeps the format's rules", Valid(b, why));
    if (!why.empty()) std::printf("      %s\n", why.c_str());
    const std::vector<bodymesh::Fixed> fixed = bodymesh::FixedOf(b);
    check("a vertex's fixed part is as the file has it", fixed.size() == 9 && sizeof(bodymesh::Fixed) == 20 && fixed[3].nrm[2] == -127 && fixed[3].uv[0] == 0.3f &&
          fixed[3].uv[1] == 0.7f && fixed[3].bones[0] == kRightLowerArm && fixed[3].bones[1] == kRightHand && fixed[3].weights[0] == 155 && fixed[3].weights[1] == 100);

    bodymesh::Face face(b);
    const std::vector<float> rest = face.pos();
    check("at rest the vertices are where the file has them, in metres", rest.size() == 27 && std::fabs(rest[3 * 5 + 1] - (b.at[kHead][1] + 0.01f)) < b.unit &&
          face.has(Shape::kBlink) && face.has(Shape::kAa) && !face.has(Shape::kOh) && !face.has(0) && !face.has(99));
    float w[bodymesh::kShapes] = {};
    int first = -1, count = -1;
    check("no weights move nothing", !face.Set(w, first, count) && face.pos() == rest);
    w[Shape::kBlink] = 0.5f;
    bool ok = face.Set(w, first, count) && first == 5 && count == 2;
    ok = ok && std::fabs(face.pos()[3 * 5 + 1] - (rest[3 * 5 + 1] - 10 * b.unit)) < 1e-6 && std::fabs(face.pos()[3 * 6 + 2] - (rest[3 * 6 + 2] + 1 * b.unit)) < 1e-6;
    check("half a blink moves its vertices half way", ok && face.pos()[3 * 7 + 1] == rest[3 * 7 + 1]);
    check("...and the same weights again move nothing", !face.Set(w, first, count));
    w[Shape::kBlink] = 0, w[Shape::kAa] = 2, w[Shape::kOh] = 1;   // over 1 counts as 1; the body has no oh
    ok = face.Set(w, first, count) && first == 5 && count == 3;   // the blink's vertices go back, the mouth's go out
    ok = ok && face.pos()[3 * 5 + 1] == rest[3 * 5 + 1] && std::fabs(face.pos()[3 * 6 + 1] - (rest[3 * 6 + 1] + 4 * b.unit)) < 1e-6;
    check("the next shape undoes the last and moves its own", ok && std::fabs(face.pos()[3 * 7 + 1] - (rest[3 * 7 + 1] - 30 * b.unit)) < 1e-6);
    w[Shape::kBlink] = 1;
    ok = face.Set(w, first, count) && first == 5 && count == 3 && std::fabs(face.pos()[3 * 6 + 1] - (rest[3 * 6 + 1] - 18 * b.unit)) < 1e-6;
    check("two shapes add up where they share a vertex", ok);
    std::memset(w, 0, sizeof w);
    check("no weights again is rest again, to the bit", face.Set(w, first, count) && first == 5 && count == 3 && face.pos() == rest);

    bodypose::Solver solver(b);
    bodypose::Pose pose;
    bodypose::Input in;
    in.eyes = {b.eyes[0], b.eyes[1], b.eyes[2]}, in.eyeHeight = b.eyes[1];
    solver.Solve(in, pose);
    float bones[kRoles][12], lo[3], hi[3], p[3];
    solver.Matrices(pose, bones);
    bodymesh::Skinned(b, &rest[3 * 4], bones, 4, p);
    ok = std::fabs(p[0] - rest[12]) < 1e-5 && std::fabs(p[1] - rest[13]) < 1e-5 && std::fabs(p[2] - rest[14]) < 1e-5;
    bodymesh::Skinned(b, &rest[3 * 3], bones, 3, p);   // on the arm, which hangs
    check("a vertex goes where its bones take it", ok && p[1] < rest[10] - 0.2);
    ok = bodymesh::Bounds(b, rest, bones, 1, lo, hi) && lo[1] < 0.12f && lo[1] > 0.05f && hi[1] > 1.5f && hi[1] < 1.6f && lo[0] < hi[0];
    check("the box round a posed body", ok && !bodymesh::Bounds(Body(), {}, bones, 1, lo, hi));
}

// The hand moved as one piece: turned by `turn` about its wrist, the wrist then at `wrist`.
static handcut::Hand moved(const handcut::Hand &h, const Turn &turn, const Vec &wrist) {
    handcut::Hand out = h;
    const Vec w = {h.p[0][0], h.p[0][1], h.p[0][2]};
    for (int k = 0; k < 21; ++k) {
        const Vec p = wrist + turn * (Vec{h.p[k][0], h.p[k][1], h.p[k][2]} - w);
        out.p[k][0] = float(p.x), out.p[k][1] = float(p.y), out.p[k][2] = float(p.z);
    }
    return out;
}

static void hands() {
    for (const bool all : {true, false}) {
        const Body b = person(all);
        const handfit::Fit fit(b);
        bool rest = true, carried = true, sides = true;
        for (int right = 0; right < 2; ++right) {
            const handcut::Hand &h = fit.rest(right);
            bodypose::Hand out;
            rest = rest && fit.Of(h, out) && out.tracked && out.fingers && out.turn.AngleTo(Turn{}) < 1e-3;
            rest = rest && std::fabs(out.wrist.x - b.at[right ? kRightHand : kLeftHand][0]) < 1e-6;
            for (const Turn &f : out.finger) rest = rest && f.AngleTo(Turn{}) < 1e-3;
            // the thumb is ahead of the little finger, and the left hand is the right's mirror
            sides = sides && h.right == bool(right) && h.p[2][2] < h.p[18][2] && (h.p[9][0] - h.p[0][0]) * (right ? 1 : -1) > 0.05;
            const Turn turn = Turn::About({0.3, 1, -0.5}, 1.1);
            const Vec wrist = {0.2, 1.1, -0.4};
            carried = carried && fit.Of(moved(h, turn, wrist), out) && out.turn.AngleTo(turn) < 2e-3 && (out.wrist - wrist).norm() < 1e-6;
            for (const Turn &f : out.finger) carried = carried && f.AngleTo(turn) < 2e-3;
        }
        check(all ? "a body's own hand at rest is no turn at all" : "...and a body with no finger bones gets fingers made up", rest && sides);
        check(all ? "a hand turned and moved as one piece turns the hand and every finger the same" : "...and they turn with the hand", carried);
    }

    const Body b = person();
    const handfit::Fit fit(b);
    handcut::Hand h = fit.rest(true);
    // the index finger's last two joints bent 0.6 down about the palm's sideways axis (z, in a T pose)
    const Vec knuckle = {h.p[5][0], h.p[5][1], h.p[5][2]};
    const Turn bend = Turn::About({0, 0, 1}, -0.6);
    for (int k = 6; k <= 8; ++k) {
        const Vec p = knuckle + bend * (Vec{h.p[k][0], h.p[k][1], h.p[k][2]} - knuckle);
        h.p[k][0] = float(p.x), h.p[k][1] = float(p.y), h.p[k][2] = float(p.z);
    }
    bodypose::Hand out;
    bool ok = fit.Of(h, out) && out.turn.AngleTo(Turn{}) < 1e-3;
    for (int i = 0; i < bodypose::kFingerBones; ++i) ok = ok && std::fabs(out.finger[i].AngleTo(Turn{}) - (i >= 3 && i < 6 ? 0.6 : 0)) < 1e-2;   // the palm's axis isn't quite z: a little twist
    check("a bent finger turns that finger's bones and no other", ok && (out.finger[3] * Vec{1, 0, 0}).y < -0.5);

    h.hasElbow = true, h.elbowConf = 0.9f, h.elbow[0] = 0.4f, h.elbow[1] = 1.2f, h.elbow[2] = 0.1f;
    ok = fit.Of(h, out) && out.hasElbow && std::fabs(out.elbow.y - 1.2) < 1e-6;
    h.elbowConf = 0.1f;
    check("the tracker's elbow is taken when it is sure of it", ok && fit.Of(h, out) && !out.hasElbow);
    for (int k = 0; k < 21; ++k) h.p[k][0] = h.p[0][0], h.p[k][1] = h.p[0][1], h.p[k][2] = h.p[0][2];
    out.tracked = true;
    check("a hand crumpled to a point isn't tracked", !fit.Of(h, out) && !out.tracked);
    h = fit.rest(false), h.p[7][1] = std::nanf("");
    check("...nor one with a joint that isn't a number", !fit.Of(h, out) && !out.tracked);
}

static void demos() {
    const Body b = person();
    const handfit::Fit fit(b);
    bodypose::Solver solver(b);
    bodypose::Pose stand, pose;
    bodypose::Input in;
    bool ok = demo::Pose("stand", 0, b, fit, in) && solver.Solve(in, stand);
    for (int r = 0; r <= kHead; ++r) ok = ok && (stand.at[r] - Vec{b.at[r][0], b.at[r][1], b.at[r][2]}).norm() < 1e-6;
    check("the demo wearer stands where the body does", ok && !in.hand[0].tracked && !in.hand[1].tracked);

    ok = true;
    char names[128];
    std::snprintf(names, sizeof names, "%s", demo::kNames);
    int count = 0;
    for (char *name = std::strtok(names, " "); name; name = std::strtok(nullptr, " "), ++count)
        for (double t : {0.0, 0.3, 1.0, 5.0}) {
            solver.Reset();
            ok = ok && demo::Pose(name, t, b, fit, in) && solver.Solve(in, pose);
            for (int r = 0; r < kRoles; ++r) ok = ok && pose.at[r].finite() && pose.turn[r].finite();
        }
    check("every named pose is one, at any moment", ok && count == 7 && !demo::Pose("flying", 0, b, fit, in) && !demo::Pose("", 0, b, fit, in));

    solver.Reset();
    demo::Pose("wave", 1, b, fit, in), solver.Solve(in, pose);
    ok = in.hand[1].tracked && !in.hand[0].tracked && (pose.at[kRightHand] - in.hand[1].wrist).norm() < 1e-6 && pose.at[kRightHand].y > pose.at[kRightUpperArm].y;
    // the fingers point up and the palm faces ahead: at rest they point along +x, the palm down
    check("a wave: the right hand up by the head, palm ahead", ok && (pose.turn[kRightHand] * Vec{1, 0, 0}).y > 0.8 && (pose.turn[kRightHand] * Vec{0, -1, 0}).z < -0.8);
    demo::Pose("reach", 1, b, fit, in), solver.Solve(in, pose);
    ok = in.hand[0].tracked && in.hand[1].tracked && pose.at[kLeftHand].z < -0.4 && pose.at[kRightHand].z < -0.4;
    check("a reach: both hands out ahead, mirrored, palms down", ok && std::fabs(pose.at[kLeftHand].x + pose.at[kRightHand].x) < 1e-6 &&
          (pose.turn[kLeftHand] * Vec{0, -1, 0}).y < -0.8 && (pose.turn[kRightHand] * Vec{0, -1, 0}).y < -0.8);
    demo::Pose("sit", 1, b, fit, in), solver.Solve(in, pose);
    check("a sit: the hips down, the feet on the floor", pose.at[kHips].y < stand.at[kHips].y - 0.4 && std::fabs(pose.at[kLeftFoot].y - stand.at[kLeftFoot].y) < 2e-3);
    Body tall = person();
    for (int r = 0; r < kRoles; ++r)
        for (float &v : tall.at[r]) v *= 1.25f;
    for (float &v : tall.eyes) v *= 1.25f;
    const handfit::Fit tallFit(tall);
    bodypose::Solver tallSolver(tall);
    demo::Pose("wave", 1, tall, tallFit, in), tallSolver.Solve(in, pose);
    check("a taller body's wearer is as tall as it", std::fabs(pose.scale - 1) < 1e-9 && std::fabs(in.eyes.y - 2.0) < 1e-6 && (pose.at[kRightHand] - in.hand[1].wrist).norm() < 1e-6);
}

static void views() {
    using namespace viewmath;
    const Mat4 view = Mul(Perspective(0.6, 2, 0.1, 50), LookAt({0, 1, -3}, {0, 1, 0}, {0, 1, 0}));
    const Vec mid = Project(view, {0, 1, 0}), up = Project(view, {0, 1.5, 0}), right = Project(view, {-0.5, 1, 0}), back = Project(view, {0, 1, 2});
    check("a camera looks at what it's pointed at", std::fabs(mid.x) < 1e-6 && std::fabs(mid.y) < 1e-6 && mid.z > -1 && mid.z < 1);
    // facing +z, the room's -x is on the camera's right; a lens twice as wide as tall halves x
    check("...the right way up and round", up.y > 0.4 && std::fabs(up.x) < 1e-6 && right.x > 0.2 && std::fabs(right.x * 2 - up.y) < 1e-6 && back.z > mid.z);
    const Mat4 mirror = Mirror({0, 0, -1}, {0, 0.6, 0.8});
    const Vec on = Project(mirror, {3, 0, -1}), seen = Project(mirror, {0, 0.6, -0.2});
    check("a mirror leaves what's on it and puts what's before it as far behind", (on - Vec{3, 0, -1}).norm() < 1e-6 && (seen - Vec{0, -0.6, -1.8}).norm() < 1e-6);
    const Mat4 twice = Mul(mirror, mirror);
    bool same = true;
    for (int i = 0; i < 16; ++i) same = same && std::fabs(twice.m[i] - Mat4().m[i]) < 1e-6;
    check("...and a mirror of a mirror is the room", same);
}

int main() {
    mesh();
    hands();
    demos();
    views();
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed != 0;
}
