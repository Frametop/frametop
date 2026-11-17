// Where the dashboard's tiles go (screens/vrtiles.cpp's dashboard tiling; tilegrid.h holds their order and sizes).
//
// A command wall (user, 2026-11-17: "more in front as a command center ... like multiple monitors
// further away"; of the mockups, "Monitor desk"): the panels stand side by side round you,
// `radius` away, like monitors of different sizes on a desk. Each keeps its natural size, the same
// number of its pixels a degree as every other (`pxPerDeg`: text the same size on all of them),
// times its own size, which you set (user: "the user gets to choose how big a display should be
// and resize it"; no cap). The columns stand on a desk line `deskDeg` up from your eyes (below
// them): a column's bottom panel's lower edge on it, the panels above resting on the one under
// them, a gap between. A column is as wide round you as its widest panel needs at its farthest
// corner (each is centred in it), and the strip of columns is centred in front of you, a gap
// between each two. A new panel goes on top of the lowest column once the desk is `wallDeg` wide
// (dashtiles.h's Board). Steam's menu is part of it (user, 2026-11-16: "the steam menu should ...
// be part of the tiling system"): a slot kept clear where it is (`reserveDeg` wide at
// `reserveAzDeg`), a split's worth of columns left of it and the rest right. The split is the
// grid's (tilegrid.h: inserts, drops and moves keep each column on its side), and with one the
// slot is always in the strip. Without one (none chosen yet: dashtiles.h's Board keeps one for
// each place the menu opens), the slot is there only when the strip centred in front of you would
// reach it, with the split that puts the strip's middle nearest in front of you. Past a full turn
// round you, or with a column reaching past kMaxTopDeg up, everything shrinks until it fits.
//
// Every panel's corners are `radius` from the centre, not its middle (user, 2026-11-17: "make the
// distances of the displays be based off the edges and not the center ... some are wider than
// others"): laid out at the radius, each one then moves along its own line from the centre, and
// grows or shrinks with it, till its corners are there. Seen from the centre nothing changes (every
// span and gap is the same), but a wider panel comes nearer, and neighbours' corners meet at the
// same depth.
//
// Every tiled panel is flat: the spans here are a flat panel's, and a curved one reaches further
// round you than its column (user, 2026-11-17: tiles "should stay the same distance around each
// other, not overlap"; a desktop curved 1.5 m round ran 9 degrees into its neighbour).
// FlatWhileTiled keeps a panel's own curve while it's tiled and gives it back when it leaves.
//
// Angles in degrees. Azimuth is round you, 0 in front, positive to the right; elevation is up
// from your eyes' height. The group frame: origin at the sphere's centre (your head when the
// dashboard opened), -Z the way you faced then, +Y up, as SteamVR's axes are.
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
    double widthM = 1.6, heightM = 0.9;   // its size with its middle at the wall: natural, times its own
};

struct Column {
    std::vector<Tile> tiles;    // top to bottom (the last one on the desk)
};

struct Params {
    double radius = 2.7;        // metres: the wall's, the tiles' corners on it
    double deskDeg = -12;       // the desk line, up from your eyes: the bottom panels' lower edges
    double pxPerDeg = 48;       // a panel's natural size: this many of its pixels a degree (1920 wide: about 40)
    double gapDeg = 0.8;        // between columns, and between the panels in one (about 4 cm at 2.7 m)
    double wallDeg = 120;       // the desk this wide round you: a new panel goes on top of a column
    double maxSpanDeg = 360;    // the strip round you, gaps included, at most
    double reserveAzDeg = 0;    // Steam's menu, a slot kept clear when the strip would reach it: its middle
    double reserveDeg = 0;      // ...and its width (0: none; the strip is centred on 0 then)
};
constexpr double kMaxTopDeg = 80;   // no column reaches higher: the wall shrinks first

// A panel's natural size: metres a pixel at the wall, `pxPerDeg` of them a degree in its middle.
inline double MetresPerPx(const Params &p) { return 2 * p.radius * std::tan(Rad(1 / std::max(1.0, p.pxPerDeg)) / 2); }

struct Placed {
    int id = 0;
    int col = 0, row = 0;
    double azDeg = 0, elDeg = 0;          // its centre
    double widthM = 0, heightM = 0;       // its size, metres
    double distM = 0;                     // its middle from the centre (its corners: Params::radius)
    double az0 = 0, az1 = 0;              // its column's span round you
    double el0 = 0, el1 = 0;              // its own span up, bottom to top
};

