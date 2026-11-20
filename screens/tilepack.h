// Packing the dashboard tiles into columns (dashtiles.h's Board): a reset packs them all again, and
// a new tile on a full desk goes on a column that suits it. A column holds panels of about the same
// width (user, 2026-11-19: twelve PC windows opened at once stacked five high on whatever column was
// lowest, and a 1.5 m screen in a column of 0.3 m windows left "a massive gap in the middle": the
// next column has to clear the wide one, the narrow ones beside it lean against one side), and is
// no taller than kColumnDeg. Sizes are degrees round you. Header only, no OpenVR, so its test runs
// anywhere (screens/tests/dashtiles-test.cpp).
#pragma once

#include <algorithm>
#include <deque>
#include <vector>

namespace tilepack {

constexpr double kSimilar = 1.6;     // a column's panels: none more than this many times as wide as another
constexpr double kColumnDeg = 45;    // a column this tall at most (a taller panel has one of its own)

// Whether a panel `w` wide may share a column whose widest is `widest`.
inline bool Similar(double widest, double w) { return widest <= w * kSimilar && w <= widest * kSimilar; }

// Whether a panel `h` tall still fits on a column `height` tall (its panels and their gaps).
inline bool Room(double height, double h, double gapDeg) { return height + gapDeg + h <= kColumnDeg + 1e-9; }

struct Item {
    int id = 0;
    double wDeg = 0, hDeg = 0;
};

// The items in columns, each listed from the desk up (the widest at the bottom), the columns left to
// right with the widest in the middle and the rest alternately right and left of it. Widest first,
// each goes on the lowest column it's Similar to with Room for it, else a column of its own.
inline std::vector<std::vector<int>> Pack(std::vector<Item> items, double gapDeg) {
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.wDeg > b.wDeg; });
    struct Col {
        double widest, height;
        std::vector<int> ids;
    };
    std::vector<Col> cols;   // (widest first: the order they were opened)
    for (const Item &it : items) {
        int best = -1;
        for (size_t c = 0; c < cols.size(); ++c)
            if (Similar(cols[c].widest, it.wDeg) && Room(cols[c].height, it.hDeg, gapDeg) &&
                (best < 0 || cols[c].height < cols[size_t(best)].height))
                best = int(c);
        if (best < 0) {
            cols.push_back({it.wDeg, it.hDeg, {it.id}});
            continue;
        }
        Col &c = cols[size_t(best)];
        c.height += gapDeg + it.hDeg;
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
