// A PC app window's scale steps (appscale.h).
#include "../appscale.h"

#include <cstdio>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using appscale::Step;
    check("one step bigger", appscale::Same(Step(1.0, 1), 1.25) && appscale::Same(Step(1.5, 1), 1.6));
    check("one step smaller", appscale::Same(Step(1.25, -1), 1.0) && appscale::Same(Step(2.0, -1), 1.75));
    check("several at once", appscale::Same(Step(1.0, 3), 1.6) && appscale::Same(Step(2.5, -7), 1.0));
    check("the ends hold", appscale::Same(Step(2.5, 2), 2.5) && appscale::Same(Step(1.0, -1), 1.0));
    check("from between two steps, to the one beside it", appscale::Same(Step(5.0 / 3, 1), 1.75) && appscale::Same(Step(5.0 / 3, -1), 1.6));
    check("past the steps: bigger stays, smaller comes in", appscale::Same(Step(3.0, 1), 3.0) && appscale::Same(Step(3.0, -1), 2.5));
    check("below them too", appscale::Same(Step(0.75, -1), 0.75) && appscale::Same(Step(0.75, 1), 1.0));
    check("no step", Step(1.1, 0) == 1.1);
    appscale::Wheel w;
    check("a whole notch is a step", w.Add(1) == 1 && w.Add(-2) == -2);
    check("a smooth wheel's parts add up", w.Add(0.4) == 0 && w.Add(0.4) == 0 && w.Add(0.4) == 1);
    check("a turn back starts over", w.Add(-0.9) == 0 && w.Add(-0.2) == -1);
    check("labels", appscale::Label(appscale::Percent(5.0 / 3)) == "167%" && appscale::Label(100) == "100%");
    using appscale::Asking;
    {
        Asking a;
        a.Scroll(1, 1000);
        check("a scroll shows at once, and asks once it rests", appscale::Same(a.scale, 1.25) && !a.Due(1219) && a.Due(1220) && !a.Due(1221));
        a.Scroll(1, 1300);
        a.Scroll(1, 1400);
        check("the next waits for the answer", !a.Due(1700));
        a.Said(1.25, true);
        check("an answer doesn't take back a newer one", appscale::Same(a.scale, 1.6) && a.Due(1700) && appscale::Same(a.scale, 1.6));
        a.Said(1.5, true);   // (the PC has another)
        check("its own answer is the PC's", appscale::Same(a.scale, 1.5) && !a.Due(9000));
    }
    {
        Asking a;
        a.Pick(2, 1000);
        check("a pick asks now", a.Due(1000));
        a.Pick(2, 1100);
        check("the same again asks nothing", !a.Due(1100));
        a.Pick(1, 1200);
        check("no answer: the next goes after 30 s", !a.Due(30999) && a.Due(31000));
        a.Said(1, false);   // (its stream starts over at it)
        a.Scroll(0.4, 40000);
        check("part of a notch asks nothing", !a.Due(50000));
    }
    {
        Asking a;
        a.Scroll(1, 1000);
        a.Due(1220);
        a.Said(1, false);   // (a stream starting over meanwhile, from before the ask)
        a.Scroll(1, 1300);
        check("what's said of before the ask isn't its answer", appscale::Same(a.scale, 1.5) && !a.Due(1600));
        a.Said(1, true);
        check("a refused ask lets the next go", a.Due(1600) && appscale::Same(a.asked, 1.5));
        a.Said(1, true);
        check("... and the last one refused goes back", appscale::Same(a.scale, 1) && !a.Due(9000));
    }
    check("a size a scale moved is near the one asked for", appscale::Near(1340, 910, 1336, 910) && !appscale::Near(1354, 910, 1336, 910));
    if (failed) std::printf("%d failed\n", failed);
    return failed != 0;
}
