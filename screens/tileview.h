// The dashboard tiles as laid out last (Board::Arrange, dashtiles.h): what fits where. Read only:
// the board owns the grid and the layout; it, the tiling keys (tilekeys.h) and the gap fill
// (tilefill.h) ask this. It sees the grid as it is now but the layout as it was last, so ask it
// before changing the grid, not after (a column added since: FitsUnder's yes, LaidOut's no).
// Header only, no OpenVR, so its test (tests/dashtiles-test.cpp) runs anywhere.
#pragma once

#include "slotsplit.h"
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
// tilepack::kColumnDeg as your eyes see it (kWideColumnDeg, a super wide display's).
inline bool EyeFits(const void *ctx, double height, bool wide) {
    const tiling::Params &p = *static_cast<const tiling::Params *>(ctx);
    return tiling::EyeElDeg(p, p.deskDeg + height) - tiling::EyeElDeg(p, p.deskDeg) <= tilepack::MaxDeg(wide) + 1e-9;
}
// ...with `under` of it hanging under the desk line.
inline bool EyeFitsRound(const tiling::Params &p, double under, double height, bool wide = false) {
    return tiling::EyeElDeg(p, p.deskDeg - under + height) - tiling::EyeElDeg(p, p.deskDeg - under) <= tilepack::MaxDeg(wide) + 1e-9;
}

// A size in metres as degrees round you, at the wall.
inline double SpanDeg(const tiling::Params &p, double m) { return tiling::Deg(2 * std::atan2(m / 2, p.radius)); }

struct View {
    const tiling::Params &params;
    const tilegrid::Grid &grid;
    const tiling::Layout &last;
    const std::vector<tilepack::Fill> &fills;   // each column's shelves as laid out last, unshrunk, from the desk line up
    const std::vector<tilepack::Fill> &underFills;   // ...and down
    const std::set<int> &absent;                // laid out at no size (Board::absent)
    const slotsplit::Policy &slot;
    int steam;                                  // Steam's window: never pulled into another column

