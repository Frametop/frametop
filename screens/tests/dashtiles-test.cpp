// The dashboard tiles' policy (screens/dashtiles.h): anchoring, tearing off and dropping back,
// the keys and a resize by hand sizing panels, profiles' grids, and what each app or display remembers.
#include "../dashtiles.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // left was centred and every other one pushed right, 30 degrees and more); recentred (the
    // dashboard opening), the centre display straight ahead (user, 2026-11-23)
    {
        dashtiles::Board b;
        for (int i = 1; i <= 3; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default);
        b.Arrange(desk);
        const auto at = [&](int id) { const tiling::Placed *p = b.PlacedOf(id); return p ? p->azDeg : NAN; };
        const double w = at(2) - at(1), a1 = at(1), a2 = at(2);
        check("pan: three as wide: the middle one the centre display, straight ahead at first",
              b.Centre() == 2 && near(at(2), 0, 1e-9) && w > 20);
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
        {   // the only one showing hides (Steam's page with the dashboard closed) and comes back: where it was
            dashtiles::Board d;
            d.Anchor(1, -1, dashtiles::Stay::Default), d.Anchor(2, -1, dashtiles::Stay::Default);
            d.absent = {1};
            d.Arrange(desk), d.Recentre(2), d.Arrange(desk);
            const double was = d.PlacedOf(2)->azDeg;
            d.absent = {1, 2}, d.Arrange(desk);
            d.absent = {1}, d.Arrange(desk);
            check("pan: the only one showing hid and came back: where it was", near(d.PlacedOf(2)->azDeg, was, 1e-6));
        }
        {   // ...and turned aside: an empty strip has no room for its turn, and it's back with it all the same
            dashtiles::Board d;
            d.Anchor(5, -1, dashtiles::Stay::Default);
            d.Arrange(desk);
            d.Drop(5, d.DropFor(5, 20, 0, desk), 20);
            d.Arrange(desk);
            d.absent = {5}, d.Arrange(desk);
            d.absent = {}, d.Arrange(desk);
            check("pan: alone, it hid and came back: where it was let go", near(d.PlacedOf(5)->azDeg, 20, 1e-6));
            d.Recentre(), d.Arrange(desk);
            d.absent = {5}, d.Arrange(desk);
            d.absent = {}, d.Arrange(desk);
            check("pan: ...recentred since: where that has it", near(d.PlacedOf(5)->azDeg, 0, 1e-6));
        }
        for (const int n : {2, 4}) {   // Steam's page or a stream hides and comes back over and over: nothing creeps round
            dashtiles::Board d;
            for (int k = 1; k <= n; ++k) d.Anchor(k, k - 1, dashtiles::Stay::Default);
            d.Arrange(desk);
            const double at1 = d.PlacedOf(1)->azDeg;
            double worst = 0;
            for (int k = 0; k < 6; ++k) {
                d.absent = {2}, d.Arrange(desk);
                d.absent = {}, d.Arrange(desk);
                worst = std::max(worst, std::fabs(d.PlacedOf(1)->azDeg - at1));
            }
            check("pan: hidden and shown again six times: nothing creeps round", worst < 1e-6);
        }
        {   // centred while it was fitted: the turns from before that don't come back
            dashtiles::Board d;
            for (int k = 1; k <= 3; ++k) d.Anchor(k, k - 1, dashtiles::Stay::Default);
            d.Arrange(desk);
            d.Recentre(3), d.Arrange(desk);   // (the right one straight ahead; fitted, the left one is)
            const double turned = d.PanDeg();
            d.params.fit = true, d.Recentre(), d.Arrange(desk);
            d.params.fit = false, d.Arrange(desk);
            check("pan: centred while fitted: not as it was turned before", std::fabs(turned) > 5 && std::fabs(d.PanDeg() - turned) > 5);
        }
        {   // they come one at a time (streams connecting): as they'd be had they come at once
            dashtiles::Board d;
            for (int k = 1; k <= 3; ++k) d.Anchor(k, k - 1, dashtiles::Stay::Default);
            d.absent = {1, 2, 3}, d.Arrange(desk);
            d.absent = {2, 3}, d.Arrange(desk);
            d.absent = {3}, d.Arrange(desk);
            d.absent = {}, d.Arrange(desk);
            check("pan: shown one at a time: centred", near(d.PlacedOf(2)->azDeg, 0, 1.5));
        }
        b.Recentre();
        b.Arrange(desk);
        check("pan: recentred: the centre display straight ahead (moves left it off to one side)",
              b.Centre() >= 0 && near(at(b.Centre()), 0, 1e-9));
        b.Recentre(1);
        b.Arrange(desk);
        check("pan: recentred on one: it straight ahead", near(at(1), 0, 1e-9));
        b.Press(2, dashtiles::Key::Right);
        b.Arrange(desk);
        check("pan: a key press: the strip still reaches straight ahead", b.Last().az0 <= 1e-9 && b.Last().az1 >= -1e-9);
    }
    // --- panels that come after the strip was laid out (back from a profile, or anchored): the strip
    // recentred with them on the centre display, not grown off to one side (testbench, 2026-11-19:
    // one tile up when the dashboard opened, two more after it, the strip -52..23)
    {
        dashtiles::Board b;
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Arrange(desk);
        b.Drop(1, b.DropFor(1, 15, 0, desk), 15);   // (moved: off centre)
        b.Arrange(desk);
        b.Anchor(2, -1, dashtiles::Stay::Default);
        b.Arrange(desk);
        check("late: an anchored one: the centre display straight ahead (two as wide: not the seam)",
              b.Centre() >= 0 && near(b.PlacedOf(b.Centre())->azDeg, 0, 1e-9));
        dashtiles::State st;
        dashtiles::Parse(b.Remember(2), &st);
        b.Restore(3, st, 2);
        const tiling::Layout &r = b.Arrange(desk);
        check("late: a restored one: three as wide, the middle one the centre, straight ahead",
              b.PlacedOf(3) && b.Centre() == b.Grid().Cols()[1].cells[0].id && near(b.PlacedOf(b.Centre())->azDeg, 0, 1e-9));
        const int centre = b.Centre();
        b.Drop(3, b.DropFor(3, r.az0 + 5, 0, desk), r.az0 + 5);
        b.Arrange(desk);
        check("late: then moved: not pulled back straight ahead (the centre where the move left it)",
              b.Centre() >= 0 && !near(b.PlacedOf(b.Centre())->azDeg, 0, 1e-3) && b.Grid().Has(centre));
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
        check("split: a menu the strip doesn't reach: no slot, the centre display straight ahead",
              b.Last().split == -1 && b.Centre() >= 0 && near(b.PlacedOf(b.Centre())->azDeg, 0, 1e-6));
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
        check("down alone: it hangs under the next column", b.Press(10, dashtiles::Key::Down) && b.Grid().Cols()[0].cells.back().id == 10);
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
    // --- the desk full: a narrow window goes over a wide screen, beside others on a shelf as wide as
    // the screen (user, 2026-11-19: "a column is defined by its widest screen"), no column past
    // kColumnDeg tall
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
        const int c3 = b.Grid().Find(3).col;
        check("desk full: a narrow window on top of a screen", b.Grid().Cols().size() == 2 && (c3 == b.Grid().Find(1).col || c3 == b.Grid().Find(2).col));
        for (int i = 4; i <= 12; ++i) b.Anchor(i, i - 1, dashtiles::Stay::Default, -1, &narrow), b.Arrange(sized);
        const tiling::Layout &l = b.Arrange(sized);
        double tallest = 0;
        int beside = 0;
        for (const tiling::Placed &p : l.tiles) {
            tallest = std::max(tallest, p.el1 - b.params.deskDeg);
            for (const tiling::Placed &q : l.tiles) beside += p.id < q.id && p.col == q.col && near(p.el0, q.el0);
        }
        check("desk full: ten narrow windows share shelves over the screens", beside >= 6);
        check("desk full: ...two shelves of four over each screen, no column past its height",
              tallest <= tilepack::kColumnDeg + 1e-6 && b.Grid().Cols().size() == 2);
    }
    // --- a reset packs them again: widest first, narrower ones on shelves over wider ones, the widest
    // column in the middle
    {
        using tilepack::Item;
        const auto cols = tilepack::Pack({{1, 5, 9}, {2, 26, 11}, {3, 5, 9}, {4, 20, 12}, {5, 6, 9}, {6, 15, 9}, {7, 5, 9},
                                          {8, 5, 5}},
                                         0.8);
        check("pack: all in one column, widest first", cols == std::vector<std::vector<int>>{{2, 4, 6, 5, 1, 3, 7, 8}});
        check("pack: ...on shelves as wide as the widest",
              tilepack::Shelves({26, 20, 15, 6, 5, 5, 5, 5}, 0.8) == std::vector<int>{0, 1, 2, 2, 3, 3, 3, 3});
        // (the user's: a window 1,200 pixels across, two of 500 over it; 48 pixels a degree)
        check("pack: two 500 px windows on a shelf over a 1,200 px one",
              tilepack::Pack({{2, 500 / 48.0, 14}, {1, 1200 / 48.0, 14}, {3, 500 / 48.0, 14}}, 0.8) ==
                      std::vector<std::vector<int>>{{1, 2, 3}} &&
                  tilepack::Shelves({1200 / 48.0, 500 / 48.0, 500 / 48.0}, 0.8) == std::vector<int>{0, 1, 1});
        const auto two = tilepack::Pack({{1, 30, 14}, {2, 20, 14}, {3, 20, 14}, {4, 20, 14}}, 0.8);
        check("pack: a full column, the next one beside it", two.size() == 2 && two[0] == std::vector<int>{1, 2, 3} && two[1] == std::vector<int>{4});
        check("pack: a panel taller than a column: alone", tilepack::Pack({{1, 10, 60}, {2, 10, 5}}, 0.8).size() == 2);
        check("pack: one not shown (no size): a column of its own", tilepack::Pack({{1, 30, 14}, {2, 0, 0}, {3, 10, 9}}, 0.8).size() == 2);
        check("pack: nothing", tilepack::Pack({}, 0.8).empty());
        tilepack::Fill f = tilepack::FillOf({{1, 30, 14}, {2, 10, 9}}, 0.8);
        check("fill: beside on the top shelf adds nothing", near(tilepack::Grown(f, {3, 10, 9}, 0.8), 14 + 0.8 + 9));
        check("fill: no room beside: a shelf above", near(tilepack::Grown(f, {3, 25, 9}, 0.8), 14 + 0.8 + 9 + 0.8 + 9));
        check("fill: wider than the column: never", tilepack::Grown(f, {3, 31, 9}, 0.8) < 0);
        tiling::Params far;
        tiling::FromEyes(tiling::Eyes{1.4, -12, 0.8, 80}, 2.0, &far);
        check("eyes: the desk line seen from the eyes again (about: it is on their sphere, not the wall's)", std::fabs(tiling::EyeElDeg(far, far.deskDeg) + 12) < 0.5);
        // (two windows 0.62 m tall at a 1.4 m wall: about 22 degrees each to the eyes, 10.5 round the centre)
        const double win = tiling::Deg(2 * std::atan2(0.31, far.radius));
        check("eyes: two windows one over the other share a column, three don't",
              tilepack::Pack({{1, 5, win}, {2, 5, win}, {3, 5, win}, {4, 5, win}}, far.gapDeg, dashtiles::EyeFits, &far).size() == 2);

        dashtiles::Board b;
        const auto sized = [](int id) { return id == 1 ? dashtiles::Info{1.48, 0.62} : dashtiles::Info{0.3, 0.53}; };
        for (int i = 1; i <= 6; ++i) b.Anchor(i, -1, dashtiles::Stay::Default);
        b.Repack(sized);
        const tiling::Layout &l = b.Arrange(sized);
        check("repack: the screen and the five windows in one column", b.Grid().Cols().size() == 1);
        int onShelf = 0;
        for (const tiling::Placed &p : l.tiles) onShelf += p.id != 1 && near(p.el0, b.PlacedOf(2)->el0);
        check("repack: four windows on the shelf over the screen, the fifth over them", onShelf == 4);
        check("repack: remembered as packed", b.Remember(1).rfind("tile 0 ", 0) == 0);
        int under = 0;
        for (const tiling::Placed &p : l.tiles) under += p.below && p.el1 < l.deskDeg;
        check("repack: round the desk line: the fifth hangs under the screen", under == 1 && b.PlacedOf(1)->el0 > l.deskDeg - 1e-6);
        dashtiles::State st;
        check("state: under the desk, leaning right, kept", dashtiles::Parse("tile 0 1 2 0 1.000 dr", &st) && st.below && st.align == 1 &&
                                                              dashtiles::Text(st) == "tile 0 1 2 0 1.000 dr");
        check("state: a place that isn't one", !dashtiles::Parse("tile 0 1 2 0 1.000 xr", &st));
        const int narrow = b.Grid().Cols()[0].cells[0].id;   // (the top shelf's first: narrower than the column)
        b.Arrange(sized);
        const int was = b.PlacedOf(narrow)->lean;
        check("keys: a narrow one leans the way first, then stays in its column",
              b.Press(narrow, was == -1 ? dashtiles::Key::Right : dashtiles::Key::Left) && b.Grid().Cols().size() == 1);

        // A hidden panel (absent) at no size: its neighbours close up; a repack packs the shown ones
        // and gives it a column of its own (not a shelf the others stand on while it's away).
        const auto span = [&] {
            const tiling::Layout &a = b.Arrange(sized);
            return a.az1 - a.az0;
        };
        const double whole = span();
        b.absent = {1};
        check("absent: the screen hidden, the strip closes up to the windows", span() < whole - 20);
        b.Repack(sized);
        const tilegrid::Where screen = b.Grid().Find(1);
        check("absent: a repack: the screen a column of its own, the windows in others",
              screen.ok() && b.Grid().Cols()[size_t(screen.col)].cells.size() == 1 && b.Grid().Cols().size() > 1);
        b.absent.clear();
        check("absent: shown again, at its size beside them", span() > whole);
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
            check("new windows set to the room: left there", TileWhenKnown("app:org.kde.dolphin", "", false) == W::Leave &&
                                                                 TileWhenKnown("remote:10.0.0.5/window:0x55aa", "", false) == W::Leave &&
                                                                 TileWhenKnown("app:firefox", "off", false) == W::Leave &&
                                                                 TileWhenKnown("app:x", "junk", false) == W::Leave);
            check("new windows set to the room: one tiled before goes back, one put in the room stays",
                  TileWhenKnown("app:firefox", "tile 0 1 0 1 1", false) == W::Restore && TileWhenKnown("app:firefox", "room", false) == W::Leave);
            check("new windows set to the room: screens and remote displays as before",
                  TileWhenKnown("screen:2", "", false) == W::Leave && TileWhenKnown("screen:2", "off", false) == W::Restore &&
                      TileWhenKnown("remote:10.0.0.5/display:DP-1", "tile 0 1 0 1 1", false) == W::Restore);
            check("floated off the desktop by the user: in the room, whatever its app did last or new windows do",
                  TileWhenKnown("app:firefox", "", true, true) == W::Leave && TileWhenKnown("app:firefox", "off", true, true) == W::Leave &&
                      TileWhenKnown("app:firefox", "tile 0 1 0 1 1", true, true) == W::Leave &&
                      TileWhenKnown("app:firefox", "room", false, true) == W::Leave);
            check("...only a window: a screen floated isn't a thing", TileWhenKnown("screen:2", "off", true, true) == W::Restore);
            check("left in the room: remembered as the room when its line said so or the user floated it",
                  dashtiles::LeftInRoom("room", false) && dashtiles::LeftInRoom("", true) && dashtiles::LeftInRoom("tile 0 1 0 1 1", true) &&
                      !dashtiles::LeftInRoom("", false) && !dashtiles::LeftInRoom("off", false));
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
    {   // dragged up into the space over its own column (user, 2026-11-20: it went back down; a
        // neighbour's panel goes in under it), and a panel leaving a stack (a PC window closing) pulls
        // a neighbour's lone panel that fits into the gap
        const auto cols = [](const dashtiles::Board &b) {
            std::string o;
            for (const auto &c : b.Grid().Cols()) {
                o += "[";
                for (const auto &cell : c.cells) o += " " + std::to_string(cell.id);
                o += " ]";
            }
            return o;
        };
        const auto same = [](int) { return dashtiles::Info{1.6, 0.9}; };
        dashtiles::Board b;
        for (const int i : {1, 2, 3}) b.Anchor(i, -1, dashtiles::Stay::Default);
        b.Arrange(same);
        const tiling::Placed p2 = *b.PlacedOf(2);
        const double az = p2.azDeg - 2, el = p2.el1 + 4;   // (over it, left of its middle)
        const tiling::Drop d = b.DropFor(2, az, el, same);
        check("lift: over its own column's top: a lift", d.kind == tiling::Drop::Lift);
        b.Drop(2, d, az);
        b.Arrange(same);
        check("lift: the left neighbour's panel in under it", cols(b) == "[ 2 1 ][ 3 ]");
        check("lift: ...and it higher than it was", b.PlacedOf(2)->el0 > p2.el1);
        const tiling::Drop on = b.DropFor(2, b.PlacedOf(2)->azDeg, b.PlacedOf(2)->elDeg, same);
        b.Drop(2, on, b.PlacedOf(2)->azDeg);
        check("lift: let go on itself: nothing moves", cols(b) == "[ 2 1 ][ 3 ]");
        b.steam = 3;
        dashtiles::Board s = b;
        s.Drop(2, s.DropFor(2, s.PlacedOf(2)->azDeg + 2, s.PlacedOf(2)->el1 + 4, same), 0);
        check("lift: Steam's window is never pulled: nothing", cols(s) == "[ 2 1 ][ 3 ]");
        b.steam = -1;
        check("leave: the stack's top goes: its column short a moment", b.Leave(2, 1000) && cols(b) == "[ 1 ][ 3 ]");
        b.Arrange(same);
        check("leave: ...nothing filled before the quiet's over", !b.FillGap(1000 + tilegap::kQuietMs - 1));
        check("leave: ...then its neighbour's lone panel in its place", b.FillGap(1000 + tilegap::kQuietMs) && cols(b) == "[ 3 1 ]");
        check("leave: ...filled once", !b.FillGap(9000));
        b.Arrange(same);
        check("leave: a lone panel goes: nothing pulled", b.Leave(3, 10000) && !b.FillGap(20000) && cols(b) == "[ 1 ]");
        dashtiles::Board w;
        const auto wide = [](int id) { return id == 3 ? dashtiles::Info{3.5, 1.97} : dashtiles::Info{1.6, 0.9}; };
        for (const int i : {1, 2, 3}) w.Anchor(i, -1, dashtiles::Stay::Default);
        w.Drop(2, {tiling::Drop::Into, 0, 0});
        w.Arrange(wide);
        check("leave: a neighbour wider than the column: not pulled", w.Leave(2, 0) && (w.Arrange(wide), !w.FillGap(5000)) && cols(w) == "[ 1 ][ 3 ]");
    }
    {   // the gap a panel leaves (council, 2026-11-20): none filled when several go at once (a desktop
        // stopping), the fill undone when it comes back, never across Steam's slot or onto a hanging one
        const auto cols = [](const dashtiles::Board &b) {
            std::string o;
            for (const auto &c : b.Grid().Cols()) {
                o += "[";
                for (const auto &cell : c.cells) o += " " + std::to_string(cell.id);
                o += " ]";
            }
            return o;
        };
        const auto same = [](int) { return dashtiles::Info{1.6, 0.9}; };
        const auto stack = [&] {   // [ 1 ][ 4 2 ][ 3 ]
            dashtiles::Board b;
            for (const int i : {1, 2, 3}) b.Anchor(i, -1, dashtiles::Stay::Default);
            b.Anchor(4, -1, dashtiles::Stay::Default);
            b.Drop(4, {tiling::Drop::Into, 1, 0});
            b.Arrange(same);
            return b;
        };
        dashtiles::Board b = stack();
        check("gap: the stack to start", cols(b) == "[ 1 ][ 4 2 ][ 3 ]");
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Leave(3, 900, "app:c"), b.Arrange(same);
        check("gap: two gone in a moment: none filled", !b.FillGap(5000) && cols(b) == "[ 1 ][ 2 ]");
        b = stack();
        const std::string line = b.Remember(4);
        b.Leave(4, 0, "app:a"), b.Arrange(same);
        check("gap: one gone: filled", b.FillGap(tilegap::kQuietMs) && cols(b) == "[ 1 2 ][ 3 ]");
        b.Arrange(same);
        check("gap: back: its place again, the fill undone", b.Restore(4, line, -1, "app:a") && cols(b) == "[ 1 ][ 4 2 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.FillGap(tilegap::kQuietMs), b.Arrange(same);
        check("gap: back on another panel number: still undone (council, 2026-11-20)",
              b.Restore(9, line, -1, "app:a") && cols(b) == "[ 1 ][ 9 2 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.FillGap(tilegap::kQuietMs), b.Arrange(same);
        check("gap: another app on its panel number: not undone for it",
              b.Restore(4, "tile 0 1 0 0 1.000", -1, "app:e") && cols(b) == "[ 1 2 ][ 4 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.FillGap(tilegap::kQuietMs), b.Arrange(same);
        b.Press(3, dashtiles::Key::Wider), b.Press(3, dashtiles::Key::Narrower), b.Arrange(same);
        check("gap: an unrelated tile resized since: still undone", b.Restore(4, line, -1, "app:a") && cols(b) == "[ 1 ][ 4 2 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Restore(4, line, -1, "app:a"), b.Arrange(same);
        check("gap: back before the quiet's over: nothing filled", !b.FillGap(tilegap::kQuietMs) && cols(b) == "[ 1 ][ 4 2 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Press(1, dashtiles::Key::Right), b.Arrange(same);
        const std::string byHand = cols(b);
        check("gap: a tile moved by hand meanwhile: nothing filled", !b.FillGap(tilegap::kQuietMs) && cols(b) == byHand);
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same);
        b.RestoreAll({{1, "tile 0 0 0 1 1.000"}, {2, "tile 0 1 0 1 1.000"}, {3, "tile 0 2 0 1 1.000"}}), b.Arrange(same);
        check("gap: a profile put back meanwhile: nothing filled", !b.FillGap(tilegap::kQuietMs) && cols(b) == "[ 1 ][ 2 ][ 3 ]");
        // (council, 2026-11-20) the panel a waiting gap stood on going too is still two gone
        b = stack();
        b.Anchor(5, -1, dashtiles::Stay::Default), b.Drop(5, {tiling::Drop::Into, 1, 1}), b.Arrange(same);
        const bool three = cols(b) == "[ 1 ][ 4 5 2 ][ 3 ]";
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Leave(2, 500, "app:b"), b.Arrange(same);
        check("gap: the one it stood on goes too: two gone, none filled", three && !b.FillGap(500 + tilegap::kQuietMs) && cols(b) == "[ 1 ][ 5 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Restore(4, "off", -1, "app:e"), b.Arrange(same);
        check("gap: another app on its panel number meanwhile: still filled", b.FillGap(tilegap::kQuietMs) && cols(b) == "[ 1 2 ][ 3 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Anchor(4, -1, dashtiles::Stay::Default), b.Arrange(same);
        check("gap: a new panel on its number meanwhile: still filled", b.FillGap(tilegap::kQuietMs) && cols(b).rfind("[ 1 2 ]", 0) == 0);
        dashtiles::Board o;   // [ 1 ][ 50 2 ][ 3 ], 50 goes, a new 60 opens into its place
        for (const int i : {1, 2, 3}) o.Anchor(i, -1, dashtiles::Stay::Default);
        o.Arrange(same);
        o.Anchor(50, -1, dashtiles::Stay::Default), o.Drop(50, {tiling::Drop::Into, 1, 0}), o.Arrange(same);
        o.Leave(50, 0, "app:x"), o.Arrange(same);
        const dashtiles::Info info{1.6, 0.9};
        o.Anchor(60, -1, dashtiles::Stay::Default, -1, &info), o.Arrange(same);
        const std::string opened = cols(o);
        check("gap: a panel opened into it meanwhile: no third on that stack", opened == "[ 1 ][ 60 2 ][ 3 ]" &&
                                                                                  !o.FillGap(tilegap::kQuietMs) && cols(o) == opened);
        {   // (council, 2026-11-20) ...with no picture yet (laid out at no size), or short enough for three
            const auto shortOnes = [](int) { return dashtiles::Info{1.2, 0.5}; };
            for (const bool unshown : {true, false}) {
                dashtiles::Board n;
                const auto info = unshown ? same : shortOnes;
                for (const int i : {1, 2, 3}) n.Anchor(i, -1, dashtiles::Stay::Default);
                n.Arrange(info);
                n.Anchor(50, -1, dashtiles::Stay::Default), n.Drop(50, {tiling::Drop::Into, 1, 0}), n.Arrange(info);
                n.Leave(50, 0, "app:x"), n.Arrange(info);
                const dashtiles::Info i60 = info(60);
                n.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60);
                if (unshown) n.absent = {60};
                n.Arrange(info);
                const std::string at = cols(n);
                check(unshown ? "gap: ...one not shown yet opened into it: nothing filled" : "gap: ...short ones: still nothing filled",
                      at.find("[ 60 2 ]") != std::string::npos && !n.FillGap(tilegap::kQuietMs) && cols(n) == at);
            }
        }
        {   // (council, 2026-11-20) the gap's stack as Leave left it: which panels, not how many
            const auto shortOnes = [](int) { return dashtiles::Info{1.2, 0.5}; };
            const auto three = [&] {   // [ 1 ][ 50 51 2 ][ 3 ]
                dashtiles::Board t;
                for (const int i : {1, 2, 3}) t.Anchor(i, -1, dashtiles::Stay::Default);
                t.Arrange(shortOnes);
                t.Anchor(50, -1, dashtiles::Stay::Default), t.Drop(50, {tiling::Drop::Into, 1, 0}), t.Arrange(shortOnes);
                t.Anchor(51, -1, dashtiles::Stay::Default), t.Drop(51, {tiling::Drop::Into, 1, 1}), t.Arrange(shortOnes);
                return t;
            };
            dashtiles::Board t = three();
            const bool built = cols(t) == "[ 1 ][ 50 51 2 ][ 3 ]";
            t.Leave(50, 0, "app:x"), t.Arrange(shortOnes), t.Unanchor(51), t.Arrange(shortOnes);
            const dashtiles::Info i60 = shortOnes(60);
            t.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), t.Arrange(shortOnes);
            const std::string swapped = cols(t);
            check("gap: one of its stack taken off, another opened into it: nothing filled",
                  built && swapped == "[ 1 ][ 60 2 ][ 3 ]" && !t.FillGap(tilegap::kQuietMs) && cols(t) == swapped);
            t = three();
            t.Leave(50, 0, "app:x"), t.Arrange(shortOnes), t.Unanchor(51), t.Arrange(shortOnes);
            check("gap: ...one taken off alone: still filled", t.FillGap(tilegap::kQuietMs) && cols(t) == "[ 1 2 ][ 3 ]");
            dashtiles::Board u;   // [ 1 ][ 70 50 2 ][ 3 ], 70 not shown yet (laid out at no size)
            for (const int i : {1, 2, 3}) u.Anchor(i, -1, dashtiles::Stay::Default);
            u.Arrange(same);
            u.Anchor(50, -1, dashtiles::Stay::Default), u.Drop(50, {tiling::Drop::Into, 1, 0}), u.Arrange(same);
            u.Anchor(70, -1, dashtiles::Stay::Default), u.Drop(70, {tiling::Drop::Into, 1, 0}), u.absent = {70}, u.Arrange(same);
            const bool unshown = cols(u) == "[ 1 ][ 70 50 2 ][ 3 ]";
            u.Leave(50, 0, "app:x"), u.Arrange(same);
            check("gap: one in its stack not shown yet (no size to test): nothing filled", unshown && !u.FillGap(tilegap::kQuietMs) && cols(u) == "[ 1 ][ 70 2 ][ 3 ]");
            const auto sizes = [](int id) {
                return id == 1 ? dashtiles::Info{2.2, 0.4} : id == 2 ? dashtiles::Info{0.8, 1.4} : id == 3 ? dashtiles::Info{2.0, 1.0} : dashtiles::Info{0.8, 1.0};
            };
            dashtiles::Board v;   // [ 1 ][ 4 2 ][ 3 ]: 4 goes, then 1 with no layout since and 70 added: the columns shift
            for (const int i : {1, 2, 3}) v.Anchor(i, -1, dashtiles::Stay::Default);
            v.Arrange(sizes);
            v.Anchor(4, -1, dashtiles::Stay::Default), v.Drop(4, {tiling::Drop::Into, 1, 0}), v.Arrange(sizes);
            v.Leave(4, 0, "app:x"), v.Arrange(sizes), v.Unanchor(1), v.Anchor(70, -1, dashtiles::Stay::Default);
            const std::string shifted = cols(v);
            check("gap: the columns shifted since the layout: nothing filled", shifted == "[ 2 ][ 3 ][ 70 ]" && !v.FillGap(tilegap::kQuietMs) && cols(v) == shifted);
            dashtiles::Board h2 = stack();   // [ 1 ][ 4 2 ][ 3 ]: 4 goes, another app back hanging under 2
            h2.Leave(4, 0, "app:a"), h2.Arrange(shortOnes), h2.Restore(7, "tile 0 1 1 0 1.000 dc", -1, "app:h"), h2.Arrange(shortOnes);
            check("gap: one back hanging under its stack (it took no gap): still filled",
                  h2.FillGap(tilegap::kQuietMs) && cols(h2) == "[ 1 2 7 ][ 3 ]");
        }
        {   // (council, 2026-11-20) the fit test as the layout stacks it: under the desk line its gap counted once
            const auto info = [](int id) {
                return id == 1 ? dashtiles::Info{1.6, 0.78} : id == 3 ? dashtiles::Info{2.6, 0.5} : id == 4 ? dashtiles::Info{1.6, 1.2}
                     : id == 5 ? dashtiles::Info{1.6, 0.6} : dashtiles::Info{1.6, 0.9};
            };
            dashtiles::Board f;   // [ 1 ][ 4 2 ][ 3 ]: 4 goes, 5 hangs under 2, 1 fits on 2 with half a gap to spare
            for (const int i : {1, 2, 3}) f.Anchor(i, -1, dashtiles::Stay::Default);
            f.Arrange(info);
            f.Anchor(4, -1, dashtiles::Stay::Default), f.Drop(4, {tiling::Drop::Into, 1, 0}), f.Arrange(info);
            f.Leave(4, 0, "app:a"), f.Arrange(info), f.Restore(5, "tile 0 1 1 0 1.000 dc", -1, "app:h"), f.Arrange(info);
            const bool filled = f.FillGap(tilegap::kQuietMs);
            f.Arrange(info);
            double lo = 1e9, hi = -1e9;
            for (const tiling::Placed &p : f.Last().tiles)
                if (p.col == f.Grid().Find(2).col) lo = std::min(lo, p.el0), hi = std::max(hi, p.el1);
            const double eye = tiling::EyeElDeg(f.params, hi) - tiling::EyeElDeg(f.params, lo);
            check("fit: a stack with one hanging under it that fits takes one more", filled && cols(f) == "[ 1 2 5 ][ 3 ]" &&
                                                                                   eye <= tilepack::kColumnDeg && eye > tilepack::kColumnDeg - f.params.gapDeg);
        }
        {   // (council, 2026-11-20) put on a stack before its size was known: settled once it's known
            const auto big = [](int id) { return id == 60 ? dashtiles::Info{1.6, 1.6} : dashtiles::Info{1.6, 0.9}; };
            const auto full = [&] {   // [ 1 ][ 2 ][ 3 ]: the desk full
                dashtiles::Board d;
                for (const int i : {1, 2, 3}) d.Anchor(i, -1, dashtiles::Stay::Default);
                d.Arrange(same);
                return d;
            };
            const dashtiles::Info guess;   // (Info's default: no picture yet)
            for (const bool sized : {false, true}) {
                dashtiles::Board d = full();
                d.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, sized), d.absent = {60}, d.Arrange(big);
                const std::string onTop = cols(d);
                const bool waits = !d.Settle();
                d.absent = {}, d.Arrange(big);
                if (!sized) {
                    check("settle: a new one with no picture on a stack: left until it has one", onTop.find("60") != std::string::npos &&
                                                                                               onTop.find("[ 60 ]") == std::string::npos && waits);
                    check("settle: ...too tall with its picture: a column of its own beside it", d.Settle() && cols(d).find("[ 60 ]") != std::string::npos && !d.Settle());
                } else {
                    check("settle: one put there at its own size: left as it is", !d.Settle() && cols(d) == onTop);
                }
            }
            dashtiles::Board d = full();
            d.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), d.Arrange(same);
            const std::string fits = cols(d);
            check("settle: ...one that fits with its picture: left there", !d.Settle() && cols(d) == fits);
            dashtiles::Board hand = full();   // three on a stack by hand, too tall: yours
            hand.Drop(1, {tiling::Drop::Into, 0, 0}), hand.Arrange(same), hand.Drop(3, {tiling::Drop::Into, 0, 0}), hand.Arrange(same);
            check("settle: a stack made too tall by hand: left as it is", cols(hand) == "[ 3 1 2 ]" && !hand.Settle());
            dashtiles::Board t = stack();   // [ 1 ][ 4 2 ][ 3 ]: 4 goes, 60 opens into its gap, 4's app comes back there
            const std::string four = t.Remember(4);
            t.Leave(4, 0, "app:a"), t.Arrange(same);
            const dashtiles::Info i60;
            t.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), t.Arrange(same);
            const bool tookIt = cols(t) == "[ 1 ][ 60 2 ][ 3 ]" && !t.FillGap(tilegap::kQuietMs);
            t.Restore(9, four, -1, "app:a"), t.Arrange(same);
            const std::string back = cols(t);
            check("settle: back where another took its gap, too tall: a column of its own", tookIt && back == "[ 1 ][ 9 60 2 ][ 3 ]" &&
                                                                                           t.Settle() && (t.Arrange(same), cols(t) == "[ 1 ][ 60 2 ][ 9 ][ 3 ]"));
            dashtiles::Board q = stack();   // (council pass 6) ...back within the quiet time: the same
            q.Leave(4, 0, "app:a"), q.Arrange(same), q.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), q.Arrange(same);
            q.Restore(9, four, -1, "app:a"), q.Arrange(same);
            check("settle: back within 1.5 s where another took its gap: a column of its own", cols(q) == "[ 1 ][ 9 60 2 ][ 3 ]" && q.Settle() &&
                                                                                               (q.Arrange(same), cols(q) == "[ 1 ][ 60 2 ][ 9 ][ 3 ]"));
            dashtiles::Board late;   // ...or after its gap was given up ([ 10 1 ][ 4 2 ][ 30 3 ]: no lone neighbour), 60 opening into it later
            for (const int i : {1, 2, 3}) late.Anchor(i, -1, dashtiles::Stay::Default);
            late.Arrange(same);
            for (const auto &[i, c] : {std::pair{10, 0}, {4, 1}, {30, 2}})
                late.Anchor(i, -1, dashtiles::Stay::Default), late.Drop(i, {tiling::Drop::Into, c, 0}), late.Arrange(same);
            const std::string four3 = late.Remember(4);
            late.Leave(4, 0, "app:a"), late.Arrange(same);
            const bool none = !late.FillGap(tilegap::kQuietMs);
            late.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), late.Arrange(same);
            const std::string before = cols(late);
            late.Restore(9, four3, -1, "app:a"), late.Arrange(same);
            check("settle: back after its gap was given up, onto a stack made for you: a column of its own",
                  none && before == "[ 10 1 ][ 60 2 ][ 30 3 ]" && late.Settle() && (late.Arrange(same), cols(late) == "[ 10 1 ][ 60 2 ][ 9 ][ 30 3 ]"));
            dashtiles::Board mine = stack();   // ...a stack you made: yours, too tall or not
            mine.Leave(4, 0, "app:a"), mine.Arrange(same);
            mine.Anchor(60, -1, dashtiles::Stay::Default), mine.Arrange(same), mine.Drop(60, {tiling::Drop::Into, 1, 0}), mine.Arrange(same);
            const std::string mine0 = cols(mine);
            mine.Restore(9, four, -1, "app:a"), mine.Arrange(same);
            check("settle: back onto a stack you made: left as it is", mine0 == "[ 1 ][ 60 2 ][ 3 ]" && !mine.Settle() && cols(mine) == "[ 1 ][ 9 60 2 ][ 3 ]");
            dashtiles::Board s2 = full();   // a stackmate not shown yet: no size to test with
            s2.steam = 2;
            s2.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), s2.absent = {60}, s2.Arrange(big);
            const std::string on2 = cols(s2);
            s2.absent = {2}, s2.Arrange(big);
            const bool waited = !s2.Settle();
            s2.absent = {}, s2.Arrange(big);
            check("settle: waits for a stackmate not shown, then checks", on2 == "[ 1 ][ 60 2 ][ 3 ]" && waited && s2.Settle() &&
                                                                         (s2.Arrange(big), cols(s2) == "[ 1 ][ 2 ][ 60 ][ 3 ]"));
            dashtiles::Board u = stack();   // a stackmate gone, not laid out since: no stale height (Neon, pass 6)
            u.Leave(4, 0, "app:a"), u.Arrange(same), u.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), u.Arrange(same);
            u.FillGap(tilegap::kQuietMs), u.Restore(9, four, -1, "app:a"), u.Arrange(same), u.Unanchor(60);
            const bool waitsU = !u.Settle();
            u.Arrange(same);
            check("settle: a stackmate gone since the layout: waits, then leaves one that fits", waitsU && !u.Settle() && cols(u) == "[ 1 ][ 9 2 ][ 3 ]");
            const auto hinfo = [](int id) {   // (Neutral, pass 6: [ 60 2 | 7 ], 2 goes, 60 settles out)
                return id == 60 ? dashtiles::Info{1.6, 1.9} : id == 7 ? dashtiles::Info{1.4, 0.6} : id == 2 ? dashtiles::Info{1.6, 0.7}
                                                                       : id == 3 ? dashtiles::Info{1.2, 0.3} : dashtiles::Info{1.6, 2.0};
            };
            dashtiles::Board g;
            for (const int i : {1, 2, 3}) g.Anchor(i, -1, dashtiles::Stay::Default);
            g.Arrange(hinfo), g.Restore(7, "tile 0 1 1 0 1.000 dc", -1, "app:h"), g.Arrange(hinfo);
            g.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), g.absent = {60}, g.Arrange(hinfo);
            const bool on = g.Grid().Find(60).col == g.Grid().Find(2).col;
            g.Leave(2, 0, "app:b"), g.Arrange(hinfo), g.absent = {}, g.Arrange(hinfo);
            const bool moved = g.Settle();
            g.Arrange(hinfo);
            check("settle: a gap it stayed in doesn't follow it", on && moved && cols(g) == "[ 1 ][ 7 ][ 60 ][ 3 ]" && !g.FillGap(tilegap::kQuietMs));
            dashtiles::Board r = full();   // a refused drop keeps it marked
            r.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), r.absent = {60}, r.Arrange(big);
            r.Drop(60, {tiling::Drop::TearOff, 0, 0}), r.absent = {}, r.Arrange(big);
            check("settle: a tile pulled off and snapped back: still checked", r.Settle() && cols(r).find("[ 60 ]") != std::string::npos);
            // (council pass 7) a stack built on by hand is yours, whichever way you built it
            const auto onTopOf4 = [&] {   // [ 4 1 ][ 60 2 ][ 3 ]: 60 put on 2 for you, then 4 goes
                dashtiles::Board k = full();
                k.Anchor(4, -1, dashtiles::Stay::Default), k.Drop(4, {tiling::Drop::Into, 0, 0}), k.Arrange(same);
                k.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), k.Arrange(same);
                return k;
            };
            for (const bool byKey : {true, false}) {
                dashtiles::Board k = onTopOf4();
                const std::string four4 = k.Remember(4);
                k.Leave(4, 0, "app:a"), k.Arrange(same);
                const bool lifted = byKey ? k.Press(1, dashtiles::Key::Up) : (k.Drop(1, {tiling::Drop::Lift, 0, 0, false, +1}), true);
                k.Arrange(same);
                const std::string mine1 = cols(k);
                k.Restore(9, four4, -1, "app:a"), k.Arrange(same);
                check(byKey ? "settle: a stack you lifted by key: yours" : "settle: a stack you lifted by a drag: yours",
                      lifted && mine1 == "[ 1 60 ][ 2 ][ 3 ]" && !k.Settle() && cols(k) == "[ 9 1 60 ][ 2 ][ 3 ]");
            }
            dashtiles::Board hb = full();   // one dropped by hand onto a stack made for you, then back
            hb.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), hb.Arrange(same);
            hb.Anchor(4, -1, dashtiles::Stay::Default), hb.Drop(4, {tiling::Drop::Into, 1, 0}), hb.Arrange(same);
            const std::string built = cols(hb), four5 = hb.Remember(4);
            hb.Leave(4, 0, "app:a"), hb.Arrange(same), hb.Restore(9, four5, -1, "app:a"), hb.Arrange(same);
            check("settle: built on a stack made for you by hand: yours", built == "[ 1 ][ 4 60 2 ][ 3 ]" && !hb.Settle() && cols(hb) == "[ 1 ][ 9 60 2 ][ 3 ]");
            dashtiles::Board over = full();   // too tall already with a stackmate not shown: no wait
            over.steam = 2;
            for (const auto &[i, c] : {std::pair{10, 0}, {5, 1}, {30, 2}})
                over.Anchor(i, -1, dashtiles::Stay::Default), over.Drop(i, {tiling::Drop::Into, c, 0}), over.Arrange(big);
            over.absent = {2}, over.Arrange(big);
            over.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), over.absent = {2, 60}, over.Arrange(big);
            const std::string over0 = cols(over);
            over.absent = {2}, over.Arrange(big);
            check("settle: too tall already, a stackmate not shown: out at once", over0 == "[ 10 1 ][ 60 5 2 ][ 30 3 ]" && over.Settle() &&
                                                                                 cols(over) == "[ 10 1 ][ 5 2 ][ 60 ][ 30 3 ]");
            dashtiles::Board fg;   // one a gap fill put on a stack: a return to that stack is checked
            for (const int i : {1, 2, 3}) fg.Anchor(i, -1, dashtiles::Stay::Default);
            fg.Arrange(same);
            fg.Anchor(10, -1, dashtiles::Stay::Default), fg.Drop(10, {tiling::Drop::Into, 0, 0}), fg.Arrange(same);
            fg.Anchor(4, -1, dashtiles::Stay::Default), fg.Drop(4, {tiling::Drop::Into, 1, 0}), fg.Arrange(same);
            const std::string four6 = fg.Remember(4), two6 = fg.Remember(2);
            fg.Leave(4, 0, "app:a"), fg.Arrange(same);
            const bool filled6 = fg.FillGap(tilegap::kQuietMs);
            fg.Arrange(same), fg.Leave(2, 5000, "app:b"), fg.Arrange(same), fg.Restore(8, two6, -1, "app:b"), fg.Arrange(same);
            const bool fits8 = !fg.Settle();   // (8 back where it was, and it fits)
            fg.Restore(9, four6, -1, "app:a"), fg.Arrange(same);
            check("settle: back onto a stack a gap fill made: checked", filled6 && fits8 && cols(fg) == "[ 10 1 ][ 9 3 8 ]" && fg.Settle() &&
                                                                          cols(fg) == "[ 10 1 ][ 3 8 ][ 9 ]");
            // (council pass 8) a wide one hidden at the bottom: the narrow ones over it on shelves of their own, taller than they'll be
            const auto wide = [](int id) {
                return id == 2 ? dashtiles::Info{2.4, 0.9} : id == 4 || id == 5 || id == 60 || id == 9 ? dashtiles::Info{0.9, 1.25} : dashtiles::Info{1.6, 0.9};
            };
            const auto wideBoard = [&](int mid) {   // [ 10 1 ][ mid 2 ][ 30 3 ], 2 Steam's
                dashtiles::Board w = full();
                w.steam = 2, w.Arrange(wide);
                for (const auto &[i, c] : {std::pair{10, 0}, {mid, 1}, {30, 2}})
                    w.Anchor(i, -1, dashtiles::Stay::Default), w.Drop(i, {tiling::Drop::Into, c, 0}), w.Arrange(wide);
                return w;
            };
            const dashtiles::Info narrowGuess{0.9, 0.9};
            dashtiles::Board sh = wideBoard(5);
            sh.Anchor(60, -1, dashtiles::Stay::Default, -1, &narrowGuess, false), sh.absent = {2, 60}, sh.Arrange(wide);
            const std::string sh0 = cols(sh);
            sh.absent = {2}, sh.Arrange(wide);
            const bool tallHidden = !sh.Laid().ColumnFits(1, sh.Laid().fills[1].height), waitsSh = !sh.Settle();
            sh.absent = {}, sh.Arrange(wide);
            check("settle: too tall only while a wide one under it is hidden: waits, then left", sh0 == "[ 10 1 ][ 60 5 2 ][ 30 3 ]" && tallHidden &&
                                                                                                   waitsSh && !sh.Settle() && cols(sh) == sh0);
            dashtiles::Board shr = wideBoard(4);   // ...back where another took its gap, the same
            const std::string four7 = shr.Remember(4);
            shr.Leave(4, 0, "app:a"), shr.Arrange(wide), shr.FillGap(tilegap::kQuietMs);
            const dashtiles::Info i5 = wide(5);
            shr.Anchor(5, -1, dashtiles::Stay::Default, -1, &i5), shr.Arrange(wide);
            shr.absent = {2}, shr.Arrange(wide), shr.Restore(9, four7, -1, "app:a"), shr.Arrange(wide);
            const std::string shr0 = cols(shr);
            check("settle: back, too tall only while a wide one under it is hidden: waits", !shr.Settle() && cols(shr) == shr0);
            // (council pass 8) one you put into a stack: its shown stackmates are yours, as you saw them
            const auto tall4 = [](int id) { return id == 4 ? dashtiles::Info{1.6, 1.6} : dashtiles::Info{1.6, 0.9}; };
            dashtiles::Board jn = full();
            jn.steam = 2, jn.Arrange(tall4);
            for (const auto &[i, c] : {std::pair{10, 0}, {30, 2}})
                jn.Anchor(i, -1, dashtiles::Stay::Default), jn.Drop(i, {tiling::Drop::Into, c, 0}), jn.Arrange(tall4);
            jn.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), jn.absent = {2, 60}, jn.Arrange(tall4);
            jn.absent = {2}, jn.Arrange(tall4);
            const bool waitsJn = !jn.Settle();
            jn.Anchor(4, -1, dashtiles::Stay::Default), jn.Drop(4, {tiling::Drop::Into, 1, 0}), jn.Arrange(tall4);
            check("settle: one dropped onto a stack with one waiting there: that one's yours", waitsJn && cols(jn) == "[ 10 1 ][ 4 60 2 ][ 30 3 ]" &&
                                                                                                 !jn.Settle() && cols(jn) == "[ 10 1 ][ 4 60 2 ][ 30 3 ]");
            // (council pass 8) reordered, resized or leaned in a stack made for you: still made for you
            const char *edits[] = {"reordered by key", "resized by its corner", "made smaller by key"};
            for (int e = 0; e < 6; ++e) {   // (2, then 60 itself: the one put there for you)
                const int who = e < 3 ? 2 : 60;
                dashtiles::Board ed;   // [ 10 1 ][ 4 2 ][ 30 3 ], 4 goes (no gap fill), 60 put on 2 for you
                for (const int i : {1, 2, 3}) ed.Anchor(i, -1, dashtiles::Stay::Default);
                ed.Arrange(same);
                for (const auto &[i, c] : {std::pair{10, 0}, {4, 1}, {30, 2}})
                    ed.Anchor(i, -1, dashtiles::Stay::Default), ed.Drop(i, {tiling::Drop::Into, c, 0}), ed.Arrange(same);
                const std::string four8 = ed.Remember(4);
                ed.Leave(4, 0, "app:a"), ed.Arrange(same), ed.FillGap(tilegap::kQuietMs);
                ed.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), ed.Arrange(same);
                const bool edited = e % 3 == 0 ? ed.Press(who, who == 2 ? dashtiles::Key::Up : dashtiles::Key::Down)
                                    : e % 3 == 1 ? ed.Resized(who, 0.9) : ed.Press(who, dashtiles::Key::Shorter);
                ed.Arrange(same), ed.Restore(9, four8, -1, "app:a"), ed.Arrange(same);
                const bool out = ed.Settle();
                ed.Arrange(same);
                check((std::string(who == 2 ? "settle: a stackmate " : "settle: the one put there for you ") + edits[e % 3] + ": a return there still checked").c_str(),
                      edited && out && cols(ed).find("[ 9 ]") != std::string::npos);
            }
            const auto lean = [](int id) {
                return id == 5 ? dashtiles::Info{1.0, 0.7} : id == 60 ? dashtiles::Info{1.0, 0.8} : id == 4 || id == 9 ? dashtiles::Info{1.0, 1.0}
                                                                                                  : dashtiles::Info{1.6, 0.7};
            };
            const dashtiles::Info i60s{1.0, 0.8};
            dashtiles::Board ln = full();
            ln.Arrange(lean);
            for (const auto &[i, c] : {std::pair{10, 0}, {5, 1}, {4, 1}, {30, 2}})
                ln.Anchor(i, -1, dashtiles::Stay::Default), ln.Drop(i, {tiling::Drop::Into, c, 0}), ln.Arrange(lean);
            const std::string four9 = ln.Remember(4);
            ln.Leave(4, 0, "app:a"), ln.Arrange(lean), ln.FillGap(tilegap::kQuietMs);
            ln.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60s), ln.Arrange(lean);
            for (const int who : {5, 60}) {   // (a stackmate, and the one put there for you)
                dashtiles::Board l = ln;
                const bool leaned = l.Press(who, dashtiles::Key::Left);
                l.Arrange(lean), l.Restore(9, four9, -1, "app:a"), l.Arrange(lean);
                check(who == 5 ? "settle: a stackmate leaned by key: a return there still checked"
                               : "settle: the one put there for you leaned by key: a return there still checked",
                      cols(ln) == "[ 10 1 ][ 60 5 2 ][ 30 3 ]" && leaned && l.Settle() && (l.Arrange(lean), cols(l) == "[ 10 1 ][ 60 5 2 ][ 9 ][ 30 3 ]"));
            }
            // (council pass 10) the one waiting, sized or moved in its stack by you while a stackmate's hidden: yours
            const auto waiting = [&] {   // [ 10 1 ][ 60 5 2 ][ 30 3 ], 2 (Steam's) hidden, 60 shown and waiting on it
                dashtiles::Board wr = full();
                wr.steam = 2, wr.Arrange(same);
                for (const auto &[i, c] : {std::pair{10, 0}, {5, 1}, {30, 2}})
                    wr.Anchor(i, -1, dashtiles::Stay::Default), wr.Drop(i, {tiling::Drop::Into, c, 0}), wr.Arrange(same);
                wr.absent = {2}, wr.Arrange(same);
                wr.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), wr.absent = {2, 60}, wr.Arrange(same);
                wr.absent = {2}, wr.Arrange(same);
                return wr;
            };
            for (const bool resize : {true, false}) {
                dashtiles::Board wr = waiting();
                const bool waitsWr = !wr.Settle(), edited = resize ? wr.Resized(60, 1.8) : wr.Press(60, dashtiles::Key::Down);
                wr.Arrange(same);
                const std::string wr0 = cols(wr);
                const bool stays = !wr.Settle();
                wr.absent = {}, wr.Arrange(same);
                check(resize ? "settle: the one waiting made bigger by you: left where you made it" : "settle: the one waiting reordered by you: left where you put it",
                      waitsWr && edited && stays && !wr.Settle() && cols(wr) == wr0 && wr0.find("60") != std::string::npos && wr.Grid().Find(60).col == wr.Grid().Find(5).col);
            }
            // (council pass 11) touched by you but changed nothing: still waiting, checked once its stackmate shows
            for (const int how : {0, 1, 2}) {
                dashtiles::Board wn = waiting();
                const bool waitsWn = !wn.Settle();
                if (how == 0) wn.Drop(60, {tiling::Drop::Into, wn.Grid().Find(60).col, wn.Grid().Find(60).row});   // let go where it was
                if (how == 1) wn.Resized(60, 1.000001);                                                            // a corner let go where it was
                if (how == 2) wn.absent = {2, 60}, wn.Arrange(same), wn.Resized(60, 1.1), wn.absent = {2};         // sized while it's not shown
                wn.Arrange(same);
                const bool on5 = wn.Grid().Find(60).col == wn.Grid().Find(5).col;
                wn.absent = {}, wn.Arrange(same);
                check(how == 0   ? "settle: the one waiting let go where it was: still checked"
                      : how == 1 ? "settle: the one waiting, a corner let go where it was: still checked"
                                 : "settle: the one waiting sized while not shown: still checked",
                      waitsWn && on5 && wn.Settle() && wn.Grid().Find(60).col != wn.Grid().Find(5).col);
            }
            // (council pass 10) put there for you, alone once its stackmate went, moved alone: a return into its column still checked
            dashtiles::Board al = full();
            al.Anchor(4, -1, dashtiles::Stay::Default), al.Drop(4, {tiling::Drop::Into, 0, 0}), al.Arrange(same);
            al.Anchor(60, -1, dashtiles::Stay::Default, -1, &i60), al.Arrange(same);   // [ 4 1 ][ 60 2 ][ 3 ]
            al.Unanchor(2), al.Arrange(same);
            const bool movedAl = al.Press(60, dashtiles::Key::Right);
            al.Arrange(same);
            dashtiles::State backSt;
            backSt.kind = dashtiles::State::Kind::Tile, backSt.col = al.Grid().Find(60).col, backSt.row = 0, backSt.alone = false, backSt.scale = 1;
            const auto tall9 = [](int id) { return id == 9 ? dashtiles::Info{1.6, 1.9} : dashtiles::Info{1.6, 0.9}; };
            al.Restore(9, backSt, -1, "app:z"), al.Arrange(tall9);
            const bool onAl = al.Grid().Find(9).col == al.Grid().Find(60).col;
            check("settle: put there for you, moved alone: a return into its column still checked", movedAl && onAl && al.Settle() &&
                                                                                                    al.Grid().Find(9).col != al.Grid().Find(60).col);
            // (council pass 9) fitting so far isn't sure while a stackmate's hidden: waits till it's shown
            const auto hang = [](int id) { return id == 1 || id == 3 ? dashtiles::Info{1.6, 1.4} : dashtiles::Info{1.6, 0.9}; };
            dashtiles::Board hh;   // [ 1 ][ 60 2 _7 ][ 3 ]: 7 hidden, hanging under the desk line
            for (const int i : {1, 2, 3}) hh.Anchor(i, -1, dashtiles::Stay::Default);
            hh.Arrange(hang), hh.Anchor(7, -1, dashtiles::Stay::Default), hh.Arrange(hang);
            hh.Drop(7, {tiling::Drop::Into, hh.Grid().Find(2).col, 1, true}), hh.Arrange(hang);
            hh.absent = {7}, hh.Arrange(hang);
            hh.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), hh.absent = {7, 60}, hh.Arrange(hang);
            const bool on7 = hh.Grid().Find(60).col == hh.Grid().Find(7).col;
            hh.absent = {7}, hh.Arrange(hang);
            const bool waitsHh = !hh.Settle();
            hh.absent = {}, hh.Arrange(hang);
            check("settle: a stackmate hanging hidden under the desk line: waits, then checked", on7 && waitsHh && hh.Settle() &&
                                                                                                    hh.Grid().Find(60).col != hh.Grid().Find(2).col);
            std::map<int, dashtiles::Info> sz = {{1, {1.6, 0.9}}, {2, {1.6, 0.5}}, {3, {1.6, 0.9}}, {8, {1.6, 1.6}}, {60, {1.6, 0.5}}};
            const auto guessed = [&](int id) { return sz.count(id) ? sz.at(id) : dashtiles::Info{}; };
            dashtiles::Board gs;   // [ 1 ][ 60 8 2 ][ 3 ]: 8 hidden with no picture (a guess for its size), bigger once it has one
            for (const int i : {1, 2, 3}) gs.Anchor(i, -1, dashtiles::Stay::Default);
            gs.Arrange(guessed), gs.Anchor(8, -1, dashtiles::Stay::Default), gs.Arrange(guessed);
            gs.Drop(8, {tiling::Drop::Into, gs.Grid().Find(2).col, 0}), gs.Arrange(guessed);
            sz.erase(8), gs.absent = {8}, gs.Arrange(guessed);
            gs.Anchor(60, -1, dashtiles::Stay::Default, -1, &guess, false), gs.absent = {8, 60}, gs.Arrange(guessed);
            const std::string gs0 = cols(gs);
            gs.absent = {8}, gs.Arrange(guessed);
            const bool waitsGs = !gs.Settle();
            sz[8] = {1.6, 1.6}, gs.absent = {}, gs.Arrange(guessed);
            check("settle: a stackmate hidden with only a guess for its size: waits, then checked", gs0 == "[ 1 ][ 60 8 2 ][ 3 ]" && waitsGs &&
                                                                                                       gs.Settle() && (gs.Arrange(guessed), cols(gs) == "[ 1 ][ 8 2 ][ 60 ][ 3 ]"));
        }
        b = stack();   // (council, 2026-11-20) a gap undone long ago doesn't count against the next one
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Restore(4, line, -1, "app:a"), b.Arrange(same);
        b.Leave(4, 10000, "app:a"), b.Arrange(same);
        check("gap: one gone long after another came back (no tick between): filled", b.FillGap(10000 + tilegap::kQuietMs) && cols(b) == "[ 1 2 ][ 3 ]");
        b = stack();
        b.Leave(3, 0, "app:c"), b.Arrange(same);
        b.RestoreAll({{1, "tile 0 0 0 1 1.000"}, {2, "tile 0 1 0 1 1.000"}, {4, "tile 0 1 0 0 1.000"}}), b.Arrange(same);
        const bool reset = cols(b) == "[ 1 ][ 4 2 ]";
        b.Leave(4, 500, "app:a"), b.Arrange(same);
        check("gap: a profile put back: the count starts again", reset && b.FillGap(500 + tilegap::kQuietMs) && cols(b) == "[ 1 2 ]");
        b = stack();
        b.Leave(4, 0, "app:a"), b.Arrange(same), b.Restore(4, "off", -1, ""), b.Arrange(same);
        check("gap: a panel with no key on its number meanwhile: still filled", b.FillGap(tilegap::kQuietMs) && cols(b) == "[ 1 2 ][ 3 ]");
        dashtiles::Board h;   // [ 1 ][ 2 3 ]: 2 on the desk, 3 hanging under it
        for (const int i : {1, 2, 3}) h.Anchor(i, -1, dashtiles::Stay::Default);
        h.Press(3, dashtiles::Key::Down);
        h.Arrange(same);
        const bool hung = cols(h) == "[ 1 ][ 2 3 ]";
        h.Leave(2, 0), h.Arrange(same);
        check("gap: the base goes, one hanging under it: no gap (it takes the desk)", hung && !h.FillGap(5000) && cols(h) == "[ 1 ][ 3 ]");
        dashtiles::Board s;   // [ 1 ] |slot| [ 4 2 ][ 3 ]
        s.params.reserveAzDeg = 0, s.params.reserveDeg = 66;
        for (const int i : {1, 2, 3}) s.Anchor(i, -1, dashtiles::Stay::Default);
        s.Arrange(same);
        const int split = s.Grid().Split(0);
        s.Anchor(4, -1, dashtiles::Stay::Default);
        s.Drop(4, {tiling::Drop::Into, 1, 0});
        s.Arrange(same);
        const std::string before = cols(s);
        s.Leave(4, 0), s.Arrange(same);
        check("gap: never across Steam's slot (council, 2026-11-20)", split == 1 && before == "[ 1 ][ 4 2 ][ 3 ]" &&
                                                                         s.FillGap(tilegap::kQuietMs) && cols(s) == "[ 1 ][ 3 2 ]" && s.Grid().Split(0) == 1);
        // the keys alone in a column keep to their side of the slot, step over a column with nothing
        // shown, and leave the strip where it was (council, 2026-11-20)
        dashtiles::Board k;   // [ 1 ] |slot| [ 2 ][ 3 ]
        k.params.reserveAzDeg = 0, k.params.reserveDeg = 66;
        for (const int i : {1, 2, 3}) k.Anchor(i, -1, dashtiles::Stay::Default);
        k.Arrange(same);
        check("keys: down next to the slot: under the right one, not across",
              k.Grid().Split(0) == 1 && k.Press(2, dashtiles::Key::Down) && cols(k) == "[ 1 ][ 3 2 ]" && k.Grid().Split(0) == 1);
        dashtiles::Board k2;
        k2.params.reserveAzDeg = 0, k2.params.reserveDeg = 66;
        for (const int i : {1, 2, 3}) k2.Anchor(i, -1, dashtiles::Stay::Default);
        k2.Arrange(same);
        check("keys: up next to the slot: the right one in under it, not across",
              k2.Press(2, dashtiles::Key::Up) && cols(k2) == "[ 1 ][ 2 3 ]" && k2.Grid().Split(0) == 1);
        dashtiles::Board g;   // [ 1 ][ 2 ][ 3 ], 2 not shown
        for (const int i : {1, 2, 3}) g.Anchor(i, -1, dashtiles::Stay::Default);
        g.absent = {2};
        g.Arrange(same);
        check("keys: up past a column with nothing shown", g.Press(3, dashtiles::Key::Up) && cols(g) == "[ 2 ][ 3 1 ]");
        dashtiles::Board gd;   // [ 1 ][ 2 ][ 3 ], 2 not shown: 3 dragged down under itself
        for (const int i : {1, 2, 3}) gd.Anchor(i, -1, dashtiles::Stay::Default);
        gd.absent = {2};
        gd.Arrange(same);
        const tiling::Placed q3 = *gd.PlacedOf(3);
        const tiling::Drop down = gd.DropFor(3, q3.azDeg, gd.Last().deskDeg - (q3.el1 - q3.el0) / 2 - 1, same);
        check("drag: down past a column with nothing shown: under the one you see", down.kind == tiling::Drop::Into && down.below && down.col == 0);
        dashtiles::Board g2;
        for (const int i : {1, 2, 3}) g2.Anchor(i, -1, dashtiles::Stay::Default);
        g2.absent = {1};
        g2.Arrange(same);
        check("keys: down, nothing shown to the left: under the right one", g2.Press(2, dashtiles::Key::Down) && cols(g2) == "[ 1 ][ 3 2 ]");
        dashtiles::Board pan;
        for (const int i : {1, 2, 3}) pan.Anchor(i, -1, dashtiles::Stay::Default);
        pan.Arrange(same);
        const double az2 = pan.PlacedOf(2)->azDeg, az3 = pan.PlacedOf(3)->azDeg;
        pan.Press(2, dashtiles::Key::Up);
        pan.Arrange(same);
        check("keys: up: it and the others stay where they were (the pulled one moves)",
              cols(pan) == "[ 2 1 ][ 3 ]" && std::fabs(pan.PlacedOf(2)->azDeg - az2) < 1 && std::fabs(pan.PlacedOf(3)->azDeg - az3) < 1);
        dashtiles::Board tall;   // a stack two high already: a third under it would pass 50 degrees
        const auto big = [](int) { return dashtiles::Info{3.5, 1.97}; };
        for (const int i : {1, 2, 3}) tall.Anchor(i, -1, dashtiles::Stay::Default);
        tall.Press(2, dashtiles::Key::Up);
        tall.Arrange(big);
        const std::string two = cols(tall);
        check("keys: up when the one pulled in won't fit: nothing", !tall.Press(2, dashtiles::Key::Up) && cols(tall) == two);
        for (const int n : {3, 13}) {   // (13 columns shrink the strip: the test is on unshrunk sizes, council 2026-11-20)
            dashtiles::Board wide;
            const auto mid = [](int) { return dashtiles::Info{2.6, 1.5}; };
            for (int i = 1; i <= n; ++i) wide.Anchor(i, -1, dashtiles::Stay::Default);
            wide.Arrange(mid);
            check(n == 3 ? "keys: up, two 1.5 m panels a stack too tall: nothing" : "keys: ...the strip shrunk: still nothing",
                  (n == 3 || wide.Last().shrink < 0.9) && !wide.Press(2, dashtiles::Key::Up));
        }
        {   // a drag-lift refused leaves the strip turned as asked (council, 2026-11-20)
            const auto mid = [](int) { return dashtiles::Info{2.6, 1.5}; };
            dashtiles::Board a, r;
            for (const int i : {1, 2}) a.Anchor(i, -1, dashtiles::Stay::Default), r.Anchor(i, -1, dashtiles::Stay::Default);
            a.Arrange(mid), r.Arrange(mid);
            a.Recentre(1), r.Recentre(1);
            r.Drop(2, {tiling::Drop::Lift, 1, 0});
            a.Arrange(mid), r.Arrange(mid);
            check("drag: a lift refused: the strip still turns as asked", cols(r) == "[ 1 ][ 2 ]" && std::fabs(a.PanDeg()) > 1 &&
                                                                               std::fabs(r.PanDeg() - a.PanDeg()) < 1e-6);
            dashtiles::Board k;   // ...and a key refused (council, 2026-11-20)
            for (const int i : {1, 2}) k.Anchor(i, -1, dashtiles::Stay::Default);
            k.Arrange(mid), k.Recentre(1);
            const bool refused = !k.Press(2, dashtiles::Key::Up);
            k.Arrange(mid);
            check("keys: one refused: the strip still turns as asked", refused && cols(k) == "[ 1 ][ 2 ]" && std::fabs(k.PanDeg() - a.PanDeg()) < 1e-6);
        }
    }
    {   // dragged mostly over a column's top or under the desk line: into the nearest column, no quarters
        // (user, 2026-11-20: a middle-row panel "near impossible" to move up or down)
        const auto cols = [](const dashtiles::Board &b) {
            std::string o;
            for (const auto &c : b.Grid().Cols()) {
                o += "[";
                for (const auto &cell : c.cells) o += " " + std::to_string(cell.id);
                o += " ]";
            }
            return o;
        };
        const auto info = [](int id) { return id == 3 ? dashtiles::Info{3.5, 1.97} : dashtiles::Info{1.6, 0.9}; };
        const auto fresh = [&] {
            dashtiles::Board b;
            for (const int i : {1, 2, 3, 4}) b.Anchor(i, -1, dashtiles::Stay::Default);
            b.Arrange(info);
            return b;
        };
        dashtiles::Board b = fresh();
        tiling::Placed p = *b.PlacedOf(2);
        double half = (p.el1 - p.el0) / 2, desk = b.Last().deskDeg;
        tiling::Drop d = b.DropFor(2, p.az1 - 1, p.el1 + half, info);   // (up, on its right quarter)
        check("past: up off its middle, still a lift", d.kind == tiling::Drop::Lift);
        d = b.DropFor(2, p.azDeg + 2, desk - half - 1, info);   // (straight down, a little right)
        check("past: down under itself: under the nearest other column", d.kind == tiling::Drop::Into && d.below);
        b.Drop(2, d, p.azDeg);
        check("past: ...hanging under the big one", cols(b) == "[ 1 ][ 3 2 ][ 4 ]");
        b = fresh();
        p = *b.PlacedOf(2);
        const tiling::Placed p4 = *b.PlacedOf(4);
        d = b.DropFor(2, p4.az0 + 1, p4.el1 + half, info);   // (up, over the far neighbour's left quarter)
        b.Drop(2, d, p4.az0 + 1);
        check("past: up over a neighbour's edge: on top of it", d.kind == tiling::Drop::Into && cols(b) == "[ 1 ][ 3 ][ 2 4 ]");
        b = fresh();
        const tiling::Placed big = *b.PlacedOf(3);
        half = (big.el1 - big.el0) / 2, desk = b.Last().deskDeg;
        check("past: a tall one dragged under the strip isn't torn off", desk - half - 1 < b.Last().el0 - tiling::kTearMarginDeg);
        d = b.DropFor(3, p4.azDeg, desk - half - 1, info);
        b.Drop(3, d, p4.azDeg);
        check("past: ...it hangs under the nearest column", d.kind == tiling::Drop::Into && cols(b) == "[ 1 ][ 2 ][ 4 3 ]");
        b = fresh();
        p = *b.PlacedOf(2);
        d = b.DropFor(2, p.az1 - 1, p.elDeg, info);
        check("past: beside, on the row: the quarters still make a new column", d.kind == tiling::Drop::NewColumn);
        // the keys, alone in a column: Meta+Shift+Up lifts it (a neighbour in under it), Down hangs it
        b = fresh();
        check("keys: up, alone: the left neighbour in under it", b.Press(2, dashtiles::Key::Up) && cols(b) == "[ 2 1 ][ 3 ][ 4 ]");
        b = fresh();
        check("keys: down, alone: under the left neighbour", b.Press(2, dashtiles::Key::Down) && cols(b) == "[ 1 2 ][ 3 ][ 4 ]" &&
                                                                  (b.Arrange(info), b.PlacedOf(2)->below));
        b = fresh();
        check("keys: down, the first column: under the right one", b.Press(1, dashtiles::Key::Down) && cols(b) == "[ 2 1 ][ 3 ][ 4 ]");
        check("keys: up again: the base, standing on the desk", b.Press(1, dashtiles::Key::Up) && (b.Arrange(info), !b.PlacedOf(1)->below));
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
    // --- the centre display, its curve round the arc, and nudges (user, 2026-11-23)
    {
        const auto wide = [](int id) { return dashtiles::Info{id == 2 ? 3.4 : 1.6, 0.9}; };
        dashtiles::Board b;
        for (int id : {1, 2, 3}) b.Anchor(id, -1, dashtiles::Stay::Default);
        b.Arrange(wide);
        check("centre: the widest by default, straight ahead", b.Centre() == 2 && near(b.PlacedOf(2)->azDeg, 0, 1e-9));
        dashtiles::Board e;
        for (int id : {1, 2, 3, 4}) e.Anchor(id, -1, dashtiles::Stay::Default);
        e.Arrange(desk);
        check("centre: four as wide: one of the middle two straight ahead, not the seam",
              (e.Centre() == 2 || e.Centre() == 3) && near(e.PlacedOf(e.Centre())->azDeg, 0, 1e-9));
        e.steam = 2;
        e.Arrange(desk);
        check("centre: never Steam's window beside others", e.Centre() == 3);
        dashtiles::Board alone;
        alone.Anchor(2, -1, dashtiles::Stay::Default), alone.steam = 2;
        alone.Arrange(desk);
        check("centre: ...alone, it is", alone.Centre() == 2);
        const tilegrid::Where was1 = b.Grid().Find(1), was2 = b.Grid().Find(2);
        check("centre: taken by a drop on it: the two trade places", b.TakeCentre(1) && b.Grid().Find(1).col == was2.col &&
                                                                         b.Grid().Find(2).col == was1.col);
        b.Arrange(wide);
        check("centre: ...the new one straight ahead", b.Centre() == 1 && near(b.PlacedOf(1)->azDeg, 0, 1e-9));
        check("centre: ...taking it again: nothing", !b.TakeCentre(1));
        check("arc: only the centre display", !b.ToggleArc(2) && b.ToggleArc(1) && b.ArcNow(1) && b.Arc(1));
        b.absent = {1};
        check("arc: ...not while it's hidden", !b.ArcNow(1));
        b.absent.clear();
        b.Arrange(wide);
        const double az2 = b.PlacedOf(2)->azDeg, e3 = b.PlacedOf(3)->elDeg;
        check("nudge: up a degree", b.Press(3, dashtiles::Key::NudgeUp) && b.Press(3, dashtiles::Key::NudgeUp));
        b.Arrange(wide);
        check("nudge: ...it alone moves up, the others and the grid stay", near(b.PlacedOf(3)->elDeg, e3 + 2, 1e-9) &&
                                                                              near(b.PlacedOf(2)->azDeg, az2, 1e-9) && b.Nudge(2) == 0);
        dashtiles::State st;
        check("state: centre, arc and nudge remembered", dashtiles::Parse(b.Remember(1), &st) && st.centre && st.arc &&
                                                            dashtiles::Parse(b.Remember(3), &st) && near(st.nudgeDeg, 2) && !st.centre);
        dashtiles::Board r;
        for (int id : {1, 2, 3}) r.Anchor(id, -1, dashtiles::Stay::Default);
        r.Restore(3, b.Remember(3), -1);
        r.Restore(1, b.Remember(1), -1);
        r.Arrange(wide);
        check("state: ...back from it: the centre, curved, the other nudged",
              r.Centre() == 1 && r.ArcNow(1) && near(r.Nudge(3), 2));
        for (int i = 0; i < 40; ++i) b.Press(3, dashtiles::Key::NudgeDown);
        check("nudge: no more than 30 degrees", near(b.Nudge(3), -30));
        // picked up and let go where it's shown: its place in the grid, not a stack with a neighbour (council, 2026-11-24)
        const auto small = [](int) { return dashtiles::Info{0.8, 0.45}; };
        const auto cells = [](const dashtiles::Board &g) {
            std::string s;
            for (const auto &c : g.Grid().Cols()) {
                for (const auto &cell : c.cells) s += std::to_string(cell.id) + " ";
                s += "| ";
            }
            return s;
        };
        bool stays = true;
        for (const int presses : {5, 12, -12, -16}) {
            dashtiles::Board n;
            tiling::FromEyes(tiling::Eyes{1.4, -12, 0.8, 70}, 2.0, &n.params);
            n.params.reserveDeg = 0;
            for (int id : {1, 2, 3}) n.Anchor(id, id - 1, dashtiles::Stay::Default), n.Arrange(small);
            for (int i = 0; i < std::abs(presses); ++i) n.Press(2, presses > 0 ? dashtiles::Key::NudgeUp : dashtiles::Key::NudgeDown);
            n.Arrange(small);
            const std::string before = cells(n);
            const tiling::Placed at = *n.PlacedOf(2);
            n.Drop(2, n.DropFor(2, at.azDeg, at.elDeg, small), at.azDeg);
            n.Arrange(small);
            stays = stays && cells(n) == before && near(n.PlacedOf(2)->elDeg, at.elDeg, 1e-9);
        }
        check("nudge: a nudged tile let go where it is stays in its place, nudged", stays);
        // Dragged off where it's shown, it goes by where it's let go: back level with its row it stays
        // in its column (not into a neighbour's, drawn over it), and onto a neighbour's top it stacks
        // over that one, not under the desk line.
        bool level = true, onto = true;
        for (const int presses : {4, 6, 8, 15, 25, -4, -8, -14}) {
            dashtiles::Board n;
            tiling::FromEyes(tiling::Eyes{1.4, -12, 0.8, 70}, 2.0, &n.params);
            n.params.reserveDeg = 0;
            for (int id : {1, 2, 3}) n.Anchor(id, id - 1, dashtiles::Stay::Default), n.Arrange(small);
            for (int i = 0; i < std::abs(presses); ++i) n.Press(2, presses > 0 ? dashtiles::Key::NudgeUp : dashtiles::Key::NudgeDown);
            n.Arrange(small);
            const std::string before = cells(n);
            const tiling::Placed one = *n.PlacedOf(1), three = *n.PlacedOf(3);
            const tiling::Drop top = n.DropFor(2, one.azDeg, one.el1 - 1, small);
            onto = onto && top.kind == tiling::Drop::Into && top.col == 0 && top.row == 0 && !top.below;
            const double az = n.PlacedOf(2)->azDeg;
            n.Drop(2, n.DropFor(2, az, three.elDeg, small), az);
            n.Arrange(small);
            level = level && cells(n) == before;
        }
        {   // another let go on a nudged one's upper part, above where its column's top was: on it, not over the column
            dashtiles::Board n;
            tiling::FromEyes(tiling::Eyes{1.4, -12, 0.8, 70}, 2.0, &n.params);
            n.params.reserveDeg = 0;
            for (int id : {1, 2, 3}) n.Anchor(id, id - 1, dashtiles::Stay::Default), n.Arrange(small);
            for (int i = 0; i < 6; ++i) n.Press(1, dashtiles::Key::NudgeUp);
            n.Arrange(small);
            const tiling::Placed one = *n.PlacedOf(1);
            const tiling::Drop d = n.DropFor(3, one.azDeg, one.el1 - 1, small);
            check("nudge: ...the others are aimed at where their nudges show them", d.kind == tiling::Drop::Into && d.col == 0 && !d.over);
        }
        check("nudge: ...dragged back level with its row, it stays in its column", level);
        check("nudge: ...dragged onto a neighbour's top, it stacks over that one", onto);
        check("state: old lines read as before, flags round trip",
              dashtiles::Parse("tile 0 1 2 0 1.000", &st) && !st.centre && !st.arc && st.nudgeDeg == 0 &&
                  dashtiles::Parse("tile 0 1 2 0 1.000 dr centre arc nudge=-3.0", &st) && st.below && st.centre && st.arc &&
                  near(st.nudgeDeg, -3) && dashtiles::Text(st) == "tile 0 1 2 0 1.000 dr centre arc nudge=-3.0");
        check("state: a shelf of its own in the text",
              dashtiles::Parse("tile 0 1 2 0 1.000 dc row", &st) && st.ownRow && !st.arc && dashtiles::Text(st) == "tile 0 1 2 0 1.000 dc row" &&
                  !dashtiles::Parse("tile 0 1 2 0 1.000 dc row row", &st));
        b.TakeOff(1);
        b.Arrange(wide);
        check("centre: the chosen one taken off: the widest meanwhile, flat", b.Centre() == 2 && !b.ArcNow(1) && !b.ArcNow(2));
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Arrange(wide);
        check("centre: ...put back: the centre again, curved, straight ahead", b.Centre() == 1 && b.ArcNow(1) && near(b.PlacedOf(1)->azDeg, 0, 1e-9));
        b.TakeOff(1);
        b.Arrange(wide);
        b.TakeCentre(3);
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Arrange(wide);
        check("centre: ...another chosen meanwhile: that one", b.Centre() == 3 && !b.ArcNow(1));
        b.TakeCentre(1);
        b.Unanchor(1);
        b.Arrange(wide);
        check("centre: the chosen one gone: the widest again", b.Centre() == 2);
        b.Anchor(1, -1, dashtiles::Stay::Default);
        b.Arrange(wide);
        check("centre: ...another panel with its number: not the centre, flat", b.Centre() == 2 && !b.Arc(1));
    }
    {   // the profile's centre first (user, 2026-11-23: "profile saved choices should take priority")
        const auto same = [](int) { return dashtiles::Info{1.6, 0.9}; };
        dashtiles::Board b;
        for (int id : {1, 2, 3}) b.Anchor(id, -1, dashtiles::Stay::Default);
        b.profileCentre = "remote:pc";
        b.RestoreAll({{1, "tile 0 0 0 1 1.000 uc centre arc"}, {2, "tile 0 1 0 1 1.000"}});
        b.Arrange(same);
        check("profile: its centre", b.Centre() == 1 && b.ArcNow(1));
        b.Restore(3, "tile 0 2 0 1 1.000 uc centre", -1, "app:x");
        b.Arrange(same);
        check("profile: ...an app back, remembered as the centre, doesn't take it", b.Centre() == 1);
        const std::string line = b.Remember(1);
        b.Leave(1, 0, "remote:pc");
        b.Restore(3, "tile 0 2 0 1 1.000 uc centre", -1, "app:x");
        b.Arrange(same);
        check("profile: ...gone: the app's, meanwhile", b.Centre() == 3);
        b.Restore(1, line, -1, "remote:pc");
        b.Arrange(same);
        check("profile: ...back: the centre again, curved", b.Centre() == 1 && b.ArcNow(1));
        b.Restore(3, "tile 0 2 0 1 1.000 uc centre", -1, "app:x");
        b.Arrange(same);
        check("profile: ...and still not the app's", b.Centre() == 1);
        check("profile: ...a drop on it, by hand: that one", b.TakeCentre(2) && (b.Arrange(same), b.Centre() == 2));
    }
    {   // a reset with the dashboard closed (Steam's window hidden), then it opens (user, 2026-11-23:
        // "off to my side on a reset"): the ultrawide stays straight ahead, columns on both sides of it
        const auto mine = [](int id) {
            return id == 101 ? dashtiles::Info{3.4, 0.96} : id == 190 ? dashtiles::Info{1.9, 1.2} : dashtiles::Info{1.6, 2.4};   // (too tall to stack)
        };
        dashtiles::Board b;
        b.steam = 190;
        for (int id : {2, 3, 101, 5, 6, 190}) b.Anchor(id, -1, dashtiles::Stay::Default);
        b.absent = {190};
        b.Repack(mine);
        b.Arrange(mine);
        check("reset: the ultrawide the centre, straight ahead", b.Centre() == 101 && near(b.PlacedOf(101)->azDeg, 0, 1e-9));
        check("reset: ...it can be curved", b.ToggleArc(101) && b.ArcNow(101));
        b.absent.clear();
        b.Recentre();
        b.Arrange(mine);
        check("reset: ...the dashboard open: still it, curved", b.Centre() == 101 && b.ArcNow(101) && near(b.PlacedOf(101)->azDeg, 0, 1e-9));
        check("reset: ...the strip on both sides of you", b.Last().az0 < -30 && b.Last().az1 > 30);
        b.TakeCentre(190);
        b.absent = {190};
        b.Repack(mine);
        b.Arrange(mine);
        check("reset: ...Steam's window chosen, hidden: the wall's middle one that's shown", b.Centre() == 190 && b.WallCentre() >= 0 && b.WallCentre() != 190);
        const int col = b.Grid().Find(190).col;
        check("reset: ...its column in the middle", col > 0 && col < int(b.Grid().Cols().size()) - 1);
        b.absent.clear();
        b.Recentre();
        b.Arrange(mine);
        check("reset: ...shown: straight ahead, the strip on both sides", near(b.PlacedOf(190)->azDeg, 0, 1e-9) && b.Last().az0 < -30 &&
                                                                              b.Last().az1 > 30 && b.WallCentre() == 190);
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
