// How far the dashboard tiles' strip was turned (dashtiles.h's Pan) each way it has been laid out since a
// panel was last moved or the strip centred: laid out that way again, it's turned as it was then. Pan
// goes by where the panels were last, and a hidden one, which has no width, is somewhere else than it
// will be shown: hidden and shown again and again (Steam's page, as the dashboard closes and opens with
// the tiles left where they were put, tilekeep.h; a display whose stream stops), a strip went about nine
// degrees round each time, or lost its turn. Header only (screens/tests/dashtiles-test.cpp).
#pragma once

#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace turnkeep {

constexpr size_t kMost = 32;   // the newest so many

struct Turns {
    // The turn for `l`, laid out centred: as it had last, else `turn`'s.
    template <typename F>
    double Keep(const tiling::Layout &l, F turn) {
        const auto it = std::find_if(kept_.begin(), kept_.end(), [&](const auto &k) { return Same(k.first, l.tiles); });
        if (it != kept_.end()) return it->second;
        const double deg = turn();
        if (kept_.size() >= kMost) kept_.erase(kept_.begin());
        kept_.emplace_back(l.tiles, deg);
        return deg;
    }

private:
    using Tiles = std::vector<tiling::Placed>;
    static bool Same(const Tiles &a, const Tiles &b) {
        const auto near = [](double x, double y) { return std::fabs(x - y) < 1e-6; };
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [&](const tiling::Placed &p, const tiling::Placed &q) {
            return p.id == q.id && near(p.azDeg, q.azDeg) && near(p.az0, q.az0) && near(p.az1, q.az1);
        });
    }
    std::vector<std::pair<Tiles, double>> kept_;
};

}  // namespace turnkeep
