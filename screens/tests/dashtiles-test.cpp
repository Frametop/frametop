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
    // --- a profile's grid
    {
        dashtiles::Board b;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, false);
        const std::string text = "col 30.00 3:1.000 1:2.000; col 0.00 9:1.000";
        check("grid set: taken", b.SetGrid(text));
        check("...9 isn't anchored: left out; 2 left out of it: a column at the end",
              b.Grid().Cols().size() == 2 && b.Grid().Cols()[0].cells.size() == 2 && b.Grid().Cols()[1].cells[0].id == 2);
        check("grid set: garbage refused, grid kept", !b.SetGrid("nonsense") && b.Grid().Cols().size() == 2);
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
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
