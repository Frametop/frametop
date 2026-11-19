// The grab bar menu's layout (menulayout.h) on each page panelmenu.h gives, and its icons (iconpath.h,
// barmenuicons.h): tiles side by side on top, rows under them, every point on one row at most, the
// texture holding them all; each icon drawn inside its box; the card's fills and corners (fillround.h).
#include "../barmenuicons.h"
#include "../fillround.h"
#include "../menulayout.h"
#include "../panelmenu.h"

#include <cmath>
#include <cstdio>
#include <limits>

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
        const auto arc0 = iconpath::Parse("M0 0h2v2H0z");
        const auto arcSame = iconpath::Parse("M0 0h2a5 5 0 0 1 0 0v2H0z");
        check("an arc to where it is draws nothing (SVG)", arcSame.size() == 1 && arcSame[0].size() == arc0[0].size());
        const auto two = iconpath::Parse("M0 0h4v4z h4v4z");
        check("after z with no M, the next outline starts where it did", two.size() == 2 && two[1][0].x == 0 && two[1][0].y == 0);
        check("a number after z ends the data (no loop)", iconpath::Parse("M0 0h4v4z 5").size() == 1 &&
                                                              iconpath::Parse("M0 0h4v4Z-1").size() == 1 && iconpath::Parse("M0 0h4v4z.5").size() == 1);
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        const auto odd = iconpath::Coverage({{{{{0, 0}, {nan, 12}, {24, 24}}, {{inf, 0}, {-inf, 24}, {0, 12}}}}}, 24);
        bool finite = true;
        for (float v : odd) finite &= std::isfinite(v) && v >= 0 && v <= 1;
        check("NaN and inf points: no crash, coverage stays 0..1", finite);
    }

    // The card's fills: an opaque colour is stored, a see-through one blended, a rounded box's inside is
    // its colour and its corners soft; Round clears only outside the card's corners.
    {
        const uint8_t red[4] = {222, 54, 24, 255}, half[4] = {200, 100, 0, 128};
        std::vector<uint8_t> px(16 * 16 * 4, 10);
        ftvr::FillRect(px, 16, 16, 2, 2, 6, 6, red);
        check("an opaque rect is its colour", px[(3 * 16 + 3) * 4] == 222 && px[(3 * 16 + 3) * 4 + 3] == 255 && px[(1 * 16 + 1) * 4] == 10);
        ftvr::FillRect(px, 16, 16, 8, 8, 9, 9, half);
        check("a see-through rect blends", px[(8 * 16 + 8) * 4] == 10 + std::lround((200 - 10) * 128 / 255.0));
        std::vector<uint8_t> r(40 * 40 * 4, 0);
        ftvr::FillRound(r, 40, 40, 0, 0, 40, 40, 10, red);
        check("a rounded box: its inside its colour", r[(20 * 40 + 20) * 4 + 1] == 54 && r[(20 * 40 + 20) * 4 + 3] == 255);
        check("...its corner clear, its corner's edge soft", r[3] == 0 && r[(2 * 40 + 4) * 4 + 3] > 0 && r[(2 * 40 + 4) * 4 + 3] < 255);
        // Round against every pixel worked out (what it did before it kept to the corners)
        const int W = 60, H = 44, rr = 20;
        std::vector<uint8_t> a(size_t(W) * H * 4, 200), b = a;
        ftvr::Round(a, W, H, rr);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double cx = std::clamp(x + 0.5, double(rr), double(W - rr)), cy = std::clamp(y + 0.5, double(rr), double(H - rr));
                const double k = std::clamp(rr - std::hypot(x + 0.5 - cx, y + 0.5 - cy) + 0.5, 0.0, 1.0);
                if (k < 1) b[(size_t(y) * W + x) * 4 + 3] = uint8_t(std::lround(b[(size_t(y) * W + x) * 4 + 3] * k));
            }
        check("Round on the corners only: the same pixels as all of them", a == b);
        std::vector<uint8_t> tiny(3 * 3 * 4, 255);
        ftvr::Round(tiny, 3, 3, 20);
        check("Round on a card smaller than its corners: no overrun", tiny.size() == 36);
    }
    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed != 0;
}
