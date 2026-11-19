// Quick Access, the palm menu's card (user, 2026-11-18: "when someone looks at their palm, they can then
// tap or use a laser with their other hand to do quick actions"). Look at an open palm (palmmenu.h) and a
// card of six tiles shows just above it, facing you: a fingertip of the other hand poking a tile
// (tiptap.h), or a laser's click on it, does it. The palm hand's own pinch still opens the Steam menu, as
// the ring's did.
//   The tiles are actions that touch every screen, the input relay's own (its "action NAME", the ones
// frunner's OS menu runs: conf/ft_input_actions.py's MENU_ACTIONS), so they keep its rules (this user
// only; while Frametop is paused for a VR game only Steam Menu works, and the rest dim: WorksPaused);
// Reset Layout asks first: its first tap turns it red for kConfirmMs, and a second does it.
// Pure: the tiles, their layout, a fingertip's pokes and Reset's ask (screens/tests/quickaccess-test.cpp);
// vrquick.cpp draws the card, places it over the palm and gives it the tip and the clicks.
#pragma once

#include "tiptap.h"

#include <cmath>
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

// What still works while Frametop is paused for a VR game: the relay's PAUSED_ACTIONS
// (input/game_pause.py) has steam_menu and none of the others; Reset is ours, and the screens are hidden.
inline bool WorksPaused(Tile t) { return t == Tile::SteamMenu; }

// No press this soon after the card shows (the hand reaching over as it came), by tip or click.
constexpr int64_t kShownMs = 300;

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

// The other hand's index tip at the card, one hands frame at a time: the tile it's over (held at an
// edge: StickyTileAt) and the one it presses as it comes through (tiptap.h). After a press the card stays
// (the bar menu closes), so the tip is spent: resting or trembling by the face it mustn't press again
// (council, 2026-11-19: tiptap arms again 8 mm out, and one poke pressed twice). It's spent until it's
// seen kOut in front of the card, or in front of it and tiptap's reach (kMargin) past the tile it pressed: off
// the card on an outer side, or as far into another tile (StickyTileAt's hold alone is a tremble's
// width: council, 2026-11-19, a pull-back drifting sideways pressed the neighbour; and only in front: a
// slanted poke goes on sideways behind the card, and off it), or out of
// sight for kLostMs (one lost frame isn't the hand gone: holds last through 10, palmmenu waits 250 ms).
// While it's spent the tile it pressed stays the lit one, so what's lit is what a poke would press.
// A press is the tile under the tip, or, when the tip is behind the face by then, where it crossed it,
// between this frame and the last: a quick or slanted poke is centimetres on, and to the side.
class Poke {
public:
    static constexpr double kOut = 0.03;
    static constexpr int64_t kLostMs = 250;

    // Shown afresh at `nowMs`: a tip already through it doesn't count, and none presses for kShownMs.
    void Reset(int64_t nowMs) {
        tap_.Reset();
        hover_ = spent_ = -1, near_ = false, haveLast_ = false, lostMs_ = -1, shownMs_ = nowMs;
    }
    // `l`: the tip on the card's face (metres from its centre: x right, y up, z out), or null; the card
    // w x h metres. The tile it pressed, or -1.
    int Step(const double *l, double w, double h, int64_t nowMs) {
        const double hw = w / 2, hh = h / 2, m = tiptap::Tap::kMargin;
        const bool through = tap_.Step(l, hw, hh);
        const int held = hover_;
        near_ = l && tap_.Near();
        if (near_) {
            double x, y;
            FaceToPx(l[0], l[1], w, h, &x, &y);
            hover_ = StickyTileAt(hover_, x, y);
        } else {
            hover_ = -1;
        }
        if (spent_ >= 0 && !l) {
            if (lostMs_ < 0) lostMs_ = nowMs;
            if (nowMs - lostMs_ >= kLostMs) spent_ = -1;
        } else if (spent_ >= 0) {
            lostMs_ = -1;
            if (l[2] > kOut || (l[2] > 0 && Past(spent_, l, w, h, m))) spent_ = -1;
        }
        if (spent_ >= 0 && hover_ >= 0) hover_ = spent_;
        // where it crossed the face (tiptap's through follows a frame in front, past kFront); still in front
        // of it (within kFace: a slow poke), where it is: council, 2026-11-19, the lerp's reach past the
        // last frame took a careful poke's jitter a tile over
        double at[2] = {l ? l[0] : 0, l ? l[1] : 0};
        if (through && haveLast_ && l[2] < 0 && last_[2] > 0) {
            const double f = last_[2] / (last_[2] - l[2]);
            at[0] = last_[0] + f * (l[0] - last_[0]), at[1] = last_[1] + f * (l[1] - last_[1]);
        }
        haveLast_ = l != nullptr;
        if (l) last_[0] = l[0], last_[1] = l[1], last_[2] = l[2];
        if (!through || spent_ >= 0) return -1;
        double x, y;
        FaceToPx(at[0], at[1], w, h, &x, &y);
        const int t = StickyTileAt(held, x, y);
        if (t < 0) return -1;
        spent_ = t, lostMs_ = -1;   // (too soon after it showed: spent, so it comes out before it presses)
        return nowMs - shownMs_ >= kShownMs ? t : -1;
    }
    int hover() const { return hover_; }   // -1 none
    bool near() const { return near_; }     // the tip near its face (tiptap's kNear), this frame
    bool spent() const { return spent_ >= 0; }

private:
    // The tip `l` more than `m` metres past tile i's box, on a card w x h; on the card's outer sides, past
    // its edge (as tiptap's reach is).
    static bool Past(int i, const double *l, double w, double h, double m) {
        int x0, y0, x1, y1;
        Box(i, &x0, &y0, &x1, &y1);
        const int c = i % kCols, r = i / kCols;
        if (c == 0) x0 = 0;
        if (c == kCols - 1) x1 = kW;
        if (r == 0) y0 = 0;
        if (r == kRows - 1) y1 = kH;
        double x, y;
        FaceToPx(l[0], l[1], w, h, &x, &y);
        const double mx = m / w * kW, my = m / h * kH;
        return x < x0 - mx || x > x1 + mx || y < y0 - my || y > y1 + my;
    }

    tiptap::Tap tap_;
    int hover_ = -1, spent_ = -1;   // spent_: the tile it pressed
    bool near_ = false, haveLast_ = false;
    double last_[3] = {0, 0, 0};    // the tip last frame
    int64_t lostMs_ = -1, shownMs_ = 0;
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
