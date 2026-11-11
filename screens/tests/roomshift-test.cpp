// ft-screens' "shift" (screens/roomshift.h): what a shift does to a pose, one after another, the
// way back, and that the sum of many undoes them all.
#include "../posemath.h"
#include "../roomshift.h"

#include <cmath>
#include <cstdio>

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
using roomshift::Back;
using roomshift::Shift;
using roomshift::Then;

static bool same(const M &a, const M &b, double tol = 1e-4) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}
static M shifted(const Shift &s, const M &pose) { return Mul(roomshift::Pose<M>(s), pose); }
static bool none(const Shift &s) { return near(s.x, 0, 1e-9) && near(s.z, 0, 1e-9) && near(std::remainder(s.yaw, 2 * M_PI), 0, 1e-9); }

int main() {
    const M ahead = shifted({0, 0, M_PI / 2}, Translation<M>(0, 1.5, -2));   // what stood 2 m ahead (-z)
    check("a quarter turn takes what was ahead to the left, at its height", near(ahead.m[0][3], -2) && near(ahead.m[1][3], 1.5) && near(ahead.m[2][3], 0));
    check("...and turns it with it: its front (z) faces +x now, its top still up", near(ahead.m[0][2], 1) && near(ahead.m[2][2], 0) && near(ahead.m[1][1], 1));
    const M panel = PanelPose<M>(1, 2, 3, 40, 10, 5);
    check("no turn is a move on the floor, the height and the turn kept", same(shifted({0.5, -1.25, 0}, panel), Mul(Translation<M>(0.5, 0, -1.25), panel)));
    const M both = shifted({2, -3, 0.7}, Translation<M>(1, 1, 1));
    check("it turns first, about the floor's middle, then moves", near(both.m[0][3], std::cos(0.7) + std::sin(0.7) + 2) && near(both.m[1][3], 1) &&
                                                                      near(both.m[2][3], -std::sin(0.7) + std::cos(0.7) - 3));
    check("no shift moves nothing", same(shifted({}, panel), panel) && none(Shift{}));

    bool chained = true, undone = true, backNone = true;
    for (double yaw = -3; yaw <= 3; yaw += 0.7)
        for (double dx = -4; dx <= 4; dx += 3) {
            const Shift first{dx, 1 - dx, yaw}, next{1, 2, 0.4};
            chained &= same(shifted(Then(first, next), panel), shifted(next, shifted(first, panel)));
            undone &= same(shifted(Back(first), shifted(first, panel)), panel);
            backNone &= none(Then(first, Back(first))) && none(Then(Back(first), first));
        }
    check("one shift then another is the one that does both", chained);
    check("the way back puts a panel where it stood", undone);
    check("a shift and its way back are no shift, either way round", backNone);

    // A walk: many small pulls and a jump or two. The sum's way back undoes it all at once.
    Shift sum;
    M walked = panel;
    for (int i = 0; i < 2000; ++i) {
        const Shift step = i % 500 == 499 ? Shift{40.0 - i * 0.05, -25, 2.5} : Shift{0.011 * std::sin(i * 0.37), 0.009 * std::cos(i * 0.11), 0.004 * std::sin(i * 0.05)};
        sum = Then(sum, step);
        walked = shifted(step, walked);
    }
    check("after a long walk the sum is where the panel went", same(shifted(sum, panel), walked, 2e-2));
    check("...the turn kept within one turn", std::fabs(sum.yaw) <= M_PI + 1e-9);
    check("...and its way back brings the panel home, to the millimetre", same(shifted(Back(sum), shifted(sum, panel)), panel, 1e-3));
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
