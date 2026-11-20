// Packing the dashboard tiles into columns (dashtiles.h's Board): a reset packs them all again, and
// a new tile on a full desk goes on a column with room for it. A column is as wide as its widest
// panel, and narrower ones share a shelf, side by side, as long as the shelf, gaps and all, is no
// wider than that (user, 2026-11-19: "if we had one window that was 1,200 pixels across, you could
// fit two windows that are 500 pixels across above it ... a column is defined by its widest
// screen"). Before that, twelve PC windows opened at once stacked five high on whatever column was
// lowest, and a 1.5 m screen in a column of 0.3 m windows left "a massive gap in the middle". A
// column is no taller than kColumnDeg as your eyes see it. Sizes are degrees round you. The layout
// (tiling.h's Stack) puts the panels on their shelves by the same rule (Shelves). Header only, no
// OpenVR, so its test runs anywhere (screens/tests/dashtiles-test.cpp).
#pragma once

#include <algorithm>
#include <deque>
#include <vector>

namespace tilepack {

constexpr double kColumnDeg = 50;    // a column this tall at most, as your eyes see it (a taller panel has one of its own;
                                     // 50: two windows 0.5 m tall about 1.4 m away, 43 degrees, share one)

// A column `height` tall: whether it's no taller than kColumnDeg. Sizes are the board's degrees, and
// the board (dashtiles.h) says how tall your eyes see them; alone, as they are.
using Fits = bool (*)(const void *ctx, double height);
inline bool AsIs(const void *, double height) { return height <= kColumnDeg + 1e-9; }

// Whether a panel `w` wide goes beside the ones on a shelf `used` wide, in a column `widest` wide.
inline bool Beside(double used, double w, double widest, double gapDeg) { return used + gapDeg + w <= widest + 1e-9; }

// A column's panels from the desk up, `wDeg` wide: each one's shelf (0 on the desk). Each goes
// beside the one before it while the shelf fits across the widest, else on a shelf above.
inline std::vector<int> Shelves(const std::vector<double> &wDeg, double gapDeg) {
    double widest = 0;
    for (const double w : wDeg) widest = std::max(widest, w);
    std::vector<int> out;
    int shelf = -1;
    double used = 0;
    for (const double w : wDeg) {
        if (shelf >= 0 && Beside(used, w, widest, gapDeg)) used += gapDeg + w;
        else ++shelf, used = w;
        out.push_back(shelf);
    }
    return out;
}

struct Item {
    int id = 0;
    double wDeg = 0, hDeg = 0;
};

// A column so far: its widest panel, its top shelf's width and height, and its height.
struct Fill {
    int n = 0;
    double widest = 0, used = 0, topH = 0, height = 0;
};
// Its height with `it` on top (beside on the top shelf or on one above), or -1: wider than the
// column (it would change every shelf under it).
inline double Grown(const Fill &f, const Item &it, double gapDeg) {
    if (f.n == 0) return it.hDeg;
    if (it.wDeg > f.widest + 1e-9) return -1;
    return Beside(f.used, it.wDeg, f.widest, gapDeg) ? f.height + std::max(0.0, it.hDeg - f.topH) : f.height + gapDeg + it.hDeg;
}
inline void Put(Fill *f, const Item &it, double gapDeg) {
    if (f->n++ == 0) {
        f->widest = std::max(f->widest, it.wDeg), f->used = it.wDeg, f->topH = f->height = it.hDeg;
        return;
    }
    if (Beside(f->used, it.wDeg, f->widest, gapDeg)) {
        f->used += gapDeg + it.wDeg;
        f->height += std::max(0.0, it.hDeg - f->topH), f->topH = std::max(f->topH, it.hDeg);
        return;
    }
    f->used = it.wDeg, f->height += gapDeg + it.hDeg, f->topH = it.hDeg;
}
// A column's Fill, its panels from the desk up (as Shelves puts them).
inline Fill FillOf(const std::vector<Item> &deskUp, double gapDeg) {
    Fill f;
    for (const Item &it : deskUp) f.widest = std::max(f.widest, it.wDeg);
    for (const Item &it : deskUp) Put(&f, it, gapDeg);
    return f;
}

// The items in columns, each listed from the desk up (the widest at the bottom), the columns left to
// right with the widest in the middle and the rest alternately right and left of it. Widest first,
// each goes where it adds the least height (beside others on a shelf: maybe none), then on the
// lowest column, among those it fits; else a column of its own. One with no size (not shown) gets
// a column of its own: those come last, so they're at the ends.
inline std::vector<std::vector<int>> Pack(std::vector<Item> items, double gapDeg, Fits fits = AsIs, const void *ctx = nullptr) {
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.wDeg > b.wDeg; });
    struct Col {
        Fill fill;
        std::vector<int> ids;
    };
    std::vector<Col> cols;   // (widest first: the order they were opened)
    for (const Item &it : items) {
        int best = -1;
        double bestAdd = 0;
        for (size_t c = 0; c < cols.size() && it.wDeg > 0; ++c) {   // (one not shown, no size: a column of its own)
            const Fill &f = cols[c].fill;
            const double h = Grown(f, it, gapDeg);
            if (h < 0 || !fits(ctx, h)) continue;
            const double add = h - f.height;
            if (best < 0 || add < bestAdd - 1e-9 || (add < bestAdd + 1e-9 && f.height < cols[size_t(best)].fill.height - 1e-9))
                best = int(c), bestAdd = add;
        }
        if (best < 0) cols.push_back({});
        Col &c = best < 0 ? cols.back() : cols[size_t(best)];
        Put(&c.fill, it, gapDeg);
        c.ids.push_back(it.id);
    }
    std::deque<std::vector<int>> out;
    for (size_t k = 0; k < cols.size(); ++k) {
        if (k % 2 == 1) out.push_back(std::move(cols[k].ids));
        else out.push_front(std::move(cols[k].ids));
    }
    return {out.begin(), out.end()};
}

}  // namespace tilepack
