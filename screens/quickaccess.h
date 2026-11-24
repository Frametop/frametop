// Quick Access, the palm menu's card (user, 2026-11-18: "when someone looks at their palm, they can then
// tap or use a laser with their other hand to do quick actions"). Look at an open palm (palmmenu.h) and a
// card of six tiles shows just above it, facing you: a fingertip of the other hand poking a tile
// (tiptap.h), or a laser's click on it, does it. The palm hand's own pinch, one-handed (user, 2026-11-19:
// "let the user drag while tapping to select different menu options"): Steam Menu lights as it pinches
// (what the pinch alone did before), the card holds still, moving the pinched hand moves what's lit
// (Drag), and letting go does the lit tile; off the card, nothing.
//   The tiles are actions that touch every screen, the input relay's own (its "action NAME", the ones
// frunner's OS menu runs: conf/ft_input_actions.py's MENU_ACTIONS), so they keep its rules (this user
// only; while Frametop is paused for a VR game only Steam Menu works, and the rest dim: WorksPaused);
// Reset Layout resets at once (ft-layout's reset counts 3-2-1 in front of you itself).
//   Above the tiles, a header (user, 2026-11-19: "add to the palm menu a resource tracker ... a quick
// settings menu that when clicked opens a small window that has toggle options"): CPU, GPU, memory,
// temperature (sysstats.h) and the battery (battery.h) on the left, and on the right a button, kButton,
// that turns the card to its Quick Settings page (quicksettings.h's toggles in the tiles' places, the
// button then Back).
//   Under the tiles, while gaze mode is on, one wide row: Quick Eye Calibrate, the gaze service's quick
// check (user, 2026-11-22: "a quick eye calibrate option to the palm menu ... only an option if eye
// tracking is on"). The card is kEyesH tall then, kH without it; a card's height in pixels follows from its
// size in metres (PxH), so the pokes and the drag find the row with no more to say.
// Pure: the tiles, their layout, a fingertip's pokes and Reset's ask (screens/tests/quickaccess-test.cpp);
// vrquick.cpp draws the card, places it over the palm and gives it the tip and the clicks.
#pragma once

#include "tiptap.h"

#include <cmath>
#include <cstdint>

