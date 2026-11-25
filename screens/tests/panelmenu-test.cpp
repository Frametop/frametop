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

    // Every button the controls had is a tile or a row, on a page, for the kinds that had it.
    panelmenu::Kind desktop, floating, game, steam;
    floating.floating = true;
    game.gsapp = true, game.theater = true;
    steam.theater = true;
    const auto d = panelmenu::Main(desktop), f = panelmenu::Main(floating), g = panelmenu::Main(game), st = panelmenu::Main(steam);
    const auto dp = panelmenu::Placement(desktop), fp = panelmenu::Placement(floating), gp = panelmenu::Placement(game);
    check("a desktop screen: curve, rotate, placement, move all, bar position, reset",
          has(d, Item::Curve) && has(d, Item::Rotate) && has(d, Item::Placement) && has(d, Item::MoveAll) &&
              has(d, Item::BarPosition) && has(d, Item::Reset));
    check("a desktop screen's placement: back, in the room, in view, dashboard",
          dp.size() == 4 && dp[0].item == Item::Back && dp[1].item == Item::InRoom && dp[2].item == Item::KeepInView &&
              dp[3].item == Item::OnDashboard);
    check("a desktop screen: no window or theater rows", !has(d, Item::Dock) && !has(d, Item::Close) && !has(d, Item::Theater));
    check("a floating window: to desktop and close tiles, move all, no reset",
          has(f, Item::Dock) && has(f, Item::Close) && !has(fp, Item::KeepInView) && has(f, Item::MoveAll) && !has(f, Item::Reset));
    check("a Steam game: View in Theater and move all, no reset or window rows",
          has(g, Item::Theater) && has(g, Item::MoveAll) && !has(g, Item::Dock) && !has(g, Item::Reset) && !has(gp, Item::KeepInView));
    check("Steam's tile: View in Theater and the layout's rows", has(st, Item::Theater) && has(st, Item::MoveAll) && has(st, Item::Reset));
    panelmenu::Kind onHead, tile = floating;
    onHead.rides = true, tile.anchored = true;
    check("pinned to the head or a wrist: no move all, the rest as ever",
          !has(panelmenu::Main(onHead), Item::MoveAll) && panelmenu::Main(onHead).size() + 1 == d.size());
    check("a window tiled on the dashboard: move all", has(panelmenu::Main(tile), Item::MoveAll));
    check("not anchored: no Stay Shown", !has(dp, Item::StayShown));
    panelmenu::Kind anchored;
    anchored.anchored = true;
    check("anchored: Stay Shown, right after Tiled, a rule above it", [&] {
        const auto a = panelmenu::Placement(anchored);
        const auto on = std::find(a.begin(), a.end(), panelmenu::Row{Item::OnDashboard});
        return on != a.end() && on + 1 != a.end() && (on + 1)->item == Item::StayShown && panelmenu::RuleAbove(a, size_t(on + 1 - a.begin()));
    }());
    for (const auto &rows : {d, f, g, st}) {
        bool listed = false, tilesFirst = true;
        for (const auto &r : rows) tilesFirst &= !(listed && panelmenu::Tile(r.item)), listed |= !panelmenu::Tile(r.item);
        check("the quick tiles come first", tilesFirst && panelmenu::Tile(rows[0].item));
    }
    check("a floating window's four tiles: curved, rotate, to desktop, close",
          f.size() > 4 && panelmenu::Tile(f[3].item) && f[3].item == Item::Close && !panelmenu::Tile(f[4].item));
    check("reset is the last row, after a rule", d.back().item == Item::Reset && panelmenu::RuleAbove(d, d.size() - 1));
    check("no rule above the first row", !panelmenu::RuleAbove(d, 0) && !panelmenu::RuleAbove(dp, 0));
    check("no rule inside the main list but Reset's", [&] {
        for (size_t i = 0; i + 1 < d.size(); ++i)
            if (panelmenu::RuleAbove(d, i)) return false;
        return true;
    }());
    check("placement's caption is over its first choice", panelmenu::Caption(dp, 1) && !panelmenu::Caption(dp, 0) && !panelmenu::Caption(dp, 2));
    check("the main page has no caption", [&] {
        for (size_t i = 0; i < d.size(); ++i)
            if (panelmenu::Caption(d, i)) return false;
        return true;
    }());
    check("Rows picks the page", panelmenu::Rows(panelmenu::Page::Placement, desktop, true) == dp &&
          panelmenu::Rows(panelmenu::Page::Main, desktop, false) == d);

    // The edges page: four edges, then outside or inside the edge; Back from the main page; from a right
    // click as before (Theater under them).
    const auto fromMain = panelmenu::Edges(game, true), fromRight = panelmenu::Edges(game, false);
    check("edges from the main page: Back, the four, then outside and inside", fromMain.size() == 7 && fromMain[0].item == Item::Back &&
          fromMain[1] == panelmenu::Row{Item::Edge, barside::Side::Bottom} && fromMain[4] == panelmenu::Row{Item::Edge, barside::Side::Right} &&
          fromMain[5].item == Item::Outside && fromMain[6].item == Item::Inside);
    check("edges from a right click: the four, outside and inside, then View in Theater", fromRight.size() == 7 &&
          fromRight[0].item == Item::Edge && fromRight[4].item == Item::Outside && fromRight[5].item == Item::Inside &&
          fromRight[6].item == Item::Theater && panelmenu::RuleAbove(fromRight, 6));
    check("the edges' caption is over the first edge only", panelmenu::Caption(fromMain, 1) && !panelmenu::Caption(fromMain, 2) &&
          panelmenu::Caption(fromRight, 0) && !panelmenu::Caption(fromRight, 6));
    check("outside and inside have a caption of their own, over the first", panelmenu::Caption(fromMain, 5) && !panelmenu::Caption(fromMain, 6) &&
          std::string(panelmenu::Caption(fromMain, 5)) != panelmenu::Caption(fromMain, 1));
    check("a desktop screen's right click: the edges, outside and inside", panelmenu::Edges(desktop, false).size() == 6);
    check("outside and inside are choices that open nothing", panelmenu::Choice(Item::Inside) && panelmenu::Choice(Item::Outside) &&
          !panelmenu::Opens(Item::Inside) && !panelmenu::Held(Item::Outside) && !panelmenu::Toggle(Item::Inside));
    check("edge rows differ by their side", !(panelmenu::Row{Item::Edge, barside::Side::Top} == panelmenu::Row{Item::Edge, barside::Side::Left}));
    for (const auto &rows : {d, f, g, st, dp, fp, gp, fromMain, fromRight})
        for (const auto &r : rows) check("every row has a label", !panelmenu::Label(r).empty());

    check("rotate and move all are held, the rest clicked", panelmenu::Held(Item::Rotate) && panelmenu::Held(Item::MoveAll) &&
          !panelmenu::Held(Item::Curve) && !panelmenu::Held(Item::Reset) && !panelmenu::Held(Item::BarPosition));
    check("the toggles", panelmenu::Toggle(Item::Curve) && panelmenu::Toggle(Item::StayShown) && !panelmenu::Toggle(Item::MoveAll) &&
          !panelmenu::Toggle(Item::Reset));
    check("the choices", panelmenu::Choice(Item::InRoom) && panelmenu::Choice(Item::KeepInView) && panelmenu::Choice(Item::OnDashboard) &&
          panelmenu::Choice(Item::Edge) && !panelmenu::Choice(Item::Curve) && !panelmenu::Choice(Item::StayShown));
    // A PC app window whose PC takes a scale: the row under Placement, and its page.
    auto app = game;
    app.scales = true;
    const auto a = panelmenu::Main(app);
    size_t scaleAt = a.size();
    for (size_t k = 0; k < a.size(); ++k) if (a[k].item == Item::Scale) scaleAt = k;
    check("Scale right after Placement", scaleAt < a.size() && a[scaleAt - 1].item == Item::Placement && a.size() == g.size() + 1);
    const auto sp = panelmenu::Rows(panelmenu::Page::Scale, app, true);
    check("its page: Back, then the steps, 100% first", sp.size() == 9 && sp[0].item == Item::Back &&
          panelmenu::Label(sp[1]) == "100%" && panelmenu::Label(sp[8]) == "250%");
    check("its caption is over the first step only", panelmenu::Caption(sp, 1) && !panelmenu::Caption(sp, 2));
    check("steps differ by their percent", !(sp[1] == sp[2]) && panelmenu::Choice(sp[1].item) && panelmenu::Opens(Item::Scale));
    check("a PC that stopped taking scales: the main page", panelmenu::Rows(panelmenu::Page::Scale, game, true) == panelmenu::Main(game));
    check("placement and bar position open pages", panelmenu::Opens(Item::Placement) && panelmenu::Opens(Item::BarPosition) &&
          !panelmenu::Opens(Item::Reset));

    // The tap: let go before the hold, the device still. The hold: the move starts.
    check("a quick still press is a tap", panelmenu::IsTap(120, 0.004, 0.01));
    check("a soft 400 ms pull is still a tap", panelmenu::IsTap(400, 0.002, 0.0));
    check("held half a second: a move, not a tap", !panelmenu::IsTap(500, 0.0, 0.0) && panelmenu::CarryNow(500, 0.0, 0.0));
    check("held still under the hold: no move yet", !panelmenu::CarryNow(499, 0.01, 0.03));
    check("moved: the move starts at once", panelmenu::CarryNow(100, 0.02, 0.0) && !panelmenu::IsTap(100, 0.02, 0.0));
    {
        using panelmenu::HoldStep;
        using panelmenu::Wait;
        check("a waiting press waits", HoldStep(200, 0.0, 0.0, false, true, true, -1) == Wait::Keep);
        check("held: it carries (controller unknown or held)", HoldStep(500, 0.0, 0.0, false, true, true, -1) == Wait::Carry &&
                                                                  HoldStep(500, 0.0, 0.0, false, true, true, 1) == Wait::Carry);
        check("held with nothing held: its release was lost, no carry", HoldStep(500, 0.0, 0.0, false, true, true, 0) == Wait::Drop);
        check("nothing held while it still waits: it waits (the release may come)", HoldStep(200, 0.0, 0.0, false, true, true, 0) == Wait::Keep);
        check("swung: it carries at once", HoldStep(50, 0.0, 0.1, false, true, true, -1) == Wait::Carry);
        check("dropped when busy, unposed or hidden", HoldStep(600, 0.0, 0.0, true, true, true, 1) == Wait::Drop &&
                                                         HoldStep(600, 0.0, 0.0, false, false, true, 1) == Wait::Drop &&
                                                         HoldStep(100, 0.0, 0.0, false, true, false, 1) == Wait::Drop);
    }
    check("turned: the move starts at once", panelmenu::CarryNow(100, 0.0, 0.07) && !panelmenu::IsTap(100, 0.0, 0.07));
    check("a 3 degree wobble is still a tap", panelmenu::IsTap(200, 0.0, 0.0524));
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
        h.Start(2, 1, 0, 1000);
        check("held from the press", h.Holds(2) && !h.Holds(1) && h.side() == 1);
        const double through[3] = {0.0, -0.2, -0.02}, front[3] = {0.0, -0.2, 0.01}, out[3] = {0.0, -0.2, 0.03};
        h.Step(through, 0, 0.4, 0.25, 1100, true);
        h.Step(front, 0, 0.4, 0.25, 1200, true);
        check("through it, then just in front: still held", h.Holds(2));
        h.Step(nullptr, 0, 0.4, 0.25, 2000, true);
        check("the hand lost in the poke: still held (it may be through it yet)", h.Holds(2));
        h.Step(out, 0, 0.4, 0.25, 2100, true);
        check("out 3 cm in front: let go", !h.Holds(2));
        h.Start(2, 1, 0, 0);
        h.Step(nullptr, 0, 0.4, 0.25, 3000, true);
        check("3 s on: let go, whatever the tip", !h.Holds(2));
        h.Start(2, 1, 0, 0);
        const double aside[3] = {0.47, 0.0, -0.01};
        h.Step(aside, 0, 0.4, 0.25, 100, true);
        check("5 cm off its edges: let go", !h.Holds(2));
        h.Start(2, 1, 0, 0);
        h.Step(through, 0, 0.4, 0.25, 100, false);
        check("its panel gone: let go", !h.Holds(2));
        // a menu lifted 21.7 mm off a tight curve's surface (posemath::CardLift): a press at its face is
        // 23.7 mm in front of the panel, still held; kOut past the lift, out
        h.Start(2, 1, 0, 0, 0.0217);
        const double atMenu[3] = {0.0, -0.2, 0.0237}, pastIt[3] = {0.0, -0.2, 0.0217 + handsoff::Hold::kOut + 0.002};
        h.Step(atMenu, 0, 0.4, 0.25, 50, true);
        check("a lifted menu's press, 23.7 mm out: still held", h.Holds(2));
        h.Step(pastIt, 0, 0.4, 0.25, 100, true);
        check("...kOut past its lift: let go", !h.Holds(2));
    }
    // A curved panel (r 0.6 m) with its bar on the left: the menu's rows 28 cm off its centre, where the
    // surface is 6.5 cm in front of the flat centre plane (council, 2026-11-18: measured on that plane,
    // the poke counted as out the tick after it pressed the row, and went on into the content).
    for (double u : {-0.28, 0.28}) {
        handsoff::Hold h;
        h.Start(0, 0, 0.6, 0);
        double q[3];
        curvedAt(0.6, u, 0.0, -0.02, q);   // 2 cm through, at the row
        check(u < 0 ? "left bar: that tip is in front of the flat plane (the old measure's let go)"
                    : "right bar: that tip is in front of the flat plane (the old measure's let go)", q[2] > handsoff::Hold::kOut);
        h.Step(q, 0.6, 0.4, 0.25, 50, true);
        check(u < 0 ? "left bar: 2 cm through its surface, still held" : "right bar: 2 cm through its surface, still held", h.Holds(0));
        curvedAt(0.6, u, 0.0, 0.03, q);
        h.Step(q, 0.6, 0.4, 0.25, 100, true);
        check(u < 0 ? "left bar: back out 3 cm in front of it, let go" : "right bar: back out 3 cm in front of it, let go", !h.Holds(0));
    }
    // The row pressed was Curved, on a curved panel with a side bar: flattened, its side drops 6.5 cm back
    // behind the tip, which is still through where the surface was (council, 2026-11-18: that let go,
    // and a poke going on 7 cm pressed the flat content).
    {
        handsoff::Hold h;
        h.Start(0, 0, 0.6, 0);
        double q[3];
        curvedAt(0.6, -0.28, 0.0, -0.03, q);
        h.Step(q, 0, 0.4, 0.25, 50, true);
        check("Curved pressed (now flat), the tip still through where its surface was: held", h.Holds(0));
        curvedAt(0.6, -0.28, 0.0, 0.03, q);
        h.Step(q, 0, 0.4, 0.25, 100, true);
        check("...out in front of both: let go", !h.Holds(0));
        h.Start(0, 0, 0, 0);
        curvedAt(0.6, -0.28, 0.0, -0.01, q);
        h.Step(q, 0.6, 0.4, 0.25, 50, true);
        check("flat, then curved: the tip through the new surface, held", h.Holds(0));
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
