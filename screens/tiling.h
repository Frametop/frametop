// Where the dashboard's tiles go (vr.cpp's dashboard tiling; tilegrid.h holds their order and sizes).
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

// The angle round you a panel this wide needs at its highest corner's elevation (`worstEl`), so it
// doesn't run into its neighbours there.
inline double SpanFor(double radius, double widthM, double worstEl) {
    const double c = std::max(0.05, std::cos(Rad(worstEl)));
    const double half = std::min(1.0, widthM / 2 / (radius * c));
    return Deg(2 * std::asin(half));
}

inline Layout Arrange(const std::vector<Column> &cols, const Params &p) {
    Layout out;
    const double top = std::min(p.bottomDeg + p.heightDeg, p.maxTopDeg);
    const double band = std::max(p.minHeightDeg, top - p.bottomDeg);
    double k = 1;
    std::vector<double> spans(cols.size());
    for (int pass = 0; pass < 6; ++pass) {
        const double h = band * k;
        double total = 0;
        for (size_t c = 0; c < cols.size(); ++c) {
            const Column &col = cols[c];
            double span = (col.widthDeg > 0 ? col.widthDeg : p.columnDeg) * k;
            double sum = 0;
            for (const Tile &t : col.tiles) sum += std::max(0.05, t.weight);
            const double avail = std::max(1.0, h - p.gapDeg * (double(col.tiles.size()) - 1));
            double elTop = p.bottomDeg + h;
            for (const Tile &t : col.tiles) {
                const double rowDeg = avail * std::max(0.05, t.weight) / std::max(0.05, sum);
                const double e1 = elTop, e0 = elTop - rowDeg;
                elTop = e0 - p.gapDeg;
                if (!t.fixed) continue;
                const double worst = std::max(std::fabs(e0), std::fabs(e1));
                span = std::max(span, SpanFor(p.radius, t.aspect * Chord(p.radius, rowDeg), worst));
            }
            spans[c] = span;
            total += span;
        }
        total += p.gapDeg * std::max(0.0, double(cols.size()) - 1);
        const double room = p.maxSpanDeg - p.gapDeg;
        if (total <= room + 1e-9 || cols.empty()) break;
        k *= room / total * 0.999;
    }
    out.shrink = k;
    const double h = band * k;
    out.el0 = p.bottomDeg, out.el1 = p.bottomDeg + h;
    double total = 0;
    for (double s : spans) total += s;
    total += p.gapDeg * std::max(0.0, double(cols.size()) - 1);
    double az = -total / 2;
    out.az0 = az, out.az1 = az + total;
    for (size_t c = 0; c < cols.size(); ++c) {
        const Column &col = cols[c];
        const double a0 = az, a1 = az + spans[c];
        az = a1 + p.gapDeg;
        double sum = 0;
        for (const Tile &t : col.tiles) sum += std::max(0.05, t.weight);
        const double avail = std::max(1.0, h - p.gapDeg * (double(col.tiles.size()) - 1));
        double elTop = p.bottomDeg + h;
        for (size_t r = 0; r < col.tiles.size(); ++r) {
            const Tile &t = col.tiles[r];
            const double rowDeg = avail * std::max(0.05, t.weight) / std::max(0.05, sum);
            Placed pl;
            pl.id = t.id, pl.col = int(c), pl.row = int(r);
            pl.el1 = elTop, pl.el0 = elTop - rowDeg;
            elTop = pl.el0 - p.gapDeg;
            pl.az0 = a0, pl.az1 = a1;
            pl.azDeg = (a0 + a1) / 2, pl.elDeg = (pl.el0 + pl.el1) / 2;
            pl.heightM = Chord(p.radius, rowDeg);
            const double worst = std::max(std::fabs(pl.el0), std::fabs(pl.el1));
            // A resizable panel takes its column's width where it's narrowest (its highest corner);
            // a fixed one its own, no wider than that.
            const double cellW = 2 * p.radius * std::cos(Rad(worst)) * std::sin(Rad(a1 - a0) / 2);
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