struct Layout {
    std::vector<Placed> tiles;
    double az0 = 0, az1 = 0;              // the strip
    double el0 = 0, el1 = 0;              // the desk line, and the tallest column's top
    double shrink = 1;                    // 1, or less when it took more than a turn (or reached past kMaxTopDeg)
    int split = -1;                       // the columns left of Steam's slot (-1: no slot in the strip)
    double slotAz = 0;                    // ...the slot's middle (a drop left or right of it)
};

// A panel's curve (a cylinder's radius, 0 flat) while it's `tiled`, its own kept in `kept` until it
// isn't: true when `curve` changed. A curve given it while it's tiled (its curve button) is kept
// for later in the same way; one given it once it has left (grab that's) stays.
inline bool FlatWhileTiled(bool tiled, double *curve, double *kept) {
    if (tiled) {
        if (*curve <= 0) return false;
        *kept = *curve, *curve = 0;
        return true;
    }
    if (*kept <= 0) return false;
    const bool back = *curve <= 0;
    if (back) *curve = *kept;
    *kept = 0;
    return back;
}

// A panel laid out at `radius` (its middle there, facing the centre) moved along its line from the
// centre, its size with it, till its corners are at `radius`: the same spans seen from the centre.
inline void CornersOnSphere(double radius, Placed *pl) {
    const double u = pl->widthM / (2 * radius), v = pl->heightM / (2 * radius);
    const double f = 1 / std::sqrt(1 + u * u + v * v);
    pl->distM = radius * f, pl->widthM *= f, pl->heightM *= f;
}

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
// `az` a turn either way to within half a turn of `centre` (a strip with Steam's slot off to one
// side can run past 180 degrees: an azimuth seen from the head is in -180..180).
inline double WrapNear(double az, double centre) { return az - 360 * std::round((az - centre) / 360); }

// A column's panels stacked on the desk at size `k` (index as its tiles: the last one on the desk):
// each one's span up and its size, and the column's span round you.
struct Stacked {
    double el0 = 0, el1 = 0, widthM = 0, heightM = 0;
};
inline std::vector<Stacked> Stack(const Column &c, const Params &p, double k, double *spanDeg) {
    std::vector<Stacked> out(c.tiles.size());
    double el = p.deskDeg;
    *spanDeg = 0;
    for (size_t r = c.tiles.size(); r-- > 0;) {
        const Tile &t = c.tiles[r];
        Stacked &s = out[r];
        s.widthM = std::max(0.01, t.widthM) * k, s.heightM = std::max(0.01, t.heightM) * k;
        s.el0 = el, s.el1 = el + Deg(2 * std::atan2(s.heightM / 2, p.radius));
        el = s.el1 + p.gapDeg * k;
        *spanDeg = std::max(*spanDeg, SpanFor(p.radius, s.widthM, s.heightM, (s.el0 + s.el1) / 2));
    }
    return out;
}

// The strip's columns at a size that fits in a turn and under kMaxTopDeg, with or without a slot
// `slot` wide: each column's span and the strip's width (its gaps in, the one behind you out), and
// the size.
struct Fit {
    std::vector<double> spans;
    double total = 0, k = 1;
};
inline Fit FitStrip(const std::vector<Column> &cols, const Params &p, double slot) {
    // the strip at size k: the panels and the gaps scale with it; a gap between each two columns,
    // one each side of a slot (not scaled: it's Steam's size), and one where the strip's ends would
    // meet behind you
    auto spansAt = [&](double k, double *total, double *top) {
        std::vector<double> s(cols.size());
        *total = p.gapDeg * k * double(cols.size()) + (slot > 0 ? slot + p.gapDeg * k : 0);
        *top = p.deskDeg;
        for (size_t c = 0; c < cols.size(); ++c) {
            const std::vector<Stacked> st = Stack(cols[c], p, k, &s[c]);
            if (!st.empty()) *top = std::max(*top, st.front().el1);
            *total += s[c];
        }
        return s;
    };
    Fit f;
    double top;
    f.spans = spansAt(1, &f.total, &top);
    const double room = p.maxSpanDeg;   // (the slot is at most half of it: the columns get the rest)
    if ((f.total > room || top > kMaxTopDeg) && !cols.empty()) {
        // too big: a k that fits, by halving (everything goes to nothing with k, so one fits)
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {   // (to within 1e-7 of the size)
            const double mid = (lo + hi) / 2;
            double t, tp;
            spansAt(mid, &t, &tp);
            (t <= room && tp <= kMaxTopDeg ? lo : hi) = mid;
        }
        f.k = std::max(lo, 1e-3);
        f.spans = spansAt(f.k, &f.total, &top);
    }
    f.total = cols.empty() ? 0 : f.total - p.gapDeg * f.k;
    return f;
}