namespace quickaccess {

enum class Tile { SteamMenu, Screens, Reset, SpinLeft, SpinRight, DockAll, EyeCal };

struct Spec {
    Tile tile;
    const char *label;
    const char *action;   // the input relay's action (null: ft-screens does it)
};

constexpr int kCols = 3, kRows = 2, kTiles = kCols * kRows;
constexpr int kButton = kTiles;      // the header's button: Quick Settings, or Back on that page
constexpr int kEyeCal = kTiles + 1;  // the row under the tiles, with gaze mode on
constexpr int kSlots = kTiles + 2;   // what TileAt can say
constexpr Spec kSpecs[kTiles] = {
    {Tile::SteamMenu, "Steam Menu", "steam_menu"},
    {Tile::Screens, "Hide Screens", "screens_toggle"},
    {Tile::Reset, "Reset Layout\xe2\x80\xa6", nullptr},
    {Tile::SpinLeft, "Spin Left", "spin_prev"},
    {Tile::SpinRight, "Spin Right", "spin_next"},
    {Tile::DockAll, "Dock Windows", "dock_all"},
};
constexpr Spec kEyeSpec = {Tile::EyeCal, "Quick Eye Calibrate", "gaze_quickcal"};

// What still works while Frametop is paused for a VR game: the relay's PAUSED_ACTIONS
// (input/game_pause.py) has steam_menu and none of the others; Reset is ours, and the screens are hidden.
inline bool WorksPaused(Tile t) { return t == Tile::SteamMenu; }

// No press this soon after the card shows (the hand reaching over as it came), by tip or click.
constexpr int64_t kShownMs = 300;

// A fingertip presses a tile from this near its face (user, 2026-11-19: "increase the sensitivity on the
// detection of a tap so it's easier to trigger"; 3 mm, tiptap's own, before), and is out again past
// kPokeOut: 6 mm between them, as tiptap's 3 and 8 had 5 (council, 2026-11-19: 2 mm let a tip's depth
// jitter press the next tile it slid over). A quick slanted jab's one frame in front of the card can be
// beside it (a 2 m/s 30 degree jab at 15 Hz; quickaccess-test): kPokeSide past its edges (and tiptap's
// margin) still arms. The bar menu and the reset question keep tiptap's. 14/19 mm and 3 cm since the user's
// second ask (2026-11-19: "increase the sensitivity ... the taps while you're facing your palm"; 10/16/2 before).
constexpr double kPokeAt = 0.014, kPokeOut = 0.019, kPokeSide = 0.03;

// The card in pixels: a header kHead tall, then kCols x kRows tiles of kTile, kGap apart and kPad in
// from its edge.
constexpr int kTile = 150, kGap = 12, kPad = 16, kHead = 112;
constexpr int kTop = kPad + kHead + kGap;   // the first row of tiles
constexpr int kW = 2 * kPad + kCols * kTile + (kCols - 1) * kGap;
constexpr int kH = kTop + kPad + kRows * kTile + (kRows - 1) * kGap;
constexpr int kEyeH = 84;                      // the eye row's height
constexpr int kEyesH = kH + kGap + kEyeH;      // ...and the card's with it

// The card's height in pixels: with the eye row or not.
inline int Height(bool eyes) { return eyes ? kEyesH : kH; }
// ...for a card w x h metres (its pixels' shape), nearest of the two.
inline int PxH(double w, double h) { return w > 0 && h * kW > w * (kH + kEyesH) / 2.0 ? kEyesH : kH; }

// Tile i's box in pixels (x right, y down from the card's top left); kButton's, the header's right.
inline void Box(int i, int *x0, int *y0, int *x1, int *y1) {
    if (i == kEyeCal) {
        *x0 = kPad, *y0 = kH - kPad + kGap, *x1 = kW - kPad, *y1 = *y0 + kEyeH;
        return;
    }
    const int c = i == kButton ? kCols - 1 : i % kCols, r = i / kCols;
    *x0 = kPad + c * (kTile + kGap), *y0 = i == kButton ? kPad : kTop + r * (kTile + kGap);
    *x1 = *x0 + kTile, *y1 = *y0 + (i == kButton ? kHead : kTile);
}

// The header's left, where the readouts (or the page's title) are: not a tile.
inline void StatsBox(int *x0, int *y0, int *x1, int *y1) {
    *x0 = kPad, *y0 = kPad, *x1 = kPad + (kCols - 1) * kTile + (kCols - 2) * kGap, *y1 = kPad + kHead;
}

// The tile under pixel (x, y) (y down) on a card `h` pixels tall (Height), -1 none: kButton in the header's
// right, none in its left, kEyeCal under the tiles on a card with the eye row. The gaps between tiles go to
// the nearer one: a fingertip 4 mm off a tile's edge meant that tile, not nothing; past the card's padding, nothing.
inline int TileAt(double x, double y, int h = kH) {
    if (x < kPad / 2.0 || x > kW - kPad / 2.0 || y < kPad / 2.0 || y > h - kPad / 2.0) return -1;
    if (h == kEyesH && y >= kH - kPad + kGap / 2.0) return kEyeCal;
    auto cell = [](double p, int n) {
        const int k = int((p - kPad + kGap / 2.0) / (kTile + kGap));
        return k < 0 ? 0 : k >= n ? n - 1 : k;
    };
    if (y < kTop - kGap / 2.0) return cell(x, kCols) == kCols - 1 ? kButton : -1;
    return cell(y - (kTop - kPad), kRows) * kCols + cell(x, kCols);
}

// ...under a laser's mouse point (OpenVR's mouse y is up from the bottom; its scale, kW x h).
inline int TileAtMouse(double x, double yUp, int h = kH) { return TileAt(x, h - yUp, h); }

// A point on its face (metres from its centre, y up) for a card w x h metres, in pixels (y down).
inline void FaceToPx(double u, double v, double w, double h, double *x, double *y) {
    *x = (u / w + 0.5) * kW, *y = (0.5 - v / h) * PxH(w, h);
}

// ...the tile under it.
inline int TileAtFace(double u, double v, double w, double h) {
    if (w <= 0 || h <= 0) return -1;
    double x, y;
    FaceToPx(u, v, w, h, &x, &y);
    return TileAt(x, y, PxH(w, h));
}

// The tile a fingertip is over, kept while it's within kStick of that one's box (council, 2026-11-19:
// a tip trembling by a tile's edge lit one and then the other, ~15 redraws a second).
constexpr int kStick = 12;   // px, ~3 mm on a 12 cm card
inline int StickyTileAt(int prev, double x, double y, int h = kH) {
    if (prev >= 0 && prev < kSlots) {
        int x0, y0, x1, y1;
        Box(prev, &x0, &y0, &x1, &y1);
        if ((prev != kEyeCal || h == kEyesH) && x >= x0 - kStick && x <= x1 + kStick && y >= y0 - kStick && y <= y1 + kStick)
            return prev;
    }
    return TileAt(x, y, h);
}

// The other hand's index tip at the card, one hands frame at a time: the tile it's over (held at an
// edge: StickyTileAt) and the one it presses as it comes through (tiptap.h). After a press the card stays
// (the bar menu closes), so the tip is spent: resting or trembling by the face it mustn't press again
// (council, 2026-11-19: tiptap arms again 8 mm out, and one poke pressed twice). It's spent until it's
// seen kOut in front of the card, or in front of it and tiptap's reach (kMargin) past the tile it pressed: off
// the card on an outer side, or as far into another tile (StickyTileAt's hold alone is a tremble's
// width: council, 2026-11-19, a pull-back drifting sideways pressed the neighbour; and only in front, or
// where a poke went in: a slanted poke goes on sideways behind the card, and off it), or out of
// sight for kLostMs (one lost frame isn't the hand gone: holds last through 10, palmmenu waits 400 ms).
// While it's spent the tile it pressed stays lit (from the frame it's pressed), not the one beside it.
// A press is the tile under the tip, or, when the tip is behind the face by then, where it crossed it,
// between this frame and the last: a quick or slanted poke is centimetres on, and to the side.
class Poke {
public:
    static constexpr double kOut = 0.03;
    static constexpr int64_t kLostMs = 250;

