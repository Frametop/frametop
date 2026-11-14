// The layout grab's rules (screens/layoutgrab.h): the level frame at the carried screen, and the other
// panels' places in it as it goes round the head, up, nearer, and through ticks without a frame.
#include "../layoutgrab.h"

#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) < tol; }

struct M {  // as vr::HmdMatrix34_t
    float m[3][4];
};
using namespace posemath;
using layoutgrab::Frame;
using layoutgrab::Grab;

static bool same(const M &a, const M &b, double tol = 1e-4) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}
static double apart(const M &a, const M &b) {
    return std::sqrt(std::pow(a.m[0][3] - b.m[0][3], 2) + std::pow(a.m[1][3] - b.m[1][3], 2) + std::pow(a.m[2][3] - b.m[2][3], 2));
}
// A head at (x, y, z), looking any way: only its place counts.
static M headAt(double x, double y, double z) { return Translation<M>(x, y, z); }
// A panel `dist` out from the head at `yawDeg` round it (0: straight ahead, -z), `up` above the eyes, facing it.
static M around(const M &head, double yawDeg, double dist, double up = 0) {
    const double a = yawDeg * M_PI / 180;
    const double at[3] = {head.m[0][3] - std::sin(a) * dist, head.m[1][3] + up, head.m[2][3] - std::cos(a) * dist};
    return FacingPose(at, head);
}
static M frameOf(const M &carried, const M &head) {
    M f{};
    check("  (a frame)", Frame(carried, head, &f));
    return f;
}

static void frames() {
    const M head = headAt(0.3, 1.6, -0.2);
    const M ahead = around(head, 0, 2);
    M f{};
    check("a frame for a screen straight ahead", Frame(ahead, head, &f));
    check("...at the screen's centre", near(f.m[0][3], ahead.m[0][3]) && near(f.m[1][3], ahead.m[1][3]) && near(f.m[2][3], ahead.m[2][3]));
    check("...its top straight up", near(f.m[0][1], 0) && near(f.m[1][1], 1) && near(f.m[2][1], 0));
    check("...its front level, toward the head", near(f.m[0][2], 0) && near(f.m[1][2], 0) && near(f.m[2][2], 1));
    check("...a level screen facing the head is the frame itself", same(f, ahead));

    // the hand turns the carried screen about its centre: the frame doesn't care
    const M twisted = Mul(ahead, Mul(RollZ<M>(0.6), PanelPose<M>(0, 0, 0, 25, -30, 0)));
    M g{};
    check("a screen tilted and rolled in the hand: the same frame", Frame(twisted, head, &g) && same(f, g));

    // higher than the eyes: still level
    const M high = around(head, 40, 2, 1.0);
    check("a screen above eye height: a level frame", Frame(high, head, &g) && near(g.m[1][1], 1) && near(g.m[1][2], 0) && near(g.m[1][0], 0));

    M keep = f;
    check("straight above the head: no frame", !Frame(Translation<M>(0.3, 3.0, -0.2), head, &keep) && same(keep, f));
    check("20 degrees off straight above: no frame", !Frame(around(head, 0, std::tan(20 * M_PI / 180), 1.0), head, &keep));
    check("40 degrees off straight above: a frame", Frame(around(head, 0, std::tan(40 * M_PI / 180), 1.0), head, &keep));
    check("straight below: no frame", !Frame(Translation<M>(0.3, 0.2, -0.2), head, &keep));
}

