// The grab bar's menu (user, 2026-11-18, Option C, "the bar is the menu": "a quick tap opens up the menu
// on the drag bar, and a hold and press acts as the current behavior"). A panel's controls are its bar
// and its resize corner; everything its round buttons did is a row here, so a pooled set of controls is
// two overlays, not ten (chromepool.h). Pure: no OpenVR (screens/tests/panelmenu-test.cpp).
// vrbarmenu.cpp draws the rows and does what they say; vr.cpp tells a tap on the bar from a hold.
#pragma once

#include "barside.h"

#include <cstdint>
#include <vector>

namespace panelmenu {

enum class Item {
    Curve,         // curved round the head, or flat (a toggle)
    Rotate,        // held: turn it like a knob; tapped: level it
    KeepInView,    // pinned to the head (a toggle; desktop screens and remote displays)
    OnDashboard,   // anchored to the SteamVR dashboard (a toggle)
    StayShown,     // ...and shown with the dashboard closed too (a toggle, while anchored)
    MoveAll,       // held: carry every screen, as they stand (desktop screens and remote displays)
    Dock,          // a floating window back onto its desktop screen
    Close,         // a floating window closed
    Theater,       // View in Theater (Steam's tile, a Steam game's panel)
    BarPosition,   // the edges page
    Reset,         // reset the layout (asks first)
    Back,          // the edges page back to the main one
    Edge,          // the bar on that edge (Row::side)
};

struct Row {
    Item item;
    barside::Side side = barside::Side::Bottom;   // an Edge row's
    bool operator==(const Row &o) const { return item == o.item && (item != Item::Edge || side == o.side); }
};

// What the panel is, for the rows it gets.
struct Kind {
    bool floating = false;   // a floating window
    bool gsapp = false;      // a gamescope app's panel
    bool theater = false;    // View in Theater offered
    bool anchored = false;   // on the dashboard now
};

// Desktop screens and remote displays have the layout's own rows (in view, move all, reset).
inline bool LayoutRows(const Kind &k) { return !k.floating && !k.gsapp; }

// The main page: the panel's own rows first, the layout's next, reset last (it asks before it does).
inline std::vector<Row> Main(const Kind &k) {
    std::vector<Row> r = {{Item::Curve}, {Item::Rotate}};
    if (LayoutRows(k)) r.push_back({Item::KeepInView});
    r.push_back({Item::OnDashboard});
    if (k.anchored) r.push_back({Item::StayShown});
    if (k.floating) r.push_back({Item::Dock}), r.push_back({Item::Close});
    if (k.theater) r.push_back({Item::Theater});
    if (LayoutRows(k)) r.push_back({Item::MoveAll});
    r.push_back({Item::BarPosition});
    if (LayoutRows(k)) r.push_back({Item::Reset});
    return r;
}

// The edges page. From the main page it starts with Back; from a right click on the bar (as before
// Option C) it's the edges alone, and View in Theater under them where the panel has it.
inline std::vector<Row> Edges(const Kind &k, bool back) {
    std::vector<Row> r;
    if (back) r.push_back({Item::Back});
    for (barside::Side s : barside::kAll) r.push_back({Item::Edge, s});
    if (!back && k.theater) r.push_back({Item::Theater});
    return r;
}

inline const char *Label(const Row &r) {
    switch (r.item) {
        case Item::Curve: return "Curved";
        case Item::Rotate: return "Rotate";
        case Item::KeepInView: return "Keep in View";
        case Item::OnDashboard: return "On Dashboard";
        case Item::StayShown: return "Stay Shown";
        case Item::MoveAll: return "Move All Screens";
        case Item::Dock: return "Back to Desktop";
        case Item::Close: return "Close Window";
        case Item::Theater: return "View in Theater";
        case Item::BarPosition: return "Bar Position";
        case Item::Reset: return "Reset Layout\xe2\x80\xa6";
        case Item::Back: return "Back";
        case Item::Edge: return barside::Label(r.side);
    }
    return "";
}

// The toggles (a dot shows them on) and the current edge.
inline bool Toggle(Item i) {
    return i == Item::Curve || i == Item::KeepInView || i == Item::OnDashboard || i == Item::StayShown || i == Item::Edge;
}

// Pressed and held, not clicked: these start on the press and end on the release, as their buttons did.
inline bool Held(Item i) { return i == Item::Rotate || i == Item::MoveAll; }

// A rule above it: the start of a group.
inline bool RuleAbove(const std::vector<Row> &rows, size_t at) {
    if (at == 0 || at >= rows.size()) return false;
    const Item i = rows[at].item, before = rows[at - 1].item;
    if (i == Item::Reset) return true;
    if (i == Item::Theater && before == Item::Edge) return true;
    return (i == Item::MoveAll || i == Item::BarPosition) && before != Item::MoveAll;
}

// ---------------------------------------------------------------- a tap on the bar

// The press starts a move at once, as it always has (no wait before a drag); a release soon after
// with the pressing device barely moved was a tap: the panel goes back where it was and the menu opens.
// The device's own movement, not the panel's: a 1 degree wobble moves a panel 2 m out 3.5 cm.
struct TapLimits {
    int64_t ms = 250;          // held under this long
    double metres = 0.015;     // the device moved under this
    double radians = 0.0524;   // ...and turned under this (3 degrees: the roll button's click, kRollTap)
};

inline bool IsTap(int64_t heldMs, double movedM, double turnedRad, const TapLimits &l = {}) {
    return heldMs >= 0 && heldMs < l.ms && movedM < l.metres && turnedRad < l.radians;
}

}  // namespace panelmenu
