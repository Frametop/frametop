// The dashboard tiles (screens/tiling.h, screens/tilegrid.h): a command wall. Every panel faces the
// head at its own size, the columns stand on the desk line with each panel resting on the one under
// it, neighbours never touch at their corners however high a column goes, a strip past a full turn
// (or a column past kMaxTopDeg) shrinks to fit, drops land where they're let go, and the keys move
// and size panels as Omarchy's do.
#include "../tilefit.h"
#include "../tiling.h"
#include "../tilegrid.h"

#include <cmath>
#include <cstdio>
#include <set>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// A placed tile's four corners in the group frame.
static void corners(const tiling::Placed &t, double out[4][3]) {
    double m[3][4];
    tiling::Pose(t.azDeg, t.elDeg, t.distM, m);   // (its middle there: its corners on the sphere)
    const double su[4] = {-1, 1, -1, 1}, sv[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; ++k)
        for (int r = 0; r < 3; ++r)
            out[k][r] = m[r][3] + m[r][0] * su[k] * t.widthM / 2 + m[r][1] * sv[k] * t.heightM / 2;
}

// No corner of one tile inside another's azimuth/elevation span seen from the centre: neighbours
// don't overlap anywhere on the sphere (a corner's own azimuth inside its column's span).
static bool cornersInColumns(const tiling::Layout &l) {
    for (const auto &t : l.tiles) {
        double c[4][3];
        corners(t, c);
        for (auto &pt : c) {
            double az, el;
            tiling::Direction(pt, &az, &el);
            az = tiling::WrapNear(az, t.azDeg);
            if (az < t.az0 - 1e-6 || az > t.az1 + 1e-6) return false;
        }
    }
    return true;
}

// Panels in different columns that share some height (a gap up or down counts) are at least a
// gap apart round you, left to right in column order.
static bool apart(const tiling::Layout &l, double gap) {
    for (const auto &t : l.tiles)
        for (const auto &u : l.tiles)
            if (t.col < u.col && t.el0 < u.el1 + gap - 1e-9 && u.el0 < t.el1 + gap - 1e-9 && t.az1 + gap > u.az0 + 1e-9) return false;
    return true;
}
// ...and no nearer than that: each column after the first has a panel exactly a gap from one
// left of it (or reaches the strip's left end).
static bool tight(const tiling::Layout &l, double gap) {
    std::set<int> cols;
    for (const auto &t : l.tiles) cols.insert(t.col);
    for (int c : cols) {
        bool touches = false;
        for (const auto &u : l.tiles) {
            if (u.col != c) continue;
            if (near(u.az0, l.az0, 1e-9)) touches = true;
            for (const auto &t : l.tiles)
                if (t.col < c && t.el0 < u.el1 + gap - 1e-9 && u.el0 < t.el1 + gap - 1e-9 && near(t.az1 + gap, u.az0, 1e-9)) touches = true;
        }
        if (!touches) return false;
    }
    return true;
}

static tiling::Column col(std::initializer_list<tiling::Tile> tiles) {
    tiling::Column c;
    c.tiles = tiles;
    return c;
}
// A panel `px` x `py` pixels at its natural size on the wall `p`.
static tiling::Tile px(int id, int w, int h, const tiling::Params &p = {}) {
    const double m = tiling::MetresPerPx(p);
    return {id, w * m, h * m};
}
static tiling::Tile hd(int id) { return px(id, 1920, 1080); }

