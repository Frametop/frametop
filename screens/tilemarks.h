// Which dashboard tiles Board::Settle (dashtiles.h) still checks, and why (council, 2026-11-20):
// one put on a stack before its size was known (unsized), and one put on a stack for you (stacked:
// Anchor's OnTopOf, FillGap), so one coming back to that stack is checked too. A stack you change
// by hand is yours: those marks go. Header only, no OpenVR, so its test (tests/dashtiles-test.cpp)
// runs anywhere.
#pragma once

#include "tilegrid.h"

#include <algorithm>
#include <set>
#include <vector>

namespace dashtiles {

class Marks {
public:
    // The others in `id`'s column, in id order (none: not tiled, or alone).
    static std::vector<int> Mates(const tilegrid::Grid &g, int id) {
        std::vector<int> mates;
        if (const tilegrid::Where w = g.Find(id); w.ok())
            for (const tilegrid::Cell &c : g.Cols()[size_t(w.col)].cells)
                if (c.id != id) mates.push_back(c.id);
        std::sort(mates.begin(), mates.end());
        return mates;
    }

    void Unsized(int id) { unsized_.insert(id); }
    void Sized(int id) { unsized_.erase(id); }
    const std::set<int> &Unsized() const { return unsized_; }
    void ClearUnsized() { unsized_.clear(); }

    void Stacked(int id) { stacked_.insert(id); }
    void NotStacked(int id) { stacked_.erase(id); }
    void ClearStacked() { stacked_.clear(); }
    // Column `col` holds a panel put on it for you: one coming back to its stack there may not fit.
    bool AutoStacked(const tilegrid::Grid &g, int col) const {
        for (const tilegrid::Cell &c : g.Cols()[size_t(col)].cells)
            if (stacked_.count(c.id)) return true;
        return false;
    }

    // Off the grid: nothing to check for it.
    void Unmark(int id) { unsized_.erase(id), stacked_.erase(id); }
    // `id` moved, sized or leaned by hand, its Mates `before` that: its own marks go. Put by hand
    // into a stack it wasn't in (others with it now that weren't), that stack is yours: its panels'
    // stacked marks go, and so do the unsized ones of those shown (`absent`: not shown, so you
    // haven't seen how tall the stack is with them). Leaving a stack, reordering one or resizing in
    // it, the others keep theirs.
    void ByHand(const tilegrid::Grid &g, const std::set<int> &absent, int id, const std::vector<int> &before) {
        Unmark(id);
        const std::vector<int> now = Mates(g, id);
        if (now.empty() || now == before) return;
        for (const int m : now) {
            stacked_.erase(m);
            if (!absent.count(m)) unsized_.erase(m);
        }
    }

private:
    std::set<int> unsized_;
    std::set<int> stacked_;
};

}  // namespace dashtiles
