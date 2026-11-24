// Round a super wide display (tiling.h's WideRow): the shelves of its column over and under it, and
// where along them a panel goes (user, 2026-11-23: "make tiling on super wide displays easier to put
// multiple tiles above and below, there is a lot of white space"; a panel let go in the empty space
// over one got a new column beside it, and on a shelf always the same end of it). A drop
// (tiledrop.h) and the tiling keys (tilekeys.h) ask this. Header only, no OpenVR.
#pragma once

#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tiling {

constexpr double kWideEndDeg = 3;   // let go on the display itself this near its end: a new column there

// The super wide display (az, el) is over, under or on, but for its ends; null: none, or another
// column's panel is there. Not the dragged one (`self`).
inline const Placed *OverWide(const Layout &l, double az, double el, int self) {
    const Placed *wide = nullptr;
    for (const Placed &p : l.tiles) {
        if (p.id == self || az < p.az0 || az > p.az1) continue;
        if (p.wide && (!wide || std::fabs(p.elDeg - el) < std::fabs(wide->elDeg - el))) wide = &p;
    }
    if (!wide) return nullptr;
    const bool on = el >= wide->el0 && el <= wide->el1;
    if (on && std::min(az - wide->az0, wide->az1 - az) < kWideEndDeg) return nullptr;
    for (const Placed &p : l.tiles)
        if (p.id != self && p.col != wide->col && az >= p.az0 && az <= p.az1 && el >= p.el0 && el <= p.el1) return nullptr;
    return wide;
}

// A place in a super wide display's column: the row to insert at, hanging under the desk line or
// not, and which way to lean (0 on a shelf that's there: its way).
struct WideSpot {
    int row = 0;
    bool below = false;
    int align = 0;
    bool own = false;   // a shelf of its own, though there's room on the last one
    int before = -1;    // the panel that started its shelf, before it on it now (-1: none)
};

// The panels on each shelf of `col` but `self`, left to right, its display's own left out.
inline std::vector<std::vector<const Placed *>> WideShelves(const Layout &l, int col, int self) {
    std::vector<std::vector<const Placed *>> out;
    for (const Placed &p : l.tiles) {
        if (p.col != col || p.id == self || p.wide || p.az1 - p.az0 < 1e-6) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const auto &s) { return s[0]->below == p.below && s[0]->shelf == p.shelf; });
        if (it == out.end()) it = out.insert(out.end(), std::vector<const Placed *>{});
        it->push_back(&p);
    }
    for (auto &s : out) std::sort(s.begin(), s.end(), [](const Placed *a, const Placed *b) { return a->azDeg < b->azDeg; });
    return out;
}
// On `shelf`, `az` along it: between the panels either side.
inline WideSpot OnShelf(const std::vector<const Placed *> &shelf, double az) {
    WideSpot s;
    s.below = shelf[0]->below;
    const Placed *left = nullptr;
    for (const Placed *p : shelf)
        if (p->azDeg < az) left = p;
    // (the shelves run from the desk line out, left to right: rows up the column above it)
    if (!left) s.own = shelf[0]->starts, s.before = s.own ? shelf[0]->id : -1;
    if (s.below) s.row = left ? left->row + 1 : shelf[0]->row;
    else s.row = left ? left->row : shelf[0]->row + 1;
    return s;
}
// A panel `half` half as tall let go at (az, el) over, under or on `wide` (OverWide), whose column
// has `n` panels: the nearest shelf that side of it, or past the last of them, one further out.
inline WideSpot SpotOnWide(const Layout &l, const Placed &wide, int n, double az, double el, double half, int self) {
    const bool over = el >= wide.elDeg;
    const std::vector<const Placed *> *best = nullptr;
    double bestOff = 0, reach = over ? wide.el1 : wide.el0;
    const auto shelves = WideShelves(l, wide.col, self);
    for (const auto &s : shelves) {
        if ((s[0]->elDeg > wide.elDeg) != over) continue;
        double lo = 1e9, hi = -1e9;
        for (const Placed *p : s) lo = std::min(lo, p->el0), hi = std::max(hi, p->el1);
        reach = over ? std::max(reach, hi) : std::min(reach, lo);
        const double off = el < lo ? lo - el : el > hi ? el - hi : 0;
        if (!best || off < bestOff) best = &s, bestOff = off;
    }
    const bool past = over ? el > reach + half / 2 : el < reach - half / 2;   // (three quarters of it past them)
    if (best && !past) return OnShelf(*best, az);
    const double w = (wide.az1 - wide.az0) / 3;
    return {over ? 0 : n, !over, az < wide.az0 + w ? -1 : az > wide.az1 - w ? 1 : 0, best != nullptr};
}

}  // namespace tiling
