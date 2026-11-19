// The grab bar's menu (panelmenu.h): the rows each kind of panel gets, the tap, the fingertip tap on a
// target of ours (tiptap.h) at every poke speed, and the panel kept from hand input after one
// (handsoff.h), on flat and curved panels.
#include "../handsoff.h"
#include "../panelmenu.h"
#include "../posemath.h"
#include "../tiptap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using panelmenu::Item;
    auto has = [](const std::vector<panelmenu::Row> &rows, Item i) {
        return std::any_of(rows.begin(), rows.end(), [&](const panelmenu::Row &r) { return r.item == i; });
    };

    // Every button the controls had is a row for the kinds that had it.
    panelmenu::Kind desktop, floating, game, steam;
    floating.floating = true;
    game.gsapp = true, game.theater = true;
    steam.theater = true;
    const auto d = panelmenu::Main(desktop), f = panelmenu::Main(floating), g = panelmenu::Main(game), st = panelmenu::Main(steam);
    check("a desktop screen: curve, rotate, in view, dashboard, move all, bar position, reset",
          has(d, Item::Curve) && has(d, Item::Rotate) && has(d, Item::KeepInView) && has(d, Item::OnDashboard) &&
              has(d, Item::MoveAll) && has(d, Item::BarPosition) && has(d, Item::Reset));
    check("a desktop screen: no window or theater rows", !has(d, Item::Dock) && !has(d, Item::Close) && !has(d, Item::Theater));
    check("a floating window: back to desktop and close, not the layout's rows",
          has(f, Item::Dock) && has(f, Item::Close) && !has(f, Item::KeepInView) && !has(f, Item::MoveAll) && !has(f, Item::Reset));
    check("a Steam game: View in Theater, no layout or window rows",
          has(g, Item::Theater) && !has(g, Item::Dock) && !has(g, Item::Reset) && !has(g, Item::KeepInView));
    check("Steam's tile: View in Theater and the layout's rows", has(st, Item::Theater) && has(st, Item::Reset));
    check("not anchored: no Stay Shown", !has(d, Item::StayShown));
    panelmenu::Kind anchored;
    anchored.anchored = true;
    check("anchored: Stay Shown, right after On Dashboard", [&] {
        const auto a = panelmenu::Main(anchored);
        const auto on = std::find(a.begin(), a.end(), panelmenu::Row{Item::OnDashboard});
        return on != a.end() && on + 1 != a.end() && (on + 1)->item == Item::StayShown;
    }());
    check("reset is the last row, after a rule", d.back().item == Item::Reset && panelmenu::RuleAbove(d, d.size() - 1));
    check("no rule above the first row", !panelmenu::RuleAbove(d, 0));

    // The edges page: four edges, Back from the main page; from a right click as before (Theater under them).
    const auto fromMain = panelmenu::Edges(game, true), fromRight = panelmenu::Edges(game, false);
    check("edges from the main page: Back, then the four", fromMain.size() == 5 && fromMain[0].item == Item::Back &&
          fromMain[1] == panelmenu::Row{Item::Edge, barside::Side::Bottom} && fromMain[4] == panelmenu::Row{Item::Edge, barside::Side::Right});
    check("edges from a right click: the four, then View in Theater", fromRight.size() == 5 && fromRight[0].item == Item::Edge &&
          fromRight[4].item == Item::Theater && panelmenu::RuleAbove(fromRight, 4));
    check("a desktop screen's right click: just the edges", panelmenu::Edges(desktop, false).size() == 4);
    check("edge rows differ by their side", !(panelmenu::Row{Item::Edge, barside::Side::Top} == panelmenu::Row{Item::Edge, barside::Side::Left}));
    for (const auto &rows : {d, f, g, st, fromMain, fromRight})
        for (const auto &r : rows) check("every row has a label", *panelmenu::Label(r) != 0);
    check("the texture fits every page", barside::RowAt(1, barside::kMenuPad + 1, int(d.size())) == int(d.size()) - 1);

    check("rotate and move all are held, the rest clicked", panelmenu::Held(Item::Rotate) && panelmenu::Held(Item::MoveAll) &&
          !panelmenu::Held(Item::Curve) && !panelmenu::Held(Item::Reset) && !panelmenu::Held(Item::BarPosition));
    check("the toggles", panelmenu::Toggle(Item::Curve) && panelmenu::Toggle(Item::KeepInView) &&
          panelmenu::Toggle(Item::OnDashboard) && !panelmenu::Toggle(Item::MoveAll) && !panelmenu::Toggle(Item::Reset));

    // The tap: quick, the device still.
    check("a quick still press is a tap", panelmenu::IsTap(120, 0.004, 0.01));
    check("held too long: a move", !panelmenu::IsTap(350, 0.0, 0.0));
    check("a soft 300 ms pull is still a tap", panelmenu::IsTap(300, 0.002, 0.0));
    check("moved: a move", !panelmenu::IsTap(100, 0.02, 0.0));
    check("turned: a move", !panelmenu::IsTap(100, 0.0, 0.07));
    check("no press: not a tap", !panelmenu::IsTap(-1, 0, 0));

    // The fingertip: from in front, through its face, within its edges; once.
    {
        tiptap::Tap t;
        const double front[3] = {0.0, 0.0, 0.03}, face[3] = {0.0, 0.0, 0.001}, beyond[3] = {0.0, 0.0, -0.01};
        check("in front: not yet", !t.Step(front, 0.05, 0.02));
        check("lit while near", t.Near());
        check("through its face: the tap", t.Step(face, 0.05, 0.02));
        check("on through: not again", !t.Step(beyond, 0.05, 0.02));
    }
    {
        tiptap::Tap t;
        const double off1[3] = {0.2, 0.0, 0.03}, off2[3] = {0.2, 0.0, 0.0};
        t.Step(off1, 0.05, 0.02);
        check("beside it: no tap", !t.Step(off2, 0.05, 0.02));
    }
    {
        tiptap::Tap t;
        const double behind[3] = {0.0, 0.0, -0.01}, face[3] = {0.0, 0.0, 0.0};
        t.Step(behind, 0.05, 0.02);
        check("already through when shown: no tap", !t.Step(face, 0.05, 0.02));
        const double far[3] = {0.0, 0.0, 0.2};
        t.Step(far, 0.05, 0.02);
        check("from far off in one frame: no tap", !t.Step(face, 0.05, 0.02));
        check("no hand: nothing", !t.Step(nullptr, 0.05, 0.02));
    }
    // Pokes at every speed a hand makes, straight in, at hand tracking's frame rates and any phase:
    // one tap each (council, 2026-11-18: one frame through a 5 mm band missed slow and quick ones).
    {
        int pokes = 0, once = 0;
        for (double hz : {15.0, 30.0})
            for (double v : {0.05, 0.1, 0.15, 0.2, 0.36, 0.6, 1.0, 1.5, 2.0})
                for (int phase = 0; phase < 8; ++phase) {
                    tiptap::Tap t;
                    int taps = 0;
                    for (double z = 0.3 - v / hz * phase / 8; z > -0.2; z -= v / hz) {
                        const double at[3] = {0.01, -0.005, z};
                        taps += t.Step(at, 0.05, 0.02);
                    }
                    ++pokes, once += taps == 1;
                }
        std::printf("pokes from 5 cm/s to 2 m/s at 15 and 30 Hz: %d of %d tapped once\n", once, pokes);
        check("every poke speed taps once", once == pokes);
    }
    {
        tiptap::Tap t;
        const double front[3] = {0.0, 0.0, 0.02}, rest[3] = {0.0, 0.0, 0.005}, face[3] = {0.0, 0.0, 0.002};
        t.Step(front, 0.05, 0.02);
        check("slowing just in front of it: no tap yet", !t.Step(rest, 0.05, 0.02) && !t.Step(rest, 0.05, 0.02));
        check("...then on to its face: the tap", t.Step(face, 0.05, 0.02));
        check("held there: once", !t.Step(face, 0.05, 0.02));
        check("back out a little, not past kFront: no second tap", !t.Step(rest, 0.05, 0.02) && !t.Step(face, 0.05, 0.02));
        t.Step(front, 0.05, 0.02);
        check("out past kFront and in again: a second tap", t.Step(face, 0.05, 0.02));
    }
    {
        tiptap::Tap t;
        const double in[3] = {0.0, 0.0, 0.03}, side[3] = {0.2, 0.0, 0.03}, face[3] = {0.0, 0.0, 0.0};
        t.Step(in, 0.05, 0.02);
        t.Step(side, 0.05, 0.02);
        check("in front, then off to the side: disarmed", !t.Step(face, 0.05, 0.02));
    }
    {
        tiptap::Tap t;
        const double in[3] = {0.0, 0.0, 0.09}, jump[3] = {0.0, 0.0, -0.4};
        t.Step(in, 0.05, 0.02);
        check("a one-frame jump far behind it: no tap (council, 2026-11-18)", !t.Step(jump, 0.05, 0.02));
        const double face[3] = {0.0, 0.0, 0.0};
        check("...nor when it comes back to its face", !t.Step(face, 0.05, 0.02));
    }

    // A point on a curved panel's surface, out of it along its normal by `d` (OnSurface's), in its frame.
    auto curvedAt = [](double r, double u, double v, double d, double q[3]) {
        const double a = u / r;
        q[0] = (r - d) * std::sin(a), q[1] = v, q[2] = r - (r - d) * std::cos(a);
    };
    {
        double q[3], s[3];
        curvedAt(0.6, 0.28, 0.1, 0.03, q);
        posemath::OnPanel(0.6, q, s);
        check("on a curved panel: u along the arc, v, d out of its surface",
              std::fabs(s[0] - 0.28) < 1e-9 && std::fabs(s[1] - 0.1) < 1e-9 && std::fabs(s[2] - 0.03) < 1e-9);
        const double flat[3] = {0.1, -0.2, 0.05};
        posemath::OnPanel(0, flat, s);
        check("flat: as it is", s[0] == 0.1 && s[1] == -0.2 && s[2] == 0.05);
    }

    // The panel out of hand input after a row's pressed: until the tip's back out in front of its surface.
    {
        handsoff::Hold h;
        h.Start(2, 1, 1000);
        check("held from the press", h.Holds(2) && !h.Holds(1) && h.side() == 1);
        const double through[3] = {0.0, -0.2, -0.02}, front[3] = {0.0, -0.2, 0.01}, out[3] = {0.0, -0.2, 0.03};
        h.Step(through, 0.4, 0.25, 1100, true);
        h.Step(front, 0.4, 0.25, 1200, true);
        check("through it, then just in front: still held", h.Holds(2));
        h.Step(nullptr, 0.4, 0.25, 2000, true);
        check("the hand lost in the poke: still held (it may be through it yet)", h.Holds(2));
        h.Step(out, 0.4, 0.25, 2100, true);
        check("out 3 cm in front: let go", !h.Holds(2));
        h.Start(2, 1, 0);
        h.Step(nullptr, 0.4, 0.25, 3000, true);
        check("3 s on: let go, whatever the tip", !h.Holds(2));
        h.Start(2, 1, 0);
        const double aside[3] = {0.47, 0.0, -0.01};
        h.Step(aside, 0.4, 0.25, 100, true);
        check("5 cm off its edges: let go", !h.Holds(2));
        h.Start(2, 1, 0);
        h.Step(through, 0.4, 0.25, 100, false);
        check("its panel gone: let go", !h.Holds(2));
    }
    // A curved panel (r 0.6 m) with its bar on the left: the menu's rows 28 cm off its centre, where the
    // surface is 6.5 cm in front of the flat centre plane (council, 2026-11-18: measured on that plane,
    // the poke counted as out the tick after it pressed the row, and went on into the content).
    for (double u : {-0.28, 0.28}) {
        handsoff::Hold h;
        h.Start(0, 0, 0);
        double q[3], s[3];
        curvedAt(0.6, u, 0.0, -0.02, q);   // 2 cm through, at the row
        check(u < 0 ? "left bar: that tip is in front of the flat plane (the old measure's let go)"
                    : "right bar: that tip is in front of the flat plane (the old measure's let go)", q[2] > handsoff::Hold::kOut);
        posemath::OnPanel(0.6, q, s);
        h.Step(s, 0.4, 0.25, 50, true);
        check(u < 0 ? "left bar: 2 cm through its surface, still held" : "right bar: 2 cm through its surface, still held", h.Holds(0));
        curvedAt(0.6, u, 0.0, 0.03, q);
        posemath::OnPanel(0.6, q, s);
        h.Step(s, 0.4, 0.25, 100, true);
        check(u < 0 ? "left bar: back out 3 cm in front of it, let go" : "right bar: back out 3 cm in front of it, let go", !h.Holds(0));
    }
    // ...and a tap beside the menu there, measured on its surface: one poke 2 cm through closes it.
    {
        tiptap::Tap t;
        int taps = 0;
        for (double d = 0.10; d > -0.02; d -= 0.01) {
            double q[3], s[3];
            curvedAt(0.6, 0.2, 0.05, d, q);
            posemath::OnPanel(0.6, q, s);
            taps += t.Step(s, 0.4, 0.25);
        }
        check("a curved panel's side, tapped 2 cm through: one tap", taps == 1);
    }
    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed != 0;
}
