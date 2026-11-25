// Packing the dashboard tiles into columns (dashtiles.h's Board): `tiles pack` packs them all again, and
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
// A super wide display's column (tilewide.h; user, 2026-11-23: "make tiling on super wide displays
// easier to put multiple tiles above and below, there is a lot of white space and it's almost
// impossible to get more than 3 tiles over the super ultrawide"). At kColumnDeg nothing stood on a
// 32:9 one but the shortest windows, and its shelves kept up to a quarter of their width empty.
constexpr double kWideColumnDeg = 80;   // a row of windows over it and one under it
constexpr double kSqueeze = 0.75;       // its shelves take one more panel by making them all smaller, this small at most,
constexpr double kSqueezeShare = 0.7;   // ...if each is no wider than this much of it (the display itself: never)

// A column `height` tall (`wide`: a super wide display's): whether it's no taller than kColumnDeg
// (kWideColumnDeg). Sizes are the board's degrees, and the board (dashtiles.h) says how tall your
// eyes see them; alone, as they are.
using Fits = bool (*)(const void *ctx, double height, bool wide);
inline double MaxDeg(bool wide) { return wide ? kWideColumnDeg : kColumnDeg; }
inline bool AsIs(const void *, double height, bool wide) { return height <= MaxDeg(wide) + 1e-9; }

// How much smaller `n` panels `used` wide (gaps and all, as they are) get to fit a shelf `widest` wide.
inline double Squeezed(double used, int n, double widest, double gapDeg) {
    const double gaps = (n - 1) * gapDeg;
    return n < 1 || used <= widest + 1e-9 ? 1 : std::max(0.0, widest - gaps) / std::max(1e-9, used - gaps);
}
// A shelf so far: how many panels are on it, how wide they are, gaps and all, and the widest of them.
struct Shelf {
    int n = 0;
    double used = 0, most = 0;
    void Add(double w, double gapDeg) { used += (n++ ? gapDeg : 0) + w, most = std::max(most, w); }
};
// Whether a panel `w` wide goes beside the ones on shelf `s`, in a column `widest` wide (`wide`, a
// super wide display's: them all that much smaller).
inline bool Beside(const Shelf &s, double w, double widest, double gapDeg, bool wide = false) {
    const double to = s.used + gapDeg + w;
    return to <= widest + 1e-9 || (wide && std::max(s.most, w) <= kSqueezeShare * widest && Squeezed(to, s.n + 1, widest, gapDeg) >= kSqueeze - 1e-9);
}

// A column's panels from the desk up (or down), `wDeg` wide: each one's shelf (0 on the desk). Each
// goes beside the one before it while the shelf fits across the widest (`widest`, or theirs: 0),
// else on a shelf above (one that `starts` one too).
inline std::vector<int> Shelves(const std::vector<double> &wDeg, double gapDeg, double widest = 0, bool wide = false, const std::vector<bool> &starts = {}) {
    for (const double w : wDeg) widest = std::max(widest, w);
    std::vector<int> out;
    int shelf = -1;
    Shelf on;
    for (size_t i = 0; i < wDeg.size(); ++i) {
        const double w = wDeg[i];
        if (shelf < 0 || (i < starts.size() && starts[i]) || !Beside(on, w, widest, gapDeg, wide)) ++shelf, on = {};
        on.Add(w, gapDeg);
        out.push_back(shelf);
    }
    return out;
}

struct Item {
    int id = 0;
    double wDeg = 0, hDeg = 0;
    bool wide = false;   // a super wide display
    bool row = false;    // starts a shelf (tilegrid.h's Cell::row)
};

