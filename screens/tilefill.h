// A dashboard tile's gap (tilegap.h keeps them) against the layout as it was last (tileview.h):
// whether it's still open, and which neighbour goes up into it (Board::FillGap, dashtiles.h).
// Header only, no OpenVR, so its test (tests/dashtiles-test.cpp) runs anywhere.
#pragma once

#include "tilegap.h"
#include "tilegrid.h"
#include "tileview.h"
#include "tiling.h"

#include <algorithm>

namespace dashtiles::tilefill {

constexpr double kFitSlackDeg = 0.5;

// A panel stands in gap `g`'s stack (column `col`) that Leave didn't leave there: it took the gap.
inline bool Newcomer(const View &v, const tilegap::Gap &g, int col) {
    const tilegrid::Col &c = v.grid.Cols()[size_t(col)];
    for (int r = 0; r < int(c.cells.size()); ++r)
        if (!c.Below(r) && std::find(g.stack.begin(), g.stack.end(), c.cells[size_t(r)].id) == g.stack.end()) return true;
    return false;
}
// Gap `g`, in column `col`, still as Leave left it (council, 2026-11-20): standing there only
// panels it left (another there took the gap, whatever its size: one opened into it, or one swapped
// for one that went), none not shown yet (no size to test), and laid out since.
inline bool Open(const View &v, const tilegap::Gap &g, int col) { return v.AllShown(col) && !Newcomer(v, g, col) && v.LaidOut(col); }

// The neighbouring column whose only panel goes up into gap `g` in column `col`: it fits in its
// place (no wider than the column, no taller than the one that went, on its side of Steam's slot,
// and standing under what hangs there now); of two, the bigger. col -1: none.
struct Pick {
    int col = -1, side = 0;
};
inline Pick For(const View &v, const tilegap::Gap &g, int col) {
    double span = 0;
    for (const tiling::Placed &p : v.last.tiles)
        if (p.col == col) span = std::max(span, p.az1 - p.az0);
    Pick best;
    double bestArea = 0;
    for (const int dir : {-1, +1}) {
        const int c = v.Neighbour(col, dir);
        if (c < 0 || v.grid.Cols()[size_t(c)].cells.size() != 1 || !v.LaidOut(c)) continue;
        const int other = v.grid.Cols()[size_t(c)].cells.front().id;
        const tiling::Placed *q = v.PlacedOf(other);
        if (!q || !v.Pullable(other) || q->az1 - q->az0 > span + kFitSlackDeg || v.Unshrunk(q->el1 - q->el0) > g.heightDeg + kFitSlackDeg ||
            !v.FitsUnder(col, other))   // (with what hangs under it now)
            continue;
        const double area = (q->az1 - q->az0) * (q->el1 - q->el0);
        if (area > bestArea) best = {c, dir}, bestArea = area;
    }
    return best;
}

}  // namespace dashtiles::tilefill
