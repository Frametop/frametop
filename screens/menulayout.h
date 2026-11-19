// Where the grab bar menu's tiles and rows sit on its texture, and which one a laser or a fingertip is
// on. The Option C design's layout (user, 2026-11-19: "Use that as the design") at twice its CSS
// pixels: a strip of quick tiles on top, a rule, then a list of rows, a rule between groups and a
// small grey caption over a page's choices. Pure: no OpenVR (screens/tests/menulayout-test.cpp).
#pragma once

#include <algorithm>
#include <vector>

namespace menulayout {

constexpr int kW = 720;            // the card (360 in the design)
constexpr int kPad = 20;           // round the tile strip
constexpr int kGap = 12;           // between tiles
constexpr int kTileH = 128;
constexpr int kRowH = 76;          // a list row: 9 + 20 + 9
constexpr int kTop = 16, kEnd = 20;   // the list's padding above (with no strip) and below
constexpr int kRuleGap = 16;       // a rule's band in the list (the line in its middle)
constexpr int kCaptionH = 56;

struct Spec {
    bool tile = false;      // a quick tile (only the first rows; a tile after a row is a row)
    bool rule = false;      // a rule above it
    bool caption = false;   // a caption above it
};

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // pixels, y down from the top
    bool Has(double x, double y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

struct Layout {
    int w = kW, h = 0;
    std::vector<Box> rows;      // each row's (tiles too), in order
    std::vector<bool> tiles;    // ...which are tiles
    std::vector<int> rules;     // each rule's y
    std::vector<int> captions;  // each row's caption band top, -1 none
};

inline Layout Lay(const std::vector<Spec> &specs) {
    Layout l;
    const int n = int(specs.size());
    int tiles = 0;
    while (tiles < n && specs[size_t(tiles)].tile) ++tiles;
    int y = kTop;
    if (tiles > 0) {
        const int tw = (kW - 2 * kPad - (tiles - 1) * kGap) / tiles;
        for (int i = 0; i < tiles; ++i) {
            const int x0 = kPad + i * (tw + kGap);
            l.rows.push_back({x0, kPad, i == tiles - 1 ? kW - kPad : x0 + tw, kPad + kTileH});
        }
        y = kPad + kTileH + kPad;
        if (tiles < n) l.rules.push_back(y), y += kRuleGap / 2 + 4;   // (the design's first row after it: 12 on top, not 9)
    }
    l.tiles.assign(size_t(n), false);
    std::fill(l.tiles.begin(), l.tiles.begin() + tiles, true);
    l.captions.assign(size_t(n), -1);
    for (int i = tiles; i < n; ++i) {
        const Spec &s = specs[size_t(i)];
        if (s.rule && i > tiles) l.rules.push_back(y + kRuleGap / 2), y += kRuleGap;
        if (s.caption) l.captions[size_t(i)] = y, y += kCaptionH;
        l.rows.push_back({0, y, kW, y + kRowH});
        y += kRowH;
    }
    l.h = tiles == n ? kPad + kTileH + kPad : y + kEnd;
    return l;
}

// The row (or tile) under a texture point, -1 none. OpenVR's mouse y is up from the bottom.
inline int At(const Layout &l, double x, double yUp) {
    const double y = l.h - yUp;
    for (size_t i = 0; i < l.rows.size(); ++i)
        if (l.rows[i].Has(x, y)) return int(i);
    return -1;
}

}  // namespace menulayout
