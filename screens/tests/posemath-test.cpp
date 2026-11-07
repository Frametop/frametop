// ft-screens' pose math (screens/posemath.h): rigid poses, PanelPose, RollZ, LevelRoll and FacingPose.
#include "../posemath.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) < tol; }

struct M {  // as vr::HmdMatrix34_t
    float m[3][4];
};
using namespace posemath;

static bool same(const M &a, const M &b, double tol = 1e-5) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}
// The rotation part is orthonormal and right-handed.
static bool rigid(const M &m) {
    double c[3][3];
    for (int j = 0; j < 3; ++j) Column(m, j, c[j]);
    const double cross[3] = {c[0][1] * c[1][2] - c[0][2] * c[1][1], c[0][2] * c[1][0] - c[0][0] * c[1][2],
                             c[0][0] * c[1][1] - c[0][1] * c[1][0]};
    return near(Dot3(c[0], c[0]), 1) && near(Dot3(c[1], c[1]), 1) && near(Dot3(c[2], c[2]), 1) &&
           near(Dot3(c[0], c[1]), 0) && near(Dot3(c[1], c[2]), 0) && near(Dot3(c[0], c[2]), 0) &&
           near(Dot3(cross, c[2]), 1);
}

int main() {
    const M I = Identity<M>();
    check("identity", I.m[0][0] == 1 && I.m[1][1] == 1 && I.m[2][2] == 1 && I.m[0][1] == 0 && I.m[0][3] == 0);
    const M t = Translation<M>(1, 2, -3);
    check("translation", t.m[0][3] == 1 && t.m[1][3] == 2 && t.m[2][3] == -3 && t.m[0][0] == 1);

    // PanelPose: yaw 0, pitch 0 is seen from straight ahead (-z), so its front faces +z: no turn at all.
    M p = PanelPose<M>(0.5, 1.2, -2, 0, 0, 0);
    check("PanelPose: seen from ahead, no turn", same(p, Translation<M>(0.5, 1.2, -2)));
    // Seen from yaw 90 (to the left, -x): the front faces +x, right is -z, up stays up.
    p = PanelPose<M>(0, 0, 0, 90, 0, 0);
    check("PanelPose: yaw 90, the front faces +x", near(p.m[0][2], 1) && near(p.m[2][0], -1) && near(p.m[1][1], 1));
    // Pitched up 30: the front tilts down toward the viewer below, and right stays horizontal.
    p = PanelPose<M>(0, 0, 0, 0, 30, 0);
    check("PanelPose: pitch 30, the front tilts down, right stays level",
          near(p.m[1][2], -0.5) && near(p.m[2][2], std::sqrt(3) / 2) && near(p.m[1][0], 0) && near(p.m[0][0], 1));
    // Roll 90 turns right into up.
    p = PanelPose<M>(0, 0, 0, 0, 0, 90);
    check("PanelPose: roll 90 turns right into up", near(p.m[1][0], 1) && near(p.m[0][1], -1));
    bool allRigid = true;
    for (double yaw = -180; yaw <= 180; yaw += 37)
        for (double pitch = -80; pitch <= 80; pitch += 23)
            for (double roll = -90; roll <= 90; roll += 45) allRigid &= rigid(PanelPose<M>(1, 2, 3, yaw, pitch, roll));
    check("PanelPose: rigid for every yaw, pitch and roll tried", allRigid);
    // Straight up (pitch 90) has no horizontal front: right falls back to zero length, not NaN.
    p = PanelPose<M>(0, 0, 0, 0, 90, 0);
    check("PanelPose: pitch 90 gives no NaN", !std::isnan(p.m[0][0]) && !std::isnan(p.m[1][1]));

    // LevelRoll (DEE-337): the roll that takes a tilted screen level where it is, its front kept.
    bool allLevel = true;
    for (double yaw : {-150.0, 0.0, 20.0, 95.0})
        for (double pitch : {-40.0, 0.0, 30.0})
            for (double roll : {-170.0, -30.0, -1.5, 0.0, 2.0, 30.0, 170.0}) {
                const M tilted = PanelPose<M>(1, 2, 3, yaw, pitch, roll);
                double rad;
                allLevel &= LevelRoll(tilted, &rad) && near(rad, -roll * M_PI / 180, 1e-4) &&
                            same(Mul(tilted, RollZ<M>(rad)), PanelPose<M>(1, 2, 3, yaw, pitch, 0), 1e-4);
            }
    check("LevelRoll: undoes the roll for every yaw, pitch and roll tried, centre and front kept", allLevel);
    double rad = 1;
    check("LevelRoll: a level screen turns 0", LevelRoll(PanelPose<M>(0, 1, -1, 10, 5, 0), &rad) && near(rad, 0));
    M up = Identity<M>();   // facing straight up: x right, y toward -z, z up
    up.m[1][1] = 0, up.m[2][1] = -1, up.m[1][2] = 1, up.m[2][2] = 0;
    check("LevelRoll: facing straight up has no level", rigid(up) && !LevelRoll(up, &rad));
    check("RollZ: a quarter turn takes right to up", near(RollZ<M>(M_PI / 2).m[1][0], 1) && rigid(RollZ<M>(0.3)));

    // FacingPose: its z points at the head, upright (x level).
    M head = Translation<M>(0, 1.6, 0);
    const double pt[3] = {1, 1.6, -1};
    M f = FacingPose(pt, head);
    double z[3], x[3];
    Column(f, 2, z), Column(f, 0, x);
    const double s = 1 / std::sqrt(2.0);
    check("FacingPose: z toward the head", near(z[0], -s) && near(z[1], 0) && near(z[2], s));
    check("FacingPose: x level, at pt", near(x[1], 0) && rigid(f) && near(f.m[0][3], 1) && near(f.m[2][3], -1));
    const double below[3] = {0, 0.6, 0};
    f = FacingPose(below, head);
    Column(f, 0, x);
    check("FacingPose: straight below the head, x falls back to +x", near(x[0], 1) && near(x[2], 0) && rigid(f));

    // FacingTurn and FaceAbout (DEE-336): a panel to your left made 0.6 m wider from its left corners,
    // its right ones kept, slides left in its own plane and ends up turned away from you; FaceAbout
    // turns it back about the kept corner, and keeps a turn it had on purpose.
    const double left[3] = {-0.9, 1.5, -1.4};
    const M facing = FacingPose(left, head);
    check("FacingTurn: a panel facing the head has none", same(FacingTurn(facing, head), I));
    const double W = 1.2, H = 0.7, kx = W / 2 + 0.3, ky = H / 2;   // the kept top right corner, after the resize
    auto cornerAt = [&](const M &p) { return Mul(p, Translation<M>(kx, ky, 0)); };
    const M slid = Mul(facing, Translation<M>(-0.3, 0, 0));        // the resize as it was: in its plane
    check("FacingTurn: a panel slid in its plane is turned from the head", !same(FacingTurn(slid, head), I, 1e-3));
    M faced{};
    check("FaceAbout: settles", FaceAbout(slid, kx, ky, I, head, &faced));
    const M kept = cornerAt(slid), now = cornerAt(faced);
    check("FaceAbout: the kept corner stays", near(now.m[0][3], kept.m[0][3], 1e-4) && near(now.m[1][3], kept.m[1][3], 1e-4) &&
                                                   near(now.m[2][3], kept.m[2][3], 1e-4));
    check("FaceAbout: facing the head again, level", rigid(faced) && same(FacingTurn(faced, head), I, 1e-4));
    const M tilted = Mul(facing, PanelPose<M>(0, 0, 0, 12, -20, 0));   // turned and tipped back on purpose
    const M turn = FacingTurn(tilted, head);
    check("FaceAbout: a turn it had stays", FaceAbout(Mul(tilted, Translation<M>(-0.3, 0, 0)), kx, ky, turn, head, &faced) &&
                                                rigid(faced) && same(FacingTurn(faced, head), turn, 1e-4));
    check("FaceAbout: no size change, no move", FaceAbout(facing, W / 2, ky, I, head, &faced) && same(faced, facing, 1e-4));
    // Overhead (lying down, a panel on the ceiling): facing has no steady level there, so no turn.
    const double above[3] = {0.02, 2.6, 0};
    M out = I;
    check("FaceAbout: not straight above the head", !FaceAbout(FacingPose(above, head), 0.4, 0.2, I, head, &out) && same(out, I));
    const double steepish[3] = {0, 2.2, -0.8};   // 37 degrees up: still turns
    check("FaceAbout: steep but not overhead still turns",
          FaceAbout(Mul(FacingPose(steepish, head), Translation<M>(-0.1, 0, 0)), 0.5, 0.2, I, head, &out) && rigid(out));

    // Mul and Inverse: a pose times its inverse is the identity, and Mul composes in order.
    const M a = Mul(Translation<M>(1, 0, 0), PanelPose<M>(0, 0, 0, 90, 0, 0));
    check("Mul: rotate, then translate", near(a.m[0][3], 1) && near(a.m[0][2], 1));
    const M q = PanelPose<M>(0.3, 1.1, -0.7, 33, -12, 8);
    check("Inverse: q * q^-1 is the identity", same(Mul(q, Inverse(q)), I) && same(Mul(Inverse(q), q), I));
    const M b = Mul(Translation<M>(0, 0, -1), Translation<M>(0, 2, 0));
    check("Mul: translations add", near(b.m[1][3], 2) && near(b.m[2][3], -1));
    return failed ? 1 : 0;
}
