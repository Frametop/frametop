// The grab bar's edge (barside.h): where the row goes on each edge, its turn, the menu's rows.
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

    const double w = 1.2, h = 0.675, out = 0.05;
    auto at = [&](Side s, double along) { return barside::Place(s, w, h, out, along); };
    check("bottom: the row under the panel, as before", near(at(Side::Bottom, 0.1).u, 0.1) && near(at(Side::Bottom, 0.1).v, -(h / 2 + out)));
    check("top: over it, the same order", near(at(Side::Top, 0.1).u, 0.1) && near(at(Side::Top, 0.1).v, h / 2 + out));
    check("left: down its left side", near(at(Side::Left, 0).u, -(w / 2 + out)) && near(at(Side::Left, 0).v, 0));
    check("right: down its right side", near(at(Side::Right, 0).u, w / 2 + out) && near(at(Side::Right, 0).v, 0));
    check("a side: what sat left of the bar goes above it", at(Side::Left, -0.1).v > 0 && at(Side::Right, -0.1).v > 0);
    check("a side: curve and roll go below it", at(Side::Left, 0.1).v < 0 && at(Side::Right, 0.1).v < 0);

    // The bar's own +x after its turn, (cos r, sin r) on the surface, runs the way `along` does.
    for (Side s : barside::kAll) {
        const double r = barside::Roll(s);
        const auto a = at(s, 0), b = at(s, 1);
        check("the bar's mouse x runs along the row", near(std::cos(r), b.u - a.u) && near(std::sin(r), b.v - a.v));
        const auto o = barside::Outward(s);
        check("outward points away from the panel", o.u * a.u + o.v * a.v > 0);
    }

    const auto row = barside::Row(Side::Right, w, h, out, -0.3, 0.2, 0.02);
    check("a side's row box stands on end", near(row.u0, w / 2 + out - 0.02) && near(row.u1, w / 2 + out + 0.02) &&
          near(row.v0, -0.22) && near(row.v1, 0.32));
    const auto both = barside::Union(row, {-0.6, 0.6, -0.3, 0.3});
    check("the union holds the panel and the row", both.Contains(0, 0) && both.Contains(w / 2 + out, 0.31) && !both.Contains(0.7, 0.4));

    check("the first row is at the top", barside::RowAt(10, barside::kMenuH - barside::kMenuPad - 1) == 0);
    check("the last row is at the bottom", barside::RowAt(10, barside::kMenuPad + 1) == barside::kSides - 1);
    check("the padding is no row", barside::RowAt(10, barside::kMenuH - 2) == -1 && barside::RowAt(10, 2) == -1);
    check("off its sides is no row", barside::RowAt(-1, 100) == -1 && barside::RowAt(barside::kMenuW + 1, 100) == -1);
    for (int r = 0; r < barside::kSides; ++r) {
        const double yUp = barside::kMenuH - barside::kMenuPad - (r + 0.5) * barside::kRowH;
        check("each row's middle is that row", barside::RowAt(barside::kMenuW / 2, yUp) == r);
    }

    const barside::UV click = at(Side::Bottom, 0.05);
    const auto m = barside::MenuAt(Side::Bottom, click, 0.02, 0.01, 0.12, 0.1);
    check("bottom: the menu hangs under the bar at the click", near(m.u, 0.05) && near(m.v, click.v - 0.02 - 0.01 - 0.05));
    const auto r = barside::MenuAt(Side::Right, at(Side::Right, 0), 0.02, 0.01, 0.12, 0.1);
    check("right: the menu sits right of the bar, clear of it", near(r.u, w / 2 + out + 0.02 + 0.01 + 0.06) && near(r.v, 0));

    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed != 0;
}