    const tiling::Placed *PlacedOf(int id) const {
        for (const auto &p : last.tiles)
            if (p.id == id) return &p;
        return nullptr;
    }
    // Degrees as laid out last, as they'd be unshrunk (tiling::Layout::shrink: the sizes fills has).
    double Unshrunk(double deg) const { return last.shrink > 0 ? deg / last.shrink : deg; }
    // Column `col` with `height` from the desk line up (unshrunk) and what hangs under it as laid
    // out last, or `under` of it: no taller than tilepack::kColumnDeg as your eyes see it
    // (kWideColumnDeg, a super wide display's; RoomFor's, Lift's test).
    bool ColumnFits(int col, double height, double under = 0) const {
        for (const tiling::Placed &p : last.tiles)
            if (p.col == col) under = std::max(under, Unshrunk(params.deskDeg - p.el0));
        return EyeFitsRound(params, under, height + under, WideCol(col));   // (`under` from the desk line: its gap and all)
    }
    bool Pullable(int id) const { return id != steam && !absent.count(id); }
    // Column `col` as laid out last: each of its panels where Arrange put it, and no others (a column
    // added or taken away since shifts the others: their sizes there are another column's; a panel
    // gone from it since is still in its height).
    bool LaidOut(int col) const {
        if (fills.size() != grid.Cols().size()) return false;
        const auto &cells = grid.Cols()[size_t(col)].cells;
        for (const tilegrid::Cell &c : cells)
            if (const tiling::Placed *p = PlacedOf(c.id); !p || p->col != col) return false;
        return size_t(std::count_if(last.tiles.begin(), last.tiles.end(), [&](const tiling::Placed &p) { return p.col == col; })) == cells.size();
    }
    // Every panel in column `col` is shown (none laid out at no size: its height there is its own).
    bool AllShown(int col) const {
        for (const tilegrid::Cell &c : grid.Cols()[size_t(col)].cells)
            if (absent.count(c.id)) return false;
        return true;
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
    // Panel `other` would fit over (or `under`) column `col`'s super wide display, on its shelf there.
    bool FitsRound(int col, int other, bool under) const {
        const tiling::Placed *q = PlacedOf(other);
        if (!q || fills.size() != grid.Cols().size()) return false;
        const tilepack::Fill &f = under ? underFills[size_t(col)] : fills[size_t(col)];
        const double h = tilepack::Grown(f, {other, Unshrunk(q->az1 - q->az0), Unshrunk(q->el1 - q->el0)}, params.gapDeg);
        return h >= 0 && (under ? ColumnFits(col, fills[size_t(col)].height, params.gapDeg + h) : ColumnFits(col, h));
    }
    // How far tile `p`'s shelf could still lean to side `dir` (-1 left): from its end there to its column's edge,
    // as laid out last. A shelf that fills its column has none.
    double LeanRoom(const tiling::Placed &p, int dir) const {
        double edge = dir < 0 ? 1e9 : -1e9, end = edge;
        for (const tiling::Placed &q : last.tiles) {
            if (q.col != p.col) continue;
            edge = dir < 0 ? std::min(edge, q.az0) : std::max(edge, q.az1);
            if (q.below == p.below && q.shelf == p.shelf) end = dir < 0 ? std::min(end, q.az0) : std::max(end, q.az1);
        }
        return dir < 0 ? end - edge : edge - end;
    }
    bool Alone(int id) const {
        const tilegrid::Where w = grid.Find(id);
        return w.ok() && grid.Cols()[size_t(w.col)].cells.size() == 1;
    }

    // The desk full (another column would take the strip past params.wallDeg): the column a new
    // panel `info` goes on top of (tilepack.h): where it adds the least height (beside others on its
    // top shelf), then the lowest, then the nearer in front, of those it fits under kColumnDeg and is
    // no wider than; hung under a super wide display's
    // (`under`) when that adds less, or as much and less stands there than over it. -1 while
    // there's room, before the grid has been laid out, or when none takes it (a column of its own then).
    struct Room {
        int col = -1;
        bool under = false;
    };
    Room RoomFor(const Info &info) const {
        Room best;
        if (grid.Empty() || last.tiles.empty() || fills.size() != grid.Cols().size()) return best;
        const tilepack::Item it{-1, SpanDeg(params, info.widthM), SpanDeg(params, info.heightM), tiling::Wide(params, info.widthM, info.heightM)};
        if (last.az1 - last.az0 + params.gapDeg + it.wDeg <= params.wallDeg) return best;
        std::map<int, double> front;   // (each column's nearest in front)
        for (const tiling::Placed &p : last.tiles) {
            const double off = std::fabs(tiling::WrapNear(p.azDeg, 0));
            if (!front.count(p.col) || off < front[p.col]) front[p.col] = off;
        }
        double bestAdd = 0;
        const auto consider = [&](int c, bool under) {
            const tilepack::Fill &f = under ? underFills[size_t(c)] : fills[size_t(c)];
            const double h = tilepack::Grown(f, it, params.gapDeg);
            if (h < 0 || !(under ? ColumnFits(c, fills[size_t(c)].height, params.gapDeg + h) : ColumnFits(c, h))) return;
            const double add = !f.wide ? h - f.height : tilepack::Cost(f, it, params.gapDeg) + (under && f.n == 0 ? params.gapDeg : 0);
            const tilepack::Fill &b = best.col < 0 ? f : fills[size_t(best.col)];
            const bool lower = f.height < b.height - 1e-6 || (f.height < b.height + 1e-6 && front[c] < front[best.col]);
            const bool same = add < bestAdd + 1e-9;
            const bool better = under ? same && c == best.col && f.height < OverWide(c) - 1e-6 : same && lower;
            if (best.col < 0 || add < bestAdd - 1e-9 || better) best = {c, under}, bestAdd = add;
        };
        for (int c = 0; c < int(fills.size()); ++c) consider(c, false);
        for (int c = 0; c < int(fills.size()); ++c)
            if (WideCol(c) && Pullable(WideId(c))) consider(c, true);
        return best;
    }

    // Column `col` is a super wide display's (tilewide.h).
    bool WideCol(int col) const { return col >= 0 && col < int(fills.size()) && fills[size_t(col)].wide; }
    // How tall what stands over that display is, unshrunk (hanging: over the desk line).
    double OverWide(int col) const {
        const tiling::Placed *w = PlacedOf(WideId(col));
        return !w || w->below ? fills[size_t(col)].height : std::max(0.0, fills[size_t(col)].height - params.gapDeg - Unshrunk(w->el1 - params.deskDeg));
    }
    // Which panel that display is (-1: none).
    int WideId(int col) const {
        for (const tiling::Placed &p : last.tiles)
            if (p.col == col && p.wide) return p.id;
        return -1;
    }
};

}  // namespace dashtiles