// The split that puts the strip's middle nearest in front of you: with `left` columns left of a
// slot at `az`, the strip runs from az - slot/2 - (their spans and gaps) to az + slot/2 + (the
// rest's). Even columns round a slot in front: half each side (an odd one: the extra one right).
inline int CentredSplit(const std::vector<double> &spans, double gap, double az) {
    double rest = 0;
    for (double s : spans) rest += gap + s;
    double leftSpan = 0, best = 1e9;
    int split = 0;
    for (size_t j = 0; j <= spans.size(); ++j) {
        const double off = std::fabs(az + (rest - leftSpan) / 2);
        if (off < best - 1e-9) best = off, split = int(j);
        if (j < spans.size()) leftSpan += spans[j] + gap, rest -= gap + spans[j];
    }
    return split;
}

// `split`: the columns left of Steam's slot (the grid's: -1 for one not chosen yet).
inline Layout Arrange(const std::vector<Column> &cols, const Params &p, int split = -1) {
    Layout out;
    // Steam's slot: with a split, always; without, when the strip centred in front of you would
    // reach it (a gap's width short of it)
    const double slot = std::min(p.reserveDeg, p.maxSpanDeg / 2);
    const double slotAz = WrapNear(p.reserveAzDeg, 0);
    const bool has = slot > 0 && !cols.empty();
    Fit f = FitStrip(cols, p, has && split >= 0 ? slot : 0);
    const bool reserve = has && (split >= 0 || std::fabs(slotAz) - slot / 2 < f.total / 2 + p.gapDeg * f.k);
    if (reserve && split < 0) f = FitStrip(cols, p, slot);
    const double k = f.k, total = f.total;
    const std::vector<double> &spans = f.spans;
    out.shrink = k;
    const double gap = p.gapDeg * k;
    out.el0 = out.el1 = p.deskDeg;
    // the split's columns left of Steam's slot (each with its gap), the rest right of it
    const size_t left = !reserve ? 0 : split >= 0 ? std::min(size_t(split), cols.size()) : size_t(CentredSplit(spans, gap, slotAz));
    double leftSpan = 0;
    for (size_t c = 0; c < left; ++c) leftSpan += spans[c] + gap;
    double az = reserve ? slotAz - slot / 2 - leftSpan : -total / 2;
    out.az0 = az, out.az1 = az + total;
    if (reserve) out.split = int(left), out.slotAz = slotAz;
    for (size_t c = 0; c < cols.size(); ++c) {
        if (reserve && c == left) az += slot + gap;
        const double a0 = az, a1 = az + spans[c];
        az = a1 + gap;
        double span;
        const std::vector<Stacked> st = Stack(cols[c], p, k, &span);
        for (size_t r = 0; r < st.size(); ++r) {
            Placed pl;
            pl.id = cols[c].tiles[r].id, pl.col = int(c), pl.row = int(r);
            pl.el0 = st[r].el0, pl.el1 = st[r].el1;
            pl.az0 = a0, pl.az1 = a1;
            pl.azDeg = (a0 + a1) / 2, pl.elDeg = (pl.el0 + pl.el1) / 2;
            pl.widthM = st[r].widthM, pl.heightM = st[r].heightM;
            CornersOnSphere(p.radius, &pl);
            out.el1 = std::max(out.el1, pl.el1);
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
    bool leftOfSlot = false;   // NewColumn: let go left of Steam's slot (at the split: which side it goes)
};
constexpr double kTearMarginDeg = 10;   // this far past the strip, under the desk or over the tallest column: torn off

inline Drop DropAt(const Layout &l, const std::vector<Column> &cols, double az, double el) {
    Drop d;
    if (cols.empty()) return d.kind = Drop::NewColumn, d.col = 0, d;
    az = WrapNear(az, (l.az0 + l.az1) / 2);
    if (az < l.az0 - kTearMarginDeg || az > l.az1 + kTearMarginDeg || el < l.el0 - kTearMarginDeg ||
        el > l.el1 + kTearMarginDeg)
        return d;
    d.leftOfSlot = l.split >= 0 && az < l.slotAz;
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
