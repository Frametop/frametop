// The dashboard tiles (screens/tiling.h, screens/tilegrid.h): every panel faces the head, neighbours
// never touch at their corners however high the band goes, the band's top is capped, a strip past a
// full turn shrinks to fit, fixed panels keep their shape, drops land where they're let go, and the
// keys move and size panels as Omarchy's do.
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
static void corners(const tiling::Placed &t, double radius, double out[4][3]) {
    double m[3][4];
    tiling::Pose(t.azDeg, t.elDeg, radius, m);
    const double su[4] = {-1, 1, -1, 1}, sv[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; ++k)
        for (int r = 0; r < 3; ++r)
            out[k][r] = m[r][3] + m[r][0] * su[k] * t.widthM / 2 + m[r][1] * sv[k] * t.heightM / 2;
}

// No corner of one tile inside another's azimuth/elevation span seen from the centre: neighbours
// don't overlap anywhere on the sphere (a corner's own azimuth inside its column's span).
static bool cornersInColumns(const tiling::Layout &l, double radius) {
    for (const auto &t : l.tiles) {
        double c[4][3];
        corners(t, radius, c);
        for (auto &pt : c) {
            double az, el;
            tiling::Direction(pt, &az, &el);
            if (az < t.az0 - 1e-6 || az > t.az1 + 1e-6) return false;
        }
    }
    return true;
}

