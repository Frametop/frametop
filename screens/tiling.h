// Where the dashboard's tiles go (screens/vrtiles.cpp's dashboard tiling; tilegrid.h holds their order and sizes).
//
// Panels anchored to the SteamVR dashboard tile on a sphere round your head, like one 360 degree
// monitor (user, 2026-11-16): columns side by side round you, each one's panels stacked in it with
// rows of their own heights, as Hyprland's scrolling columns are laid out. Every panel faces your
// head: a column curves up the sphere as well as round it. The band the columns fill starts at the
// dashboard's top edge and goes up `heightDeg`, but never past `maxTopDeg`: near the top of a
// sphere the circles round it shrink, so panels in neighbouring columns would run into each other
// at their upper corners ("so we don't end up with lots of displays overlapping at the corners on
// the top and bottom"). Each column is as wide as the widest of its panels needs at the panel's
// highest corner, so they never touch there, and the strip of columns is centred over the
// dashboard. Past a full turn round you everything shrinks until it fits.
//
// A panel with pixels of its own to change (a floating window, an app gamescope hands us) fills
// its cell: its width is the column's. A panel at a fixed resolution (a desktop screen, a remote
// display) keeps its shape: its row's height and its aspect, and its column grows to fit it.
//
// Angles in degrees. Azimuth is round you, 0 over the dashboard, positive to the right;
// elevation is up from your eyes' height. The group frame: origin at the sphere's centre (your
// head when the dashboard opened), -Z towards the dashboard, +Y up, as SteamVR's axes are.
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace tiling {

constexpr double kPi = 3.14159265358979323846;
inline double Rad(double deg) { return deg * kPi / 180; }
inline double Deg(double rad) { return rad * 180 / kPi; }

struct Tile {
    int id = 0;
    double aspect = 16.0 / 9;   // width / height, a fixed panel's (a resizable one's now, for nothing)
    bool fixed = true;          // keeps its shape: its column fits it
    double weight = 1;          // its share of its column's height
    double scale = 1;           // a fixed one: its size, as a share of what its row gives it (at most 1)
};

struct Column {
    std::vector<Tile> tiles;    // top to bottom
    double widthDeg = 0;        // the column's own width, round you (0: Params::columnDeg)
};

struct Params {
    double radius = 1.6;        // metres: the sphere's (the dashboard's distance)
    double bottomDeg = 5;       // the band's bottom: the dashboard's top edge
    double heightDeg = 60;      // how tall the band is asked to be
    double maxTopDeg = 55;      // ...but its top never higher (the column height cap)
    double minHeightDeg = 12;   // ...and never shorter than this
    double gapDeg = 1.5;        // between columns, and between the panels in one
    double columnDeg = 40;      // a column's width when it has none of its own
    double maxSpanDeg = 360;    // the strip round you, gaps included, at most
};

struct Placed {
    int id = 0;
    int col = 0, row = 0;
    double azDeg = 0, elDeg = 0;          // its centre
    double widthM = 0, heightM = 0;       // its size, metres
    double az0 = 0, az1 = 0;              // its column's span round you
    double el0 = 0, el1 = 0;              // its row's span, bottom to top
};

struct Layout {
    std::vector<Placed> tiles;
    double az0 = 0, az1 = 0;              // the strip
    double el0 = 0, el1 = 0;              // the band
    double shrink = 1;                    // 1, or less when it took more than a turn
};

// The chord across an angle on the sphere: a row's height in metres.
inline double Chord(double radius, double deg) { return 2 * radius * std::sin(Rad(deg) / 2); }

// A flat panel facing the centre at elevation `elDeg`, `widthM` x `heightM`: the angle round you
// its corners take. Its corners nearer the pole (its top ones above your eyes, its bottom ones
// below) are nearest the vertical axis, so they're the widest round you: half of it is
// atan2(w/2, r cos e - h/2 |sin e|). 180 when the panel reaches over the pole.
inline double SpanFor(double radius, double widthM, double heightM, double elDeg) {
    const double e = Rad(elDeg);
    const double depth = radius * std::cos(e) - heightM / 2 * std::fabs(std::sin(e));
    if (depth <= 1e-6) return 180;
    return std::min(180.0, Deg(2 * std::atan2(widthM / 2, depth)));
}
// The other way: how wide a panel can be in `spanDeg` round you (0 at or over the pole).
inline double WidthFor(double radius, double spanDeg, double heightM, double elDeg) {
    const double e = Rad(elDeg);
    const double depth = radius * std::cos(e) - heightM / 2 * std::fabs(std::sin(e));
    if (depth <= 1e-6 || spanDeg <= 0) return 0;
    return 2 * depth * std::tan(Rad(std::min(spanDeg, 179.0)) / 2);
}

inline double Scale(const Tile &t) { return std::clamp(t.scale, 0.05, 1.0); }

// The band the columns fill: from the dashboard's top edge up heightDeg, its top never past
// maxTopDeg, at least minHeightDeg tall. A dashboard high up (you lying back, a raise set high)
// moves the bottom down rather than the band past the cap.
inline void Band(const Params &p, double *bottom, double *height) {
    const double cap = std::clamp(p.maxTopDeg, p.minHeightDeg - 80, 85.0);
    const double b = std::clamp(p.bottomDeg, -85.0, cap - p.minHeightDeg);
    *bottom = b;
    *height = std::max(p.minHeightDeg, std::min(b + p.heightDeg, cap) - b);
}

// The rows' share of a column `h` tall with `gap` between them (at least a degree, or all of a
// band shrunk smaller: the rows stay in the band, not below it, nearer the pole).
inline double RowsAvail(double h, double gap, size_t rows) {
    return std::max(std::min(1.0, h), h - gap * (double(rows) - 1));
}

// Each column's span round you that its fixed panels need, in a band `h` tall from `bottom`,
// `gap` between its rows.
inline std::vector<double> Spans(const std::vector<Column> &cols, const Params &p, double bottom, double h, double gap) {
    std::vector<double> spans(cols.size());
    for (size_t c = 0; c < cols.size(); ++c) {
        const Column &col = cols[c];
        double span = 0;
        double sum = 0;
        for (const Tile &t : col.tiles) sum += std::max(0.05, t.weight);
        const double avail = RowsAvail(h, gap, col.tiles.size());
        double elTop = bottom + h;
        for (const Tile &t : col.tiles) {
            const double rowDeg = avail * std::max(0.05, t.weight) / std::max(0.05, sum);
            const double e1 = elTop, e0 = elTop - rowDeg;
            elTop = e0 - gap;
            if (!t.fixed) continue;
            const double hm = Chord(p.radius, rowDeg) * Scale(t);
            span = std::max(span, SpanFor(p.radius, t.aspect * hm, hm, (e0 + e1) / 2));
        }
        spans[c] = span;
    }
    return spans;
}

inline Layout Arrange(const std::vector<Column> &cols, const Params &p) {
    Layout out;
    double bottom, band;
    Band(p, &bottom, &band);
    // the strip at size k: the band, the gaps and the columns' own widths scale with it, fixed
    // panels' spans from their rows; a gap between each two columns, and one where the strip's ends
    // would meet behind you
    auto spansAt = [&](double k, double *total) {
        std::vector<double> s = Spans(cols, p, bottom, band * k, p.gapDeg * k);
        *total = p.gapDeg * k * double(cols.size());
        for (size_t c = 0; c < cols.size(); ++c) {
            s[c] = std::max(s[c], (cols[c].widthDeg > 0 ? cols[c].widthDeg : p.columnDeg) * k);
            *total += s[c];
        }
        return s;
    };
    double k = 1, total = 0;
    std::vector<double> spans = spansAt(1, &total);
    const double room = p.maxSpanDeg;
    if (total > room && !cols.empty()) {
        // past a turn: a k that fits, by halving. (The strip grows with k nearly everywhere: rows
        // that shrink move the panels' centres, so it can dip a little, and halving then finds one
        // within a few percent of the largest, which is fine. Everything goes to nothing with k,
        // so one fits, down to hundreds of columns in a band at the clamps' far end, -85.)
        double lo = 0, hi = 1;
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2;
            double t;
            spansAt(mid, &t);
            (t <= room ? lo : hi) = mid;
        }
        k = std::max(lo, 1e-3);
        spans = spansAt(k, &total);
    }
    out.shrink = k;
    const double h = band * k, gap = p.gapDeg * k;
    out.el0 = bottom, out.el1 = bottom + h;
    total = cols.empty() ? 0 : total - gap;   // (the one behind you isn't in the strip; no columns, no strip)
    double az = -total / 2;
    out.az0 = az, out.az1 = az + total;
    for (size_t c = 0; c < cols.size(); ++c) {
        const Column &col = cols[c];
        const double a0 = az, a1 = az + spans[c];
        az = a1 + gap;
        double sum = 0;
        for (const Tile &t : col.tiles) sum += std::max(0.05, t.weight);
        const double avail = RowsAvail(h, gap, col.tiles.size());
        double elTop = bottom + h;
        for (size_t r = 0; r < col.tiles.size(); ++r) {
            const Tile &t = col.tiles[r];
            const double rowDeg = avail * std::max(0.05, t.weight) / std::max(0.05, sum);
            Placed pl;
            pl.id = t.id, pl.col = int(c), pl.row = int(r);
            pl.el1 = elTop, pl.el0 = elTop - rowDeg;
            elTop = pl.el0 - gap;
            pl.az0 = a0, pl.az1 = a1;
            pl.azDeg = (a0 + a1) / 2, pl.elDeg = (pl.el0 + pl.el1) / 2;
            pl.heightM = Chord(p.radius, rowDeg);
            if (t.fixed) pl.heightM *= Scale(t);
            // A resizable panel fills its cell; a fixed one keeps its shape, no wider than the cell.
            const double cellW = WidthFor(p.radius, a1 - a0, pl.heightM, pl.elDeg);
            pl.widthM = t.fixed ? std::min(t.aspect * pl.heightM, cellW) : cellW;
            if (t.fixed && pl.widthM < t.aspect * pl.heightM - 1e-9) pl.heightM = pl.widthM / t.aspect;
            out.tiles.push_back(pl);
        }
    }
    return out;
}