    // Shown afresh at `nowMs`: a tip already through it doesn't count, and none presses for kShownMs.
    void Reset(int64_t nowMs) {
        tap_.Reset();
        hover_ = spent_ = -1, near_ = false, haveLast_ = false, lostMs_ = tipLostMs_ = -1, shownMs_ = nowMs;
    }
    // `l`: the tip on the card's face (metres from its centre: x right, y up, z out), or null; the card
    // w x h metres. The tile it pressed, or -1.
    int Step(const double *l, double w, double h, int64_t nowMs) {
        const double hw = w / 2, hh = h / 2, m = tiptap::Tap::kMargin;
        // (the tip out of sight, the palm in front of it as it reaches the card (user, 2026-11-19: the model
        // "doesn't handle occlusion well enough"): its way in holds for kLostMs, so it presses where it
        // shows again behind the face; longer, it's gone)
        if (l) tipLostMs_ = -1;
        else if (tipLostMs_ < 0) tipLostMs_ = nowMs;
        const bool gone = !l && nowMs - tipLostMs_ >= kLostMs;
        const bool through = l || gone ? tap_.Step(l, hw, hh) : false;
        const int held = hover_;
        near_ = l && tap_.Near();
        if (near_) {
            double x, y;
            FaceToPx(l[0], l[1], w, h, &x, &y);
            hover_ = StickyTileAt(hover_, x, y, PxH(w, h));
        } else {
            hover_ = -1;
        }
        // where it crossed the face (tiptap's through follows a frame in front, past kFront); still in front
        // of it (within kFace: a slow poke), where it is: council, 2026-11-19, the lerp's reach past the
        // last frame took a careful poke's jitter a tile over
        double at[3] = {l ? l[0] : 0, l ? l[1] : 0, 0};
        if (through && haveLast_ && l[2] < 0 && last_[2] > 0) {
            const double f = last_[2] / (last_[2] - l[2]);
            at[0] = last_[0] + f * (l[0] - last_[0]), at[1] = last_[1] + f * (l[1] - last_[1]);
        }
        if (spent_ >= 0 && !l) {
            if (lostMs_ < 0) lostMs_ = nowMs;
            if (nowMs - lostMs_ >= kLostMs) spent_ = -1;
        } else if (spent_ >= 0) {
            lostMs_ = -1;
            // (a slanted jab onto the next tile can be in reach in front and behind it the next frame: where
            // it went in counts too)
            if (l[2] > kOut || (l[2] > 0 && Past(spent_, l, w, h, m)) || (through && Past(spent_, at, w, h, m))) spent_ = -1;
        }
        if (spent_ >= 0 && hover_ >= 0) hover_ = spent_;
        if (l) haveLast_ = true, last_[0] = l[0], last_[1] = l[1], last_[2] = l[2];
        else if (gone) haveLast_ = false;
        if (!through || spent_ >= 0) return -1;
        double x, y;
        FaceToPx(at[0], at[1], w, h, &x, &y);
        const int t = StickyTileAt(held, x, y, PxH(w, h));
        if (t < 0) return -1;
        spent_ = t, lostMs_ = -1;   // (too soon after it showed: spent, so it comes out before it presses)
        if (near_) hover_ = t;
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
        if (x0 == kPad) x0 = 0;
        if (x1 == kW - kPad) x1 = kW;
        if (y0 == kPad) y0 = 0;
        const int H = PxH(w, h);
        if (y1 == H - kPad) y1 = H;
        double x, y;
        FaceToPx(l[0], l[1], w, h, &x, &y);
        const double mx = m / w * kW, my = m / h * H;
        return x < x0 - mx || x > x1 + mx || y < y0 - my || y > y1 + my;
    }

