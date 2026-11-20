// The dashboard tiles fitted by size, with no columns (DASH_TILE_FIT=1; tiling.h's columns
// otherwise). User, 2026-11-19: "instead of having rigid rows and columns, we could try to have
// displays auto-fit based off of their size to try to make the best use of real estate. Let's go
// ahead and test both options".
//
// Largest first, each panel goes where the layout stays smallest: of the spots that touch a panel
// already placed (beside it, over it or under it, lined up with one of its edges or its middle) and
// keep a gap from every other one, the one that leaves the layout's box narrowest plus tallest
// (kUpDownCost), the box as wide either way as its farthest side so it stays centred; then the one
// nearest the middle. (A first try put each panel where its own far edge was nearest the middle:
// it stacked panels up and down, 56 x 45 degrees for 13 panels the columns put in 54 x 27.) The first one stands on the desk line
// straight ahead (Steam's slot, when there is one, kept clear all the way up, as tiling.h keeps it),
// and the rest spread round it both ways, up to kBandDeg above or below its middle. Too wide for a
// turn round you: every panel shrinks by the same factor till it fits.
//
// The grid (tilegrid.h) still holds the panels and their sizes, so the keys, drops and memory work
// as they do with columns; its order only breaks ties between panels of the same size.
// Header only, no OpenVR, so its test runs anywhere (screens/tests/tiling-test.cpp).
#pragma once

#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tilefit {

constexpr double kBandDeg = 28;     // panels up to this far above or below the first one's middle (a taller one: centred on it)
constexpr double kUpDownCost = 1.6; // a degree up or down counts this much more than one round you (1.3 packed 13 real panels 34 x 45 degrees, 1.6: 49 x 32)

struct Box {
    double az0, az1, el0, el1;
};
inline bool Clear(const Box &a, const Box &b, double gap) {
    return !(a.az0 < b.az1 + gap - 1e-9 && b.az0 < a.az1 + gap - 1e-9 && a.el0 < b.el1 + gap - 1e-9 && b.el0 < a.el1 + gap - 1e-9);
}

