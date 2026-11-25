// Where a dragged dashboard tile goes when it's let go, against the layout as it is (tiling.h's
// Arrange). Header only, no OpenVR, so its test (tests/tiling-test.cpp) runs anywhere.
#pragma once

#include "tiling.h"
#include "tilewide.h"

#include <algorithm>
#include <vector>

namespace tiling {

// Where a dragged tile let go at (az, el) goes: into a column at a row, a new column at an index, or
// off the grid (torn off: it stays anchored where it is). A drop on a column's middle half stacks it
// there, above or below the panel it's on (under the desk line: hanging under it), leaning to the
// side of the column it was let go on; on its outer quarters, or a gap, a new column that side.
//
// Let go in the space over its own column's top, already the top: it goes up, a neighbouring column's
// top panel in under it (Lift; user, 2026-11-20: dragged up into an empty space "it just pulls the
// display back down to where it was before instead of pulling displays from the right or left to
// fill in the gap and put the display up higher").
//
// Mostly (three quarters of it) over a column's top or under the desk line, there are no quarters:
// it goes into the column whose panel is nearest, on top or hanging under, the others making room
// (user, 2026-11-20: "If I drag a display to an empty space in the row below it should attach it to
// the closest anchor and push some things slightly to the side"). Under the desk, its own column
// doesn't count when nothing of it stands over the desk without it. `self`: the dragged panel (its
// own place is in `l`), which also moves the margins out to its edges.
//
// Anywhere over or under a super wide display, its own ends too, it goes on the shelf there, at the
// place along it it was let go (tilewide.h).
struct Drop {
    enum Kind { Into, NewColumn, TearOff, Lift } kind = TearOff;
    int col = 0, row = 0;   // Into: the column and the row it takes; NewColumn: the index it takes
    bool below = false;     // Into: under the desk line
    int align = 0;          // Into: the side of the column it leans against (tilegrid.h's Cell::align); Lift: the side to pull from first
    bool over = false;      // Into: let go over every panel in the column
    bool ownRow = false;    // Into: it starts a shelf (tilewide.h's WideSpot::own), `before` no longer does
    int before = -1;
    bool leftOfSlot = false;   // NewColumn: let go left of Steam's slot (at the split: which side it goes)
};
constexpr double kTearMarginDeg = 10;   // its edge this far past the strip, under the desk or over the tallest column: torn off

inline Drop DropAt(const Layout &l, const std::vector<Column> &cols, double az, double el, int self = -1) {
    Drop d;
    if (cols.empty()) return d.kind = Drop::NewColumn, d.col = 0, d;
    az = WrapNear(az, (l.az0 + l.az1) / 2);
    double half = 0, halfAz = 0;   // the dragged panel's half height and half width, and its column
    int own = -1;
    for (const Placed &p : l.tiles)
        if (p.id == self) half = (p.el1 - p.el0) / 2, halfAz = (p.az1 - p.az0) / 2, own = p.col;
    // (round you too: a wide tile set down beside the strip has its middle half its width past it, torn off
    // by the bare margin; user, 2026-11-24: "dragging the window from above Dodie to the side of Dodie doesn't work")
    if (az < l.az0 - kTearMarginDeg - halfAz || az > l.az1 + kTearMarginDeg + halfAz || el < l.el0 - kTearMarginDeg - half ||
        el > l.el1 + kTearMarginDeg + half)
        return d;
    d.leftOfSlot = l.split >= 0 && az < l.slotAz;
    if (az < l.az0) return d.kind = Drop::NewColumn, d.col = 0, d;
    if (az > l.az1) return d.kind = Drop::NewColumn, d.col = int(cols.size()), d;
    if (const Placed *w = OverWide(l, az, el, self)) {
        const WideSpot s = SpotOnWide(l, *w, int(cols[size_t(w->col)].tiles.size()), az, el, half, self);
        return d.kind = Drop::Into, d.col = w->col, d.row = s.row, d.below = s.below, d.align = s.align, d.ownRow = s.own, d.before = s.before, d;
    }
    // the column it's over: nearest round you, then up (packed columns can share an azimuth)
    const auto gap = [&](const Placed &p, double *dAz, double *dEl) {
        *dAz = az < p.az0 ? p.az0 - az : az > p.az1 ? az - p.az1 : 0;
        *dEl = el < p.el0 ? p.el0 - el : el > p.el1 ? el - p.el1 : 0;
    };
    // (a column with nothing shown, laid out at no size: Board::absent's, never aimed at)
    const auto ghost = [&](int col) {
        for (const Tile &t : cols[size_t(col)].tiles)
            if (t.widthM > 0 || t.heightM > 0) return false;
        return true;
    };
    const auto over = [&](int skipCol) {
        int col = -1;
        double bAz = 1e9, bEl = 1e9, dAz, dEl;
        for (const Placed &p : l.tiles) {
            if (p.col == skipCol || ghost(p.col)) continue;
            gap(p, &dAz, &dEl);
            if (dAz < bAz - 1e-9 || (dAz < bAz + 1e-9 && dEl < bEl)) bAz = dAz, bEl = dEl, col = p.col;
        }
        return col;
    };
    const auto topOf = [&](int col) {
        double t = -1e9;
        for (const Placed &p : l.tiles)
            if (p.col == col) t = std::max(t, p.el1);
        return t;
    };
    const double past = half / 2;   // (three quarters of it past an edge)
    int c = over(-1);
    if (c < 0) return d;
    const bool under = el < l.deskDeg - past;
    if (under && c == own) {   // (its own column, standing on nothing without it: the nearest other one)
        bool stands = false;
        for (const Placed &p : l.tiles) stands = stands || (p.col == own && p.id != self && !p.below);
        if (!stands && over(own) >= 0) c = over(own);
    }
    if (!under && el <= topOf(c) + past) {   // beside a panel: the nearest one's column, its outer quarters a new one
        double best = 1e9, a0 = 0, a1 = 0, dAz, dEl;
        for (const Placed &p : l.tiles) {
            if (ghost(p.col)) continue;
            gap(p, &dAz, &dEl);
            if (dAz + dEl < best) best = dAz + dEl, c = p.col, a0 = p.az0, a1 = p.az1;
        }
        const double u = (az - a0) / std::max(1e-6, a1 - a0);
        if (u < 0.25) return d.kind = Drop::NewColumn, d.col = c, d;
        if (u > 0.75) return d.kind = Drop::NewColumn, d.col = c + 1, d;
    }
    double c0 = 1e9, c1 = -1e9;
    for (const Placed &p : l.tiles)
        if (p.col == c) c0 = std::min(c0, p.az0), c1 = std::max(c1, p.az1);
    const double top = topOf(c);
    const int n = int(cols[c].tiles.size()), b = cols[c].Base();
    d.kind = Drop::Into, d.col = c, d.below = el < l.deskDeg;
    d.row = d.below ? n : b + 1;   // (the lowest under the desk line, or the new base)
    for (const Placed &p : l.tiles) {
        if (p.col != c || p.below != d.below) continue;
        if (el >= (p.el0 + p.el1) / 2) {   // above this one's middle: before it (rows go top down)
            d.row = std::min(d.row, p.row);
        }
    }
    d.align = az < (c0 + c1) / 2 ? -1 : 1;
    d.over = !d.below && el > top;
    return d;
}

}  // namespace tiling
