// Palm Quick Access (quickaccess.h): the tiles and their relay actions, which tile a fingertip or a
// laser lands on (gaps to the nearer tile, nothing past the padding, the mouse's y up, the face's metres),
// Reset's ask, the fingertip's hover held at a tile's edge and the tip spent after a press.
#include "../quickaccess.h"
#include "../tiptap.h"

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

    // After a press the tip is spent until it comes 3 cm out, goes off the card or out of sight.
    {
        Spent s;
        const double hw = 0.06, hh = 0.04;
        s.Pressed();
        const double resting[3] = {0.0, 0.0, 0.006}, out[3] = {0.0, 0.0, Spent::kOut + 0.001}, off[3] = {0.07, 0.0, 0.005};
        s.Step(resting, hw, hh);
        check("spent: resting 6 mm in front, still spent", s.on());
        s.Step(out, hw, hh);
        check("...3 cm out: not", !s.on());
        s.Pressed(), s.Step(off, hw, hh);
        check("...off the card's side: not", !s.on());
        s.Pressed(), s.Step(nullptr, hw, hh);
        check("...out of sight: not", !s.on());
        // the noisy pull-back the council found (mm): one press, however tiptap arms again
        tiptap::Tap tap;
        Spent sp;
        int presses = 0;
        for (const double mm : {60, 40, 20, 8, 1, -4, 10, 2, 12, 30, 9, 2, 14, 1}) {
            const double l[3] = {0.0, 0.0, mm / 1000.0};
            const bool through = tap.Step(l, hw, hh);
            sp.Step(l, hw, hh);
            if (through && !sp.on()) ++presses, sp.Pressed();
        }
        check("a noisy pull-back that never leaves 3 cm: one press", presses == 1);
    }

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
