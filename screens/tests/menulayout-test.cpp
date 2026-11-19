// The grab bar menu's layout (menulayout.h) on each page panelmenu.h gives, and its icons (iconpath.h,
// barmenuicons.h): tiles side by side on top, rows under them, every point on one row at most, the
// texture holding them all; each icon drawn inside its box.
#include "../barmenuicons.h"
#include "../menulayout.h"
#include "../panelmenu.h"

#include <cmath>
#include <cstdio>

namespace {
std::vector<menulayout::Spec> Specs(const std::vector<panelmenu::Row> &rows) {
    std::vector<menulayout::Spec> s;
    for (size_t r = 0; r < rows.size(); ++r)
        s.push_back({panelmenu::Tile(rows[r].item), panelmenu::RuleAbove(rows, r), panelmenu::Caption(rows, r) != nullptr});
    return s;
}
double Ink(const std::vector<float> &c) {
    double sum = 0;
    for (float v : c) sum += v;
    return sum;
}
}  // namespace

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using menulayout::At;
    panelmenu::Kind desktop, floating, game, anchored;
    floating.floating = true;
    game.gsapp = true, game.theater = true;
    anchored.anchored = true;

    const auto d = panelmenu::Main(desktop);
    const auto L = menulayout::Lay(Specs(d));
    check("a box for each row", L.rows.size() == d.size() && L.tiles.size() == d.size() && L.captions.size() == d.size());
    check("curved and rotate are tiles, side by side on top", L.tiles[0] && L.tiles[1] && !L.tiles[2] &&
          L.rows[0].y0 == L.rows[1].y0 && L.rows[0].x1 < L.rows[1].x0);
    check("the tiles fill the strip", L.rows[0].x0 == menulayout::kPad && L.rows[1].x1 == menulayout::kW - menulayout::kPad);
    check("the list starts under the tiles, past their rule", L.rows[2].y0 > L.rows[0].y1 && !L.rules.empty() &&
          L.rules[0] > L.rows[0].y1 && L.rules[0] < L.rows[2].y0);
    check("rows span the card", L.rows[2].x0 == 0 && L.rows[2].x1 == menulayout::kW);
    check("the texture holds every row", L.rows.back().y1 < L.h);
    // OpenVR's mouse y is up from the bottom
    auto up = [&](const menulayout::Layout &l, const menulayout::Box &b) { return l.h - (b.y0 + b.y1) / 2.0; };
    bool mids = true;
    for (size_t i = 0; i < L.rows.size(); ++i) mids &= At(L, (L.rows[i].x0 + L.rows[i].x1) / 2.0, up(L, L.rows[i])) == int(i);
    check("each box's middle is that row", mids);
    check("between the tiles is no row", At(L, (L.rows[0].x1 + L.rows[1].x0) / 2.0, up(L, L.rows[0])) == -1);
    check("the rule's band is no row", At(L, 100, L.h - L.rules[0]) == -1);
    check("off the card is no row", At(L, -1, up(L, L.rows[2])) == -1 && At(L, menulayout::kW + 1, up(L, L.rows[2])) == -1 &&
          At(L, 100, L.h + 1) == -1 && At(L, 100, -1) == -1);
    bool apart = true;
    for (size_t i = 0; i < L.rows.size(); ++i)
        for (size_t j = i + 1; j < L.rows.size(); ++j) {
            const auto &a = L.rows[i], &b = L.rows[j];
            apart &= a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0;
        }
    check("no two boxes overlap", apart);
    check("reset's rule is above it", L.rules.size() == 2 && L.rules[1] < L.rows.back().y0 && L.rules[1] > L.rows[L.rows.size() - 2].y1);

    const auto f = menulayout::Lay(Specs(panelmenu::Main(floating)));
    check("a floating window: four tiles in the strip", f.tiles[3] && !f.tiles[4] && f.rows[3].x1 == menulayout::kW - menulayout::kPad &&
          f.rows[0].y0 == f.rows[3].y0);

    const auto pr = panelmenu::Placement(anchored);
    const auto P = menulayout::Lay(Specs(pr));
    check("placement: no tiles, back on top", !P.tiles[0] && P.rows[0].y0 == menulayout::kTop);
    check("placement: a caption over in the room", P.captions[1] >= P.rows[0].y1 && P.captions[1] + menulayout::kCaptionH == P.rows[1].y0);
    check("placement: the caption's band is no row", At(P, 100, P.h - (P.captions[1] + 10)) == -1);
    check("placement: a rule above stay shown", P.rules.size() == 1 && P.rules[0] > P.rows[3].y1 && P.rules[0] < P.rows[4].y0);

    const auto E = menulayout::Lay(Specs(panelmenu::Edges(game, false)));
    check("edges from a right click: caption first", E.captions[0] == menulayout::kTop && E.rows[0].y0 == menulayout::kTop + menulayout::kCaptionH);
    check("edges: the texture holds them", E.rows.back().y1 + menulayout::kEnd == E.h);
    check("an empty page is no row", menulayout::Lay({}).rows.empty() && At(menulayout::Lay({}), 1, 1) == -1);

    // Icons: each one inks its box, inside it, and the rasterizer's fills are right.
    using barmenuicons::Icon;
    for (int i = int(Icon::Curved); i <= int(Icon::Check); ++i) {
        const auto c = iconpath::Coverage(barmenuicons::Shapes(Icon(i)), 48);
        const double ink = Ink(c);
        char what[64];
        std::snprintf(what, sizeof what, "icon %d draws (%.0f px)", i, ink);
        check(what, ink > 48 && ink < 48 * 48 * 0.9);
    }
    {
        const auto c = iconpath::Coverage({{{iconpath::Rect(0, 0, 12, 24)}}}, 24);
        check("a half box covers half", std::fabs(Ink(c) - 288) < 1 && c[0] > 0.99f && c[23] < 0.01f);
        const auto hole = iconpath::Coverage({{iconpath::Parse("M0 0h24v24H0zm6 6v12h12V6z"), true}}, 24);
        check("even-odd leaves the hole", std::fabs(Ink(hole) - (576 - 144)) < 1 && hole[12 * 24 + 12] < 0.01f);
        const auto disc = iconpath::Coverage({iconpath::Shape{iconpath::Parse("M12 2a10 10 0 1 1 0 20a10 10 0 1 1 0-20z")}}, 24);
        check("two arcs make a disc", std::fabs(Ink(disc) - M_PI * 100) < 4);
        const auto quarter = iconpath::Coverage({iconpath::Shape{iconpath::Parse("M0 0H12V12H0Z")}, iconpath::Shape{iconpath::Parse("m0 0 12 0 0 12-12 0z")}}, 24);
        check("absolute and relative agree (the shapes over each other)", std::fabs(Ink(quarter) - 144) < 1);
        const auto half = iconpath::Coverage({{{iconpath::Rect(0, 0, 24, 24)}, false, 0.5}}, 24);
        check("opacity halves it", std::fabs(Ink(half) - 288) < 1);
        check("bad path data stops, no crash", iconpath::Parse("M1 1 L x").empty() && iconpath::Parse("").empty());
    }
    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed != 0;
}
