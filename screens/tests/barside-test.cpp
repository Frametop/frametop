// The grab bar's edge (barside.h): where the row goes on each edge, its turn, where its menu sits.
#include "../barside.h"

#include <cstdio>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    using barside::Side;

    for (Side s : barside::kAll) {
        Side back = Side::Bottom;
        check("names parse back", barside::Parse(barside::Name(s), &back) && back == s);
    }
    Side kept = Side::Left;
    check("an unknown name is refused and leaves it", !barside::Parse("middle", &kept) && kept == Side::Left);
    check("only the sides stand the bar on end", !barside::Vertical(Side::Bottom) && !barside::Vertical(Side::Top) &&
          barside::Vertical(Side::Left) && barside::Vertical(Side::Right));

    const double w = 1.2, h = 0.675, in = 0.05;
    auto at = [&](Side s, double along) { return barside::Place(s, w, h, in, along); };
    check("bottom: the row inside the bottom edge", near(at(Side::Bottom, 0.1).u, 0.1) && near(at(Side::Bottom, 0.1).v, -(h / 2 - in)));
    check("top: inside the top edge, the same order", near(at(Side::Top, 0.1).u, 0.1) && near(at(Side::Top, 0.1).v, h / 2 - in));
    check("left: down inside its left edge", near(at(Side::Left, 0).u, -(w / 2 - in)) && near(at(Side::Left, 0).v, 0));
    check("right: down inside its right edge", near(at(Side::Right, 0).u, w / 2 - in) && near(at(Side::Right, 0).v, 0));
    for (Side s : barside::kAll) {
        const auto c = at(s, 0);
        check("every edge's row is on the panel", std::fabs(c.u) < w / 2 && std::fabs(c.v) < h / 2);
    }
    check("a side: what sat left of the bar goes above it", at(Side::Left, -0.1).v > 0 && at(Side::Right, -0.1).v > 0);
    check("a side: curve and roll go below it", at(Side::Left, 0.1).v < 0 && at(Side::Right, 0.1).v < 0);

    // The bar's own +x after its turn, (cos r, sin r) on the surface, runs the way `along` does.
    for (Side s : barside::kAll) {
        const double r = barside::Roll(s);
        const auto a = at(s, 0), b = at(s, 1);
        check("the bar's mouse x runs along the row", near(std::cos(r), b.u - a.u) && near(std::sin(r), b.v - a.v));
        const auto o = barside::Outward(s);
        check("outward points from the middle to that edge", o.u * a.u + o.v * a.v > 0);
    }

    const auto row = barside::Row(Side::Right, w, h, in, -0.3, 0.2, 0.02);
    check("a side's row box stands on end", near(row.u0, w / 2 - in - 0.02) && near(row.u1, w / 2 - in + 0.02) &&
          near(row.v0, -0.22) && near(row.v1, 0.32));
    const auto both = barside::Union(row, {-0.6, 0.6, -0.3, 0.3});
    const auto tall = barside::Union(barside::Row(Side::Bottom, w, h, in, -0.1, 0.1, 0.02), {-0.6, 0.6, -0.3, 0.3});
    check("the union holds the panel and the row", both.Contains(0, 0) && both.Contains(w / 2 - in, 0.31) && !both.Contains(0.7, 0.4) &&
          tall.Contains(0, -0.3) && !tall.Contains(0, -0.34));

    const barside::UV click = at(Side::Bottom, 0.05);
    const auto m = barside::MenuAt(Side::Bottom, click, 0.02, 0.01, 0.12, 0.1);
    check("bottom: the menu sits above the bar at the click", near(m.u, 0.05) && near(m.v, click.v + 0.02 + 0.01 + 0.05));
    const auto t = barside::MenuAt(Side::Top, at(Side::Top, 0), 0.02, 0.01, 0.12, 0.1);
    check("top: under the bar", near(t.v, h / 2 - in - 0.02 - 0.01 - 0.05));
    const auto r = barside::MenuAt(Side::Right, at(Side::Right, 0.1), 0.02, 0.01, 0.12, 0.1);
    check("right: left of the bar, at the click's height", near(r.u, w / 2 - in - 0.02 - 0.01 - 0.06) && near(r.v, -0.1));
    const auto l = barside::MenuAt(Side::Left, at(Side::Left, 0), 0.02, 0.01, 0.12, 0.1);
    check("left: right of the bar", near(l.u, -(w / 2 - in) + 0.02 + 0.01 + 0.06));
    for (Side s : barside::kAll) {
        const auto c = barside::MenuAt(s, at(s, 0), 0.02, 0.01, 0.12, 0.1);
        check("every edge's menu is on its own panel", std::fabs(c.u) + 0.06 < w / 2 && std::fabs(c.v) + 0.05 < h / 2);
    }

    // Inside the edge or past it (barside::Inset, In).
    using barside::Inset;
    for (Inset i : {Inset::Default, Inset::Inside, Inset::Outside}) {
        Inset back = Inset::Inside;
        check("inset names parse back", barside::Parse(barside::Name(i), &back) && back == i);
    }
    Inset keptInset = Inset::Outside;
    check("an edge's name is no inset", !barside::Parse("left", &keptInset) && keptInset == Inset::Outside);
    check("a panel's own inset wins over the setting", barside::Inside(Inset::Inside, false) && !barside::Inside(Inset::Outside, true));
    check("none of its own: the setting's", barside::Inside(Inset::Default, true) && !barside::Inside(Inset::Default, false));

    check("memory text: the edge alone with no inset of its own", barside::Text(Side::Left, Inset::Default) == "left");
    check("memory text: the edge and the inset", barside::Text(Side::Top, Inset::Outside) == "top outside" &&
          barside::Text(Side::Bottom, Inset::Inside) == "bottom inside");
    for (Side s : barside::kAll)
        for (Inset i : {Inset::Default, Inset::Inside, Inset::Outside}) {
            Side bs = Side::Bottom;
            Inset bi = Inset::Default;
            check("memory text parses back", barside::Parse(barside::Text(s, i), &bs, &bi) && bs == s && bi == i);
        }
    Side ms = Side::Right;
    Inset mi = Inset::Inside;
    for (const char *bad : {"", "inside", "middle outside", "top sideways", "top default", "top  outside", "top outside "})
        check("a bad memory line is refused and leaves both", !barside::Parse(bad, &ms, &mi) && ms == Side::Right && mi == Inset::Inside);

    // (half: the bar's half depth, its hit padding with it)
    const double clear = 0.05, half = 0.014;
    check("inside: the row's line clear of the edge, in", near(barside::In(true, clear), clear));
    check("outside: as far past the edge, a tile's too", near(barside::In(false, clear), -clear));
    {
        const auto row = barside::Row(Side::Bottom, w, h, barside::In(false, clear), 0, 0, half);
        check("outside: the bar, its padding with it, hangs clear of its panel", row.v1 < -h / 2);
    }
    const auto out = barside::Place(Side::Bottom, w, h, barside::In(false, clear), 0.1);
    check("a row past the bottom edge is under the panel", near(out.u, 0.1) && near(out.v, -(h / 2 + clear)));
    const auto outRow = barside::Row(Side::Bottom, w, h, barside::In(false, clear), -0.1, 0.1, 0.03);
    check("...and none of it over the content", outRow.v1 < -h / 2 && !outRow.Contains(0, -h / 2 + 0.001));
    for (Side s : barside::kAll) {   // (a flipped sign on one side would put its row inside)
        const auto c = barside::Place(s, w, h, barside::In(false, clear), 0);
        const auto o = barside::Outward(s);
        check("a row past each edge is beyond that edge", near(c.u * o.u + c.v * o.v, (barside::Vertical(s) ? w : h) / 2 + clear));
        const auto in = barside::Place(s, w, h, barside::In(true, clear), 0);
        check("...and a row inside is short of it", near(in.u * o.u + in.v * o.v, (barside::Vertical(s) ? w : h) / 2 - clear));
    }
    for (Side s : barside::kAll) {
        const auto c = barside::MenuAt(s, barside::Place(s, w, h, -clear, 0), 0.02, 0.01, 0.12, 0.1);
        check("a bar past the edge: its menu still on its own panel", std::fabs(c.u) < w / 2 && std::fabs(c.v) < h / 2);
    }

    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed != 0;
}