// A panel's pose in the group frame, at `radius` from its centre and facing it: rows of a 3x4.
inline void Pose(double azDeg, double elDeg, double radius, double m[3][4]) {
    const double a = Rad(azDeg), e = Rad(elDeg);
    // Ry(-a) * Rx(e): pitched up by e, then turned right by a; +Z (the panel's front) at the centre.
    const double ca = std::cos(a), sa = std::sin(a), ce = std::cos(e), se = std::sin(e);
    const double r[3][3] = {{ca, -sa * se, -sa * ce}, {0, ce, -se}, {sa, ca * se, ca * ce}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = r[i][j];
    m[0][3] = radius * sa * ce;
    m[1][3] = radius * se;
    m[2][3] = -radius * ca * ce;
}

// A point (group frame) as azimuth and elevation from the centre.
inline void Direction(const double pt[3], double *azDeg, double *elDeg) {
    const double flat = std::hypot(pt[0], pt[2]);
    *azDeg = Deg(std::atan2(pt[0], -pt[2]));
    *elDeg = Deg(std::atan2(pt[1], flat));
}

// Where a dragged tile let go at (az, el) goes: into a column at a row, a new column at an index, or
// off the grid (torn off: it stays anchored where it is). A drop on a column's middle half stacks it
// there, above or below the panel it's on; on its outer quarters, or a gap, a new column that side.
struct Drop {
    enum Kind { Into, NewColumn, TearOff } kind = TearOff;
    int col = 0, row = 0;   // Into: the column and the row it takes; NewColumn: the index it takes
};
constexpr double kTearMarginDeg = 10;   // this far past the strip or the band: torn off

inline Drop DropAt(const Layout &l, const std::vector<Column> &cols, double az, double el) {
    Drop d;
    if (cols.empty()) return d.kind = Drop::NewColumn, d.col = 0, d;
    if (az < l.az0 - kTearMarginDeg || az > l.az1 + kTearMarginDeg || el < l.el0 - kTearMarginDeg ||
        el > l.el1 + kTearMarginDeg)
        return d;
    if (az < l.az0) return d.kind = Drop::NewColumn, d.col = 0, d;
    if (az > l.az1) return d.kind = Drop::NewColumn, d.col = int(cols.size()), d;
    // the column it's over (a gap: the nearer side's)
    int c = 0;
    double best = 1e9;
    double a0 = 0, a1 = 0;
    for (const Placed &p : l.tiles) {
        const double dist = az < p.az0 ? p.az0 - az : az > p.az1 ? az - p.az1 : 0;
        if (dist < best) best = dist, c = p.col, a0 = p.az0, a1 = p.az1;
    }
    const double u = (az - a0) / std::max(1e-6, a1 - a0);
    if (u < 0.25) return d.kind = Drop::NewColumn, d.col = c, d;
    if (u > 0.75) return d.kind = Drop::NewColumn, d.col = c + 1, d;
    d.kind = Drop::Into, d.col = c, d.row = int(cols[c].tiles.size());
    for (const Placed &p : l.tiles) {
        if (p.col != c) continue;
        if (el >= (p.el0 + p.el1) / 2) {   // above this one's middle: before it (rows go top down)
            d.row = std::min(d.row, p.row);
        }
    }
    return d;
}

}  // namespace tiling
