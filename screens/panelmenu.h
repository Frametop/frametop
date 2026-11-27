// The grab bar's menu (user, 2026-11-18, Option C, "the bar is the menu": "a quick tap opens up the menu
// on the drag bar, and a hold and press acts as the current behavior"). A panel's controls are its bar
// and its resize corner; everything its round buttons did is a row here, so a pooled set of controls is
// two overlays, not ten (chromepool.h). Pure: no OpenVR (screens/tests/panelmenu-test.cpp).
// vrbarmenu.cpp draws the rows and does what they say; vr.cpp tells a tap on the bar from a hold.
#pragma once

#include "appscale.h"
#include "barside.h"

#include <cstdint>
#include <string>
#include <vector>

namespace panelmenu {

enum class Item {
    Curve,         // curved round the head, or flat (a tile, blue when on)
    Rotate,        // held: turn it like a knob; tapped: level it (a tile)
    Dock,          // a floating window back onto its desktop screen (a tile)
    Close,         // a floating window closed (a tile)
    Placement,     // the placement page (its value: where the panel lives)
    InRoom,        // placement: left in the room
    KeepInView,    // placement: pinned to the head (desktop screens and remote displays)
    OnDashboard,   // placement: pinned to the SteamVR dashboard on its own, a panel in the room that rides on it (desktop screens and remote displays)
    Tiled,         // placement: on the dashboard's tiles (dashtiles.h)
    StayShown,     // ...and shown with the dashboard closed too (checked when on, while anchored)
    CentreDisplay, // ...and the tiles' centre display, straight ahead (checked when it is; a tile)
    MoveAll,       // held: carry every panel, in the room and on the dashboard, as they stand (not from one on the head or a wrist)
    Theater,       // View in Theater (Steam's tile, a Steam game's panel)
    BarPosition,   // the edges page (its value: the bar's edge)
    Reset,         // reset the layout (asks first)
    Back,          // a page back to the main one
    Edge,          // the bar on that edge (Row::side)
    Inside,        // the bar inside the panel's edge, over its content
    Outside,       // the bar past the panel's edge
    Scale,         // a PC app window's scale page (its value: the scale)
    ScaleStep,     // that scale (Row::percent)
};

struct Row {
    Item item;
    barside::Side side = barside::Side::Bottom;   // an Edge row's
    int percent = 100;                            // a ScaleStep row's
    bool operator==(const Row &o) const {
        return item == o.item && (item != Item::Edge || side == o.side) && (item != Item::ScaleStep || percent == o.percent);
    }
};

// What the panel is, for the rows it gets.
struct Kind {
    bool floating = false;   // a floating window
    bool gsapp = false;      // a gamescope app's panel
    bool theater = false;    // View in Theater offered
    bool anchored = false;   // on the dashboard now
    bool scales = false;     // a PC app window's panel whose PC takes a scale for it
    bool rides = false;      // pinned to the head or a wrist: it goes with that, not with the others
    bool centres = false;    // a tile that can be the centre display (on the grid, not Steam's page)
};

// Desktop screens and remote displays have the layout's own rows (in view, reset).
inline bool LayoutRows(const Kind &k) { return !k.floating && !k.gsapp; }

// The pages (user, 2026-11-19: the Option C design): Main's quick tiles on top, then its list (Placement,
// View in Theater, Move All Windows, Bar Position, Reset Layout); Placement's and the edges' choices;
// a PC app window's scales.
enum class Page { Main, Placement, Edges, Scale };

// Quick tiles: the toggles and actions flipped often, in a strip across the top of the main page.
inline bool Tile(Item i) { return i == Item::Curve || i == Item::Rotate || i == Item::Dock || i == Item::Close; }

inline std::vector<Row> Main(const Kind &k) {
    std::vector<Row> r;  // (reserved, then pushed: GCC 16's -O2 -Warray-bounds misreads growing an initializer list)
    r.reserve(11);
    r.push_back({Item::Curve}), r.push_back({Item::Rotate});
    if (k.floating) r.push_back({Item::Dock}), r.push_back({Item::Close});
    r.push_back({Item::Placement});
    if (k.scales) r.push_back({Item::Scale});
    if (k.theater) r.push_back({Item::Theater});
    if (!k.rides) r.push_back({Item::MoveAll});
    r.push_back({Item::BarPosition});
    if (LayoutRows(k)) r.push_back({Item::Reset});
    return r;
}

// Where the panel lives: in the room, in view and on the dashboard (the layout's panels), tiled, and
// Stay Shown while it's tiled (user, 2026-11-27: the dashboard pin "completely separate from tiled mode"); a tile, Centre Display (user, 2026-11-26: a drop onto the centre display
// took it only within 4 degrees of its middle, hard to hit with a big panel).
inline std::vector<Row> Placement(const Kind &k) {
    std::vector<Row> r;
    r.reserve(7);
    r.push_back({Item::Back}), r.push_back({Item::InRoom});
    if (LayoutRows(k)) r.push_back({Item::KeepInView}), r.push_back({Item::OnDashboard});
    r.push_back({Item::Tiled});
    if (k.anchored) r.push_back({Item::StayShown});
    if (k.anchored && k.centres) r.push_back({Item::CentreDisplay});
    return r;
}

// The edges page. From the main page it starts with Back; from a right click on the bar (as before
// Option C) it's the edges alone, and View in Theater under them where the panel has it. Under the
// edges, inside that edge or past it (barside::Inset): this panel's own choice.
inline std::vector<Row> Edges(const Kind &k, bool back) {
    std::vector<Row> r;
    if (back) r.push_back({Item::Back});
    for (barside::Side s : barside::kAll) r.push_back({Item::Edge, s});
    r.push_back({Item::Outside}), r.push_back({Item::Inside});
    if (!back && k.theater) r.push_back({Item::Theater});
    return r;
}

// A PC app window's scales.
inline std::vector<Row> Scales() {
    std::vector<Row> r;
    r.push_back({Item::Back});
    for (int p : appscale::kPercents) r.push_back({Item::ScaleStep, barside::Side::Bottom, p});
    return r;
}

inline std::vector<Row> Rows(Page p, const Kind &k, bool back) {
    if (p == Page::Scale) return k.scales ? Scales() : Main(k);
    return p == Page::Placement ? Placement(k) : p == Page::Edges ? Edges(k, back) : Main(k);
}

inline std::string Label(const Row &r) {
    switch (r.item) {
        case Item::Curve: return "Curved";
        case Item::Rotate: return "Rotate";
        case Item::Dock: return "To Desktop";
        case Item::Close: return "Close";
        case Item::Placement: return "Placement";
        case Item::InRoom: return "In the Room";
        case Item::KeepInView: return "Keep in View";
        case Item::OnDashboard: return "On Dashboard";
        case Item::Tiled: return "Tiled";
        case Item::StayShown: return "Stay Shown";
        case Item::CentreDisplay: return "Centre Display";
        case Item::MoveAll: return "Move All Windows";
        case Item::Theater: return "View in Theater";
        case Item::BarPosition: return "Bar Position";
        case Item::Reset: return "Reset Layout";
        case Item::Back: return "Back";
        case Item::Edge: return barside::Label(r.side);
        case Item::Inside: return "Inside the Edge";
        case Item::Outside: return "Outside the Edge";
        case Item::Scale: return "Scale";
        case Item::ScaleStep: return appscale::Label(r.percent);
    }
    return "";
}

// One of a page's choices: the one picked gets a check.
inline bool Choice(Item i) {
    return i == Item::InRoom || i == Item::KeepInView || i == Item::OnDashboard || i == Item::Tiled || i == Item::Edge || i == Item::Inside ||
           i == Item::Outside || i == Item::ScaleStep;
}
// On or off (Curved's tile blue, Stay Shown and Centre Display checked; Centre Display once on is
// turned off only by choosing another).
inline bool Toggle(Item i) { return i == Item::Curve || i == Item::StayShown || i == Item::CentreDisplay; }
// It opens a page (a chevron at its right).
inline bool Opens(Item i) { return i == Item::Placement || i == Item::BarPosition || i == Item::Scale; }

// Pressed and held, not clicked: these start on the press and end on the release, as their buttons did.
inline bool Held(Item i) { return i == Item::Rotate || i == Item::MoveAll; }

// A rule above it: the start of a group (the tiles' own rule is the layout's).
inline bool RuleAbove(const std::vector<Row> &rows, size_t at) {
    if (at == 0 || at >= rows.size()) return false;
    const Item i = rows[at].item, before = rows[at - 1].item;
    return i == Item::Reset || i == Item::StayShown || (i == Item::Theater && before == Item::Inside);
}

// The grey capitals over a page's choices, or none.
inline const char *Caption(const std::vector<Row> &rows, size_t at) {
    if (at >= rows.size()) return nullptr;
    const Item i = rows[at].item, before = at > 0 ? rows[at - 1].item : Item::Back;
    if (i == Item::InRoom) return "Placement";
    if (i == Item::ScaleStep && before != Item::ScaleStep) return "Scale";
    if (i == Item::Edge && before != Item::Edge) return "Controls Position";
    if (i == Item::Outside) return "Outside or Inside";
    return nullptr;
}

// ---------------------------------------------------------------- a tap on the bar

// A press on the bar waits before it moves the panel (user, 2026-11-19: "It should wait a second to half
// a second before actually moving the display when you grab it", so a tap doesn't make it jump). Held
// still that long, the move starts (CarryNow); moved or turned past the limits sooner, it starts then,
// so a quick swing isn't left behind. Either way it starts from where the panel is then: nothing jumps.
// Let go before either, still, and it was a tap: the menu opens. The panel never moved.
// The device's own movement, not the panel's: a 1 degree wobble moves a panel 2 m out 3.5 cm.
struct TapLimits {
    int64_t ms = 500;          // the hold before the move starts (a soft trigger pull takes 300 to 400 ms)
    double metres = 0.02;      // the device moved under this
    double radians = 0.07;     // ...and turned under this (4 degrees)
};

inline bool Still(double movedM, double turnedRad, const TapLimits &l = {}) {
    return movedM < l.metres && turnedRad < l.radians;
}

// The press held this long, the device moved and turned this much: does the move start now?
inline bool CarryNow(int64_t heldMs, double movedM, double turnedRad, const TapLimits &l = {}) {
    return heldMs >= l.ms || !Still(movedM, turnedRad, l);
}

// Let go now: was it a tap (the move never started)?
inline bool IsTap(int64_t heldMs, double movedM, double turnedRad, const TapLimits &l = {}) {
    return heldMs >= 0 && !CarryNow(heldMs, movedM, turnedRad, l);
}

// A press waiting on the bar, each tick (vr.cpp's BarHold): it waits on, its move starts, or it's dropped.
// Dropped: the panel's already moving (`busy`), the laser has no pose, the panel isn't shown (hidden,
// paused). A press that would carry while its controller holds nothing (`holds` 0; 1 held, -1 unknown,
// SteamVR tells only in VR games) lost its release somewhere: no move with no button down.
enum class Wait { Keep, Carry, Drop };
inline Wait HoldStep(int64_t heldMs, double movedM, double turnedRad, bool busy, bool posed, bool shown, int holds,
                     const TapLimits &l = {}) {
    if (busy || !posed || !shown) return Wait::Drop;
    if (!CarryNow(heldMs, movedM, turnedRad, l)) return Wait::Keep;
    return holds == 0 ? Wait::Drop : Wait::Carry;
}

}  // namespace panelmenu
