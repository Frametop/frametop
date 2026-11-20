// The dashboard tiles' policy (screens/dashtiles.h): anchoring, tearing off and dropping back,
// the keys and a resize by hand sizing panels, profiles' grids, and what each app or display remembers.
#include "../dashtiles.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
static const double kI[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

int main() {
    // (16:9 desktops about as big round you as the sphere made them, 66 degrees: the splits' cases need a strip that wide)
    const auto desk = [](int) { return dashtiles::Info{3.5, 1.97}; };
    const auto info = [](int id) { return dashtiles::Info{id >= 10 ? 1.2 : 1.6, 0.9}; };
    // --- the group frame
    {
        double g[3][4];
        const double head[3] = {1, 1.6, 0}, ahead[3] = {0, 0, -1}, up[3] = {0, 1, 0};
        dashtiles::GroupFrame(head, ahead, up, g);
        check("group: at the head", near(g[0][3], 1) && near(g[1][3], 1.6) && near(g[2][3], 0));
        check("group: -Z the way the head faces", near(g[2][2], 1) && near(g[0][2], 0));
        check("group: level", near(g[1][1], 1) && near(g[1][0], 0) && near(g[1][2], 0));
        // the way the head faces, level, at any pitch, with or without roll: yaw 20 degrees right
        const double yaw = tiling::Rad(20);
        bool steady = true;
        for (double pitchDeg : {-89.0, -80.0, -76.0, -75.0, -40.0, 0.0, 40.0, 80.0, 89.0})
            for (double rollDeg : {0.0, 30.0, 60.0}) {
                const double pt = tiling::Rad(pitchDeg), rl = tiling::Rad(rollDeg);
                // the head's axes: yaw (about Y), then pitch (about X), then roll (about Z)
                const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pt), sp = std::sin(pt), cr = std::cos(rl), sr = std::sin(rl);
                // yaw Ry, pitch Rx, roll Rz: M = Ry * Rx * Rz; forward = M * (0,0,-1), up = M * (0,1,0)
                const double Ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
                const double Rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
                const double Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
                double A[3][3] = {}, M[3][3] = {};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k) A[i][j] += Ry[i][k] * Rx[k][j];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k) M[i][j] += A[i][k] * Rz[k][j];
                const double f[3] = {-M[0][2], -M[1][2], -M[2][2]}, u[3] = {M[0][1], M[1][1], M[2][1]};
                dashtiles::GroupFrame(head, f, u, g);
                const double fx = -g[0][2], fz = -g[2][2];   // the group's -Z
                const double want = std::atan2(-Ry[0][2], -Ry[2][2]);   // (where the yaw alone turns -Z)
                double err = std::fabs(std::atan2(fx, fz) - want);
                err = std::min(err, 2 * tiling::kPi - err);
                // exact within 75 degrees of level at any roll, and at any pitch without roll; past
                // 75 with roll, near (looking straight down, a rolled head's way is its up's)
                const double tol = std::fabs(pitchDeg) <= dashtiles::kUpFromDeg || rollDeg == 0 ? 1e-6 : tiling::Rad(rollDeg) + 1e-6;
                steady = steady && err <= tol && near(g[1][2], 0);
            }
        check("group: the head's way at every pitch to 89 degrees and roll to 60 (no turn to the bar)", steady);
        // and it turns smoothly with the pitch, rolled (council, 2026-11-16: it jumped 20 degrees at 60)
        double worst = 0, last = 0;
        for (double rollDeg : {10.0, 30.0, 60.0})
            for (int i = 0; i <= 1798; ++i) {
                const double pt = tiling::Rad(-89.9 + i * 0.1), rl = tiling::Rad(rollDeg);
                // pitch then roll, facing -Z: forward = Rx * Rz * (0,0,-1), up = Rx * Rz * (0,1,0)
                const double f[3] = {0, std::sin(pt), -std::cos(pt)};
                const double u[3] = {-std::sin(rl), std::cos(rl) * std::cos(pt), std::cos(rl) * std::sin(pt)};
                dashtiles::GroupFrame(head, f, u, g);
                const double way = tiling::Deg(std::atan2(-g[0][2], g[2][2]));   // (0: -Z)
                if (i > 0) worst = std::max(worst, std::fabs(tiling::WrapNear(way - last, 0)));
                last = way;
            }
        // (a rolled head's way goes from its forward's to its up's over the ease: steepest at a 60
        // degree roll, 1.6 degrees a tenth of a degree, against the 20 of the jump)
        check("group: no jump as the pitch goes (under 2 degrees a tenth of a degree)", worst < 2);
        const double nan = std::nan(""), bad[3] = {nan, 0, -1};
        dashtiles::GroupFrame(head, bad, up, g);
        check("group: a forward that isn't finite: -Z", near(g[2][2], 1) && near(g[0][2], 0));
        const double sideways[3] = {1, 0, 0}, down[3] = {0, -1, 0};   // rolled onto its side, looking ahead
        dashtiles::GroupFrame(head, ahead, sideways, g);
        check("group: a head on its side takes its forward", near(g[2][2], 1));
        dashtiles::GroupFrame(head, down, down, g);   // (not a head pose: no way at all)
        check("group: no way at all: -Z", near(g[2][2], 1));
    }
    // --- the wall at Steam's bar: facing it, a little behind it, the desk just over its top edge
    {
        // the bar as read on the Frame (2026-11-18): 1.1 m out, half a metre down, off to the right, tilted back
        const double head[3] = {0, 1.1, 0}, bar[3] = {0.286, 0.564, -0.992}, up[3] = {0, 0.767, -0.642};
        double g[3][4], r = 0, desk = 0;
        check("at the bar: placed", dashtiles::AtBar(head, bar, up, 0.0246, 0.8, g, &r, &desk));
        double az = 1, span = 0;
        dashtiles::SteamSlot(g, bar, 0.37, &az, &span);
        check("at the bar: the strip's middle over it", near(az, 0, 1e-9));
        check("at the bar: level, at the head", near(g[1][1], 1) && near(g[1][2], 0) && near(g[1][3], 1.1));
        const double barDist = std::sqrt(0.286 * 0.286 + 0.536 * 0.536 + 0.992 * 0.992);
        check("at the bar: the wall a little behind the bar", near(r, barDist + dashtiles::kBehindBarM) && r > barDist + 0.2);
        const double barEl = tiling::Deg(std::atan2(-0.536, std::hypot(0.286, 0.992)));
        check("at the bar: the desk just over it", desk > barEl + 0.8 && desk < barEl + 0.8 + 1.0);
        double noH = 0;
        dashtiles::AtBar(head, bar, up, 0, 0.8, g, &r, &noH);
        check("at the bar: no height known, its middle", near(noH, barEl + 0.8, 1e-9));
        const double close[3] = {0, 1.0, -0.25}, high[3] = {0, 3.5, -0.5};
        dashtiles::AtBar(head, close, up, 0, 0.8, g, &r, &desk);
        check("at the bar: no nearer than the wall's least", near(r, dashtiles::kWallMinM));
        dashtiles::AtBar(head, high, up, 0, 0.8, g, &r, &desk);
        check("at the bar: the desk no higher than its most", near(desk, dashtiles::kDeskMaxDeg));
        const double under[3] = {0.05, 0.3, -0.05}, nan[3] = {std::nan(""), 0.5, -1};
        r = 7, desk = 7;
        check("at the bar: straight under the head, none", !dashtiles::AtBar(head, under, up, 0, 0.8, g, &r, &desk) && r == 7);
        check("at the bar: not finite, none", !dashtiles::AtBar(head, nan, up, 0, 0.8, g, &r, &desk) && desk == 7);
    }
    // --- Steam's slot: wherever the menu is round you (the strip keeps it only when it reaches it)
    {
        double g[3][4];
        const double head[3] = {0, 1.6, 0}, ahead[3] = {0, 0, -1}, up[3] = {0, 1, 0};
        dashtiles::GroupFrame(head, ahead, up, g);
        double az = 99, span = 0;
        const double front[3] = {0, 1.0, -1.2};   // a Steam-button open: ahead, low
        check("slot: Steam's menu ahead is one", dashtiles::SteamSlot(g, front, 1.0, &az, &span) && near(az, 0) &&
                                                     near(span, 2 * tiling::Deg(std::atan2(0.5, 1.2))));
        const double palm[3] = {0.8, 1.05, 0.64};   // the palm menu's: low, right and behind
        check("slot: the palm menu's, off to the side, is one too", dashtiles::SteamSlot(g, palm, 1.0, &az, &span) && az > 90);
        const double right[3] = {0.6, 1.2, -1.2};
        check("slot: a little to the right, at its place", dashtiles::SteamSlot(g, right, 1.0, &az, &span) &&
                                                               near(az, tiling::Deg(std::atan2(0.6, 1.2))));
        check("slot: no width, none", !dashtiles::SteamSlot(g, front, 0, &az, &span));
        const double over[3] = {0.05, 2.5, 0};
        check("slot: right over the head, none", !dashtiles::SteamSlot(g, over, 1.0, &az, &span));
        const double nan = std::nan(""), lost[3] = {nan, 1, -1};
        check("slot: a bar that isn't finite, none", !dashtiles::SteamSlot(g, lost, 1.0, &az, &span));
        check("slot: a width that isn't finite, none", !dashtiles::SteamSlot(g, front, nan, &az, &span) &&
                                                           !dashtiles::SteamSlot(g, front, INFINITY, &az, &span));
        const double close[3] = {0, 1.2, -0.25};
        check("slot: a bar right in front of you, at most kMaxSlotDeg", dashtiles::SteamSlot(g, close, 1.2, &az, &span) &&
                                                                            near(span, dashtiles::kMaxSlotDeg));
    }
    // --- Steam's menu tile anchored left of Steam's window's
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::Default), b.Anchor(2, 1, dashtiles::Stay::Default), b.Anchor(3, 2, dashtiles::Stay::Default);
        b.Anchor(9, 1, dashtiles::Stay::Default, 2);
        const auto &cols = b.Grid().Cols();
        check("left of: a column of its own just left of it, with the focused one ignored",
              cols.size() == 4 && cols[0].cells[0].id == 1 && cols[1].cells[0].id == 9 && cols[2].cells[0].id == 2);
        b.Anchor(8, 1, dashtiles::Stay::Default, 77);
        check("left of one not tiled: as without it", b.Grid().Find(8).col == 1);
    }
    // --- moved, the strip stays where it was (user, 2026-11-18: a monitor moved from the right to the
    // left was centred and every other one pushed right, 30 degrees and more)
    {
        dashtiles::Board b;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default);
        b.Arrange(desk);
        const auto at = [&](int id) { const tiling::Placed *p = b.PlacedOf(id); return p ? p->azDeg : NAN; };
        const double w = at(2) - at(1), a1 = at(1), a2 = at(2);
        check("pan: three, centred at first", near(at(2), 0, 1e-9) && w > 20);
        const double left = a1 - w * 0.4;   // (on 1's left edge)
        b.Drop(3, b.DropFor(3, left, 0, desk), left);
        b.Arrange(desk);
        check("pan: right one to the left end: it's leftmost", at(3) < at(1) && at(1) < at(2));
        check("pan: ...the others stay put", near(at(1), a1, 1e-9) && near(at(2), a2, 1e-9));
        check("pan: ...it roughly where let go", std::fabs(at(3) - left) < w);
        const double b1 = at(1), b2 = at(2), b3 = at(3), mid = (b1 + b2) / 2;
        b.Drop(3, b.DropFor(3, mid, 0, desk), mid);   // (back between them)
        b.Arrange(desk);
        check("pan: dragged into the middle: it's there", at(1) < at(3) && at(3) < at(2));
        check("pan: ...its neighbours make room both ways", at(1) < b1 - w * 0.25 && at(2) > b2 + w * 0.25 && std::fabs(at(3) - mid) < w * 0.25);
        (void)b3;
        {   // (alone on the board, it goes where it was let go)
            dashtiles::Board one;
            one.Anchor(5, -1, dashtiles::Stay::Default);
            one.Arrange(desk);
            one.Drop(5, one.DropFor(5, 20, 0, desk), 20);
            one.Arrange(desk);
            const tiling::Placed *p5 = one.PlacedOf(5);
            check("pan: alone, moved: where let go", p5 && near(p5->azDeg, 20, 1e-9));
        }
        bool reach = true;
        for (int k = 0; k < 6; ++k) {   // (the left end dragged further left over and over)
            const tiling::Layout &l = b.Last();
            int end = -1;
            for (const tiling::Placed &p : l.tiles)
                if (p.az1 >= l.az1 - 1e-9) end = p.id;
            b.Drop(end, b.DropFor(end, l.az0 + 5, 0, desk), l.az0 + 5);
            const tiling::Layout &m = b.Arrange(desk);
            reach = reach && m.az0 <= 1e-9 && m.az1 >= -1e-9;
        }
        check("pan: moved and moved, the strip still reaches straight ahead", reach);
        b.Arrange(desk);
        const double steady = b.PanDeg();
        b.Arrange(desk);
        check("pan: arranged again unchanged: still", near(b.PanDeg(), steady, 1e-9));
        b.Recentre();
        const tiling::Layout &c = b.Arrange(desk);
        check("pan: recentred: the strip's middle straight ahead", near(c.az0, -c.az1, 1e-9));
        b.Recentre(1);
        b.Arrange(desk);
        check("pan: recentred on one: it straight ahead", near(at(1), 0, 1e-9));
        b.Press(2, dashtiles::Key::Right);
        b.Arrange(desk);
        check("pan: a key press: the strip still reaches straight ahead", b.Last().az0 <= 1e-9 && b.Last().az1 >= -1e-9);
    }
    // --- panels that come after the strip was laid out (back from a profile, or anchored): the strip
    // centred with them, not grown off to one side (testbench, 2026-11-19: one tile up when the
    // dashboard opened, two more after it, the strip -52..23)
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Arrange(desk);
        b.Drop(1, b.DropFor(1, 15, 0, desk), 15);   // (moved: off centre)
        b.Arrange(desk);
        b.Anchor(2, -1, dashtiles::Stay::Default);
        const tiling::Layout &a = b.Arrange(desk);
        check("late: an anchored one: the strip centred", near(a.az0, -a.az1, 1e-9));
        dashtiles::State st;
        dashtiles::Parse(b.Remember(2), &st);
        b.Restore(3, st, 2);
        const tiling::Layout &r = b.Arrange(desk);
        check("late: a restored one: the strip centred", near(r.az0, -r.az1, 1e-9) && b.PlacedOf(3));
        b.Drop(3, b.DropFor(3, r.az0 + 5, 0, desk), r.az0 + 5);
        const tiling::Layout &m = b.Arrange(desk);
        check("late: then moved: not centred again", !near(m.az0, -m.az1, 1e-3));
    }
    // --- a narrow panel flipping its lean doesn't move the other columns (testbench, 2026-11-18: one
    // flip took the column next to it 6 degrees)
    {
        const auto mixed = [](int id) { return id == 2 ? dashtiles::Info{1.4, 0.8} : dashtiles::Info{3.5, 1.97}; };
        dashtiles::Board b;
        for (const int i : {1, 3, 4}) b.Anchor(i, -1, dashtiles::Stay::Default);
        b.Anchor(2, -1, dashtiles::Stay::Default);
        b.Drop(2, {tiling::Drop::Into, 0, 1});   // (the narrow one under 1)
        const tiling::Layout &l0 = b.Arrange(mixed);
        const auto mid = [&](int id) { const tiling::Placed *p = b.PlacedOf(id); return p ? p->azDeg : NAN; };
        const auto off = [&](int id) { const tiling::Placed *p = b.PlacedOf(id), *w = b.PlacedOf(1); return p && w ? p->azDeg - w->azDeg : NAN; };
        const double lean0 = off(2), a3 = mid(3), a1 = mid(1);
        const double right = l0.az1 + 2;
        int first = -1;
        for (const tiling::Placed &p : l0.tiles)
            if (p.az0 <= l0.az0 + 1e-9) first = p.id;
        b.Drop(first, b.DropFor(first, right - 5, 0, mixed), right - 5);   // (the left end to the right end)
        b.Arrange(mixed);
        check("lean: the wide one moved from the left end: the narrow one alone now", first == 1 && b.PlacedOf(2)->col == 0);
        check("lean: ...the columns not touched stay put (not pulled by the narrow one's)", near(mid(3), a3, 1e-9));
        // (between a stacked column and a single one: both make room, the stacked one doesn't
        // outvote it; testbench, 2026-11-18: let go between them, it went 33 degrees off)
        dashtiles::Board c;
        for (const int i : {1, 3, 4}) c.Anchor(i, -1, dashtiles::Stay::Default);
        c.Anchor(2, -1, dashtiles::Stay::Default);
        c.Drop(2, {tiling::Drop::Into, 0, 1});
        c.Arrange(mixed);
        const auto cmid = [&](int id) { const tiling::Placed *p = c.PlacedOf(id); return p ? p->azDeg : NAN; };
        const double c1 = cmid(1), c3 = cmid(3), at = (c.PlacedOf(1)->az1 + c.PlacedOf(3)->az0) / 2;
        c.Drop(4, c.DropFor(4, at, 0, mixed), at);
        c.Arrange(mixed);
        check("columns: let go between two, it's there", cmid(1) < cmid(4) && cmid(4) < cmid(3));
        check("columns: ...both sides make room, it near where let go",
              cmid(1) < c1 - 10 && cmid(3) > c3 + 10 && std::fabs(cmid(4) - at) < 10);
        (void)lean0, (void)a1, (void)off;
    }
    // --- the slot's side stays put (council, 2026-11-16: it was the column count's middle, so an
    // anchor or a drop moved panels across Steam's menu)
    {
        dashtiles::Board b;
        b.params.reserveAzDeg = 0, b.params.reserveDeg = 66;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default);
        const auto side = [&](int id) {   // -1 left of the slot, +1 right
            const tiling::Placed *p = b.PlacedOf(id);
            return p ? (p->az1 <= -33 + 1e-9 ? -1 : p->az0 >= 33 - 1e-9 ? 1 : 0) : 9;
        };
        b.Arrange(desk);
        check("split: three round a slot in front: one left, two right, none in it",
              b.Grid().Split(0) == 1 && side(1) == -1 && side(2) == 1 && side(3) == 1);
        const double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        auto d = b.DropFor(3, -40, 0, desk);   // (on 1's right quarter, left of Steam's menu)
        b.Drop(3, d);
        b.Arrange(desk);
        check("split: let go left of the menu, it goes left", side(1) == -1 && side(3) == -1 && side(2) == 1);
        b.Anchor(4, 3, dashtiles::Stay::Default);
        b.Arrange(desk);
        check("split: anchored next to a left one, it goes left; the others stay", side(4) == -1 && side(2) == 1 && side(1) == -1);
        b.Unanchor(4);
        b.Arrange(desk);
        check("split: unanchored, the others stay", side(3) == -1 && side(2) == 1);
        check("split: Meta+Right steps across the menu", b.Press(3, dashtiles::Key::Right) && (b.Arrange(desk), side(3) == 1) &&
                                                             side(1) == -1 && side(2) == 1);
        d = b.DropFor(3, -5, 0, desk);   // (in the slot, left of its middle)
        b.Drop(3, d);
        b.Arrange(desk);
        check("split: let go in the slot left of its middle, it goes left", side(3) == -1 && side(2) == 1);
        b.SlotMoved();
        b.Arrange(desk);
        check("split: the dashboard opened again, the menu where it was: kept", b.Grid().Split(0) == 2 && side(3) == -1);
        bool steady = true;   // (council, 2026-11-16: a fresh choice flipped on the sign of a degree's head turn)
        for (double az : {0.3, -0.3, 1.0, -1.0, 4.0, -4.0}) {
            b.params.reserveAzDeg = az;
            b.SlotMoved();
            b.Arrange(desk);
            steady = steady && b.Grid().Split(0) == 2 && b.Split() == 2;
        }
        check("split: opened again a few degrees either way: kept", steady);
        b.params.reserveAzDeg = 110;   // (the palm menu's, after the Steam button's)
        b.SlotMoved();
        b.Arrange(desk);
        const auto clearOf = [&](double a0, double a1) {
            for (const auto &t : b.Last().tiles)
                if (t.az1 > a0 && t.az0 < a1) return false;
            return true;
        };
        check("split: opened with the palm menu on the right: its own split, the strip nearer in front",
              b.Grid().Split(1) == 3 && b.Split() == 3 && clearOf(77, 143) && std::fabs(b.Last().az0 + b.Last().az1) / 2 < 30);
        check("split: ...the front one kept", b.Grid().Split(0) == 2);
        b.Unanchor(1);
        b.Arrange(desk);
        check("split: a panel gone, the strip short of the menu now: the slot stays (no one crosses it)",
              b.Last().split == 2 && clearOf(77, 143));
        b.params.reserveAzDeg = 160;   // (behind you)
        b.SlotMoved(), b.Recentre();   // (as Regroup does)
        b.Arrange(desk);
        check("split: a menu the strip doesn't reach: no slot, centred", b.Last().split == -1 && near(b.Last().az0, -b.Last().az1));
        check("split: ...the splits kept for when it does (council, 2026-11-16)", b.Grid().Split(1) == 2 && b.Grid().Split(0) == 1);
        b.params.reserveAzDeg = 0;   // (the Steam button's again, after the palm menu's)
        b.SlotMoved();
        b.Arrange(desk);
        check("split: in front again: the front split, each panel on the side it was put", b.Split() == 1 && side(3) == -1 && side(2) == 1);
        b.Unanchor(2), b.Unanchor(3);
        check("split: all gone: no splits", b.Grid().Split(0) == -1 && b.Grid().Split(1) == -1);
        b.Anchor(7, 7, dashtiles::Stay::Default);
        b.Arrange(desk);
        const tiling::Placed *seven = b.PlacedOf(7);
        check("split: a new panel then: beside the menu in front, not past it", seven && std::fabs(seven->az0 + seven->az1) / 2 < 90 &&
                                                                                       (seven->az1 <= -33 + 1e-9 || seven->az0 >= 33 - 1e-9));
        const auto floating = [](int) { return dashtiles::Info{2.0, 1.25}; };
        // one panel alone, let go left of the menu: it goes left, and stays when let go on itself
        // (council, 2026-11-16: the drop's Remove emptied the grid and cleared the split)
        dashtiles::Board one;
        one.params.reserveAzDeg = 0, one.params.reserveDeg = 66;
        one.Anchor(1, -1, dashtiles::Stay::Default);
        one.Arrange(desk);
        const auto sideIn = [](const dashtiles::Board &x, int id) {
            const tiling::Placed *p = x.PlacedOf(id);
            return p ? (p->az1 <= -33 + 1e-9 ? -1 : p->az0 >= 33 - 1e-9 ? 1 : 0) : 9;
        };
        const bool rightFirst = sideIn(one, 1) == 1;
        one.Drop(1, one.DropFor(1, -20, 0, desk), -20);
        one.Arrange(desk);
        const bool wentLeft = sideIn(one, 1) == -1;
        const tiling::Placed *p1 = one.PlacedOf(1);
        one.Drop(1, one.DropFor(1, p1 ? (p1->az0 + p1->az1) / 2 : 0, 0, desk), p1 ? (p1->az0 + p1->az1) / 2 : 0);
        one.Arrange(desk);
        check("split: one panel let go left of the menu goes left, and stays on itself",
              rightFirst && wentLeft && sideIn(one, 1) == -1 && one.Grid().Split(0) == 1);
        // splits only chosen for you: chosen again when a fresh one is much nearer in front
        dashtiles::Board drift;
        drift.params.reserveAzDeg = 0, drift.params.reserveDeg = 66;
        drift.Anchor(1, -1, dashtiles::Stay::Default);
        drift.Arrange(desk);
        for (int i = 2; i <= 4; ++i) drift.Anchor(i, 1, dashtiles::Stay::Default), drift.Arrange(desk);   // (all next to 1: right of the menu)
        const bool lopsided = drift.Grid().Split(0) == 0 && slotsplit::Off(drift.Last()) > 90;
        drift.SlotMoved();
        drift.Arrange(desk);
        check("split: adds all went one side: the next open chooses again, the strip in front",
              lopsided && drift.Grid().Split(0) == 2 && slotsplit::Off(drift.Last()) < 1);
        // a reorder on one side doesn't make it yours (council, 2026-11-17)
        dashtiles::Board same;
        same.params.reserveAzDeg = 0, same.params.reserveDeg = 66;
        same.Anchor(1, -1, dashtiles::Stay::Default);
        same.Arrange(desk);
        for (int i = 2; i <= 4; ++i) same.Anchor(i, 1, dashtiles::Stay::Default), same.Arrange(desk);   // S [ 1 ][ 4 ][ 3 ][ 2 ]
        const bool swapped = same.Press(3, dashtiles::Key::Right);
        same.Arrange(desk);
        const tiling::Placed *p4 = same.PlacedOf(4);
        same.Drop(4, same.DropFor(4, p4 ? (p4->az0 + p4->az1) / 2 : 0, 0, desk), p4 ? (p4->az0 + p4->az1) / 2 : 0);   // (let go where it was)
        same.Arrange(desk);
        const bool stillLopsided = same.Grid().Split(0) == 0;
        same.SlotMoved();
        same.Arrange(desk);
        check("split: moves on one side of the menu: still chosen again on the next open",
              swapped && stillLopsided && same.Grid().Split(0) == 2 && slotsplit::Off(same.Last()) < 1);
        // where the menu happens to open doesn't flip a default split (council, 2026-11-17)
        dashtiles::Board flip;
        flip.params.reserveAzDeg = 0, flip.params.reserveDeg = 66;
        flip.Anchor(1, -1, dashtiles::Stay::Default);
        for (int i = 2; i <= 6; ++i) flip.Anchor(i, i - 1, dashtiles::Stay::Default);
        flip.Arrange(floating);
        const int first = flip.Grid().Split(0);
        bool flat = true;
        for (double az : {40.0, 0.0, -40.0, 0.0, 44.0, -44.0}) {
            flip.params.reserveAzDeg = az;
            flip.SlotMoved();
            flip.Arrange(floating);
            flat = flat && flip.Grid().Split(0) == first;
        }
        check("split: the menu opening 40 degrees either way: no panel crosses it", first >= 0 && flat);
        // torn off and docked again: a new panel, the split still only chosen for you
        dashtiles::Board dock;
        dock.params.reserveAzDeg = 0, dock.params.reserveDeg = 66;
        dock.Anchor(1, -1, dashtiles::Stay::Default);
        dock.Arrange(desk);
        for (int i = 2; i <= 4; ++i) dock.Anchor(i, 1, dashtiles::Stay::Default), dock.Arrange(desk);   // S [ 1 ][ 4 ][ 3 ][ 2 ]
        const tiling::Placed *p3 = dock.PlacedOf(3);
        const double at3 = p3 ? (p3->az0 + p3->az1) / 2 : 0;
        dock.TearOff(3, rel);
        dock.Arrange(desk);
        dock.Drop(3, dock.DropFor(3, at3 - 30, 0, desk), at3 - 30);   // (by its old left edge: a column of its own again)
        dock.Arrange(desk);
        const bool docked = dock.Grid().Has(3) && dock.Grid().Split(0) == 0;
        dock.SlotMoved();
        dock.Arrange(desk);
        check("split: torn off and docked again: still chosen again on the next open",
              docked && dock.Grid().Split(0) == 2 && slotsplit::Off(dock.Last()) < 30);   // (four the same: two each side)
        // a drop across the menu makes the split yours
        dashtiles::Board across;
        across.params.reserveAzDeg = 0, across.params.reserveDeg = 66;
        for (int i = 1; i <= 2; ++i) across.Anchor(i, i - 1, dashtiles::Stay::Default), across.Arrange(desk);   // S [ 1 ][ 2 ]
        across.Drop(2, across.DropFor(2, -40, 0, desk), -40);   // (left of the menu)
        across.Arrange(desk);
        const bool crossed = across.Grid().Split(0) == 1;   // [ 2 ] S [ 1 ]
        for (int i = 3; i <= 4; ++i) across.Anchor(i, 1, dashtiles::Stay::Default), across.Arrange(desk);   // (next to 1: right)
        const bool leans = slotsplit::Off(across.Last()) > 60;
        across.SlotMoved();
        across.Arrange(desk);
        check("split: a drop across the menu is yours: kept on the next open", crossed && leans && across.Grid().Split(0) == 1);
        // the first choice made with the menu off to one side: followed from the second open in front
        dashtiles::Board aside;
        aside.params.reserveAzDeg = 35, aside.params.reserveDeg = 66;
        aside.SlotMoved();
        for (int i = 1; i <= 4; ++i) aside.Anchor(i, i - 1, dashtiles::Stay::Default);
        aside.Arrange(floating);
        const int atSide = aside.Grid().Split(0);
        aside.params.reserveAzDeg = 0;
        aside.SlotMoved();
        aside.Arrange(floating);
        const bool firstKept = aside.Grid().Split(0) == atSide;
        aside.SlotMoved();
        aside.Arrange(floating);
        check("split: chosen with the menu aside, followed from the second open in front",
              firstKept && aside.Grid().Split(0) != atSide && slotsplit::Off(aside.Last()) < 15);
        // ...but not one you set
        drift.Press(4, dashtiles::Key::Right);   // ([ 1 ][ 4 ] S [ 3 ][ 2 ]: 4 steps across)
        drift.Arrange(desk);
        const bool pushed = drift.Grid().Split(0) == 1 && slotsplit::Off(drift.Last()) > 30 + 1;
        drift.SlotMoved();
        drift.Arrange(desk);
        check("split: one you set is kept on the next open", pushed && drift.Grid().Split(0) == 1);
        check("split keys: in front, right, left, wrapped", slotsplit::KeyOf(0) == 0 && slotsplit::KeyOf(44) == 0 &&
                                                               slotsplit::KeyOf(110) == 1 && slotsplit::KeyOf(-110) == 2 &&
                                                               slotsplit::KeyOf(370) == 0 && slotsplit::KeyOf(250) == 2);
        // anchored with nothing tiled in focus (the anchor button: the focus is the panel itself):
        // at the end that brings the strip's middle nearer in front of you, not always the right
        dashtiles::Board e;
        e.params.reserveAzDeg = 0, e.params.reserveDeg = 66;
        for (int i = 1; i <= 3; ++i) e.Anchor(i, i - 1, dashtiles::Stay::Default);
        e.Arrange(desk);   // [ 1 ] S [ 2 ][ 3 ]: pushed right
        e.Anchor(4, 4, dashtiles::Stay::Default);
        e.Arrange(desk);
        check("split: a new panel, nothing in focus: the left end when the strip leans right",
              e.Grid().Cols()[0].cells[0].id == 4 && e.Grid().Split(0) == 2 && near(e.Last().az0, -e.Last().az1));
        e.Anchor(5, 5, dashtiles::Stay::Default);
        e.Arrange(desk);
        check("split: ...and the right end when it doesn't", e.Grid().Cols().back().cells[0].id == 5 && e.Grid().Split(0) == 2);
    }
    // --- anchoring
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Anchor(2, 1, dashtiles::Stay::On);
        b.Anchor(3, 1, dashtiles::Stay::Default);
        check("anchor: a column right of the focused one", b.Grid().Cols().size() == 3 && b.Grid().Cols()[1].cells[0].id == 3);
        check("anchor: stay as asked", b.Stay(2) && !b.Stay(1));
        b.Anchor(2, -1, dashtiles::Stay::Default);
        check("anchor twice: once, stay kept", b.Grid().Cols().size() == 3 && b.Stay(2));
        check("unanchor", b.Unanchor(3) && !b.Anchored(3) && b.Grid().Cols().size() == 2 && !b.Unanchor(3));
        b.SetStay(9, true);
        check("stay on a panel not anchored: nothing", !b.Anchored(9));
    }
    // --- stay: as DASH_TILE_STAY says unless set for the panel (user, 2026-11-18: on by default)
    {
        dashtiles::Board b;
        b.stayDefault = true;
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Anchor(2, 1, dashtiles::Stay::Default);
        check("stay default: an anchored panel stays", b.Stay(1) && b.Stay(2));
        b.SetStay(2, false);
        check("stay default: set off for one, kept as 2", !b.Stay(2) && b.Remember(2).rfind("tile 2 ", 0) == 0);
        b.stayDefault = false;
        check("stay default: turned off, the one following it follows", !b.Stay(1) && b.Remember(1).rfind("tile 0 ", 0) == 0);
        b.stayDefault = true;
        b.SetStay(2, true);
        check("stay default: set back as the default is, it follows it again",
              b.Stay(2) && b.Remember(2).rfind("tile 0 ", 0) == 0 && (b.stayDefault = false, !b.Stay(2)));
        dashtiles::State st;
        check("stay: 0 the default, 1 on, 2 off, any other on (the old yes)",
              dashtiles::Parse("tile 0 0 0 1 1", &st) && st.stay == dashtiles::Stay::Default &&
                  dashtiles::Parse("tile 1 0 0 1 1", &st) && st.stay == dashtiles::Stay::On &&
                  dashtiles::Parse("tile 2 0 0 1 1", &st) && st.stay == dashtiles::Stay::Off &&
                  dashtiles::Parse("tile 7 0 0 1 1", &st) && st.stay == dashtiles::Stay::On);
        dashtiles::Board c;
        c.stayDefault = true;
        check("stay: restored off, it stays off with the default on", c.Restore(4, "tile 2 0 0 1 1", -1) && !c.Stay(4));
    }
    // --- tear off and back
    {
        dashtiles::Board b;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default);
        const auto &l = b.Arrange(desk);
        const double rel[3][4] = {{1, 0, 0, 0.5}, {0, 1, 0, 2}, {0, 0, 1, -1}};
        auto d = b.DropFor(2, l.az1 + 40, 20, desk);
        check("drop: far off: torn off", d.kind == tiling::Drop::TearOff);
        b.Drop(2, d);
        check("pulled off: back in its place, still on the grid", !b.Satellite(2) && b.Anchored(2) && b.Tiled(2));
        b.TearOff(2, rel);
        b.Drop(2, d);
        check("a satellite pulled off again: onto the grid", !b.Satellite(2) && b.Tiled(2));
        b.TearOff(2, rel);
        b.Arrange(desk);
        const tiling::Placed *one = b.PlacedOf(1);
        d = b.DropFor(2, one->azDeg, one->el1 - 0.5, desk);
        check("dropped on a tile's middle: into its column", d.kind == tiling::Drop::Into && d.col == 0 && d.row == 0);
        b.Drop(2, d);
        check("back on the grid: not a satellite", !b.Satellite(2) && b.Tiled(2) && b.Grid().Cols()[0].cells[0].id == 2);
        // dragged out of its own column, the indices are the grid's without it
        b.Arrange(desk);
        const tiling::Placed *three = b.PlacedOf(3);
        d = b.DropFor(1, three->az1 - 0.5, three->elDeg, desk);
        b.Drop(1, d);
        check("moved to the right end: a new column there", b.Grid().Cols().back().cells[0].id == 1 &&
                                                                 b.Grid().Cols()[0].cells.size() == 1);
    }
    // --- drops aim at the grid as shown (council, 2026-11-16: it used the grid re-centred without
    // the dragged one, so a drop on the middle column's middle made a new column)
    {
        dashtiles::Board b;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default);
        b.Arrange(desk);
        const tiling::Placed p2 = *b.PlacedOf(2);
        auto d = b.DropFor(3, p2.azDeg, p2.elDeg - 1, desk);
        check("drop: the right one on the middle one's middle: into it", d.kind == tiling::Drop::Into && d.col == 1);
        b.Drop(3, d);
        check("...stacked under 2", b.Grid().Cols().size() == 2 && b.Grid().Cols()[1].cells.size() == 2 &&
                                        b.Grid().Cols()[1].cells[0].id == 2 && b.Grid().Cols()[1].cells[1].id == 3);
        b.Arrange(desk);
        const tiling::Placed p1 = *b.PlacedOf(1);
        d = b.DropFor(1, p1.azDeg, p1.elDeg, desk);
        b.Drop(1, d);
        check("drop: onto itself: back where it was", b.Grid().Cols().size() == 2 && b.Grid().Cols()[0].cells[0].id == 1);
    }
    // --- the keys
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Anchor(10, 1, dashtiles::Stay::Default);
        b.Arrange(info);
        const double before = b.PlacedOf(10)->heightM / b.PlacedOf(10)->distM;   // (its size seen from you)
        check("bigger: Meta+Ctrl+Right", b.Press(10, dashtiles::Key::Wider) && near(b.Grid().CellOf(10)->scale, tilegrid::kStep));
        b.Arrange(info);
        check("...10% bigger, its shape kept", near(b.PlacedOf(10)->heightM / b.PlacedOf(10)->distM / before, tilegrid::kStep) &&
                                                   near(b.PlacedOf(10)->widthM / b.PlacedOf(10)->heightM, 1.2 / 0.9, 1e-6));
        check("bigger: Meta+Ctrl+Up too", b.Press(10, dashtiles::Key::Taller) && near(b.Grid().CellOf(10)->scale, 1.21));
        check("smaller: Meta+Ctrl+Left and Down", b.Press(10, dashtiles::Key::Narrower) && b.Press(10, dashtiles::Key::Shorter) &&
                                                     near(b.Grid().CellOf(10)->scale, 1));
        check("smaller: below natural size too", b.Press(1, dashtiles::Key::Narrower) && b.Grid().CellOf(1)->scale < 1);
        check("left: the columns swap", b.Press(10, dashtiles::Key::Left) && b.Grid().Cols()[0].cells[0].id == 10);
        check("down alone: nothing", !b.Press(10, dashtiles::Key::Down));
        const double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        b.TearOff(10, rel);
        check("a satellite: the keys do nothing", !b.Press(10, dashtiles::Key::Right));
        dashtiles::Key k;
        check("key names", dashtiles::ParseKey("shorter", &k) && k == dashtiles::Key::Shorter && !dashtiles::ParseKey("x", &k));
    }
    // --- a resize by hand
    {
        dashtiles::Board b;
        b.Anchor(10, -1, dashtiles::Stay::Default);
        b.Anchor(11, 10, dashtiles::Stay::Default);
        b.Drop(11, {tiling::Drop::Into, 0, 1});
        b.Arrange(info);
        const double h = b.PlacedOf(10)->heightM / b.PlacedOf(10)->distM;   // (its size seen from you)
        check("resized by hand: its size", b.Resized(10, 1.5) && near(b.Grid().CellOf(10)->scale, 1.5));
        b.Arrange(info);
        check("...1.5 times as big", near(b.PlacedOf(10)->heightM / b.PlacedOf(10)->distM / h, 1.5));
        check("...the one under it as it was", near(b.Grid().CellOf(11)->scale, 1));
        check("a size that isn't one: nothing", !b.Resized(10, 0) && !b.Resized(10, NAN));
    }
    // --- the desk full: a new panel goes on top of the lowest column
    {
        dashtiles::Board b;
        b.params.wallDeg = 100;
        const auto sized = [](int id) { return id == 2 ? dashtiles::Info{1.2, 1.6} : dashtiles::Info{1.6, 0.9}; };
        for (int i = 1; i <= 3; ++i) {
            const dashtiles::Info in = sized(i);
            b.Anchor(i, i - 1, dashtiles::Stay::Default, -1, &in), b.Arrange(sized);
        }
        check("desk: three side by side while there's room", b.Grid().Cols().size() == 3);
        const dashtiles::Info in = sized(4);
        b.Anchor(4, 3, dashtiles::Stay::Default, -1, &in);
        b.Arrange(sized);
        check("desk: full, the next one on top of the lowest column (not the tall middle one)",
              b.Grid().Cols().size() == 3 && b.Grid().Cols()[1].cells.size() == 1 &&
                  ((b.Grid().Cols()[0].cells.size() == 2 && b.Grid().Cols()[0].cells[0].id == 4) ||
                   (b.Grid().Cols()[2].cells.size() == 2 && b.Grid().Cols()[2].cells[0].id == 4)));
        const tiling::Placed *four = b.PlacedOf(4);
        const tiling::Placed *under = four ? b.PlacedOf(b.Grid().Cols()[size_t(four->col)].cells[1].id) : nullptr;
        check("desk: ...resting on it", four && under && near(four->el0, under->el1 + b.params.gapDeg));
        dashtiles::Board fresh;
        check("desk: a first panel: a column", (fresh.Anchor(1, -1, dashtiles::Stay::Default, -1, &in), fresh.Grid().Cols().size() == 1));
    }
    // --- the desk full: a narrow window doesn't go on a wide screen's column (user, 2026-11-19: "a
    // massive gap in the middle"), nor on one already as tall as a column gets
    {
        dashtiles::Board b;
        b.params.wallDeg = 70;   // (two screens fit, 67 degrees; a third column doesn't)
        const auto sized = [](int id) { return id <= 2 ? dashtiles::Info{1.6, 0.9} : dashtiles::Info{0.3, 0.53}; };
        for (int i = 1; i <= 2; ++i) {
            const dashtiles::Info in = sized(i);
            b.Anchor(i, i - 1, dashtiles::Stay::Default, -1, &in), b.Arrange(sized);
        }
        const dashtiles::Info narrow = sized(3);
        b.Anchor(3, 2, dashtiles::Stay::Default, -1, &narrow), b.Arrange(sized);
        const auto alone = [&](int id) {
            const tilegrid::Where w = b.Grid().Find(id);
            return w.ok() && b.Grid().Cols()[size_t(w.col)].cells.size() == 1;
        };
        check("desk full: a narrow window gets a column of its own, not the screens'",
              b.Grid().Cols().size() == 3 && alone(1) && alone(2) && alone(3));
        for (int i = 4; i <= 12; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default, -1, &narrow), b.Arrange(sized);
        size_t most = 0;
        for (const auto &c : b.Grid().Cols()) most = std::max(most, c.cells.size());
        check("desk full: ten narrow windows stack with each other, no column past its height",
              alone(1) && alone(2) && most >= 2 && most <= 4);
    }
    // --- a reset packs them again: like widths together, the widest at the bottom and in the middle
    {
        using tilepack::Item;
        const auto cols = tilepack::Pack({{1, 5, 9}, {2, 26, 11}, {3, 5, 9}, {4, 20, 12}, {5, 6, 9}, {6, 15, 9}, {7, 5, 9},
                                          {8, 5, 5}},
                                         0.8);
        const auto colOf = [&](int id) {
            for (size_t c = 0; c < cols.size(); ++c)
                for (int x : cols[c])
                    if (x == id) return int(c);
            return -1;
        };
        bool all = true;
        for (int id = 1; id <= 8; ++id) all &= colOf(id) >= 0;
        check("pack: every one in a column", all);
        check("pack: the two screens together, the widest at the bottom", colOf(2) == colOf(4) && cols[size_t(colOf(2))][0] == 2);
        check("pack: no narrow window with them", cols[size_t(colOf(2))].size() == 2);
        check("pack: the narrow ones together, at most kColumnDeg tall",
              colOf(1) == colOf(3) && colOf(1) != colOf(2) && cols[size_t(colOf(1))].size() <= 5);
        check("pack: the widest column in the middle", colOf(2) == int(cols.size() / 2) || colOf(2) == int((cols.size() - 1) / 2));
        check("pack: a panel taller than a column: alone", tilepack::Pack({{1, 10, 50}, {2, 10, 5}}, 0.8).size() == 2);
        check("pack: nothing", tilepack::Pack({}, 0.8).empty());

        dashtiles::Board b;
        const auto sized = [](int id) { return id == 1 ? dashtiles::Info{1.48, 0.62} : dashtiles::Info{0.3, 0.53}; };
        for (int i = 1; i <= 6; ++i) b.Anchor(i, -1, dashtiles::Stay::Default);
        for (int i = 2; i <= 6; ++i) b.Drop(i, {tiling::Drop::Into, 0, 0});   // (all in one column with the screen)
        b.Repack(sized);
        b.Arrange(sized);
        const tilegrid::Where screen = b.Grid().Find(1);
        check("repack: the screen alone in the middle, the windows in columns of their own either side",
              b.Grid().Cols().size() == 3 && screen.col == 1 && b.Grid().Cols()[1].cells.size() == 1 &&
                  b.Grid().Cols()[0].cells.size() + b.Grid().Cols()[2].cells.size() == 5);
        check("repack: remembered as packed", b.Remember(1).rfind("tile 0 ", 0) == 0);

        // A hidden panel (absent) at no size: its neighbours close up; a repack still packs it at its own.
        const auto span = [&] {
            const tiling::Layout &l = b.Arrange(sized);
            return l.az1 - l.az0;
        };
        const double whole = span();
        b.absent = {1};
        const double without = span();
        check("absent: the screen hidden, the strip closes up its width", whole - without > 25);
        b.Repack(sized);
        check("absent: a repack keeps its place in the middle", b.Grid().Find(1).col == 1);
        b.absent.clear();
        check("absent: shown again, back at its size", std::fabs(span() - whole) < 1e-6);
    }
    // --- memory
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::On);
        b.Anchor(2, 1, dashtiles::Stay::Default);
        b.Anchor(3, 2, dashtiles::Stay::Default);
        b.Drop(3, {tiling::Drop::Into, 1, 1});
        const std::string one = b.Remember(1), three = b.Remember(3);
        check("remember: a tile", one.rfind("tile 1 0 0 1 ", 0) == 0);
        check("remember: under another", three.rfind("tile 0 1 1 0 ", 0) == 0);
        check("remember: not anchored", b.Remember(7) == "off");
        const double rel[3][4] = {{1, 0, 0, 0.25}, {0, 1, 0, 0.5}, {0, 0, 1, -2}};
        b.TearOff(2, rel);
        const std::string sat = b.Remember(2);
        check("remember: a satellite", sat.rfind("sat 0 ", 0) == 0);
        dashtiles::Board c;
        c.Anchor(5, -1, dashtiles::Stay::Default);
        check("restore: a tile in a column of its own", c.Restore(1, one, 5) && c.Tiled(1) && c.Stay(1) &&
                                                            c.Grid().Cols().size() == 2 && c.Grid().Cols()[0].cells[0].id == 1);
        check("restore: under another", c.Restore(3, three, 5) && c.Grid().Cols()[1].cells.size() == 2 &&
                                            c.Grid().Cols()[1].cells[1].id == 3);
        check("restore: a satellite goes on the grid", c.Restore(2, sat, 5) && !c.Satellite(2) && c.Tiled(2));
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
        b.Anchor(199, -1, dashtiles::Stay::On);   // a gamescope app, the only tile
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
        b.Anchor(199, -1, dashtiles::Stay::On);
        auto line = [](int col, int row, int alone, double scale) {
            char t[96];
            std::snprintf(t, sizeof t, "tile 0 %d %d %d %.3f", col, row, alone, scale);
            return std::string(t);
        };
        b.RestoreAll({{10, line(0, 0, 1, 1)}, {11, line(1, 0, 1, 1)}, {12, line(2, 0, 0, 1.5)}});
        b.RestoreAll({{13, line(2, 1, 0, 1)}, {14, line(3, 0, 1, 1)}}, true);
        std::string order;
        for (const auto &c : b.Grid().Cols()) {
            order += "[";
            for (const auto &cell : c.cells) order += " " + std::to_string(cell.id);
            order += " ]";
        }
        check("split restore: columns in the profile's order, the app after them",
              order == "[ 10 ][ 11 ][ 12 13 ][ 14 ][ 199 ]");
        check("split restore: a stacked panel keeps its size", near(b.Grid().CellOf(12)->scale, 1.5));
        b.RestoreAll({{14, line(0, 0, 1, 1)}});   // a set again: a new profile, its columns first
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
        for (int id : {14, 13, 12, 11, 10}) r.Anchor(id, -1, dashtiles::Stay::Default);
        r.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        r.Restore(60, line(1, 0, 1), -1);   // an app back from its memory between the two
        r.RestoreAll({{13, line(3, 0, 1)}, {14, line(4, 0, 1)}}, true);
        check("set, an app, add: the profile's order, then the app", order(r) == "[ 10 ][ 11 ][ 12 ][ 13 ][ 14 ][ 60 ]");
        dashtiles::Board q;
        for (int id : {14, 13, 12, 11, 10, 70}) q.Anchor(id, -1, dashtiles::Stay::Default);
        q.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        q.Unanchor(11);   // gone between the two
        q.RestoreAll({{13, line(3, 0, 1)}, {14, line(4, 0, 1)}}, true);
        check("set, a panel goes, add: the rest in order", order(q) == "[ 10 ][ 12 ][ 13 ][ 14 ][ 70 ]");
        // council pass 4: columns by their saved number, whatever order the lines come in
        dashtiles::Board o;
        for (int id : {2, 3}) o.Anchor(id, -1, dashtiles::Stay::Default);
        o.RestoreAll({{3, line(1, 0, 1)}, {2, line(0, 0, 1)}});
        check("lines out of order: columns by number", order(o) == "[ 2 ][ 3 ]");
        // council pass 4: a tile moved after a set; an add then doesn't undo the move
        dashtiles::Board m;
        for (int id : {10, 11, 12, 20}) m.Anchor(id, -1, dashtiles::Stay::Default);
        m.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        m.Press(12, dashtiles::Key::Left), m.Press(12, dashtiles::Key::Left);
        check("moved: [ 12 ][ 10 ][ 11 ]", order(m).rfind("[ 12 ][ 10 ][ 11 ]", 0) == 0);
        m.RestoreAll({{20, line(3, 0, 1)}}, true);
        check("an add after a move keeps the move, its tile after", order(m) == "[ 12 ][ 10 ][ 11 ][ 20 ]");
        // council pass 5: a stack listed before a lower column; per-panel forgetting; a key that changes nothing
        dashtiles::Board u;
        for (int id : {1, 2, 3}) u.Anchor(id, -1, dashtiles::Stay::Default);
        u.RestoreAll({{2, line(1, 0, 0)}, {3, line(1, 1, 0)}, {1, line(0, 0, 1)}});
        check("a stack listed first: kept, columns by number", order(u) == "[ 1 ][ 2 3 ]");
        dashtiles::Board f;
        for (int id : {10, 11, 12, 20}) f.Anchor(id, -1, dashtiles::Stay::Default);
        f.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}, {12, line(2, 0, 1)}});
        f.Unanchor(10), f.Anchor(10, 12, dashtiles::Stay::Default);   // off and on again: now after 12
        f.SetStay(11, true);
        f.RestoreAll({{20, line(3, 0, 1)}}, true);
        check("an add after an unanchor keeps it where it went", order(f) == "[ 11 ][ 12 ][ 20 ][ 10 ]");
        check("an add after a stay change keeps it", f.Stay(11));
        dashtiles::Board n;
        for (int id : {10, 11, 12, 99}) n.Anchor(id, -1, dashtiles::Stay::Default);
        n.RestoreAll({{10, line(0, 0, 1)}, {11, line(1, 0, 1)}});
        n.Press(10, dashtiles::Key::Left);   // (the first column: nothing moves)
        n.RestoreAll({{12, line(2, 0, 1)}}, true);
        check("a key that changes nothing keeps the set going", order(n).rfind("[ 10 ][ 11 ][ 12 ]", 0) == 0);
        dashtiles::Board e;
        for (int id : {1, 2, 50}) e.Anchor(id, -1, dashtiles::Stay::Default);
        e.RestoreAll({{7, "nonsense"}});   // a set with no line it could take
        e.RestoreAll({{1, line(0, 0, 1)}, {2, line(1, 0, 1)}}, true);
        check("an add after an empty set: still the set's, first", order(e).rfind("[ 1 ][ 2 ]", 0) == 0);
        dashtiles::Board o2;
        for (int id : {10, 11, 30}) o2.Anchor(id, -1, dashtiles::Stay::Default);
        o2.RestoreAll({{10, line(0, 0, 1)}, {30, "off"}});
        check("an off line: unanchored", !o2.Anchored(30));
        o2.Anchor(30, -1, dashtiles::Stay::Default);
        o2.RestoreAll({{11, line(1, 0, 1)}}, true);
        check("anchored again after it, an add keeps it", o2.Anchored(30));
    }
    // --- follow-ups: the state as a record (any whitespace, junk refused), one "placed" rule, a set ended
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
        dashtiles::State s;
        check("state: tabs and doubled spaces parse", dashtiles::Parse(" tile\t1  2 0 1   1.5 \n", &s) &&
                                                          s.kind == dashtiles::State::Kind::Tile && s.stay == dashtiles::Stay::On && s.col == 2 &&
                                                          s.row == 0 && s.alone && near(s.scale, 1.5));
        check("state: text round trip", dashtiles::Text(s) == "tile 1 2 0 1 1.500");
        check("state: the sphere's older form: its scale the size, its weight and width dropped",
              dashtiles::Parse("tile 0 3 1 0 2.5 0.8 40.00", &s) && s.col == 3 && s.row == 1 && !s.alone && near(s.scale, 0.8) &&
                  dashtiles::Text(s) == "tile 0 3 1 0 0.800");
        bool junk = false;
        for (const char *bad : {"tile 0 1 0 1 1 1 0 x", "tile 0 a 0 1 1 1 0", "tile 0 1 0 1 nan 1 0", "sat 1", "off off",
                                "tile 0 1 0 1 1 1", "", "tile 0 1.5 0 1 1 1 0", "sat 1 1 0 0 0 0 1 0 0 0 0 1 inf"})
            junk |= dashtiles::Parse(bad, &s);
        check("state: junk refused", !junk);
        check("state: room parses, off with room set", dashtiles::Parse(" room\n", &s) && s.kind == dashtiles::State::Kind::Off && s.room &&
                                                           dashtiles::Text(s) == "room");
        check("state: off isn't room", dashtiles::Parse("off", &s) && !s.room && dashtiles::Text(s) == "off" && !dashtiles::Parse("room 1", &s));
        {
            using W = dashtiles::WhenKnown;
            using dashtiles::TileWhenKnown;
            check("new window: tiled", TileWhenKnown("app:org.kde.dolphin", "") == W::Anchor &&
                                           TileWhenKnown("remote:10.0.0.5/app:code", "") == W::Anchor &&
                                           TileWhenKnown("remote:10.0.0.5/window:0x55aa", "") == W::Anchor);
            check("a window unanchored before (not in the room on purpose): tiled", TileWhenKnown("app:firefox", "off") == W::Anchor);
            check("a window put in the room: stays there", TileWhenKnown("app:firefox", "room") == W::Leave &&
                                                              TileWhenKnown("remote:h/app:code", "room") == W::Leave);
            check("a window's tile: back as it was", TileWhenKnown("app:firefox", "tile 0 1 0 1 1") == W::Restore &&
                                                         TileWhenKnown("app:firefox", "sat 0 1 0 0 0 0 1 0 0 0 0 1 0") == W::Restore);
            check("screens and remote displays: as before", TileWhenKnown("screen:2", "") == W::Leave &&
                                                                TileWhenKnown("screen:2", "off") == W::Restore &&
                                                                TileWhenKnown("remote:10.0.0.5/display:DP-1", "") == W::Leave &&
                                                                TileWhenKnown("steam:dashboard", "") == W::Leave);
            check("a junk line: Restore (which says it isn't one)", TileWhenKnown("screen:1", "junk") == W::Restore &&
                                                                       TileWhenKnown("app:x", "junk") == W::Anchor);
        }
        check("state: a satellite parses", dashtiles::Parse("sat 0 1 0 0 0.5 0 1 0 0 0 0 1 -2", &s) &&
                                               s.kind == dashtiles::State::Kind::Sat && near(s.rel[0][3], 0.5) &&
                                               near(s.rel[2][3], -2));
        // (council pass 7: a satellite of huge numbers overran Text's buffer, from a file or a profile)
        std::string huge = "sat 1";
        for (int k = 0; k < 12; ++k) huge += " 1e300";
        check("state: numbers past 1e6 refused", !dashtiles::Parse(huge, &s) && !dashtiles::Parse("tile 0 1 0 1 2e6 1 0", &s));
        dashtiles::State big;
        big.kind = dashtiles::State::Kind::Sat;
        for (auto &row : big.rel)
            for (double &v : row) v = -1e308;
        const std::string bigText = dashtiles::Text(big);
        check("state: any state's text is whole", bigText.size() > 12 * 300 && bigText.rfind("sat 0 -1", 0) == 0);
        dashtiles::Board hb;
        hb.Anchor(5, -1, dashtiles::Stay::Default);
        check("state: a huge satellite from a profile is refused", !hb.Restore(5, huge, -1));
        dashtiles::Board w;
        for (int id : {10, 11, 20}) w.Anchor(id, -1, dashtiles::Stay::Default);
        w.RestoreAll({{10, "tile  0   0 0 1 1 1 0"}, {11, "tile\t0\t1\t0\t1\t1\t1\t0"}});
        w.SetStay(10, true);
        w.RestoreAll({{20, "tile 0 2 0 1 1 1 0"}}, true);
        check("odd whitespace: a stay change before an add keeps the tile on the grid",
              w.Stay(10) && order(w).rfind("[ 10 ][ 11 ][ 20 ]", 0) == 0);
        dashtiles::Board r;
        for (int id : {10, 11, 12}) r.Anchor(id, -1, dashtiles::Stay::Default);
        r.RestoreAll({{10, "tile 0 0 0 1 1 1 0"}, {11, "tile 0 5 -1 1 1 1 0"}, {12, "tile 0 6 0 1 1 1 0"}});
        check("a tile with no row: no column kept for it, put where a new one goes", order(r) == "[ 10 ][ 12 ][ 11 ]");
        dashtiles::Board t;
        for (int id : {10, 11, 50}) t.Anchor(id, -1, dashtiles::Stay::Default);
        t.RestoreAll({{10, "tile 0 0 0 1 1 1 0"}});
        t.EndRestore();   // (vrtiles.cpp: an add long after the set)
        t.RestoreAll({{11, "tile 0 1 0 1 1 1 0"}}, true);
        check("an add after the set ended goes after the others", order(t) == "[ 10 ][ 50 ][ 11 ]");
    }
    {   // the memory's file: one write a moment after a burst of changes, and a flush on the way out
        dashtiles::SaveTimer s;
        check("nothing changed: never due", !s.Due(0) && !s.Due(100000) && !s.Flush());
        s.Changed(1000);
        s.Changed(1500);
        s.Changed(2900);   // (a burst: still due 2 s after the first)
        check("not due before the delay", !s.Due(2999));
        check("due once after it", s.Due(3000) && !s.Due(3001) && !s.Due(9000));
        s.Changed(10000);
        check("a change after the write: due again later", !s.Due(11000) && s.Due(12000));
        s.Changed(20000);
        check("shutting down: flushed once", s.Flush() && !s.Flush() && !s.Due(30000));
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
