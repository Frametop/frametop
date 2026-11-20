// The dashboard tiles as laid out last (Board::Arrange, dashtiles.h): what fits where. Read only:
// the board owns the grid and the layout; it, the tiling keys (tilekeys.h) and the gap fill ask
// this. Header only, no OpenVR, so its test (tests/dashtiles-test.cpp) runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilegap.h"
#include "tilegrid.h"
#include "tilepack.h"
#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace dashtiles {

// What the host says about a panel: its natural size at the wall (its pixels, tiling::MetresPerPx each).
struct Info {
    double widthM = 1.6, heightM = 0.9;
};

// A column `height` tall from the desk line (ctx: the board's tiling::Params): no taller than
// tilepack::kColumnDeg as your eyes see it.
inline bool EyeFits(const void *ctx, double height) {
    const tiling::Params &p = *static_cast<const tiling::Params *>(ctx);
    return tiling::EyeElDeg(p, p.deskDeg + height) - tiling::EyeElDeg(p, p.deskDeg) <= tilepack::kColumnDeg + 1e-9;
}
// ...with `under` of it hanging under the desk line.
inline bool EyeFitsRound(const tiling::Params &p, double under, double height) {
    return tiling::EyeElDeg(p, p.deskDeg - under + height) - tiling::EyeElDeg(p, p.deskDeg - under) <= tilepack::kColumnDeg + 1e-9;
}

struct View {
    const tiling::Params &params;
    const tilegrid::Grid &grid;
    const tiling::Layout &last;
    const std::vector<tilepack::Fill> &fills;   // each column's shelves as laid out last, unshrunk
    const std::set<int> &absent;                // laid out at no size (Board::absent)
    const slotsplit::Policy &slot;
    int steam;                                  // Steam's window: never pulled into another column

    static constexpr double kFitSlackDeg = 0.5;

    const tiling::Placed *PlacedOf(int id) const {
        for (const auto &p : last.tiles)
            if (p.id == id) return &p;
        return nullptr;
    }
    // A size in metres as degrees round you, at the wall.
    double Span(double m) const { return tiling::Deg(2 * std::atan2(m / 2, params.radius)); }
    // Degrees as laid out last, as they'd be unshrunk (tiling::Layout::shrink: the sizes fills has).
    double Unshrunk(double deg) const { return last.shrink > 0 ? deg / last.shrink : deg; }
    // Column `col` with `height` from the desk line up (unshrunk) and what hangs under it as laid
    // out last: no taller than tilepack::kColumnDeg as your eyes see it (OnTopOf's, Lift's test).
    bool ColumnFits(int col, double height) const {
        double under = 0;
        for (const tiling::Placed &p : last.tiles)
            if (p.col == col) under = std::max(under, Unshrunk(params.deskDeg - p.el0));
        return EyeFitsRound(params, under, height + under);   // (`under` from the desk line: its gap and all)
    }
    bool Pullable(int id) const { return id != steam && !absent.count(id); }
    // Column `col` as laid out last: each of its panels where Arrange put it (a column added or
    // taken away since shifts the others: their sizes there are another column's).
    bool LaidOut(int col) const {
        if (fills.size() != grid.Cols().size()) return false;
        for (const tilegrid::Cell &c : grid.Cols()[size_t(col)].cells)
            if (const tiling::Placed *p = PlacedOf(c.id); !p || p->col != col) return false;
        return true;
    }
    // A panel stands in gap `g`'s stack (column `col`) that Leave didn't leave there: it took the gap.
    bool Newcomer(const tilegap::Gap &g, int col) const {
        const tilegrid::Col &c = grid.Cols()[size_t(col)];
        for (int r = 0; r < int(c.cells.size()); ++r)
            if (!c.Below(r) && std::find(g.stack.begin(), g.stack.end(), c.cells[size_t(r)].id) == g.stack.end()) return true;
        return false;
    }
    // Gap `g`, in column `col`, still as Leave left it (council, 2026-11-20): standing there only
    // panels it left (another there took the gap, whatever its size: one opened into it, or one swapped
    // for one that went), none not shown yet (no size to test), and laid out since.
    bool GapOpen(const tilegap::Gap &g, int col) const {
        for (const tilegrid::Cell &c : grid.Cols()[size_t(col)].cells)
            if (absent.count(c.id)) return false;
        return !Newcomer(g, col) && LaidOut(col);
    }
    int SideOf(int col) const { return slot.SideOfCol(grid, col); }
    // Nothing in column `col` is shown (Board::absent: laid out at no size).
    bool Ghost(int col) const {
        for (const tilegrid::Cell &c : grid.Cols()[size_t(col)].cells)
            if (!absent.count(c.id)) return false;
        return true;
    }
    // The column next to `col` on side `dir` (-1 left) as you see them (past columns with nothing
    // shown), on its side of Steam's slot (only Meta+Shift+Left/Right step across it); -1: none.
    int Neighbour(int col, int dir) const {
        const int n = int(grid.Cols().size());
        int c = col + dir;
        while (c >= 0 && c < n && Ghost(c)) c += dir;
        return c < 0 || c >= n || SideOf(c) != SideOf(col) ? -1 : c;
    }
    // Panel `other` would fit standing under column `col` on the desk line (not laid out yet: yes).
    bool FitsUnder(int col, int other) const {
        const tiling::Placed *q = PlacedOf(other);
        if (!q || fills.size() != grid.Cols().size()) return true;
        return ColumnFits(col, fills[size_t(col)].height + params.gapDeg + Unshrunk(q->el1 - q->el0));
    }
    bool Alone(int id) const {
        const tilegrid::Where w = grid.Find(id);
        return w.ok() && grid.Cols()[size_t(w.col)].cells.size() == 1;
    }