// A column so far: its widest panel (super wide or not), its top shelf and how tall that is (as
// they are: TopSqueeze says how small they stand there), and its height, and that up to the top shelf.
struct Fill {
    int n = 0;
    double widest = 0, topH = 0, height = 0, under = 0;
    bool wide = false;
    Shelf top;
    double TopSqueeze(double gapDeg) const { return wide ? Squeezed(top.used, top.n, widest, gapDeg) : 1; }
};
inline void Put(Fill *f, const Item &it, double gapDeg) {
    if (f->n == 0 && it.wDeg > f->widest) f->wide = it.wide;   // (an empty one's first panel)
    f->widest = std::max(f->widest, it.wDeg);
    if (f->n == 0 || (it.row && f->wide) || !Beside(f->top, it.wDeg, f->widest, gapDeg, f->wide))
        f->under = f->n ? f->height + gapDeg : 0, f->top = {}, f->topH = 0;
    ++f->n;
    f->top.Add(it.wDeg, gapDeg), f->topH = std::max(f->topH, it.hDeg);
    f->height = f->under + f->topH * f->TopSqueeze(gapDeg);
}
// Its height with `it` on top (beside on the top shelf or on one above), or -1: wider than the
// column (it would change every shelf under it).
inline double Grown(const Fill &f, const Item &it, double gapDeg) {
    if (f.n == 0) return it.hDeg;
    if (it.wDeg > f.widest + 1e-9) return -1;
    Fill g = f;
    Put(&g, it, gapDeg);
    return g.height;
}
// What it costs there: the height it adds, and how much smaller it makes its shelf (a fresh shelf
// on the other side of a super wide display rather than a third of the size gone).
inline double Cost(const Fill &f, const Item &it, double gapDeg) {
    Fill g = f;
    Put(&g, it, gapDeg);
    const double squeezed = g.under == f.under && f.n > 0 ? f.TopSqueeze(gapDeg) - g.TopSqueeze(gapDeg) : 1 - g.TopSqueeze(gapDeg);
    return std::max(0.0, g.height - f.height) + std::max(0.0, squeezed) * g.widest;
}
// The shelves of a column packed from the desk up (`deskUp`, as Pack lists it) shared out round the
// desk line (user, 2026-11-19: "using the bottom and top equally ... displays go from the center
// row down or up"): the first on the desk, then one above it, one under it, and so on. The ones
// that go up, from the desk up, and the ones that hang under it, from the desk down.
struct Sides {
    std::vector<int> up, down;
};
inline Sides UpAndDown(const std::vector<Item> &deskUp, double gapDeg) {
    std::vector<double> w;
    const Item *widest = nullptr;
    for (const Item &it : deskUp) {
        w.push_back(it.wDeg);
        if (!widest || it.wDeg > widest->wDeg) widest = &it;
    }
    const std::vector<int> shelf = Shelves(w, gapDeg, 0, widest && widest->wide);
    Sides s;
    for (size_t i = 0; i < deskUp.size(); ++i) (shelf[i] > 0 && shelf[i] % 2 == 0 ? s.down : s.up).push_back(deskUp[i].id);
    return s;
}
// A column's Fill, its panels from the desk up (as Shelves puts them). (`widest`, `wide`: the
// column's, with what hangs under it.)
inline Fill FillOf(const std::vector<Item> &deskUp, double gapDeg, double widest = 0, bool wide = false) {
    Fill f;
    f.widest = widest, f.wide = wide;
    for (const Item &it : deskUp)
        if (it.wDeg > f.widest) f.widest = it.wDeg, f.wide = it.wide;
    for (const Item &it : deskUp) Put(&f, it, gapDeg);
    return f;
}

// The items in columns, each listed from the desk up (the widest at the bottom), the columns left to
// right with the widest in the middle and the rest alternately right and left of it. Widest first,
// each goes where it adds the least height (beside others on a shelf: maybe none), then on the
// lowest column, among those it fits; else a column of its own. One with no size (not shown) gets
// a column of its own: those come last, so they're at the ends. `centre`'s column is the middle one
// whatever it is (dashtiles.h's centre display; -1: none).
inline std::vector<std::vector<int>> Pack(std::vector<Item> items, double gapDeg, Fits fits = AsIs, const void *ctx = nullptr, int centre = -1) {
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
            if (h < 0 || !fits(ctx, h, f.wide)) continue;
            const double add = f.wide ? Cost(f, it, gapDeg) : h - f.height;
            if (best < 0 || add < bestAdd - 1e-9 || (add < bestAdd + 1e-9 && f.height < cols[size_t(best)].fill.height - 1e-9))
                best = int(c), bestAdd = add;
        }
        if (best < 0) cols.push_back({});
        Col &c = best < 0 ? cols.back() : cols[size_t(best)];
        Put(&c.fill, it, gapDeg);
        c.ids.push_back(it.id);
    }
    const auto mid = std::find_if(cols.begin(), cols.end(), [&](const Col &c) { return std::find(c.ids.begin(), c.ids.end(), centre) != c.ids.end(); });
    if (mid != cols.end()) std::rotate(cols.begin(), mid, mid + 1);
    std::deque<std::vector<int>> out;
    for (size_t k = 0; k < cols.size(); ++k) {
        if (k % 2 == 1) out.push_back(std::move(cols[k].ids));
        else out.push_front(std::move(cols[k].ids));
    }
    return {out.begin(), out.end()};
}

}  // namespace tilepack
