// Palm Quick Access (quickaccess.h): the tiles and their relay actions, which tile a fingertip or a
// laser lands on (gaps to the nearer tile, nothing past the padding, the mouse's y up, the face's metres),
// Reset's ask, what works while paused, a fingertip's pokes (hover held at a tile's edge, quick pokes
// that end well behind the card, the tip spent after a press through tremble and lost frames, the easier
// 1 cm press) and the palm hand's pinch drag.
#include "../quickaccess.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using namespace quickaccess;

    // The actions are the relay's MENU_ACTIONS names (conf/ft_input_actions.py); Reset is ours.
    const char *relay[] = {"steam_menu", "screens_toggle", "spin_prev", "spin_next", "dock_all"};
    bool known = true, resetOurs = false;
    for (const Spec &s : kSpecs) {
        if (!s.action) {
            resetOurs = s.tile == Tile::Reset;
            continue;
        }
        bool found = false;
        for (const char *r : relay) found = found || !std::strcmp(r, s.action);
        known = known && found;
    }
    check("every tile but Reset runs a relay action", known && resetOurs);

    // Each tile's centre is that tile, in pixels, through the mouse (y up) and on the face.
    bool centres = true;
    for (int i = 0; i < kTiles; ++i) {
        int x0, y0, x1, y1;
        Box(i, &x0, &y0, &x1, &y1);
        const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
        centres = centres && TileAt(cx, cy) == i && TileAtMouse(cx, kH - cy) == i;
        const double w = 0.12, h = w * kH / kW;
        centres = centres && TileAtFace((cx / kW - 0.5) * w, (0.5 - cy / kH) * h, w, h) == i;
    }
    check("each tile's centre is it, in pixels, mouse and face", centres);
    check("the top left tile is Steam Menu, top on the face", TileAtFace(-0.04, 0.02, 0.12, 0.12 * kH / kW) == 0);
    check("the bottom right is Dock Windows", kSpecs[TileAtFace(0.04, -0.02, 0.12, 0.12 * kH / kW)].tile == Tile::DockAll);
    check("the mouse's y is up: low y is the bottom row", TileAtMouse(kW / 2.0, 20) == 4 && TileAtMouse(kW / 2.0, kH - 20) == 1);

    // The gaps go to the nearer tile.
    int a0, ay0, a1, ay1;
    Box(0, &a0, &ay0, &a1, &ay1);
    check("just right of tile 0 in the gap: tile 0", TileAt(a1 + kGap / 2.0 - 1, (ay0 + ay1) / 2.0) == 0);
    check("...just left of tile 1: tile 1", TileAt(a1 + kGap / 2.0 + 1, (ay0 + ay1) / 2.0) == 1);
    check("just below tile 0 in the gap: tile 0", TileAt((a0 + a1) / 2.0, ay1 + kGap / 2.0 - 1) == 0);
    check("...just above tile 3: tile 3", TileAt((a0 + a1) / 2.0, ay1 + kGap / 2.0 + 1) == 3);
    check("in the padding: the edge tile", TileAt(kPad - 2, kPad - 2) == 0 && TileAt(kW - kPad + 2, kH - kPad + 2) == 5);

    // Past the padding's outer half, and off the card, nothing.
    check("the card's very edge: nothing", TileAt(1, kH / 2.0) == -1 && TileAt(kW - 1, kH / 2.0) == -1);
    check("off the card: nothing", TileAt(-10, 10) == -1 && TileAt(kW + 10, kH + 10) == -1);
    check("off the face: nothing", TileAtFace(0.07, 0, 0.12, 0.12 * kH / kW) == -1);
    check("a card of no size: nothing", TileAtFace(0, 0, 0, 0) == -1);

    // A tip trembling by a tile's edge keeps that tile; well past it, the next.
    {
        int x0, y0, x1, y1;
        Box(0, &x0, &y0, &x1, &y1);
        const double mid = (y0 + y1) / 2.0, edge = x1 + kGap / 2.0;
        check("past the gap's middle, fresh: the next tile", StickyTileAt(-1, edge + 2, mid) == 1);
        check("...but tile 0 held: still tile 0", StickyTileAt(0, edge + 2, mid) == 0);
        check("...until it's kStick past tile 0's box", StickyTileAt(0, x1 + kStick + 1, mid) == 1);
        check("held, off the card: nothing", StickyTileAt(0, -40, mid) == -1);
        int flips = 0, prev = 0;
        for (int k = 0; k < 200; ++k) {   // +-2 px about the gap's middle
            const int now = StickyTileAt(prev, edge + ((k * 7919) % 5 - 2), mid);
            flips += now != prev, prev = now;
        }
        check("a 2 px tremble at the gap: no flips", flips == 0);
    }

    // A fingertip's pokes (Poke). The card as palmmenu makes it: 12 cm wide.
    {
        const double w = 0.12, h = w * kH / kW;
        int x0, y0, x1, y1;
        auto faceOf = [&](int i, double *u, double *v) {   // tile i's centre on the face
            Box(i, &x0, &y0, &x1, &y1);
            *u = ((x0 + x1) / 2.0 / kW - 0.5) * w, *v = (0.5 - (y0 + y1) / 2.0 / kH) * h;
        };
        // a run of frames, `hz` a second, from 1 s after it showed; `mm` the tip's z (mm), a NaN: unseen
        struct Frame { double u, v, mm; };
        auto run = [&](std::initializer_list<Frame> frames, double hz, int *tiles = nullptr) {
            Poke p;
            p.Reset(0);
            int presses = 0;
            int64_t t = 1000;
            for (const Frame &f : frames) {
                const double l[3] = {f.u, f.v, f.mm / 1000.0};
                const int got = p.Step(std::isnan(f.mm) ? nullptr : l, w, h, t);
                if (got >= 0) {
                    if (tiles) tiles[presses] = got;
                    ++presses;
                }
                t += int64_t(1000 / hz);
            }
            return presses;
        };
        const double lost = std::nan("");
        double u4, v4, u5, v5;
        faceOf(4, &u4, &v4), faceOf(5, &u5, &v5);
        int tiles[16];

        check("a poke on Spin Right presses it", run({{u4, v4, 60}, {u4, v4, 30}, {u4, v4, 10}, {u4, v4, -5}}, 30, tiles) == 1 && tiles[0] == 4);
        // quick pokes, the tip ending 4-6 cm behind the card (nothing stops it), at every phase
        bool quick = true;
        for (const double hz : {15.0, 30.0})
            for (const double ms : {1.0, 1.5, 2.0})
                for (int k = 0; k < 20; ++k) {
                    Poke p;
                    p.Reset(0);
                    const double step = ms * 1000 / hz, start = 80 + k * step / 20;
                    int presses = 0;
                    int64_t t = 1000;
                    for (double z = start; z > -60; z -= step, t += int64_t(1000 / hz)) {
                        const double l[3] = {u4, v4, z / 1000.0};
                        presses += p.Step(l, w, h, t) == 4;
                    }
                    const double l[3] = {u4, v4, -0.05};
                    presses += p.Step(l, w, h, t) == 4;
                    quick = quick && presses == 1;
                }
        check("quick pokes up to 2 m/s at 15 and 30 Hz, ending 5 cm behind: each presses once", quick);
        // slanted ones (council, 2026-11-19: by the through frame the tip is cm to the side): the tile it
        // crossed, Spin Right, every time
        bool slanted = true;
        for (const double hz : {15.0, 30.0})
            for (const double ms : {1.0, 1.5, 2.0})
                for (const double deg : {20.0, 30.0, 45.0})
                    for (const double dir : {-1.0, 1.0})
                        for (int k = 0; k < 20; ++k) {
                            Poke p;
                            p.Reset(0);
                            const double step = ms / hz, s = std::tan(deg * 3.14159265358979 / 180) * dir;
                            const double z0 = 0.08 + k * step / 20;
                            int right = 0, wrong = 0;
                            int64_t t = 1000;
                            for (double z = z0; z > -0.05 - step; z -= step * std::cos(deg * 3.14159265358979 / 180), t += int64_t(1000 / hz)) {
                                const double zz = z < -0.05 ? -0.05 : z;
                                const double l[3] = {u4 + s * zz, v4, zz};
                                const int got = p.Step(l, w, h, t);
                                right += got == 4, wrong += got >= 0 && got != 4;
                            }
                            // (45 degrees at 15 Hz and 1.5 m/s or more, the tip goes ~9 cm sideways a frame and
                            // is never seen in front over the card: tiptap never arms, so nothing; never another tile)
                            const bool reach = deg < 40 || hz > 20 || ms < 1.25;
                            slanted = slanted && wrong == 0 && (right == 1 || (!reach && right == 0));
                        }
        check("slanted quick pokes (20-45 degrees, either way): Spin Right once, never another tile", slanted);
        // slow careful pokes easing in (speed 3|z|, at least 1 cm/s) with +-2 mm sideways jitter, aimed all
        // over every tile but its outer 3 mm: that tile once, every time
        {
            unsigned seed = 12345;
            auto noise = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 8) % 4001) / 1000.0 - 2.0; };   // mm
            int wrong = 0, none = 0, twice = 0;
            for (const double hz : {15.0, 30.0})
                for (int i = 0; i < kTiles; ++i)
                    for (int a = 0; a < 25; ++a) {
                        Box(i, &x0, &y0, &x1, &y1);
                        const double in = 3 / 1000.0 * kW / w;   // 3 mm in px
                        const double ax = x0 + in + (x1 - x0 - 2 * in) * (a % 5) / 4.0, ay = y0 + in + (y1 - y0 - 2 * in) * (a / 5) / 4.0;
                        const double u = (ax / kW - 0.5) * w, v = (0.5 - ay / kH) * h;
                        Poke p;
                        p.Reset(0);
                        int64_t t = 1000;
                        int right = 0, other = 0;
                        for (double z = 0.04; z > -0.004; t += int64_t(1000 / hz)) {
                            const double l[3] = {u + noise() / 1000, v + noise() / 1000, z};
                            const int got = p.Step(l, w, h, t);
                            right += got == i, other += got >= 0 && got != i;
                            z -= std::fmax(3 * std::fabs(z), 0.01) / hz;
                        }
                        wrong += other > 0, none += right == 0, twice += right > 1;
                    }
            check("slow careful pokes with 2 mm jitter: the tile aimed at, once, every time", wrong == 0 && none == 0 && twice == 0);
        }
        // Spin Right pressed, the tip lifted 25 mm over its right edge, then a 30 degree jab at 1 m/s onto
        // Dock Windows: in reach of Spin Right in front, behind Dock Windows the next frame; it presses it
        {
            int sx0, sy0, sx1, sy1;
            Box(4, &sx0, &sy0, &sx1, &sy1);
            const double e = (sx1 - kW / 2.0) * w / kW;   // Spin Right's right edge (m)
            Poke p;
            p.Reset(0);
            int64_t t = 1000;
            int got = -1, presses = 0;
            for (const double mm : {40.0, 10.0, -3.0}) {   // press Spin Right
                const double l[3] = {u4, v4, mm / 1000};
                presses += p.Step(l, w, h, t += 33) >= 0;
            }
            const double s = std::tan(30 * 3.14159265358979 / 180);
            for (const double zmm : {25.0, 10.6, -18.3}) {   // at the edge, then the jab (1 m/s, 30 Hz)
                const double z = zmm / 1000, l[3] = {zmm > 20 ? e : e + 0.0083 + s * (0.0106 - z), v4, z};
                const int k = p.Step(l, w, h, t += 33);
                if (k >= 0) got = k, ++presses;
            }
            check("a slanted jab from over Spin Right's edge onto Dock Windows presses Dock Windows", presses == 2 && got == 5);
        }
        // ...but a jab that goes in within Spin Right's reach is a re-poke of it, however far past it the next
        // frame is (behind the card): pressed at its centre, pulled back to 16 or 20 mm over it, then a 45
        // degree jab at 1 m/s toward Dock Windows: no second press (council, 2026-11-19: the crossing, not
        // the frame after it, decides)
        {
            bool once = true;
            for (const double z0mm : {16.0, 20.0}) {
                Poke p;
                p.Reset(0);
                int64_t t = 1000;
                int presses = 0;
                auto step = [&](double u, double z) {
                    const double l[3] = {u, v4, z};
                    presses += p.Step(l, w, h, t += 33) >= 0;
                };
                step(u4, 0.04), step(u4, 0.01), step(u4, -0.002);   // the press
                const double z0 = z0mm / 1000;
                for (double z = 0.004; z < z0; z += 0.004) step(u4, z);
                step(u4, z0);
                const double dz = 1.0 / 30 * std::cos(3.14159265358979 / 4);
                for (double z = z0 - dz / 2;; z -= dz) {
                    const double zz = z < -0.04 ? -0.04 : z;
                    step(u4 + (z0 - zz), zz);
                    if (z < -0.04) break;
                }
                once = once && presses == 1;
            }
            check("a 45 degree jab going in within Spin Right's reach, past it behind: no second press", once);
        }
        // the last two frames 3.1 then 2.9 mm out, 2 mm apart sideways (the lerp once took that a tile over)
        check("a slow poke's last frames 3.1 -> 2.9 mm, 2 mm jitter: Spin Right",
              run({{u4, v4, 40}, {u4, v4, 20}, {u4, v4, 10}, {u4, v4, 5}, {u4, v4, 3.1}, {u4 + 0.002, v4, 2.9}}, 30, tiles) == 1 && tiles[0] == 4);
        // Dock Windows pressed 2.5 mm in from the card's right edge; the tip wanders 7 mm off the edge, lifts
        // to 9 mm and comes back in: within tiptap's reach, one press
        {
            const double ex = w / 2 - 0.0025;
            check("an outer tile's press, the tip wandering 7 mm off the card's edge and back: one press",
                  run({{ex, v5, 40}, {ex, v5, 10}, {ex, v5, 1}, {w / 2 + 0.007, v5, 4}, {w / 2 + 0.007, v5, 9}, {ex, v5, 9}, {ex, v5, 1}}, 30) == 1);
        }
        // a 20 or 30 degree poke at 1 m/s at Spin Right's centre, ending 5 cm behind, then a noisy pull-back
        // along the same line: one press (at 45, or at 30 aimed 12 mm or more off centre toward the hand, the
        // pull-back's 28 mm frame is a centimetre past the tile: it left it, and may press it again)
        {
            bool once = true;
            for (const double deg : {20.0, 30.0})
                for (const double dir : {-1.0, 1.0}) {
                    const double s = std::tan(deg * 3.14159265358979 / 180) * dir;
                    Poke p;
                    p.Reset(0);
                    int64_t t = 1000;
                    int presses = 0;
                    for (const double mm : {80.0, 51.0, 22.0, -7.0, -36.0, -50.0, -50.0, 10.0, 2.0, 12.0, 28.0, 9.0, 2.0, 14.0, 1.0, 35.0}) {
                        const double z = mm / 1000, l[3] = {u4 + s * z, v4, z};
                        presses += p.Step(l, w, h, t += 33) >= 0;
                    }
                    once = once && presses == 1;
                }
            check("a slanted poke on past the card, then a noisy pull-back: one press", once);
        }

        check("resting 6 mm in front after it: spent", run({{u4, v4, 40}, {u4, v4, -2}, {u4, v4, 6}, {u4, v4, 10}, {u4, v4, 1}}, 30) == 1);
        check("...3 cm out and in again: a second press", run({{u4, v4, 40}, {u4, v4, -2}, {u4, v4, 31}, {u4, v4, 10}, {u4, v4, 1}}, 30) == 2);
        check("...over to Dock Windows (lifted 2 cm): it presses", run({{u4, v4, 40}, {u4, v4, -2}, {u4, v4, 20}, {u5, v5, 20}, {u5, v5, 10}, {u5, v5, 1}}, 30, tiles) == 2 && tiles[1] == 5);
        check("...out of sight 8 frames (231 ms): spent", run({{u4, v4, 40}, {u4, v4, -2}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {u4, v4, 20}, {u4, v4, 1}}, 30) == 1);
        check("...10 frames (297 ms): a second press", run({{u4, v4, 40}, {u4, v4, -2}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {0, 0, lost}, {u4, v4, 20}, {u4, v4, 1}}, 30) == 2);
        // the noisy pull-back the council found (mm): one press, however tiptap arms again
        check("a noisy pull-back that never leaves 3 cm: one press",
              run({{u4, v4, 60}, {u4, v4, 40}, {u4, v4, 20}, {u4, v4, 8}, {u4, v4, 1}, {u4, v4, -4}, {u4, v4, 10}, {u4, v4, 2}, {u4, v4, 12},
                   {u4, v4, 28}, {u4, v4, 9}, {u4, v4, 2}, {u4, v4, 14}, {u4, v4, 1}}, 30) == 1);
        check("...with the hand lost a frame after the press: one press",
              run({{u4, v4, 40}, {u4, v4, 20}, {u4, v4, 8}, {u4, v4, -4}, {0, 0, lost}, {u4, v4, 10}, {u4, v4, 2}, {0, 0, lost}, {u4, v4, 12}, {u4, v4, 1}}, 30) == 1);
        // pressed 4 mm inside Steam Menu's right edge, then a noisy pull-back drifting 10 mm toward Hide Screens
        {
            int b0, by0, b1, by1;
            Box(0, &b0, &by0, &b1, &by1);
            const double px = w / kW, at = (b1 - 4 / 1000.0 / px - kW / 2.0) * px, v0 = (kH / 2.0 - (by0 + by1) / 2.0) * h / kH;
            const double drift = 0.010, zs[] = {40, 20, 8, 1, -4, 10, 2, 12, 28, 9, 2, 14, 1};
            Poke p;
            p.Reset(0);
            int presses = 0, other = 0;
            int64_t t = 1000;
            for (int k = 0; k < 13; ++k) {
                const double u = at + (k > 4 ? drift * (k - 4) / 8 : 0), l[3] = {u, v0, zs[k] / 1000.0};
                const int got = p.Step(l, w, h, t += 33);
                presses += got >= 0, other += got > 0;
            }
            check("a pull-back drifting 10 mm sideways across a tile edge: one press, Steam Menu", presses == 1 && other == 0);
        }
        // pressed 2.5 mm in from the card's right edge, trembling 3 mm sideways off it and back
        const double edge = w / 2 - 0.0025, side = w / 2 + 0.0005;
        check("pressed by the card's edge, a sideways tremble off it: one press",
              run({{edge, 0, 40}, {edge, 0, 10}, {edge, 0, 1}, {side, 0, 2}, {edge, 0, 10}, {edge, 0, 2}, {side, 0, 12}, {edge, 0, 1}}, 30) == 1);
        // (15 mm over it: out past kPokeOut)
        check("...well off it (2 cm) and back: a second press", run({{edge, 0, 40}, {edge, 0, 1}, {w / 2 + 0.02, 0, 15}, {edge, 0, 15}, {edge, 0, 1}}, 30) == 2);
        // hover: near lights the tile it's over; held at the edge
        {
            Poke p;
            p.Reset(0);
            const double l[3] = {u4, v4, 0.05};
            p.Step(l, w, h, 1000);
            check("a tip 5 cm in front: near, Spin Right lit", p.near() && p.hover() == 4);
            const double far[3] = {u4, v4, 0.12};
            p.Step(far, w, h, 1033);
            check("...12 cm: not near, nothing lit", !p.near() && p.hover() == -1);
        }
        // while spent, the tile it pressed is the lit one: from the press frame of a 45 degree poke (a tile
        // over by then), and with the tip 6 mm into Spin Left after
        {
            Poke p;
            p.Reset(0);
            int64_t t = 1000;
            int pressed = -1, litAtPress = -2;
            for (const double mm : {40.0, 15.0, -25.0}) {   // through 25 mm behind, over Spin Left by then
                const double z = mm / 1000, l[3] = {u4 + z, v4, z};
                const int got = p.Step(l, w, h, t += 33);
                if (got >= 0) pressed = got, litAtPress = p.hover();
            }
            check("a slanted poke's press frame lights the tile it pressed", pressed == 4 && litAtPress == 4);
            int bx0, by0, bx1, by1;
            Box(4, &bx0, &by0, &bx1, &by1);
            const double in6 = (bx0 - kGap - 6 / 1000.0 * kW / w - kW / 2.0) * w / kW;   // 6 mm inside Spin Left
            bool held = true;
            for (const double mm : {15.0, 10.0, 1.0, -3.0}) {
                const double l[3] = {in6, v4, mm / 1000};
                const int got = p.Step(l, w, h, t += 33);
                held = held && got < 0 && p.hover() == 4;
            }
            check("...and stays lit while spent with the tip over Spin Left", held);
        }
        // the easier press (kPokeAt, user 2026-11-19): a tip easing in that stops 9 mm out presses; 13 mm, not
        check("a tip easing in that stops 9 mm out: a press", run({{u4, v4, 60}, {u4, v4, 30}, {u4, v4, 15}, {u4, v4, 9}, {u4, v4, 20}}, 30, tiles) == 1 && tiles[0] == 4);
        check("...one that stops 13 mm out: nothing", run({{u4, v4, 60}, {u4, v4, 30}, {u4, v4, 15}, {u4, v4, 13}, {u4, v4, 20}}, 30) == 0);
        // a frame in front beside the card (kPokeSide) arms it; farther, not
        {   // (crossing ~7 mm in from the card's edge)
            const double in = w / 2 - 0.012;
            check("in front 1.5 cm beside the card, then through Dock Windows: a press",
                  run({{w / 2 + 0.015, v5, 20}, {in, v5, -5}}, 30, tiles) == 1 && tiles[0] == 5);
            check("...3.5 cm beside it: nothing", run({{w / 2 + 0.035, v5, 20}, {in, v5, -5}}, 30) == 0);
        }
        // too soon after it showed: no press, and the tip comes out before it can
        {
            Poke p;
            p.Reset(1000);
            int64_t t = 1000;
            int presses = 0;
            for (const double mm : {40.0, 10.0, -3.0}) {
                const double l[3] = {u4, v4, mm / 1000.0};
                presses += p.Step(l, w, h, t += 33) >= 0;
            }
            check("a poke within kShownMs of showing: nothing", presses == 0);
            for (const double mm : {10.0, 1.0}) {
                const double l[3] = {u4, v4, mm / 1000.0};
                presses += p.Step(l, w, h, t += 400) >= 0;
            }
            check("...and its tremble after: nothing", presses == 0);
        }
    }

    // The palm hand's pinch drag (Drag): from Steam Menu, the pinch's moves on the card's face, 1:1.
    {
        const double w = 0.12, h = w * kH / kW, pitch = (kTile + kGap) * w / kW;
        auto at = [](double x, double y) { static double p[3]; p[0] = x, p[1] = y, p[2] = 0.01; return p; };
        Drag d;
        check("not dragging: nothing", !d.on() && d.Step(at(0, 0), w, h, true) == -1);
        d.Start();
        check("a pinch starts on Steam Menu", d.on() && d.tile() == int(Tile::SteamMenu));
        check("...unseen: still Steam Menu", d.Step(nullptr, w, h, true) == 0);
        const double x0 = 0.31, y0 = -0.2;   // (anywhere: it's the moves that count)
        check("...its first point: Steam Menu", d.Step(at(x0, y0), w, h, true) == 0);
        check("a tile right: Hide Screens", d.Step(at(x0 + pitch, y0), w, h, true) == 1);
        check("two right: Reset", d.Step(at(x0 + 2 * pitch, y0), w, h, true) == 2);
        check("down one, left two: Spin Left", d.Step(at(x0, y0 - pitch), w, h, true) == 3);
        check("down one, right one: Spin Right", d.Step(at(x0 + pitch, y0 - pitch), w, h, true) == 4);
        check("down one, right two: Dock Windows", d.Step(at(x0 + 2 * pitch, y0 - pitch), w, h, true) == 5);
        check("...unseen: still Dock Windows", d.Step(nullptr, w, h, true) == 5);
        check("the palm turned away: nothing", d.Step(nullptr, w, h, false) == -1 && d.tile() == -1);
        check("...back up: Dock Windows again", d.Step(nullptr, w, h, true) == 5);
        check("off the card: nothing", d.Step(at(x0 + 0.2, y0), w, h, true) == -1);
        d.End();
        check("let go: done, nothing", !d.on() && d.tile() == -1 && d.Step(at(x0, y0), w, h, true) == -1);
        d.Start();
        d.Step(at(0, 0), w, h, true);
        check("a fresh pinch starts again from Steam Menu, at its own first point", d.Step(at(0.002, 0), w, h, true) == 0);
        check("past the gap's middle by 2 px from Steam Menu: still Steam Menu",
              d.Step(at(pitch / 2 + 2 * w / kW, 0), w, h, true) == 0);
        check("...past kStick: Hide Screens", d.Step(at(pitch / 2 + (kGap / 2.0 + kStick + 1) * w / kW, 0), w, h, true) == 1);
    }

    check("paused for a game, only Steam Menu works", WorksPaused(Tile::SteamMenu) && !WorksPaused(Tile::Screens) &&
                                                          !WorksPaused(Tile::Reset) && !WorksPaused(Tile::SpinLeft) &&
                                                          !WorksPaused(Tile::SpinRight) && !WorksPaused(Tile::DockAll));

    // Reset asks: the first tap arms, a second from kLeastMs to kConfirmMs does it, one after doesn't.
    Confirm c;
    check("not armed at first", !c.Armed(0));
    check("the first tap asks", !c.Tap(1000) && c.Armed(1000));
    check("a second tap 100 ms after: too soon, still asking", !c.Tap(1100) && c.Armed(1100));
    check("...at kLeastMs: resets", c.Tap(1000 + Confirm::kLeastMs));
    check("the first tap asks again", !c.Tap(5000) && c.Armed(5000));
    check("a second tap inside the window resets", c.Tap(5000 + Confirm::kConfirmMs - 1));
    check("...and it's disarmed after", !c.Armed(1000 + Confirm::kConfirmMs - 1));
    check("a tap arms again", !c.Tap(10000) && c.Armed(10000));
    check("it lapses at kConfirmMs", !c.Armed(10000 + Confirm::kConfirmMs));
    check("a tap after it lapsed asks again, not resets", !c.Tap(10000 + Confirm::kConfirmMs) && c.Armed(10000 + Confirm::kConfirmMs));
    c.Clear();
    check("Clear disarms it", !c.Armed(10000 + Confirm::kConfirmMs) && !c.Tap(20000));

    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
