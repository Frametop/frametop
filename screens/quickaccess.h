// Quick Access, the palm menu's card (user, 2026-11-18: "when someone looks at their palm, they can then
// tap or use a laser with their other hand to do quick actions"). Look at an open palm (palmmenu.h) and a
// card of six tiles shows just above it, facing you: a fingertip of the other hand poking a tile
// (tiptap.h), or a laser's click on it, does it. The palm hand's own pinch still opens the Steam menu, as
// the ring's did.
//   The tiles are actions that touch every screen, the input relay's own (its "action NAME", the ones
// frunner's OS menu runs: conf/ft_input_actions.py's MENU_ACTIONS), so they keep its rules (this user
// only, not while paused); Reset Layout asks first: its first tap turns it red for kConfirmMs, and a
// second does it.
// Pure: the tiles, their layout and Reset's ask (screens/tests/quickaccess-test.cpp); vrquick.cpp draws
// the card, places it over the palm and takes its pokes and clicks.
#pragma once

#include <cstdint>

namespace quickaccess {

enum class Tile { SteamMenu, Screens, Reset, SpinLeft, SpinRight, DockAll };

struct Spec {
    Tile tile;
    const char *label;
    const char *action;   // the input relay's action (null: ft-screens does it)
};

constexpr int kCols = 3, kRows = 2, kTiles = kCols * kRows;
constexpr Spec kSpecs[kTiles] = {
    {Tile::SteamMenu, "Steam Menu", "steam_menu"},
    {Tile::Screens, "Hide Screens", "screens_toggle"},
    {Tile::Reset, "Reset Layout\xe2\x80\xa6", nullptr},
    {Tile::SpinLeft, "Spin Left", "spin_prev"},
    {Tile::SpinRight, "Spin Right", "spin_next"},
    {Tile::DockAll, "Dock Windows", "dock_all"},
};

// The card in pixels: kCols x kRows tiles of kTile, kGap apart and kPad in from its edge.
constexpr int kTile = 150, kGap = 12, kPad = 16;
constexpr int kW = 2 * kPad + kCols * kTile + (kCols - 1) * kGap;
constexpr int kH = 2 * kPad + kRows * kTile + (kRows - 1) * kGap;

// Tile i's box in pixels (x right, y down from the card's top left).
inline void Box(int i, int *x0, int *y0, int *x1, int *y1) {
    const int c = i % kCols, r = i / kCols;
    *x0 = kPad + c * (kTile + kGap), *y0 = kPad + r * (kTile + kGap);
    *x1 = *x0 + kTile, *y1 = *y0 + kTile;
}

// The tile under pixel (x, y) (y down), -1 none. The gaps between tiles go to the nearer one: a
// fingertip 4 mm off a tile's edge meant that tile, not nothing; past the card's padding, nothing.
inline int TileAt(double x, double y) {
    if (x < kPad / 2.0 || x > kW - kPad / 2.0 || y < kPad / 2.0 || y > kH - kPad / 2.0) return -1;
    auto cell = [](double p, int n) {
        const int k = int((p - kPad + kGap / 2.0) / (kTile + kGap));
        return k < 0 ? 0 : k >= n ? n - 1 : k;
    };
    return cell(y, kRows) * kCols + cell(x, kCols);
}

// ...under a laser's mouse point (OpenVR's mouse y is up from the bottom; its scale, kW x kH).
inline int TileAtMouse(double x, double yUp) { return TileAt(x, kH - yUp); }

// A point on its face (metres from its centre, y up) for a card w x h metres, in pixels (y down).
inline void FaceToPx(double u, double v, double w, double h, double *x, double *y) {
    *x = (u / w + 0.5) * kW, *y = (0.5 - v / h) * kH;
}

// ...the tile under it.
inline int TileAtFace(double u, double v, double w, double h) {
    if (w <= 0 || h <= 0) return -1;
    double x, y;
    FaceToPx(u, v, w, h, &x, &y);
    return TileAt(x, y);
}

// The tile a fingertip is over, kept while it's within kStick of that one's box (council, 2026-11-19:
// a tip trembling by a tile's edge lit one and then the other, ~15 redraws a second).
constexpr int kStick = 12;   // px, ~3 mm on a 12 cm card
inline int StickyTileAt(int prev, double x, double y) {
    if (prev >= 0 && prev < kTiles) {
        int x0, y0, x1, y1;
        Box(prev, &x0, &y0, &x1, &y1);
        if (x >= x0 - kStick && x <= x1 + kStick && y >= y0 - kStick && y <= y1 + kStick) return prev;
    }
    return TileAt(x, y);
}

// After a fingertip pressed a tile: the card stays (the bar menu closes), so the same tip resting or
// trembling by its face mustn't press again (council, 2026-11-19: tiptap arms again 8 mm out, and one
// poke pressed twice). It's spent until it has come kOut in front of the card, gone off it, or out of sight.
class Spent {
public:
    static constexpr double kOut = 0.03;
    void Pressed() { on_ = true; }
    void Clear() { on_ = false; }
    // One hands frame: `l`, the tip on the card's face (x right, y up, z out, metres), or null.
    void Step(const double *l, double halfW, double halfH) {
        if (on_ && (!l || l[2] > kOut || l[0] < -halfW || l[0] > halfW || l[1] < -halfH || l[1] > halfH)) on_ = false;
    }
    bool on() const { return on_; }

private:
    bool on_ = false;
};

// Reset's ask: armed by its first tap, it resets on a second from kLeastMs to kConfirmMs after it (one
// poke's tremble, or a double click, sooner: not a second tap).
class Confirm {
public:
    static constexpr int64_t kConfirmMs = 3000;
    static constexpr int64_t kLeastMs = 400;
    // A tap on Reset at `nowMs`: true, do it (the second); false, it asks now (or still: too soon).
    bool Tap(int64_t nowMs) {
        if (Armed(nowMs)) {
            if (nowMs - armedMs_ < kLeastMs) return false;
            armedMs_ = -1;
            return true;
        }
        armedMs_ = nowMs;
        return false;
    }
    bool Armed(int64_t nowMs) const { return armedMs_ >= 0 && nowMs - armedMs_ < kConfirmMs; }
    void Clear() { armedMs_ = -1; }

private:
    int64_t armedMs_ = -1;
};

}  // namespace quickaccess