    // The desk full (another column would take the strip past params.wallDeg): the column a new
    // panel `info` goes on top of (tilepack.h): where it adds the least height (beside others on its
    // top shelf), then the lowest, then the nearer in front, of those it fits under kColumnDeg and is
    // no wider than; -1 while there's room, before the grid has been laid out, or when none takes it
    // (a column of its own then).
    int OnTopOf(const Info &info) const {
        if (grid.Empty() || last.tiles.empty() || fills.size() != grid.Cols().size()) return -1;
        const tilepack::Item it{-1, Span(info.widthM), Span(info.heightM)};
        if (last.az1 - last.az0 + params.gapDeg + it.wDeg <= params.wallDeg) return -1;
        std::map<int, double> front;   // (each column's nearest in front)
        for (const tiling::Placed &p : last.tiles) {
            const double off = std::fabs(tiling::WrapNear(p.azDeg, 0));
            if (!front.count(p.col) || off < front[p.col]) front[p.col] = off;
        }
        int best = -1;
        double bestAdd = 0;
        for (size_t c = 0; c < fills.size(); ++c) {
            const tilepack::Fill &f = fills[c];
            const double h = tilepack::Grown(f, it, params.gapDeg);
            if (h < 0 || !ColumnFits(int(c), h)) continue;
            const double add = h - f.height;
            const tilepack::Fill &b = best < 0 ? f : fills[size_t(best)];
            const bool lower = f.height < b.height - 1e-6 || (f.height < b.height + 1e-6 && front[int(c)] < front[best]);
            if (best < 0 || add < bestAdd - 1e-9 || (add < bestAdd + 1e-9 && lower)) best = int(c), bestAdd = add;
        }
        return best;
    }

    // The neighbouring column whose only panel goes up into gap `g` in column `col` (Board::FillGap):
    // it fits in its place (no wider than the column, no taller than the one that went, on its side
    // of Steam's slot, and standing under what hangs there now); of two, the bigger. col -1: none.
    struct Pick {
        int col = -1, side = 0;
    };
    Pick FillFor(const tilegap::Gap &g, int col) const {
        double span = 0;
        for (const tiling::Placed &p : last.tiles)
            if (p.col == col) span = std::max(span, p.az1 - p.az0);
        Pick best;
        double bestArea = 0;
        for (const int dir : {-1, +1}) {
            const int c = Neighbour(col, dir);
            if (c < 0 || grid.Cols()[size_t(c)].cells.size() != 1 || !LaidOut(c)) continue;
            const int other = grid.Cols()[size_t(c)].cells.front().id;
            const tiling::Placed *q = PlacedOf(other);
            if (!q || !Pullable(other) || q->az1 - q->az0 > span + kFitSlackDeg || Unshrunk(q->el1 - q->el0) > g.heightDeg + kFitSlackDeg ||
                !FitsUnder(col, other))   // (with what hangs under it now)
                continue;
            const double area = (q->az1 - q->az0) * (q->el1 - q->el0);
            if (area > bestArea) best = {c, dir}, bestArea = area;
        }
        return best;
    }
};

}  // namespace dashtiles