    tiptap::Tap tap_{kPokeAt, kPokeOut, kPokeSide};
    int hover_ = -1, spent_ = -1;   // spent_: the tile it pressed
    bool near_ = false, haveLast_ = false;
    double last_[3] = {0, 0, 0};    // the tip last frame
    int64_t lostMs_ = -1, shownMs_ = 0;
    int64_t tipLostMs_ = -1;   // the tip out of sight since (-1: seen)
};

// The palm hand's pinch, dragged across the card, which holds still meanwhile (vrquick.cpp). It starts
// on Steam Menu, where the pinch alone always went (on the Quick Settings page, on Back); the pinch moving on the card's face (x right, y up,
// metres) moves the lit point kGain times as far, from that tile's middle, so the far column is about
// 7 cm over and the second row 4 cm down. Held at a tile's edge it keeps that tile (StickyTileAt); up to
// kCancel past the card's edge, the edge tile (council, 2026-11-19: a still pinch drifting 2 cm up or left
// off Steam Menu did nothing); farther off, nothing is lit: let go there, nothing happens. The palm
// turned away lights nothing too, as its release does nothing then (palmmenu.h).
class Drag {
public:
    static constexpr double kGain = 1.0;
    static constexpr double kCancel = 0.015;   // m past the card's edge

    void Start(int from = int(Tile::SteamMenu)) { on_ = true, haveFrom_ = false, place_ = tile_ = start_ = from; }
    void End() { on_ = false, tile_ = -1; }
    // `at`: the pinch on the card's face this hands frame (null: not seen, what's lit stays, whatever
    // `up` says then: council, 2026-11-19, a lost frame blinked it off); the card w x h metres; `up`: the
    // palm still up. The tile lit, -1 none.
    int Step(const double *at, double w, double h, bool up) {
        if (!on_) return -1;
        if (!at || w <= 0 || h <= 0) return tile_;
        {
            if (!haveFrom_) from_[0] = at[0], from_[1] = at[1], haveFrom_ = true;
            int x0, y0, x1, y1;
            Box(start_, &x0, &y0, &x1, &y1);
            const int H = PxH(w, h);
            const double u0 = ((x0 + x1) / 2.0 / kW - 0.5) * w, v0 = (0.5 - (y0 + y1) / 2.0 / H) * h;
            double x, y;
            FaceToPx(u0 + kGain * (at[0] - from_[0]), v0 + kGain * (at[1] - from_[1]), w, h, &x, &y);
            const double mx = kCancel / w * kW, my = kCancel / h * H;
            if (x < -mx || x > kW + mx || y < -my || y > H + my) place_ = -1;
            else {
                // (over the header's readouts, which aren't a tile: the tile below them, so a pinch drifting up
                // off Steam Menu keeps it as it did when the card's top was there)
                const double cx = std::fmin(std::fmax(x, kPad), kW - kPad), cy = std::fmin(std::fmax(y, kPad), H - kPad);
                place_ = StickyTileAt(place_, cx, cy, H);
                if (place_ < 0) place_ = StickyTileAt(-1, cx, kTop, H);
            }
        }
        tile_ = up ? place_ : -1;
        return tile_;
    }
    bool on() const { return on_; }
    int tile() const { return tile_; }   // -1 none
    // The tile under it at its last point, the palm up or not (-1 off the card): what a release does
    // (palmmenu lets go only with the palm up), from the last frame it was held, not the release's.
    int place() const { return on_ ? place_ : -1; }

private:
    bool on_ = false, haveFrom_ = false;
    double from_[2] = {0, 0};
    int place_ = -1, tile_ = -1;   // place_: the tile under it, the palm up or not
    int start_ = int(Tile::SteamMenu);
};


}  // namespace quickaccess