// The panels laid out at size `k` (tiling.h's Layout; each Placed keeps its grid column and row).
inline tiling::Layout FitAt(const std::vector<tiling::Column> &cols, const tiling::Params &p, double k) {
    struct Item {
        int col, row;
        double widthM, heightM, hDeg;
    };
    std::vector<Item> items;
    for (size_t c = 0; c < cols.size(); ++c)
        for (size_t r = 0; r < cols[c].tiles.size(); ++r) {
            const tiling::Tile &t = cols[c].tiles[r];
            const double w = std::max(0.01, t.widthM) * k, h = std::max(0.01, t.heightM) * k;
            items.push_back({int(c), int(r), w, h, tiling::Deg(2 * std::atan2(h / 2, p.radius))});
        }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.widthM * a.heightM > b.widthM * b.heightM; });
    const double gap = p.gapDeg * k;
    std::vector<Box> boxes;   // (Steam's slot first, when there is one)
    const double slot = std::min(p.reserveDeg, p.maxSpanDeg / 2);
    const double slotAz = tiling::WrapNear(p.reserveAzDeg, 0);
    if (slot > 0) boxes.push_back({slotAz - slot / 2, slotAz + slot / 2, -1e9, 1e9});
    const size_t fixed = boxes.size();
    tiling::Layout out;
    out.el0 = out.el1 = out.deskDeg = p.deskDeg, out.shrink = k;
    double ref = p.deskDeg;   // (the first one's middle, up)
    bool any = false;
    double az0 = 0, az1 = 0;
    double wide = 0, up = 0, down = 0;   // (the layout so far: its farthest round you either way, and up and down from `ref`)
    for (const Item &it : items) {
        const double band = std::max(kBandDeg, it.hDeg / 2);
        std::vector<double> els, azs;
        if (!any) els.push_back(p.deskDeg);
        for (size_t b = fixed; b < boxes.size(); ++b) {
            const Box &r = boxes[b];
            els.insert(els.end(), {r.el1 + gap, r.el0 - gap - it.hDeg, r.el0, r.el1 - it.hDeg, (r.el0 + r.el1 - it.hDeg) / 2});
        }
        double best = 1e18, bestTie = 0;
        Box pick{};
        for (const double e0 : els) {
            const Box vert{0, 0, e0, e0 + it.hDeg};
            if (any && (vert.el0 < ref - band - 1e-9 || vert.el1 > ref + band + 1e-9)) continue;
            if (vert.el0 < -tiling::kMaxTopDeg || vert.el1 > tiling::kMaxTopDeg) continue;
            const double span = tiling::SpanFor(p.radius, it.widthM, it.heightM, e0 + it.hDeg / 2);
            azs.clear();
            azs.push_back(-span / 2);
            for (const Box &r : boxes) azs.insert(azs.end(), {r.az1 + gap, r.az0 - gap - span, r.az0, r.az1 - span, (r.az0 + r.az1 - span) / 2});
            for (const double a0 : azs) {
                const Box b{a0, a0 + span, vert.el0, vert.el1};
                bool ok = true;
                for (const Box &o : boxes)
                    if (!Clear(b, o, gap)) {
                        ok = false;
                        break;
                    }
                if (!ok) continue;
                // (the layout's size with it: as wide either way as its farthest side, and as tall)
                const double far = 2 * std::max({wide, std::fabs(b.az0), std::fabs(b.az1)}) +
                                   kUpDownCost * (any ? std::max(up, b.el1 - ref) + std::max(down, ref - b.el0) : 0);
                const double tie = std::hypot((b.az0 + b.az1) / 2, kUpDownCost * ((b.el0 + b.el1) / 2 - ref));
                if (far < best - 1e-9 || (far < best + 1e-9 && tie < bestTie - 1e-9)) best = far, bestTie = tie, pick = b;
            }
        }
        if (best > 1e17) {   // (nowhere in the band: on the desk line past the right end)
            const double span = tiling::SpanFor(p.radius, it.widthM, it.heightM, p.deskDeg + it.hDeg / 2);
            double right = 0;
            for (const Box &o : boxes) right = std::max(right, o.az1 > 1e8 ? 0 : o.az1);
            pick = {right + gap, right + gap + span, p.deskDeg, p.deskDeg + it.hDeg};
        }
        if (!any) ref = (pick.el0 + pick.el1) / 2;
        wide = std::max({wide, std::fabs(pick.az0), std::fabs(pick.az1)});
        up = std::max(up, pick.el1 - ref), down = std::max(down, ref - pick.el0);
        boxes.push_back(pick);
        az0 = any ? std::min(az0, pick.az0) : pick.az0, az1 = any ? std::max(az1, pick.az1) : pick.az1;
        any = true;
        tiling::Placed pl;
        pl.id = cols[size_t(it.col)].tiles[size_t(it.row)].id, pl.col = it.col, pl.row = it.row;
        pl.az0 = pick.az0, pl.az1 = pick.az1, pl.el0 = pick.el0, pl.el1 = pick.el1;
        pl.azDeg = (pick.az0 + pick.az1) / 2, pl.elDeg = (pick.el0 + pick.el1) / 2;
        pl.widthM = it.widthM, pl.heightM = it.heightM, pl.below = pick.el1 <= p.deskDeg + 1e-9;
        tiling::CornersOnSphere(p.radius, &pl);
        out.el0 = std::min(out.el0, pl.el0), out.el1 = std::max(out.el1, pl.el1);
        out.tiles.push_back(pl);
    }
    if (slot > 0 && any) {
        az0 = std::min(az0, slotAz - slot / 2), az1 = std::max(az1, slotAz + slot / 2);
        out.split = 0, out.slotAz = slotAz;   // (a slot in the strip; its columns mean nothing here)
    }
    out.az0 = any ? az0 : 0, out.az1 = any ? az1 : 0;
    return out;
}

// Fitted at 1, or the largest smaller size whose strip fits in a turn round you.
inline tiling::Layout Fit(const std::vector<tiling::Column> &cols, const tiling::Params &p) {
    tiling::Layout l = FitAt(cols, p, 1);
    for (double k = 0.9; k > 0.05 && l.az1 - l.az0 + p.gapDeg * k > p.maxSpanDeg; k *= 0.9) l = FitAt(cols, p, k);
    return l;
}

}  // namespace tilefit
