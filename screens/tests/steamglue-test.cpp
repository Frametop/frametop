// Steam's menu glued to its page (screens/steamglue.h): the part it draws, and where it goes.
#include "../steamglue.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

using namespace steamglue;

int main() {
    {   // Steam's menu as measured on the Frame (300 x 800 CSS at 1.5 in a 450 x 1200 window)
        Rect r;
        check("the strip", FromCss(246, 154.5, 300, 645.5, 1.5, 450, 1200, &r) && r.x == 369 && r.y == 231 && r.w == 81 &&
                               r.h == 738);
        check("opened, its labels too", FromCss(57.6, 154.5, 300, 645.5, 1.5, 450, 1200, &r) && r.x == 86 && r.w == 364);
        check("clipped to the window", FromCss(-10, -5, 400, 900, 1.5, 450, 1200, &r) && r.x == 0 && r.y == 0 && r.w == 450 &&
                                           r.h == 1200);
        check("nothing drawn: none", !FromCss(1e9, 1e9, -1e9, -1e9, 1.5, 450, 1200, &r));
        check("off the window: none", !FromCss(500, 0, 600, 10, 1.5, 450, 1200, &r));
        check("no pixel ratio: none", !FromCss(0, 0, 10, 10, 0, 450, 1200, &r));
        check("NaN: none", !FromCss(std::nan(""), 0, 10, 10, 1, 450, 1200, &r));
        check("no window: none", !FromCss(0, 0, 10, 10, 1, 0, 1200, &r));
    }
    {   // the crop sliding: Steam's strip, then its labels open, then shut again mid-way
        const Rect strip{369, 231, 81, 738}, open{86, 231, 364, 738};
        Slide s;
        check("nothing yet", !s.Set());
        s.To(strip, 1000);
        const Rect a = s.At(1000);
        check("the first rect as it is", a.x == 369 && a.w == 81 && a.y == 231 && a.h == 738);
        s.To(open, 2000);
        check("opening starts where it was", s.At(2000).x == 369);
        const Rect half = s.At(2000 + kSlideMs / 2);
        check("half the time: most of the way (eased out)", half.x < (369 + 86) / 2 && half.x > 86);
        check("its right edge stays put", half.x + half.w == 450);
        check("there after kSlideMs", s.At(2000 + kSlideMs).x == 86 && s.At(2000 + kSlideMs).w == 364);
        s.To(open, 2000 + kSlideMs / 2);   // (the same target again: not started over)
        check("the same target: no restart", s.At(2000 + kSlideMs).x == 86);
        const int mid = s.At(5000 + 50).x;   // (long since open)
        s.To(strip, 5000);
        const int turnAt = s.At(5000 + 40).x;
        s.To(open, 5000 + 40);   // the laser back mid-way: turned round from where it is
        check("turned round without a jump", std::abs(s.At(5000 + 40).x - turnAt) <= 1 && mid == 86);
        check("then open again", s.At(5000 + 40 + kSlideMs).x == 86);
    }
    {   // its rest, after the laser left: not Steam's open menu, however long Steam takes to shut it
        const Rect rest{360, 231, 90, 738}, open{77, 231, 373, 738}, ring{356, 231, 94, 738}, none{};
        check("still open as the laser left: not the rest", !NewRest(open, rest, open));
        check("shut again: no change", !NewRest(rest, rest, open));
        check("a new strip once it's shut: the rest", NewRest(ring, rest, open));
        check("never opened: a new strip is the rest", NewRest(ring, rest, rest));
        check("opened after the laser left (not what it drew then): the rest", NewRest(open, rest, rest));
        check("no rest yet: the first is taken", NewRest(open, none, open));
    }
    {   // flat: flush left, level, facing the same way
        double m[3][4];
        Beside(1.6, 0, 0.1, 0.02, m);
        check("its middle half of both widths left", Near(m[0][3], -0.85) && Near(m[1][3], 0.02) && Near(m[2][3], 0));
        check("not turned", Near(m[0][0], 1) && Near(m[2][2], 1) && Near(m[0][2], 0));
        // its right edge (x + w/2 along its own x) on the page's left edge
        check("its right edge on the page's left edge", Near(m[0][3] + m[0][0] * 0.05, -0.8));
    }
    {   // curved: on round the page's circle
        const double R = 2, pageW = 1.6, w = 0.1;
        double m[3][4];
        Beside(pageW, R, w, 0, m);
        // the circle's centre is (0, 0, R) in the page's frame: the glued one's middle is R from it
        const double dx = m[0][3], dz = m[2][3] - R;
        check("on the circle", Near(std::hypot(dx, dz), R, 1e-12));
        const double a = std::atan2(-dx, -dz);   // round from the page's middle (0, 0, 0)
        check("as far round as both half widths", Near(a, (pageW + w) / (2 * R), 1e-12));
        // its front (its z axis) points at the circle's centre
        const double fx = m[0][2], fz = m[2][2];
        check("facing the centre", Near(fx * -dx + fz * -dz, R, 1e-12));
        check("upright", Near(m[1][1], 1) && Near(m[1][0], 0) && Near(m[0][1], 0));
        check("a rotation", Near(m[0][0] * m[0][0] + m[2][0] * m[2][0], 1, 1e-12));
    }
    {   // the goal: open once Steam has, shut once Steam has (its icons first: user, 2026-11-18)
        const Rect rest{369, 231, 81, 738}, open{86, 231, 364, 738};
        check("goal: the laser on it, what Steam draws", Goal(true, open, rest, 0) == open && Goal(true, rest, rest, 0) == rest);
        check("goal: rest not known yet, what Steam draws", Goal(false, open, Rect{}, 99999) == open);
        check("goal: just left, Steam still open: stays open", Goal(false, open, rest, 0) == open && Shutting(open, rest, 0));
        check("goal: ...800 ms on, Steam still open: still open", Goal(false, open, rest, 800) == open);
        check("goal: Steam shut: the rest", Goal(false, rest, rest, 850) == rest && !Shutting(rest, rest, 850));
        check("goal: Steam never shuts: the rest after kShutWaitMs",
              Goal(false, open, rest, kShutWaitMs - 1) == open && Goal(false, open, rest, kShutWaitMs) == rest &&
                  !Shutting(open, rest, kShutWaitMs));
    }
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