static tiling::Column col(std::initializer_list<tiling::Tile> tiles, double widthDeg = 0) {
    tiling::Column c;
    c.tiles = tiles;
    c.widthDeg = widthDeg;
    return c;
}

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
    // --- the band and its cap
    {
        tiling::Params q = p;
        q.bottomDeg = 10, q.heightDeg = 80, q.maxTopDeg = 55;
        const auto l = tiling::Arrange({col({{1, 16.0 / 9, true, 1}})}, q);
        check("band: capped at maxTopDeg", near(l.el1, 55));
        check("band: starts at the dashboard's top", near(l.el0, 10));
        q.heightDeg = 3;
        const auto s = tiling::Arrange({col({{1, 1, true, 1}})}, q);
        check("band: never shorter than minHeightDeg", near(s.el1 - s.el0, q.minHeightDeg));
    }
    // --- columns: centred, ordered, gaps, no corner overlap high up
    {
        tiling::Params q = p;
        q.bottomDeg = 20, q.heightDeg = 60, q.maxTopDeg = 70;   // a high band: corners pinch together
        std::vector<tiling::Column> cols;
        for (int i = 0; i < 4; ++i) cols.push_back(col({{i * 10 + 1, 16.0 / 9, true, 1}, {i * 10 + 2, 4.0 / 3, true, 1}}));
        const auto l = tiling::Arrange(cols, q);
        check("columns: strip centred over the dashboard", near(l.az0, -l.az1));
        bool ordered = true;
        for (const auto &t : l.tiles)
            for (const auto &u : l.tiles)
                if (t.col + 1 == u.col) ordered = ordered && t.az1 + q.gapDeg - 1e-9 <= u.az0;
        check("columns: left to right, a gap between", ordered);
        check("columns: no corner pokes into the next column, even at 70 degrees up", cornersInColumns(l, q.radius));
        bool stacked = true;
        for (const auto &t : l.tiles)
            for (const auto &u : l.tiles)
                if (t.col == u.col && t.row + 1 == u.row) stacked = stacked && t.el0 - q.gapDeg + 1e-9 >= u.el1;
        check("rows: top down, a gap between", stacked);
    }
    // --- fixed keep their shape, resizable fill the cell
    {
        tiling::Params q = p;
        const auto l = tiling::Arrange({col({{1, 21.0 / 9, true, 1}}, 20), col({{2, 1, false, 1}, {3, 1, false, 3}}, 30)}, q);
        const auto &ultra = l.tiles[0];
        check("fixed: keeps its aspect", near(ultra.widthM / ultra.heightM, 21.0 / 9, 1e-6));
        check("fixed: its column grew past the 20 degrees asked for", ultra.az1 - ultra.az0 > 20 + 1e-6);
        const auto &a = l.tiles[1], &b = l.tiles[2];
        check("resizable: the lower one no narrower (the circles round you are wider lower down)", b.widthM >= a.widthM - 1e-9);
        check("resizable: rows by weight (1:3)", near((b.el1 - b.el0) / (a.el1 - a.el0), 3, 1e-6));
        check("resizable: its column kept the 30 degrees asked for", near(a.az1 - a.az0, 30));
        check("columns: no corner overlap with mixed kinds", cornersInColumns(l, q.radius));
    }
    // --- past a full turn: shrinks to fit
    {
        tiling::Params q = p;
        std::vector<tiling::Column> cols;
        for (int i = 0; i < 20; ++i) cols.push_back(col({{i + 1, 16.0 / 9, true, 1}}));
        const auto l = tiling::Arrange(cols, q);
        check("overflow: the strip fits in a turn", l.az1 - l.az0 <= q.maxSpanDeg - q.gapDeg + 1e-6);
        check("overflow: shrunk", l.shrink < 1);
        check("overflow: still no corner overlap", cornersInColumns(l, q.radius));
        check("overflow: the band shrank with it (shapes kept)", l.el1 - l.el0 < std::min(q.heightDeg, q.maxTopDeg - q.bottomDeg));
    }
    // --- drops
    {
        tiling::Params q = p;
        std::vector<tiling::Column> cols = {col({{1, 1, false, 1}, {2, 1, false, 1}}, 40), col({{3, 1, false, 1}}, 40)};
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
        check("drop: well above the band: torn off", d.kind == tiling::Drop::TearOff);
        d = tiling::DropAt(l, cols, 0, l.el0 - 25);
        check("drop: well below it (in front of the dashboard): torn off", d.kind == tiling::Drop::TearOff);
        d = tiling::DropAt(tiling::Arrange({}, q), {}, 0, 0);
        check("drop: an empty grid takes it as its first column", d.kind == tiling::Drop::NewColumn && d.col == 0);
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
        g.Insert(4, 1, 1, false);   // under 1
        check("insert: into a column at a row", g.Cols()[1].cells.size() == 2 && g.Cols()[1].cells[1].id == 4);
        check("move right with company: joins the next column", g.MoveSide(4, +1) && g.Cols()[2].cells.size() == 2 &&
                                                                   g.Cols()[2].cells[1].id == 4 && g.Cols()[1].cells.size() == 1);
        check("move right at the right end with company: a column of its own", g.MoveSide(4, +1) &&
                                                                                g.Cols().size() == 4 && g.Cols()[3].cells[0].id == 4);
        g.Insert(5, 0, 0, false);   // over 3
        check("move down: swaps with the one under", g.MoveUpDown(5, +1) && g.Cols()[0].cells[1].id == 5);
        check("move down at the bottom: nothing", !g.MoveUpDown(5, +1));
        check("taller: its weight grows", g.Heighten(5, tilegrid::kStep) && g.Cols()[0].cells[1].weight > 1);
        check("taller alone in a column: nothing", !g.Heighten(1, tilegrid::kStep));
        check("wider: its column's width set", g.Widen(1, tilegrid::kStep, 40) && near(g.Cols()[1].widthDeg, 44));
        check("narrower: never below the minimum", g.Widen(1, 0.01, 44) && near(g.Cols()[1].widthDeg, tilegrid::kMinWidthDeg));
        const std::string text = g.Text();
        tilegrid::Grid h;
        check("text: round trip", h.FromText(text, [](int) { return true; }) && h.Text() == text);
        tilegrid::Grid k;
        check("text: panels that went are dropped, empty columns too",
              k.FromText(text, [](int id) { return id != 1; }) && k.Cols().size() == 3 && !k.Has(1));
        check("text: garbage is refused", !k.FromText("rows 1 2", [](int) { return true; }) && k.Cols().size() == 3);
        check("text: a panel twice is once", k.FromText("col 0 7:1 7:1; col 0 7:1", [](int) { return true; }) &&
                                                 k.Cols().size() == 1 && k.Cols()[0].cells.size() == 1);
        check("remove: its column goes with its last panel", g.Remove(1) && !g.Has(1) && g.Cols().size() == 3);
        check("remove: not there", !g.Remove(1));
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
