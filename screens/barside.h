// Which edge of its panel a panel's grab bar and buttons sit on (user, 2026-11-18: "if you right-click
// on the grab bar, a context menu will open up that lets you choose for that one display where the
// grab bar should actually be... bottom, top, left, or right. And if it's left or right, then the grab
// bar should be vertical rather than horizontal, and all the buttons should stack on top of it").
// Pure geometry and the context menu's rows: no OpenVR (screens/tests/barside-test.cpp).
// vrbarmenu.cpp keeps each panel's choice and draws the menu; vr.cpp places the controls by it.
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

// Down a side: the bar stands on end.
inline bool Vertical(Side s) { return s == Side::Left || s == Side::Right; }

struct UV {
    double u, v;   // metres from the panel's centre: u right (along its surface), v up
};

// A spot in the bar's row: `along` metres along the row from the bar's middle (positive: the side
// the curve and roll buttons are on; negative: dock, close, reset, pin, layout and anchor), `in`
// metres in from the panel's edge, on a panel `w` wide and `h` high. The row sits inside the panel
// (user, 2026-11-18: "the overlay should be inside of the display": past the edge, a bar between two
// stacked tiles was hard to grab), over its content while it shows.
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
// the panel's middle. Past the edge is the gap to the next tile, and the panel's plane runs on behind
// that tile (the tiles curve round the head), so a popup there was hidden; over its own panel
// nothing covers it.
// `at`: the clicked spot on the bar; `barHalf`: the bar's half thickness; mw, mh: its size.
inline UV MenuAt(Side s, UV at, double barHalf, double gap, double mw, double mh) {
    const UV o = Outward(s);
    const double reach = barHalf + gap + (Vertical(s) ? mw : mh) / 2;
    return {at.u - o.u * reach, at.v - o.v * reach};
}

}  // namespace barside
