// The quick reset's rules (screens/recentre.h): one level step for the whole room, the lead where the
// layout has it round you, as far and as high as it was.
#include "../recentre.h"

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
using recentre::Lead;
using recentre::Level;
using recentre::Step;

static constexpr double kDeg = M_PI / 180;
static bool same(const M &a, const M &b, double tol = 1e-4) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}
// A panel `dist` out from the head at `yaw` degrees to its left (0: where it faces), `up` above it, facing it.
static M around(const M &head, double yaw, double dist, double up = 0) {
    const double a = recentre::YawOf(head) + yaw * kDeg;
    const double at[3] = {head.m[0][3] - std::sin(a) * dist, head.m[1][3] + up, head.m[2][3] - std::cos(a) * dist};
    return FacingPose(at, head);
}
static Lead<M> lead(const M &pose, const M &head, double bearing) {
    Lead<M> l;
    l.pose = pose, l.from = recentre::StoodAt(head), l.planned = true, l.bearing = bearing * kDeg;
    return l;
}

int main() {
    const M was = Level<M>(0.3, 1.6, -0.2, 0.4), now = Level<M>(-1.2, 1.1, 2.0, -2.1);
    check("a head's yaw, as ft-screens' head says it", near(recentre::YawOf(was), 0.4) && near(recentre::YawAlong(-std::sin(0.4), -std::cos(0.4)), 0.4));
    {   // looking up and to a side, tilted: the turn only
        const M tilted = Mul(now, PanelPose<M>(0, 0, 0, 0, 35, 12));
        check("a head tilted up and over: level, at its place", near(recentre::LevelHead(tilted).m[1][1], 1) && near(recentre::LevelHead(tilted).m[0][3], -1.2));
    }
    const M main = around(was, 0, 2), left = around(was, 55, 2.2, 0.3), low = around(was, -20, 0.6, -0.5);
    {
        const M s = Step(lead(main, was, 0), now, {main, left, low});
        check("where everything was placed from: all of it round you as it was", same(Mul(s, main), around(now, 0, 2)) &&
              same(Mul(s, left), around(now, 55, 2.2, 0.3)) && same(Mul(s, low), around(now, -20, 0.6, -0.5)));
        check("...and nothing tilts", near(s.m[1][1], 1) && near(s.m[1][0], 0) && near(s.m[1][2], 0));
        const M again = Step(lead(Mul(s, main), now, 0), now, {});
        check("again: it stays", same(again, Identity<M>()));
    }
    {   // the other screen moved by hand: the main one leads, and it keeps its new place beside it
        const M moved = around(was, -70, 1.1, -0.2);
        const M s = Step(lead(main, was, 0), now, {main, moved});
        check("another panel moved by hand stays where it was put", same(Mul(s, moved), around(now, -70, 1.1, -0.2)));
    }
    {   // the main one moved by hand: nearer, higher and off to the right
        const M mine = around(was, -40, 0.9, 0.25);
        const M s = Step(lead(mine, was, 0), now, {mine, left});
        check("the main one moved by hand comes straight ahead, as near and as high as you put it", same(Mul(s, mine), around(now, 0, 0.9, 0.25)));
        check("...and the others keep their place beside it", same(Mul(s, left), around(now, 95, 2.2, 0.3)));
    }
    {   // a layout with its main screen off to the left
        const M s = Step(lead(around(was, 30, 2), was, 30), now, {});
        check("a main screen the layout has to the left: to the left", same(Mul(s, around(was, 30, 2)), around(now, 30, 2)));
    }
    {   // turned on the spot by 200 degrees
        const M turned = Level<M>(0.3, 1.6, -0.2, 0.4 + 200 * kDeg);
        check("turned on the spot: it all comes round", same(Mul(Step(lead(main, was, 0), turned, {}), left), around(turned, 55, 2.2, 0.3)));
    }
    {   // no layout: the middle of what you see
        const M a = around(was, 10, 1.2), b = around(was, 50, 1.2);
        Lead<M> l = lead(a, was, 0);
        l.planned = false;
        const M s = Step(l, now, {a, b});
        check("no layout: their middle is straight ahead", same(Mul(s, a), around(now, -20, 1.2)) && same(Mul(s, b), around(now, 20, 1.2)));
        check("...a single one is", same(Mul(Step(l, now, {a}), a), around(now, 0, 1.2)));
        const M behind = around(was, 190, 1.2);
        check("...all round you there's no middle: the lead's front is", same(Mul(Step(l, now, {a, behind}), a), around(now, 0, 1.2)));
    }
    {   // the room moved meanwhile (a universe's jump, a shared room's shift): where you stood moved with it
        const M jump = Level<M>(3, 0.4, -2, 1.1), main = around(was, 20, 2);
        Lead<M> l = lead(Mul(jump, main), was, 20);
        l.from = recentre::Moved(jump, recentre::StoodAt(was));
        check("a room that jumped, and back where you stood in it: nothing moves",
              same(Step(l, Mul(jump, was), {}), Level<M>(0, 0, 0, 0)));
        check("where you stood, not known, stays that", !recentre::Moved(jump, recentre::Stood{}).known);
    }
    {   // not known where you stood
        Lead<M> l = lead(main, was, 0);
        l.from.known = false;
        const M s = Step(l, now, {main});
        check("not known from where: straight ahead, 1.5 m off, as high as it is", near(Mul(s, main).m[1][3], main.m[1][3]) && near(std::hypot(Mul(s, main).m[0][3] - now.m[0][3], Mul(s, main).m[2][3] - now.m[2][3]), 1.5));
    }
    {   // straight above you: which way it is means nothing, its own front says
        const M above = PanelPose<M>(0.3, 2.6, -0.2, 0.4 / kDeg, 60, 0);
        const M s = Step(lead(above, was, 0), now, {above});
        check("a main screen straight above: its front faces you", near(std::remainder(recentre::YawOf(Mul(s, above)) - recentre::YawOf(now), 2 * M_PI), 0) &&
              near(Mul(s, above).m[1][3], now.m[1][3] + 1.0));
    }
    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
