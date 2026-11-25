// Which edge of its panel a panel's grab bar and buttons sit on (user, 2026-11-18: "if you right-click
// on the grab bar, a context menu will open up that lets you choose for that one display where the
// grab bar should actually be... bottom, top, left, or right. And if it's left or right, then the grab
// bar should be vertical rather than horizontal, and all the buttons should stack on top of it").
// And whether the row sits inside that edge, over the panel's content, or past it (a tester, 2026-11-24:
// "the new grab bar overlaps every display and window, so i can't click in the bottom center of the
// window without clicking the bar"; user: "Make it toggle able in a setting. Per display in the menu
// options when you tap on it to choose for it to be outside or inside in addition to which side of the
// display it sits on").
// Pure geometry and the context menu's rows: no OpenVR (screens/tests/barside-test.cpp).
// vrbarside.cpp keeps each panel's choices, vrbarmenu.cpp draws the menu; vr.cpp places the controls by them.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace barside {

enum class Side { Bottom, Top, Left, Right };
constexpr int kSides = 4;
constexpr Side kAll[kSides] = {Side::Bottom, Side::Top, Side::Left, Side::Right};   // the menu's order

inline const char *Name(Side s) {
    switch (s) {
        case Side::Top: return "top";
        case Side::Left: return "left";
        case Side::Right: return "right";
        default: return "bottom";
    }
}
inline const char *Label(Side s) {
    switch (s) {
        case Side::Top: return "Top";
        case Side::Left: return "Left";
        case Side::Right: return "Right";
        default: return "Bottom";
    }
}
inline bool Parse(const std::string &text, Side *out) {
    for (Side s : kAll)
        if (text == Name(s)) return *out = s, true;
    return false;
}

// Inside the panel's edge or past it. Default: the panel has no choice of its own, the setting
// decides (GRAB_BAR_INSIDE).
enum class Inset { Default, Inside, Outside };
inline const char *Name(Inset i) { return i == Inset::Inside ? "inside" : i == Inset::Outside ? "outside" : "default"; }
inline bool Parse(const std::string &text, Inset *out) {
    for (Inset i : {Inset::Default, Inset::Inside, Inset::Outside})
        if (text == Name(i)) return *out = i, true;
    return false;
}
inline bool Inside(Inset own, bool setting) { return own == Inset::Default ? setting : own == Inset::Inside; }

// A panel's choices as its memory keeps them: "EDGE", or "EDGE inside" / "EDGE outside" with a choice
// of its own (an older build reads the first form only, and takes the other two as nothing remembered).
inline std::string Text(Side s, Inset i) { return i == Inset::Default ? Name(s) : std::string(Name(s)) + " " + Name(i); }
inline bool Parse(const std::string &text, Side *side, Inset *inset) {
    const size_t gap = text.find(' ');
    Side s;
    Inset i = Inset::Default;
    if (!Parse(text.substr(0, gap), &s)) return false;
    if (gap != std::string::npos && (!Parse(text.substr(gap + 1), &i) || i == Inset::Default)) return false;
    return *side = s, *inset = i, true;
}

// Down a side: the bar stands on end.
inline bool Vertical(Side s) { return s == Side::Left || s == Side::Right; }

struct UV {
    double u, v;   // metres from the panel's centre: u right (along its surface), v up
};

// A spot in the bar's row: `along` metres along the row from the bar's middle (positive: the side
// the curve and roll buttons are on; negative: dock, close, reset, pin, layout and anchor), `in`
// metres in from the panel's edge (negative: past it; In says which), on a panel `w` wide and `h` high.
//   Bottom and Top: the row along the bottom or the top edge.
//   Left and Right: the row turned down the side; what sat left of the bar goes above it, the curve
// and roll buttons below.
inline UV Place(Side s, double w, double h, double in, double along) {
    switch (s) {
        case Side::Top: return {along, h / 2 - in};
        case Side::Left: return {-(w / 2 - in), -along};
        case Side::Right: return {w / 2 - in, -along};
        default: return {along, -(h / 2 - in)};
    }
}

// How far in from the panel's edge the row's line is; negative: past the edge.
//   Inside (user, 2026-11-18: "the overlay should be inside of the display"): `clear` in, the row clear
// of the edge by its gap, over the panel's content while it shows.
//   Outside: `clear` out, so nothing of the row is over the content, a dashboard tile's too. For a
// day (2026-11-24) a tile's bar stopped at its half of the gap to the next tile, which with the usual
// gap left most of it over the tile's own edge (user: "it actually just moves to the top of the display
// instead of floating outside of the display like the behavior used to"). So it hangs clear of its tile
// as a floating panel's does, over the gap and the next tile's edge when the gap is narrower than the
// bar; a panel whose bar is hard to grab there can have it inside (its bar's menu).
inline double In(bool inside, double clear) { return inside ? clear : -clear; }

// The bar's turn about its own face, radians: none along the bottom or the top; a quarter turn
// clockwise down a side, so the bar's own +x (its mouse x) runs down, as `along` does there.
inline double Roll(Side s) { return Vertical(s) ? -M_PI / 2 : 0; }

// Away from the panel, at that edge (a unit step in u, v).
inline UV Outward(Side s) {
    switch (s) {
        case Side::Top: return {0, 1};
        case Side::Left: return {-1, 0};
        case Side::Right: return {1, 0};
        default: return {0, -1};
    }
}

struct Box {
    double u0, u1, v0, v1;
    bool Contains(double u, double v) const { return u >= u0 && u <= u1 && v >= v0 && v <= v1; }
};

// The row from `alongLo` to `alongHi`, `half` metres either side of its line: a box on the panel's surface.
inline Box Row(Side s, double w, double h, double in, double alongLo, double alongHi, double half) {
    const UV a = Place(s, w, h, in, alongLo), b = Place(s, w, h, in, alongHi);
    return {std::min(a.u, b.u) - half, std::max(a.u, b.u) + half, std::min(a.v, b.v) - half, std::max(a.v, b.v) + half};
}
inline Box Union(const Box &a, const Box &b) {
    return {std::min(a.u0, b.u0), std::max(a.u1, b.u1), std::min(a.v0, b.v0), std::max(a.v1, b.v1)};
}

// ---------------------------------------------------------------- the context menu

// Its layout and its look are menulayout.h's and barmenuart.h's.

// Its centre on the panel's surface: beside the bar at the clicked spot, a gap clear of it, toward
// the panel's middle (a bar past the edge too: over the panel's edge then). Past the edge is the gap
// to the next tile, and the panel's plane runs on behind that tile (the tiles curve round the head),
// so a popup there was hidden; over its own panel nothing covers it.
// `at`: the clicked spot on the bar; `barHalf`: the bar's half thickness; mw, mh: its size.
inline UV MenuAt(Side s, UV at, double barHalf, double gap, double mw, double mh) {
    const UV o = Outward(s);
    const double reach = barHalf + gap + (Vertical(s) ? mw : mh) / 2;
    return {at.u - o.u * reach, at.v - o.v * reach};
}

}  // namespace barside
