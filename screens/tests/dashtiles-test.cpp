// The dashboard tiles' policy (screens/dashtiles.h): anchoring, tearing off and dropping back,
// the keys on fixed and resizable panels, profiles' grids, and what each app or display remembers.
#include "../dashtiles.h"

#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
static const double kI[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

int main() {
    const auto desk = [](int) { return dashtiles::Info{16.0 / 9, true}; };
    const auto info = [](int id) { return dashtiles::Info{id >= 10 ? 4.0 / 3 : 16.0 / 9, id < 10}; };
    // --- the group frame
    {
        double g[3][4], el;
        const double head[3] = {1, 1.6, 0}, bar[3] = {1, 1.2, -1.5}, back[3] = {0, 0, 1};
        dashtiles::GroupFrame(head, bar, back, g, &el);
        check("group: at the head", near(g[0][3], 1) && near(g[1][3], 1.6) && near(g[2][3], 0));
        check("group: -Z towards the bar", near(g[2][2], 1) && near(g[0][2], 0));
        check("group: level", near(g[1][1], 1) && near(g[1][0], 0) && near(g[1][2], 0));
        check("group: the bar's elevation", near(el, tiling::Deg(std::atan2(-0.4, 1.5))));
        const double right[3] = {2.5, 1.6, 0};
        dashtiles::GroupFrame(head, right, back, g, &el);
        check("group: turned to a bar on the right (-Z is +X)", near(g[0][2], -1) && near(g[0][0], 0) && near(g[2][0], 1));
        const double over[3] = {1, 2.5, 0}, tilted[3] = {0, 0.3, 1};
        dashtiles::GroupFrame(head, over, tilted, g, &el);
        check("group: a bar right overhead faces its back", near(g[2][2], 1) && near(el, 90, 1e-3));
    }
    // --- anchoring
    {
        dashtiles::Board b;
        b.Anchor(1, -1, false);
        b.Anchor(2, 1, true);
        b.Anchor(3, 1, false);
        check("anchor: a column right of the focused one", b.Grid().Cols().size() == 3 && b.Grid().Cols()[1].cells[0].id == 3);
        check("anchor: stay as asked", b.Stay(2) && !b.Stay(1));
        b.Anchor(2, -1, false);
        check("anchor twice: once, stay kept", b.Grid().Cols().size() == 3 && b.Stay(2));
        check("unanchor", b.Unanchor(3) && !b.Anchored(3) && b.Grid().Cols().size() == 2 && !b.Unanchor(3));
        b.SetStay(9, true);
        check("stay on a panel not anchored: nothing", !b.Anchored(9));
    }
    // --- tear off and back
    {
        dashtiles::Board b;
        b.params.heightDeg = 25;   // (full-height desktops at the default band take most of a turn)
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, false);
        const auto &l = b.Arrange(desk);
        const double rel[3][4] = {{1, 0, 0, 0.5}, {0, 1, 0, 2}, {0, 0, 1, -1}};
        auto d = b.DropFor(2, l.az1 + 40, 20, desk);
        check("drop: far off: torn off", d.kind == tiling::Drop::TearOff);
        b.Drop(2, d, rel);
        check("torn off: a satellite, still anchored, off the grid", b.Satellite(2) && b.Anchored(2) && !b.Tiled(2) &&
                                                                         near(b.SpotOf(2)->rel[1][3], 2));
        b.Arrange(desk);
        const tiling::Placed *one = b.PlacedOf(1);
        d = b.DropFor(2, one->azDeg, one->el1 - 0.5, desk);
        check("dropped on a tile's middle: into its column", d.kind == tiling::Drop::Into && d.col == 0 && d.row == 0);
        b.Drop(2, d, rel);
        check("back on the grid: not a satellite", !b.Satellite(2) && b.Tiled(2) && b.Grid().Cols()[0].cells[0].id == 2);
        // dragged out of its own column, the indices are the grid's without it
        b.Arrange(desk);
        const tiling::Placed *three = b.PlacedOf(3);
        d = b.DropFor(1, three->az1 - 0.5, three->elDeg, desk);
        b.Drop(1, d, rel);
        check("moved to the right end: a new column there", b.Grid().Cols().back().cells[0].id == 1 &&
                                                                 b.Grid().Cols()[0].cells.size() == 1);
    }
    // --- drops aim at the grid as shown (council, 2026-11-16: it used the grid re-centred without
    // the dragged one, so a drop on the middle column's middle made a new column)
    {
        dashtiles::Board b;
        b.params.heightDeg = 25;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, false);
        const double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        b.Arrange(desk);
        const tiling::Placed p2 = *b.PlacedOf(2);
        auto d = b.DropFor(3, p2.azDeg, p2.elDeg - 1, desk);
        check("drop: the right one on the middle one's middle: into it", d.kind == tiling::Drop::Into && d.col == 1);
        b.Drop(3, d, rel);
        check("...stacked under 2", b.Grid().Cols().size() == 2 && b.Grid().Cols()[1].cells.size() == 2 &&
                                        b.Grid().Cols()[1].cells[0].id == 2 && b.Grid().Cols()[1].cells[1].id == 3);
        b.Arrange(desk);
        const tiling::Placed p1 = *b.PlacedOf(1);
        d = b.DropFor(1, p1.azDeg, p1.elDeg, desk);
        b.Drop(1, d, rel);
        check("drop: onto itself: back where it was", b.Grid().Cols().size() == 2 && b.Grid().Cols()[0].cells[0].id == 1);
    }
    // --- the keys
    {
        dashtiles::Board b;
        b.Anchor(1, -1, false);    // a desktop screen
        b.Anchor(10, 1, false);    // a floating window
        b.Arrange(info);
        const double before = b.PlacedOf(10)->az1 - b.PlacedOf(10)->az0;
        check("wider, resizable: its column", b.Press(10, dashtiles::Key::Wider, info(10)));
        b.Arrange(info);
        check("...10% wider", near(b.PlacedOf(10)->az1 - b.PlacedOf(10)->az0, before * tilegrid::kStep, 1e-6));
        check("wider, fixed, already its row's height: nothing", !b.Press(1, dashtiles::Key::Wider, info(1)));
        check("narrower, fixed: smaller in its row", b.Press(1, dashtiles::Key::Narrower, info(1)) &&
                                                         b.Grid().CellOf(1)->scale < 1);
        check("taller, fixed, alone: back up", b.Press(1, dashtiles::Key::Taller, info(1)) && near(b.Grid().CellOf(1)->scale, 1));
        check("taller, resizable, alone: nothing", !b.Press(10, dashtiles::Key::Taller, info(10)));
        check("left: the columns swap", b.Press(10, dashtiles::Key::Left, info(10)) && b.Grid().Cols()[0].cells[0].id == 10);
        check("down alone: nothing", !b.Press(10, dashtiles::Key::Down, info(10)));
        const double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        b.TearOff(10, rel);
        check("a satellite: the keys do nothing", !b.Press(10, dashtiles::Key::Right, info(10)));
        dashtiles::Key k;
        check("key names", dashtiles::ParseKey("shorter", &k) && k == dashtiles::Key::Shorter && !dashtiles::ParseKey("x", &k));
    }
    // --- a resize by hand
    {
        dashtiles::Board b;
        b.Anchor(10, -1, false);
        b.Anchor(11, 10, false);
        b.Drop(11, {tiling::Drop::Into, 0, 1}, kI);
        b.Arrange(info);
        const double w = b.PlacedOf(10)->az1 - b.PlacedOf(10)->az0;
        check("resized by hand: column and row", b.Resized(10, 1.5, 1.2, info(10)) && b.Grid().CellOf(10)->weight > 1);
        b.Arrange(info);
        check("...its column 1.5 times as wide", near(b.PlacedOf(10)->az1 - b.PlacedOf(10)->az0, w * 1.5, 1e-6));
    }
    // --- memory
    {
        dashtiles::Board b;
        b.Anchor(1, -1, true);
        b.Anchor(2, 1, false);
        b.Anchor(3, 2, false);
        b.Drop(3, {tiling::Drop::Into, 1, 1}, kI);
        const std::string one = b.Remember(1), three = b.Remember(3);
        check("remember: a tile", one.rfind("tile 1 0 0 1 ", 0) == 0);
        check("remember: under another", three.rfind("tile 0 1 1 0 ", 0) == 0);
        check("remember: not anchored", b.Remember(7) == "off");
        const double rel[3][4] = {{1, 0, 0, 0.25}, {0, 1, 0, 0.5}, {0, 0, 1, -2}};
        b.TearOff(2, rel);
        const std::string sat = b.Remember(2);
        check("remember: a satellite", sat.rfind("sat 0 ", 0) == 0);
        dashtiles::Board c;
        c.Anchor(5, -1, false);
        check("restore: a tile in a column of its own", c.Restore(1, one, 5) && c.Tiled(1) && c.Stay(1) &&
                                                            c.Grid().Cols().size() == 2 && c.Grid().Cols()[0].cells[0].id == 1);
        check("restore: under another", c.Restore(3, three, 5) && c.Grid().Cols()[1].cells.size() == 2 &&
                                            c.Grid().Cols()[1].cells[1].id == 3);
        check("restore: a satellite", c.Restore(2, sat, 5) && c.Satellite(2) && near(c.SpotOf(2)->rel[2][3], -2));
        check("restore: off", c.Restore(2, "off", 5) && !c.Anchored(2));
        check("restore: garbage refused", !c.Restore(4, "tile 1 2", 5) && !c.Restore(4, "sat 1 1 2 3", 5) &&
                                              !c.Restore(4, "tile 1 0 0 1 nan 1 0", 5) && !c.Anchored(4));
        dashtiles::Memory m;
        check("memory: set", m.Set("app:org.kde.dolphin", one) && !m.Set("app:org.kde.dolphin", one));
        check("memory: a key is one word", m.Set("app:My App\tx", "off") && m.Get("app:My_App_x") == "off");
        check("memory: no tab or newline in a state", !m.Set("k", "a\nb"));
        dashtiles::Memory n;
        n.Load(m.Text() + "junk line\n\tnokey\n");
        check("memory: round trip, junk skipped", n.Text() == m.Text() && n.Size() == 2);
        for (int i = 0; i < 600; ++i) n.Set("k" + std::to_string(i), "off");
        check("memory: capped", n.Size() == dashtiles::Memory::kMax);
        // the one set longest ago goes, not the first in order (council, 2026-11-16)
        check("memory: the newest stays", n.Set("app:aa.just.opened", "off") && n.Get("app:aa.just.opened") == "off");
        check("memory: the oldest went", n.Get("k100").empty() && n.Get("k101") == "off");
        dashtiles::Memory o;
        o.Load(n.Text());
        o.Set("zz.new", "off");
        check("memory: the order survives a save and load", o.Get("k101").empty() && o.Get("app:aa.just.opened") == "off");
    }
    // --- a profile's tiles back: its columns first, as saved; the tiles it doesn't name after
    // (council, 2026-11-16: they were inserted into the live grid, so they joined its columns)
    {
        dashtiles::Board b;
        b.Anchor(199, -1, true);   // a gamescope app, the only tile
        const std::string top = "tile 0 0 0 0 1.000 1.000 0.00", under = "tile 0 0 1 0 1.000 1.000 0.00";
        check("restore all: both taken", b.RestoreAll({{0, top}, {1, under}, {5, "nonsense"}}) == 2);
        const auto &cols = b.Grid().Cols();
        check("restore all: the profile's column first, stacked as saved", cols.size() == 2 && cols[0].cells.size() == 2 &&
                                                                               cols[0].cells[0].id == 0 && cols[0].cells[1].id == 1);
        check("restore all: the app in a column of its own after it", cols[1].cells.size() == 1 && cols[1].cells[0].id == 199);
        check("restore all: a line that isn't one: left alone", !b.Anchored(5));
    }
    // --- memory: kept by use
    {
        dashtiles::Memory m;
        m.Set("app:daily", "off");
        for (size_t i = 1; i < dashtiles::Memory::kMax; ++i) {
            m.Set("app:once" + std::to_string(i), "off");
            if (i % 100 == 0) m.Used("app:daily");   // (it keeps coming back)
        }
        m.Set("app:new", "off");
        check("memory: the one used often stays, the oldest one-off goes",
              !m.Get("app:daily").empty() && m.Get("app:once1").empty() && m.Size() == dashtiles::Memory::kMax);
    }
    // --- a profile split over two commands (layout_tiles.py: "tiles set", then "tiles add")
    {
        dashtiles::Board b;
        b.Anchor(199, -1, true);
        auto line = [](int col, int row, int alone, double width) {
            char t[96];
            std::snprintf(t, sizeof t, "tile 0 %d %d %d 1.000 1.000 %.2f", col, row, alone, width);
            return std::string(t);
        };
        b.RestoreAll({{10, line(0, 0, 1, 0)}, {11, line(1, 0, 1, 0)}, {12, line(2, 0, 0, 53.24)}});
        b.RestoreAll({{13, line(2, 1, 0, 53.24)}, {14, line(3, 0, 1, 0)}}, true);
        std::string order;
        for (const auto &c : b.Grid().Cols()) {
            order += "[";
            for (const auto &cell : c.cells) order += " " + std::to_string(cell.id);
            order += " ]";
        }
        check("split restore: columns in the profile's order, the app after them",
              order == "[ 10 ][ 11 ][ 12 13 ][ 14 ][ 199 ]");
        check("split restore: a stacked column keeps its width", std::fabs(b.Grid().Cols()[2].widthDeg - 53.24) < 1e-6);
        b.RestoreAll({{14, line(0, 0, 1, 0)}});   // a set again: a new profile, its columns first
        check("a set again starts over", b.Grid().Cols()[0].cells[0].id == 14);
    }
    // --- council pass 3: saved columns with gaps (panels the profile doesn't keep), and changes between set and add
    {
        auto order = [](const dashtiles::Board &b) {
            std::string o;
            for (const auto &c : b.Grid().Cols()) {
                o += "[";
                for (const auto &cell : c.cells) o += " " + std::to_string(cell.id);
                o += " ]";
            }
            return o;
        };
        auto line = [](int col, int row, int alone) {
            return "tile 0 " + std::to_string(col) + " " + std::to_string(row) + " " + std::to_string(alone) + " 1.000 1.000 0.00";
        };
        dashtiles::Board g;   // saved as [ 0 ][ 50 ][ 1 2 ]: 50 a floating window, not in the profile
        g.RestoreAll({{0, line(0, 0, 1)}, {1, line(2, 0, 0)}, {2, line(2, 1, 0)}});
        check("gaps: the stack kept, the columns closed up", order(g) == "[ 0 ][ 1 2 ]");
        dashtiles::Board r;
        for (int id : {14, 13, 12, 11, 10}) r.Anchor(id, -1, false);
        r.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        r.Restore(60, line(1, 0, 1), -1);   // an app back from its memory between the two
        r.RestoreAll({{13, line(3, 0, 1)}, {14, line(4, 0, 1)}}, true);
        check("set, an app, add: the profile's order, then the app", order(r) == "[ 10 ][ 11 ][ 12 ][ 13 ][ 14 ][ 60 ]");
        dashtiles::Board q;
        for (int id : {14, 13, 12, 11, 10, 70}) q.Anchor(id, -1, false);
        q.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        q.Unanchor(11);   // gone between the two
        q.RestoreAll({{13, line(3, 0, 1)}, {14, line(4, 0, 1)}}, true);
        check("set, a panel goes, add: the rest in order", order(q) == "[ 10 ][ 12 ][ 13 ][ 14 ][ 70 ]");
        // council pass 4: columns by their saved number, whatever order the lines come in
        dashtiles::Board o;
        for (int id : {2, 3}) o.Anchor(id, -1, false);
        o.RestoreAll({{3, line(1, 0, 1)}, {2, line(0, 0, 1)}});
        check("lines out of order: columns by number", order(o) == "[ 2 ][ 3 ]");
        // council pass 4: a tile moved after a set; an add then doesn't undo the move
        dashtiles::Board m;
        for (int id : {10, 11, 12, 20}) m.Anchor(id, -1, false);
        m.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        dashtiles::Info info;
        m.Press(12, dashtiles::Key::Left, info), m.Press(12, dashtiles::Key::Left, info);
        check("moved: [ 12 ][ 10 ][ 11 ]", order(m).rfind("[ 12 ][ 10 ][ 11 ]", 0) == 0);
        m.RestoreAll({{20, line(3, 0, 1)}}, true);
        check("an add after a move keeps the move, its tile after", order(m) == "[ 12 ][ 10 ][ 11 ][ 20 ]");
        // council pass 5: a stack listed before a lower column; per-panel forgetting; a key that changes nothing
        dashtiles::Board u;
        for (int id : {1, 2, 3}) u.Anchor(id, -1, false);
        u.RestoreAll({{2, line(1, 0, 0)}, {3, line(1, 1, 0)}, {1, line(0, 0, 1)}});
        check("a stack listed first: kept, columns by number", order(u) == "[ 1 ][ 2 3 ]");
        dashtiles::Board f;
        for (int id : {10, 11, 12, 20}) f.Anchor(id, -1, false);
        f.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        f.Unanchor(10), f.Anchor(10, 12, false);   // off and on again: now after 12
        f.SetStay(11, true);
        f.RestoreAll({{20, line(3, 0, 1)}}, true);
        check("an add after an unanchor keeps it where it went", order(f) == "[ 11 ][ 12 ][ 20 ][ 10 ]");
        check("an add after a stay change keeps it", f.Stay(11));
        dashtiles::Board n;
        for (int id : {10, 11, 12, 99}) n.Anchor(id, -1, false);
        n.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}});
        n.Press(10, dashtiles::Key::Left, info);   // (the first column: nothing moves)
        n.RestoreAll({{12, line(2, 0, 1)}}, true);
        check("a key that changes nothing keeps the set going", order(n).rfind("[ 10 ][ 11 ][ 12 ]", 0) == 0);
        dashtiles::Board e;
        for (int id : {1, 2, 50}) e.Anchor(id, -1, false);
        e.RestoreAll({{7, "nonsense"}});   // a set with no line it could take
        e.RestoreAll({{1, line(0, 0, 1)}, {2, line(1, 0, 1)}}, true);
        check("an add after an empty set: still the set's, first", order(e).rfind("[ 1 ][ 2 ]", 0) == 0);
        dashtiles::Board o2;
        for (int id : {10, 11, 30}) o2.Anchor(id, -1, false);
        o2.RestoreAll({{10, line(0, 0, 1)}, {30, "off"}});
        check("an off line: unanchored", !o2.Anchored(30));
        o2.Anchor(30, -1, false);
        o2.RestoreAll({{11, line(1, 0, 1)}}, true);
        check("anchored again after it, an add keeps it", o2.Anchored(30));
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