int main() {
    tiling::Params p;
    // --- facing and pose
    {
        double m[3][4];
        tiling::Pose(30, 20, 2, m);
        const double len = std::sqrt(m[0][3] * m[0][3] + m[1][3] * m[1][3] + m[2][3] * m[2][3]);
        check("pose: on the sphere", near(len, 2));
        // +Z (the panel's front) points at the centre
        check("pose: faces the head", near(m[0][2], -m[0][3] / 2) && near(m[1][2], -m[1][3] / 2) && near(m[2][2], -m[2][3] / 2));
        check("pose: no roll (its x axis level)", near(m[1][0], 0));
        double az, el;
        const double pt[3] = {m[0][3], m[1][3], m[2][3]};
        tiling::Direction(pt, &az, &el);
        check("direction: back to azimuth and elevation", near(az, 30) && near(el, 20));
        tiling::Pose(0, 0, 1.5, m);
        check("pose: 0,0 straight ahead (-Z)", near(m[2][3], -1.5) && near(m[0][3], 0) && near(m[1][3], 0));
        tiling::Pose(90, 0, 1, m);
        check("pose: positive azimuth is to the right (+X)", near(m[0][3], 1));
    }
    // --- natural size: the same pixels a degree for every panel
    {
        const auto l = tiling::Arrange({col({hd(1)}), col({px(2, 1080, 1920)}), col({px(3, 800, 600)})}, p);
        bool same = true;
        for (const auto &t : l.tiles) {
            const int w = t.id == 1 ? 1920 : t.id == 2 ? 1080 : 800;
            const double deg = tiling::Deg(2 * std::atan2(t.widthM / 2, t.distM));   // (its span seen from you)
            same = same && near(deg, 2 * tiling::Deg(std::atan(std::tan(tiling::Rad(0.5 / p.pxPerDeg)) * w)), 1e-6);
        }
        check("natural: every panel the same pixels a degree, whatever its shape", same);
        check("natural: 1920 wide is about 40 degrees at 48 a degree", near(l.az1 - l.az0, l.az1 - l.az0) &&
                                                                          near(l.tiles[0].az1 - l.tiles[0].az0, 40, 1.5));
        check("natural: a portrait one is taller than a landscape one", l.tiles[1].el1 > l.tiles[0].el1);
    }
    // --- the desk: every column stands on it, each panel resting on the one under it
    {
        const auto l = tiling::Arrange({col({px(1, 800, 600), hd(2)}), col({px(3, 1080, 1920)}), col({hd(4)})}, p);
        bool onDesk = true;
        for (const auto &t : l.tiles)
            if (t.id != 1) onDesk = onDesk && near(t.el0, p.deskDeg);
        check("desk: each column's bottom panel on the desk line", onDesk && near(l.el0, p.deskDeg));
        const auto &top = l.tiles[0], &under = l.tiles[1];
        check("desk: rows top down, the last on the desk", top.id == 1 && under.id == 2 && top.row == 0 && under.row == 1);
        check("desk: the one above rests on the one under it, a gap between", near(top.el0, under.el1 + p.gapDeg));
        check("desk: the tallest column's top is the layout's", near(l.el1, std::max(top.el1, l.tiles[2].el1)));
        check("desk: a narrow panel against its column's edge toward the strip's middle (nothing to tuck under it)",
              near(top.azDeg, (top.az0 + top.az1) / 2) && near(top.az1, under.az1) && top.az0 > under.az0);
        const auto ownSpan = [&](const tiling::Placed &t) {
            return near(t.az1 - t.az0, tiling::SpanFor(p.radius, t.widthM / t.distM * p.radius, t.heightM / t.distM * p.radius, t.elDeg), 1e-6);
        };
        check("desk: each panel spans its own width round you, not its column's widest", ownSpan(under) && ownSpan(top) &&
                                                                                          top.az1 - top.az0 < under.az1 - under.az0);
        check("desk: below your eyes by default (the bottom panels' tops above them)", p.deskDeg < 0 && under.el1 > 0);
    }
    // --- columns: centred, ordered, gaps, no corner overlap high up
    {
        std::vector<tiling::Column> cols;
        for (int i = 0; i < 4; ++i) cols.push_back(col({px(i * 10 + 1, 1600, 1200), hd(i * 10 + 2), hd(i * 10 + 3)}));
        const auto l = tiling::Arrange(cols, p);
        check("columns: strip centred in front of you", near(l.az0, -l.az1));
        check("columns: left to right, a gap between", apart(l, p.gapDeg * l.shrink) && tight(l, p.gapDeg * l.shrink));
        check("columns: no corner pokes into the next column, three high", cornersInColumns(l));
        bool stacked = true;
        for (const auto &t : l.tiles)
            for (const auto &u : l.tiles)
                if (t.col == u.col && t.row + 1 == u.row) stacked = stacked && near(t.el0 - p.gapDeg * l.shrink, u.el1);
        check("rows: top down, a gap between", stacked);
    }
    // --- packed: as little room between monitors as their panels allow (user, 2026-11-17: "very
    // large gaps between the left and right sides of the displays")
    {
        // a wide one on top of a narrow one, a short neighbour: the neighbour comes in under the
        // wide one's overhang, a gap from the narrow one it shares height with
        const std::vector<tiling::Column> cols = {col({px(1, 3440, 1440), px(2, 1280, 800)}), col({px(3, 1024, 600)})};
        const auto l = tiling::Arrange(cols, p);
        const auto &wide = l.tiles[0], &narrow = l.tiles[1], &shortOne = l.tiles[2];
        check("packed: the wide one is the strip's middle: the narrow one stays centred", near(narrow.azDeg, wide.azDeg));
        check("packed: a short neighbour a gap from the panel beside it", near(shortOne.az0, narrow.az1 + p.gapDeg));
        check("packed: ...under the wide one's overhang (apart up and down)", shortOne.az0 < wide.az1 && shortOne.el1 + p.gapDeg <= wide.el0);
        check("packed: nothing nearer than a gap, the strip as narrow as that", apart(l, p.gapDeg) && tight(l, p.gapDeg) &&
                                                                                    near(l.az1 - l.az0, std::max(wide.az1, shortOne.az1) - wide.az0));
        // a neighbour as tall as the wide one: it can't come under, so the wide one sets the gap
        const std::vector<tiling::Column> tall = {col({px(1, 3440, 1440), px(2, 1280, 800)}), col({px(3, 1080, 1920), hd(4)})};
        const auto t = tiling::Arrange(tall, p);
        check("packed: a neighbour reaching the wide one's height keeps a gap from it", apart(t, p.gapDeg) && tight(t, p.gapDeg) &&
                                                                                            t.tiles[2].az0 >= t.tiles[0].az1 + p.gapDeg - 1e-9);
        // ...and the narrow one under the wide one leans to it: no room between them, it's outside
        check("packed: the narrow one leans to the tall neighbour, a gap between (its room outside)",
              near(t.tiles[1].az1, t.tiles[0].az1) && near(t.tiles[2].az0, t.tiles[1].az1 + p.gapDeg));
        // the same mirrored: the tall one left, the wide over narrow right of it
        const std::vector<tiling::Column> mirror = {col({px(3, 1080, 1920), hd(4)}), col({px(1, 3440, 1440), px(2, 1280, 800)})};
        const auto m = tiling::Arrange(mirror, p);
        check("packed: mirrored, the narrow one leans left to it", near(m.tiles[3].az0, m.tiles[2].az0) &&
                                                                     near(m.tiles[3].az0, m.tiles[0].az1 + p.gapDeg) &&
                                                                     apart(m, p.gapDeg) && cornersInColumns(m));
        // between two tall neighbours: either way leaves the room between monitors, so the strip's middle
        const std::vector<tiling::Column> between = {col({px(5, 1080, 1920)}), col({px(1, 3440, 1440), px(2, 1280, 800)}),
                                                     col({px(3, 1080, 1920)})};
        const auto b = tiling::Arrange(between, p);
        check("packed: alone in the middle, between tall ones: centred", near(b.tiles[1].azDeg, b.tiles[2].azDeg) && near(b.az0, -b.az1));
        // three columns: a far column can't run into an earlier one past a short middle one
        const std::vector<tiling::Column> three = {col({px(1, 3440, 1440), px(2, 800, 600)}), col({px(3, 640, 480)}),
                                                   col({px(4, 3440, 1440), px(5, 800, 600)})};
        const auto f = tiling::Arrange(three, p);
        check("packed: past a short middle column, the high wide ones keep a gap", apart(f, p.gapDeg) && tight(f, p.gapDeg) &&
                                                                                       f.tiles[3].az0 >= f.tiles[0].az1 + p.gapDeg - 1e-9);
        check("packed: still centred, no corner overlap", near(f.az0, -f.az1) && cornersInColumns(f));
        // a drop on the wide one's overhang, above the short neighbour: that wide one's column
        const auto d = tiling::DropAt(l, cols, (wide.az1 + shortOne.az0) / 2 + 1, wide.elDeg);
        check("packed: a drop on an overhang goes by the panel there", d.kind == tiling::Drop::NewColumn && d.col == 1);
        const auto under = tiling::DropAt(l, cols, shortOne.azDeg, shortOne.elDeg);
        check("packed: ...and under it, the short one's column", under.kind == tiling::Drop::Into && under.col == 1);
    }
    // --- past a full turn: shrinks to fit
    {
        std::vector<tiling::Column> cols;
        for (int i = 0; i < 20; ++i) cols.push_back(col({hd(i + 1)}));
        const auto l = tiling::Arrange(cols, p);
        const double gap = p.gapDeg * l.shrink;   // (the gaps shrink with it)
        check("overflow: the strip fits in a turn", l.az1 - l.az0 <= p.maxSpanDeg - gap + 1e-6);
        check("overflow: and fills it (shrunk no more than it needs)", l.az1 - l.az0 >= 0.99 * (p.maxSpanDeg - gap));
        check("overflow: shrunk", l.shrink < 1);
        check("overflow: still no corner overlap", cornersInColumns(l));
    }
    // --- a column past kMaxTopDeg: shrinks till it's under it
    {
        const auto l = tiling::Arrange({col({hd(1), hd(2), hd(3), hd(4), hd(5)})}, p);
        check("too tall: under the cap", l.el1 <= tiling::kMaxTopDeg + 1e-6 && l.shrink < 1 && l.el1 > tiling::kMaxTopDeg - 1);
        check("too tall: still on the desk", near(l.tiles.back().el0, p.deskDeg) && cornersInColumns(l));
    }
    // --- the clamps' far end: dozens of tall columns at a low desk still fit a turn
    for (int n : {60, 200, 400}) {
        tiling::Params q = p;
        q.deskDeg = -40;
        std::vector<tiling::Column> cols;
        for (int i = 0; i < n; ++i) cols.push_back(col({hd(i + 1), px(1000 + i, 1080, 1920)}));
        const auto l = tiling::Arrange(cols, q);
        char what[96];
        std::snprintf(what, sizeof what, "extreme: %d columns fit a turn, no corner overlap", n);
        check(what, l.az1 - l.az0 <= q.maxSpanDeg + 1e-6 && l.shrink > 0 && cornersInColumns(l));
    }
    // --- a desk set high or low: sizes positive, in front
    for (double desk : {-40.0, 20.0}) {
        tiling::Params q = p;
        q.deskDeg = desk;
        const auto l = tiling::Arrange({col({hd(1), px(2, 1080, 1920)}), col({px(3, 800, 600)})}, q);
        bool sane = l.el1 <= tiling::kMaxTopDeg + 1e-9 && near(l.el0, desk);
        for (const auto &t : l.tiles) sane = sane && t.widthM > 0.05 && t.heightM > 0.05 && std::fabs(t.azDeg) < 180;
        char what[96];
        std::snprintf(what, sizeof what, "desk %.0f up: on it, sizes positive, in front", desk);
        check(what, sane && cornersInColumns(l));
    }
    // --- Steam's menu, a slot in the strip when the strip reaches it: the split's columns left of
    // it, the rest right, none in it
    {
        const auto clear = [](const tiling::Layout &l, double az0, double az1) {
            for (const auto &t : l.tiles)
                if (t.az1 > az0 + 1e-9 && t.az0 < az1 - 1e-9) return false;
            return true;
        };
        tiling::Params q = p;
        q.reserveAzDeg = 0, q.reserveDeg = 40;
        const auto one = tiling::Arrange({col({hd(1)})}, q);
        check("slot: one column goes right of it, a gap between", near(one.tiles[0].az0, 20 + q.gapDeg));
        std::vector<tiling::Column> two = {col({hd(1)}), col({hd(2)})};
        const auto l = tiling::Arrange(two, q);
        check("slot: two go either side of it", near(l.tiles[0].az1, -20 - q.gapDeg) && near(l.tiles[1].az0, 20 + q.gapDeg));
        check("slot: the strip takes it in", l.az0 < -20 && l.az1 > 20 && near(l.az1 - l.az0, (l.tiles[0].az1 - l.tiles[0].az0) * 2 + 40 + 2 * q.gapDeg));
        auto d = tiling::DropAt(l, two, 0, 0);
        check("slot: a drop on Steam's menu is a new column there", d.kind == tiling::Drop::NewColumn && d.col == 1);
        q.reserveAzDeg = 30;
        std::vector<tiling::Column> three = {col({hd(1)}), col({px(2, 1200, 1200)}), col({px(3, 1024, 768)})};
        const auto off = tiling::Arrange(three, q);
        const auto mid = [](const tiling::Layout &l) { return std::fabs(l.az0 + l.az1) / 2; };
        check("slot: where the menu is (30 degrees round), clear", clear(off, 10, 50) && off.split >= 0);
        check("slot: the split puts the strip's middle nearest in front",
              off.split == 2 && mid(off) <= mid(tiling::Arrange(three, q, 1)) && mid(off) <= mid(tiling::Arrange(three, q, 3)) &&
                  mid(off) <= mid(tiling::Arrange(three, q, 0)));
        const auto kept = tiling::Arrange(three, q, 0);
        check("slot: a split given is kept (the grid's)", kept.split == 0 && clear(kept, 10, 50) && kept.tiles[0].az0 >= 50);
        check("slot: a split past the columns is all of them", tiling::Arrange(three, q, 7).split == 3);
        std::vector<tiling::Column> many;
        for (int i = 0; i < 20; ++i) many.push_back(col({hd(i + 1)}));
        const auto full = tiling::Arrange(many, q);
        check("slot: past a turn the columns shrink, the slot doesn't", full.shrink < 1 && clear(full, 10, 50) &&
                                                                          full.az1 - full.az0 <= q.maxSpanDeg + 1e-6 &&
                                                                          cornersInColumns(full));
        const auto right = tiling::Arrange(many, q, 0);   // (all right of the menu: the strip runs past 180)
        const auto &last = right.tiles.back();   // (past 180 degrees: seen from the head, -180 + ...)
        d = tiling::DropAt(right, many, last.azDeg - 360, last.elDeg);
        check("slot: a drop on a tile past 180 degrees finds it", last.az1 > 180 && d.kind == tiling::Drop::Into && d.col == 19);
        q.reserveAzDeg = 0;
        d = tiling::DropAt(l, two, -3, 0);
        check("slot: let go in it left of its middle: a new column there, left", d.kind == tiling::Drop::NewColumn && d.col == 1 && d.leftOfSlot);
        d = tiling::DropAt(l, two, 3, 0);
        check("slot: ...right of it: there, right", d.kind == tiling::Drop::NewColumn && d.col == 1 && !d.leftOfSlot);
        // a menu the strip doesn't reach is no slot: the strip stays centred in front of you
        q.reserveAzDeg = 100;
        const auto far = tiling::Arrange(two, q);
        check("slot: a menu past the strip: none, centred", far.split == -1 && near(far.az0, -far.az1));
        check("slot: no slot, a drop isn't left of one", !tiling::DropAt(far, two, far.az0 + 1, 0).leftOfSlot);
        q.reserveAzDeg = 50, q.reserveDeg = 62;   // (council, 2026-11-16: past the old 45 degree gate, covered)
        const auto fifty = tiling::Arrange(two, q);
        check("slot: a menu 50 degrees round the strip reaches: kept clear", fifty.split >= 0 && clear(fifty, 19, 81));
        q.reserveAzDeg = 70, q.reserveDeg = 52;   // (the palm menu's, off to the side, with four columns)
        std::vector<tiling::Column> four = three;
        four.push_back(col({px(4, 1600, 1600)}));
        const auto palm = tiling::Arrange(four, q);
        check("slot: the palm menu's, the strip reaching it: kept clear", palm.split >= 0 && clear(palm, 44, 96));
        q.reserveAzDeg = -170, q.reserveDeg = 40;   // (behind you: the strip's ends wrap round to it)
        const auto behind = tiling::Arrange(many, q);
        bool wrapClear = true;
        for (const auto &t : behind.tiles)
            for (double a : {t.az0 + 1e-6, t.az1 - 1e-6, t.azDeg}) wrapClear = wrapClear && std::fabs(tiling::WrapNear(a - -170, 0)) >= 20 - 1e-6;
        check("slot: behind you, a full strip keeps it clear too", behind.split >= 0 && wrapClear);
        q.reserveDeg = 0;
        const auto none = tiling::Arrange(two, q);
        check("slot: none, the strip centred in front again", near(none.az0, -none.az1) && none.split == -1);
    }
    // --- drops
    {
        tiling::Params q = p;
        std::vector<tiling::Column> cols = {col({px(1, 1600, 900), px(2, 1600, 900)}), col({px(3, 1600, 900)})};
        const auto l = tiling::Arrange(cols, q);
        const auto &left = l.tiles[0], &lower = l.tiles[1];
        auto d = tiling::DropAt(l, cols, left.azDeg, left.el1 - 0.5);
        check("drop: on a column's middle, high: into it at row 0", d.kind == tiling::Drop::Into && d.col == 0 && d.row == 0);
        d = tiling::DropAt(l, cols, left.azDeg, lower.el0 + 0.5);
        check("drop: low in the column: after the last", d.kind == tiling::Drop::Into && d.col == 0 && d.row == 2);
        d = tiling::DropAt(l, cols, left.az0 + 2, left.elDeg);
        check("drop: a column's left quarter: a new column before it", d.kind == tiling::Drop::NewColumn && d.col == 0);
        d = tiling::DropAt(l, cols, left.az1 - 2, left.elDeg);
        check("drop: its right quarter: a new column after it", d.kind == tiling::Drop::NewColumn && d.col == 1);
        d = tiling::DropAt(l, cols, l.az1 + 5, left.elDeg);
        check("drop: just past the strip: a new column at the end", d.kind == tiling::Drop::NewColumn && d.col == 2);
        d = tiling::DropAt(l, cols, l.az1 + 30, left.elDeg);
        check("drop: well past the strip: torn off", d.kind == tiling::Drop::TearOff);
        d = tiling::DropAt(l, cols, 0, l.el1 + 25);
        check("drop: well above the tallest column: torn off", d.kind == tiling::Drop::TearOff);
        d = tiling::DropAt(l, cols, 0, l.el0 - 25);
        check("drop: well below it (in front of the dashboard): torn off", d.kind == tiling::Drop::TearOff);
        d = tiling::DropAt(tiling::Arrange({}, q), {}, 0, 0);
        check("drop: an empty grid takes it as its first column", d.kind == tiling::Drop::NewColumn && d.col == 0);
        const tiling::Layout none = tiling::Arrange({}, q);
        check("an empty strip has no span", none.az0 == 0 && none.az1 == 0);
    }
    // --- the grid model
    {
        tilegrid::Grid g;
        g.Add(1);
        g.Add(2, 1);
        g.Add(3, 1);   // right of 1: between 1 and 2
        check("add: right of the focused one", g.Cols().size() == 3 && g.Cols()[1].cells[0].id == 3);
        g.Add(3);
        check("add: twice is once", g.Cols().size() == 3);
        check("move right alone: columns swap", g.MoveSide(1, +1) && g.Cols()[0].cells[0].id == 3 && g.Cols()[1].cells[0].id == 1);
        check("move left at the left end alone: nothing", !g.MoveSide(3, -1));
        check("split: none chosen yet", g.Split(0) == -1 && g.Split(1) == -1 && g.Split(2) == -1);
        g.Insert(4, 1, 1, false);   // under 1
        check("insert: into a column at a row", g.Cols()[1].cells.size() == 2 && g.Cols()[1].cells[1].id == 4);
        check("move right with company: joins the next column", g.MoveSide(4, +1) && g.Cols()[2].cells.size() == 2 &&
                                                                   g.Cols()[2].cells[1].id == 4 && g.Cols()[1].cells.size() == 1);
        check("move right at the right end with company: a column of its own", g.MoveSide(4, +1) &&
                                                                                g.Cols().size() == 4 && g.Cols()[3].cells[0].id == 4);
        g.Insert(5, 0, 0, false);   // over 3
        check("move down: swaps with the one under", g.MoveUpDown(5, +1) && g.Cols()[0].cells[1].id == 5);
        check("move down at the bottom: hangs under the desk line", g.MoveUpDown(5, +1) && g.Cols()[0].base == 0 &&
                                                                      g.Cols()[0].Below(1) && g.Cols()[0].cells[1].id == 5);
        check("move down under the desk at the bottom: nothing", !g.MoveUpDown(5, +1));
        check("move up from under the desk: back on it", g.MoveUpDown(5, -1) && g.Cols()[0].base == 1 && g.Cols()[0].cells[1].id == 5);
        check("down and up: as it was", g.MoveUpDown(5, +1) && g.MoveUpDown(5, -1) && g.Cols()[0].base == 1);
        check("bigger: its size grows", g.Scale(5, tilegrid::kStep) && near(g.CellOf(5)->scale, 1.1));
        check("bigger: past its natural size, to the most", g.Scale(5, 100) && near(g.CellOf(5)->scale, tilegrid::kMaxScale) &&
                                                               !g.Scale(5, 2));
        check("smaller: never below the least", g.Scale(1, 0.001) && near(g.CellOf(1)->scale, tilegrid::kMinScale));
        check("size: a factor that isn't one changes nothing", !g.Scale(1, 0) && !g.Scale(1, -1) && !g.Scale(1, NAN));
        check("remove: its column goes with its last panel", g.Remove(1) && !g.Has(1) && g.Cols().size() == 3);
        check("remove: not there", !g.Remove(1));
    }
    // --- the grid's split round Steam's slot: each column stays on its side
    {
        tilegrid::Grid g;
        for (int id = 1; id <= 4; ++id) g.Add(id, id - 1);   // [ 1 ][ 2 ] S [ 3 ][ 4 ]
        g.SetSplit(0, 2);
        const auto ids = [&](int key = 0) {
            std::string s;
            for (size_t c = 0; c < g.Cols().size(); ++c) {
                if (int(c) == g.Split(key)) s += "S ";
                for (const auto &cell : g.Cols()[c].cells) s += std::to_string(cell.id);
                s += " ";
            }
            if (g.Split(key) == int(g.Cols().size())) s += "S ";
            return s;
        };
        check("split: set", ids() == "1 2 S 3 4 ");
        g.SetSplit(1, 4);   // (the palm menu's on the right: all four left of it)
        check("split: another place's split, the first one as it was", ids(1) == "1 2 3 4 S " && ids() == "1 2 S 3 4 ");
        g.Add(5, 2);
        check("split: a new one next to a left one goes left", ids() == "1 2 5 S 3 4 ");
        g.Add(6, 3);
        check("split: next to a right one, right", ids() == "1 2 5 S 3 6 4 ");
        g.Add(7);
        check("split: none focused: the right end", ids() == "1 2 5 S 3 6 4 7 ");
        g.Remove(1);
        check("split: a left column gone: one fewer left", ids() == "2 5 S 3 6 4 7 ");
        g.Remove(6);
        check("split: a right one gone: the same", ids() == "2 5 S 3 4 7 ");
        g.Insert(8, 2, 0, true, 1, true);
        check("split: a new column at the slot, left of it", ids() == "2 5 8 S 3 4 7 ");
        g.Insert(9, 3, 0, true, 1, false);
        check("split: ...right of it", ids() == "2 5 8 S 9 3 4 7 ");
        check("split: the other place's kept through the adds and removes", ids(1) == "2 5 8 9 3 4 S 7 ");
        check("split: alone, Meta+Right steps across the slot", g.MoveSide(8, +1, 0) && ids() == "2 5 S 8 9 3 4 7 ");
        check("split: ...the other place's split as it was", ids(1) == "2 5 8 9 3 4 S 7 ");
        check("split: ...and Meta+Left back", g.MoveSide(8, -1, 0) && ids() == "2 5 8 S 9 3 4 7 ");
        check("split: no slot in the strip: a plain swap", g.MoveSide(8, +1, -1) && ids() == "2 5 9 S 8 3 4 7 ");
        g.Insert(10, 2, 1, false);   // under 9
        check("split: with company, Meta+Right: a column of its own past the slot", g.MoveSide(10, +1, 0) &&
                                                                                   ids() == "2 5 9 S 10 8 3 4 7 ");
        g.Insert(11, 3, 1, false);   // under 10
        check("split: ...Meta+Left: one of its own left of it", g.MoveSide(11, -1, 0) && ids() == "2 5 9 11 S 10 8 3 4 7 ");
        g.SetSplit(0, 99);
        check("split: past the columns: all of them", g.Split(0) == int(g.Cols().size()));
        g.SetSplit(0, -5);
        check("split: none", g.Split(0) == -1 && (g.Add(12, 2), g.Split(0) == -1));
        g.AddAtEnd(13, true);
        check("split: at the left end, left of the slots", g.Cols()[0].cells[0].id == 13 && ids(1) == "13 2 12 5 9 11 10 8 3 4 S 7 ");
        check("split: a key that isn't one: none", g.Split(3) == -1 && g.Split(-1) == -1);
        for (int id = 2; id <= 13; ++id) g.Remove(id);
        check("split: the grid emptied, no splits (a new panel isn't put past a slot)",
              g.Cols().empty() && g.Split(0) == -1 && g.Split(1) == -1);
        g.SetSplit(1, 0);
        check("split: none set on an empty grid", g.Split(1) == -1);
    }
    // --- tiled panels are flat (a curved one reaches past its column), their own curves kept
    {
        double curve = 1.5, kept = 0;
        check("flat: a curved panel tiled goes flat", tiling::FlatWhileTiled(true, &curve, &kept) && curve == 0 && near(kept, 1.5));
        check("flat: ...and stays so (nothing to change)", !tiling::FlatWhileTiled(true, &curve, &kept) && curve == 0 && near(kept, 1.5));
        check("flat: its curve back when it leaves", tiling::FlatWhileTiled(false, &curve, &kept) && near(curve, 1.5) && kept == 0);
        check("flat: a panel not tiled keeps its own", !tiling::FlatWhileTiled(false, &curve, &kept) && near(curve, 1.5));
        double flat = 0, none = 0;
        check("flat: a flat one tiled: nothing kept", !tiling::FlatWhileTiled(true, &flat, &none) && flat == 0 && none == 0);
        curve = 2, kept = 0;
        tiling::FlatWhileTiled(true, &curve, &kept);
        curve = 1.2;   // its curve button while tiled
        check("flat: a curve given it tiled goes flat, kept for later", tiling::FlatWhileTiled(true, &curve, &kept) && curve == 0 && near(kept, 1.2));
        tiling::FlatWhileTiled(true, &curve, &kept);
        tiling::FlatWhileTiled(false, &curve, &kept);
        curve = 3, kept = 2;   // (one given it once it left, by grab that, before the sweep)
        check("flat: a curve given it after it left stays", !tiling::FlatWhileTiled(false, &curve, &kept) && near(curve, 3) && kept == 0);
    }
    // --- why: a panel curved round its own cylinder covers more of a turn than its (flat) column
    {
        tiling::Params q;
        const tiling::Layout l = tiling::Arrange({col({px(1, 3440, 1440)}), col({px(2, 2560, 1600)})}, q);
        const tiling::Placed &a = l.tiles[0], &b = l.tiles[1];
        // half the angle a panel w wide covers from the centre at r, curved round R (0 flat): OpenVR
        // curves it toward its front, the cylinder's axis R in front of it
        const auto half = [&](const tiling::Placed &t, double R) {
            if (R <= 0) return tiling::Deg(std::atan2(t.widthM / 2, t.distM));
            return tiling::Deg(std::atan2(R * std::sin(t.widthM / 2 / R), t.distM - R + R * std::cos(t.widthM / 2 / R)));
        };
        const double flatGap = (b.azDeg - half(b, 0)) - (a.azDeg + half(a, 0));
        const double curvedGap = (b.azDeg - half(b, 1.2)) - (a.azDeg + half(a, 1.2));
        check("flat: two flat tiles on the desk line keep at least the gap", flatGap >= q.gapDeg - 1e-6);
        check("flat: curved round 1.2 m they'd overlap", curvedGap < 0);
    }
    // --- every tile's corners on the sphere, not its middle: different widths, the corners at one depth
    {
        tiling::Params q;
        const auto allOn = [&](const tiling::Layout &l, double r) {
            for (const auto &t : l.tiles) {
                double c[4][3];
                corners(t, c);
                for (auto &pt : c)
                    if (!near(std::sqrt(pt[0] * pt[0] + pt[1] * pt[1] + pt[2] * pt[2]), r, 1e-9)) return false;
            }
            return true;
        };
        const tiling::Layout l = tiling::Arrange({col({px(1, 3440, 1440)}), col({hd(2)}), col({px(3, 1000, 1000)})}, q);
        check("corners: a wide, a 16:9 and a square one: every corner at the radius", allOn(l, q.radius));
        check("corners: the wider one nearer", l.tiles[0].distM < l.tiles[1].distM && l.tiles[1].distM < q.radius);
        check("corners: a 16:9 one's middle about 2.5 m away", near(l.tiles[1].distM, 2.5, 0.05));
        check("corners: and they still don't reach past their columns", cornersInColumns(l));
        tiling::Params hi = q;
        hi.deskDeg = 10;
        const tiling::Layout s2 = tiling::Arrange({col({px(1, 1600, 1000), px(2, 2400, 1000)}), col({px(3, 1000, 1000), px(4, 1000, 1000), px(5, 700, 1000)})}, hi);
        check("corners: stacked rows high up: every corner at the radius", allOn(s2, hi.radius));
        check("corners: ...and inside their columns", cornersInColumns(s2));
        // seen from the centre, each looks as big as it did with its middle at the radius
        tiling::Placed one;
        one.widthM = 1.2, one.heightM = 0.7;
        const double before = std::atan2(one.widthM / 2, q.radius);
        tiling::CornersOnSphere(q.radius, &one);
        check("corners: the same span seen from the centre", near(std::atan2(one.widthM / 2, one.distM), before, 1e-12));
    }
    // --- round the desk line (user, 2026-11-19: "displays go from the center row down or up", and a
    // 600 px one "on the right, or the left of below the 1400")
    {
        tiling::Column c = col({px(1, 1300, 700), px(2, 1400, 800), px(3, 600, 400)});
        c.base = 1;   // 1 over 2 on the desk, 3 hanging under it
        const tiling::Layout l = tiling::Arrange({c}, p);
        const auto &t1 = l.tiles[0], &t2 = l.tiles[1], &t3 = l.tiles[2];
        check("under the desk: the base on the desk line", near(t2.el0, p.deskDeg) && !t2.below);
        check("under the desk: the one after the base hangs under it, a gap down", t3.below && near(t3.el1, p.deskDeg - p.gapDeg) &&
                                                                                      t3.el0 < t3.el1);
        check("under the desk: the one before it above", t1.el0 > t2.el1 && !t1.below);
        check("under the desk: the layout reaches down to it", near(l.el0, t3.el0) && near(l.deskDeg, p.deskDeg));
        check("under the desk: the narrow one can lean", t3.narrow && !t2.narrow);
        tiling::Column left = c, right = c;
        left.tiles[2].align = -1, right.tiles[2].align = 1;
        const tiling::Layout ll = tiling::Arrange({left}, p), lr = tiling::Arrange({right}, p);
        check("align: left, against the column's left edge", near(ll.tiles[2].az0, ll.tiles[1].az0, 1e-6) && ll.tiles[2].lean == -1);
        check("align: right, against its right edge", near(lr.tiles[2].az1, lr.tiles[1].az1, 1e-6) && lr.tiles[2].lean == 1);
        // two 600s side by side on a shelf over the 1300
        tiling::Column four = col({px(4, 600, 400), px(5, 600, 400), px(1, 1300, 700), px(2, 1400, 800)});
        const tiling::Layout lf = tiling::Arrange({four}, p);
        check("sub-columns: the 600s share a shelf over the 1300", near(lf.tiles[0].el0, lf.tiles[1].el0) &&
                                                                     lf.tiles[0].el0 > lf.tiles[2].el1);
        // a drop under the desk line hangs it there, leaning the side it was let go
        const std::vector<tiling::Column> cols = {c};
        const tiling::Drop d = tiling::DropAt(l, cols, t3.az0 + 0.3 * (t3.az1 - t3.az0), t3.el0 - 1);
        check("drop: under the desk line, below", d.kind == tiling::Drop::Into && d.below && d.row == 3 && d.align == -1);
        const tiling::Drop up = tiling::DropAt(l, cols, t2.azDeg + 1, t2.el0 + 0.5);
        check("drop: low on the base, above the desk: the new base", up.kind == tiling::Drop::Into && !up.below && up.row == 2 &&
                                                                       up.align == 1);
    }
    {
        tilegrid::Grid g;
        g.Add(1), g.Insert(2, 0, 1, false), g.Insert(3, 0, 2, false, 1, false, true);   // 1 over 2, 3 under the desk
        check("grid: below keeps the base", g.Cols()[0].base == 1 && g.Cols()[0].Below(2));
        g.Insert(4, 0, 0, false);   // on top
        check("grid: one on top moves the base down a row", g.Cols()[0].base == 2 && g.Cols()[0].cells[2].id == 2);
        g.Remove(2);
        check("grid: the base gone, the one above takes the desk", g.Cols()[0].base == 1 && g.Cols()[0].cells[1].id == 1 &&
                                                                    g.Cols()[0].Below(2));
        g.Add(5);
        check("grid: moved right from under the desk: under it there too", g.MoveSide(3, +1) && g.Cols()[1].cells[1].id == 3 &&
                                                                              g.Cols()[1].Below(1));
        check("grid: align", g.SetAlign(3, -1) && g.CellOf(3)->align == -1 && !g.SetAlign(3, -1));
    }
    // --- fitted by size, no columns (tilefit.h; user, 2026-11-19: "have displays auto-fit based off of
    // their size to try to make the best use of real estate")
    {
        std::vector<tiling::Column> cols = {col({px(1, 1400, 900)}), col({px(2, 1300, 800)}), col({px(3, 600, 500), px(4, 600, 500)}),
                                            col({px(5, 600, 500)}), col({hd(6)}), col({px(7, 800, 1200)}), col({px(8, 500, 700)})};
        tiling::Params q = p;
        const tiling::Layout f = tilefit::Fit(cols, q);
        bool clear = f.tiles.size() == 8;
        for (const auto &a : f.tiles)
            for (const auto &b : f.tiles)
                if (a.id < b.id && !tilefit::Clear({a.az0, a.az1, a.el0, a.el1}, {b.az0, b.az1, b.el0, b.el1}, q.gapDeg)) clear = false;
        check("fit: every panel placed, a gap from every other", clear);
        bool up = false, down = false;
        for (const auto &a : f.tiles) up |= a.el0 > f.tiles[0].el1 - 1e-6, down |= a.el1 < f.tiles[0].el0 + 1e-6;
        check("fit: the largest on the desk line, the rest round it", near(f.tiles[0].el0, q.deskDeg) && f.tiles[0].id == 6);
        check("fit: some over it and some under it", up && down);
        check("fit: corners at the radius", cornersInColumns(f));
        const tiling::Layout g = tiling::Arrange(cols, q);
        std::printf("      (grid: %.0f x %.0f degrees, fit: %.0f x %.0f)\n", g.az1 - g.az0, g.el1 - g.el0, f.az1 - f.az0, f.el1 - f.el0);
        check("fit: narrower round you than the grid's single row", f.az1 - f.az0 < g.az1 - g.az0);
        tiling::Params s = q;
        s.reserveDeg = 30, s.reserveAzDeg = 0;
        const tiling::Layout fs = tilefit::Fit(cols, s);
        bool slotClear = true;
        for (const auto &a : fs.tiles) slotClear &= a.az1 <= -15 + 1e-6 || a.az0 >= 15 - 1e-6;
        check("fit: Steam's slot kept clear", slotClear && fs.split >= 0);
        std::vector<tiling::Column> many;
        for (int i = 0; i < 40; ++i) many.push_back(col({hd(100 + i)}));
        const tiling::Layout fm = tilefit::Fit(many, q);
        check("fit: forty screens shrink to fit in a turn", fm.shrink < 1 && fm.az1 - fm.az0 <= q.maxSpanDeg);
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