static void moves() {
    const M head = headAt(0, 1.6, 0);
    const M centre = around(head, 0, 2), left = around(head, 55, 2.2), right = around(head, -55, 2.2), low = around(head, 10, 1.2, -0.5);
    Grab<M> g;
    check("no grab at first", !g.on() && !g.Moves(0));
    g.Begin(0, frameOf(centre, head));
    g.Take(1, left), g.Take(2, right), g.Take(3, low);
    check("on, and it moves the carried screen and the others", g.on() && g.Moves(0) && g.Moves(1) && g.Moves(3) && !g.Moves(7));
    check("nothing moved yet: each is where it was", same(g.Pose(1), left) && same(g.Pose(2), right) && same(g.Pose(3), low));

    // the carried screen goes a quarter turn round the head
    M step = g.Turn(frameOf(around(head, 90, 2), head));
    check("round the head: the others go round it too", same(g.Pose(1), around(head, 145, 2.2)) && same(g.Pose(2), around(head, 35, 2.2)));
    check("...as far from the head as they were", near(apart(g.Pose(3), head), apart(low, head)));
    check("...and the tick's step is that turn", same(Mul(step, left), g.Pose(1)) && same(Mul(step, low), g.Pose(3)));

    // back, then up 0.4 m: all of it rises, nothing tilts
    g.Turn(frameOf(centre, head));
    check("back again: exactly where they were", same(g.Pose(1), left) && same(g.Pose(2), right) && same(g.Pose(3), low));
    g.Turn(frameOf(around(head, 0, 2, 0.4), head));
    M up = left;
    up.m[1][3] += 0.4f;
    check("the carried screen up 0.4 m: the others up 0.4 m, turned as they were", same(g.Pose(1), up));

    // nearer by 0.5 m: the layout comes 0.5 m the same way
    g.Turn(frameOf(around(head, 0, 1.5), head));
    M nearer = right;
    nearer.m[2][3] += 0.5f;
    check("the carried screen 0.5 m nearer: the others 0.5 m that way", same(g.Pose(2), nearer));

    // the head walks a metre to the side with the carried screen held as far ahead of it
    const M walked = headAt(1, 1.6, 0);
    g.Turn(frameOf(centre, head));
    g.Turn(frameOf(around(walked, 0, 2), walked));
    check("the wearer walks with it: the layout is round them there", same(g.Pose(1), around(walked, 55, 2.2)));

    g.End();
    check("ended: off, and it moves nothing", !g.on() && !g.Moves(0) && !g.Moves(1));
}

static void heldMeanwhile() {
    const M head = headAt(0, 1.6, 0);
    Grab<M> g;
    g.Begin(0, frameOf(around(head, 0, 2), head));
    g.Take(1, around(head, 50, 2));
    g.Turn(frameOf(around(head, 30, 2), head));
    // another laser took panel 1 and put it somewhere else: it goes on from there
    const M put = around(head, -120, 1.1, 0.2);
    g.Take(1, put);
    check("taken again where another hand put it", same(g.Pose(1), put));
    g.Turn(frameOf(around(head, 50, 2), head));
    check("...and it goes on from there", same(g.Pose(1), around(head, -100, 1.1, 0.2)));
}

static void lost() {
    const M head = headAt(0, 1.6, 0);
    Grab<M> g;
    g.Begin(0, frameOf(around(head, 0, 2), head));
    g.Take(1, around(head, 50, 2));
    g.Turn(frameOf(around(head, 20, 2), head));
    const M before = g.Pose(1);
    // the carried screen goes over the head and comes down behind it: no frame on the way
    g.Lose();
    check("a tick without a frame: the others stay", same(g.Pose(1), before));
    M step = g.Turn(frameOf(around(head, 170, 2), head));
    check("the frame is back somewhere else: nothing jumps", same(g.Pose(1), before) && same(step, Identity<M>()));
    g.Turn(frameOf(around(head, 180, 2), head));
    check("...and the layout goes on from where it stayed", same(g.Pose(1), around(head, 80, 2)));
}

static void noDrift() {
    const M head = headAt(0.1, 1.5, 0.4);
    const M start = around(head, 0, 2), other = around(head, -70, 2.5, 0.3);
    Grab<M> g;
    g.Begin(0, frameOf(start, head));
    g.Take(1, other);
    M f{};
    for (int k = 1; k <= 5000; ++k)   // about a minute of ticks, all over the room
        if (Frame(around(head, 0.37 * k, 1.2 + 0.8 * std::sin(k * 0.01), 0.5 * std::sin(k * 0.013)), head, &f)) g.Turn(f);
    g.Turn(frameOf(start, head));
    check("5000 ticks round the room and back: where it was, to a tenth of a millimetre", same(g.Pose(1), other));
}

int main() {
    frames();
    moves();
    heldMeanwhile();
    lost();
    noDrift();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
