// Palm Quick Access (quickaccess.h): the tiles and their relay actions, which tile a fingertip or a
// laser lands on (gaps to the nearer tile, nothing past the padding, the mouse's y up, the face's metres),
// and Reset's ask.
#include "../quickaccess.h"

#include <cstdio>
#include <cstring>

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

    // Reset asks: the first tap arms, a second inside kConfirmMs does it, one after doesn't.
    Confirm c;
    check("not armed at first", !c.Armed(0));
    check("the first tap asks", !c.Tap(1000) && c.Armed(1000));
    check("a second tap inside the window resets", c.Tap(1000 + Confirm::kConfirmMs - 1));
    check("...and it's disarmed after", !c.Armed(1000 + Confirm::kConfirmMs - 1));
    check("a tap arms again", !c.Tap(10000) && c.Armed(10000));
    check("it lapses at kConfirmMs", !c.Armed(10000 + Confirm::kConfirmMs));
    check("a tap after it lapsed asks again, not resets", !c.Tap(10000 + Confirm::kConfirmMs) && c.Armed(10000 + Confirm::kConfirmMs));
    c.Clear();
    check("Clear disarms it", !c.Armed(10000 + Confirm::kConfirmMs) && !c.Tap(20000));

    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
