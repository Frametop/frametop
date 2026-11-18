// Where the dashboard's tiles go (screens/vrtiles.cpp's dashboard tiling; tilegrid.h holds their order and sizes).
//
// A command wall (user, 2026-11-17: "more in front as a command center ... like multiple monitors
// further away"; of the mockups, "Monitor desk"): the panels stand side by side round you,
// `radius` away, like monitors of different sizes on a desk. Each keeps its natural size, the same
// number of its pixels a degree as every other (`pxPerDeg`: text the same size on all of them),
// times its own size, which you set (user: "the user gets to choose how big a display should be
// and resize it"; no cap). The columns stand on a desk line `deskDeg` up from your eyes (below
// them): a column's bottom panel's lower edge on it, the panels above resting on the one under
// them, a gap between. The columns pack as close as their panels let them (Pack: a gap between
// panels that share some height, not between whole columns), the strip centred in front of you. A
// column's narrower panels lean against one edge of its widest (Lean: the way that packs the strip
// narrowest, else the strip's middle), so what room is left goes to the outside, not between monitors. A new panel goes on top of the lowest column once the desk is `wallDeg` wide
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
    double az0 = 0, az1 = 0;              // its own span round you
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
// each one's span up, its size, and its span round you.
struct Stacked {
    double el0 = 0, el1 = 0, widthM = 0, heightM = 0, spanDeg = 0;
    double offDeg = 0;   // its middle from its column's, round you (Pack's lean)
};
inline std::vector<Stacked> Stack(const Column &c, const Params &p, double k) {
    std::vector<Stacked> out(c.tiles.size());
    double el = p.deskDeg;
    for (size_t r = c.tiles.size(); r-- > 0;) {
        const Tile &t = c.tiles[r];
        Stacked &s = out[r];
        s.widthM = std::max(0.01, t.widthM) * k, s.heightM = std::max(0.01, t.heightM) * k;
        s.el0 = el, s.el1 = el + Deg(2 * std::atan2(s.heightM / 2, p.radius));
        s.spanDeg = SpanFor(p.radius, s.widthM, s.heightM, (s.el0 + s.el1) / 2);
        el = s.el1 + p.gapDeg * k;
    }
    return out;
}

// The columns side by side at size `k`, left to right, with Steam's slot (`slot` wide, not scaled:
// it's Steam's size) before column `slotAt` (-1 or a 0 `slot`: none). As little room between them
// as can be (user, 2026-11-17: "very large gaps between the left and right sides of the displays
// ... as minimal space between the monitors as possible"): each panel keeps its own span round
// you, and each column comes as near the ones left of it as lets every panel in it keep a gap
// from every panel there it shares some height with (a gap up or down counts as sharing). So a
// wide or tall panel high in one column doesn't push a short neighbour's panels away at the desk.
// The slot reaches all the way up. `lean` (one a column, or none: all 0) says where a column's
// narrower panels go under its widest: -1 against its left edge, 0 centred, +1 its right.
struct Packed {
    std::vector<std::vector<Stacked>> stacks;   // each column's (Stack)
    std::vector<double> mid;                    // each column's middle, from the strip's left end
    double slotMid = -1;                        // the slot's (-1: none)
    double width = 0;                           // the strip, round you
    double top = 0;                             // the tallest column's top
};
inline Packed Pack(const std::vector<Column> &cols, const Params &p, double k, double slot, int slotAt,
                   const std::vector<int> &lean = {}) {
    Packed out;
    out.top = p.deskDeg;
    const double gap = p.gapDeg * k;
    struct Box {
        double lo, hi, el0, el1;   // (round you from its column's middle)
    };
    std::vector<std::vector<Box>> placed;   // (the columns and the slot so far, left to right)
    std::vector<double> mids;
    const auto place = [&](const std::vector<Box> &boxes) {
        // (nothing past the strip's left end; left to right in order as every column has a panel on
        // the desk, which shares height with the ones before)
        double m = 0;
        for (const Box &b : boxes) m = std::max(m, -b.lo);
        for (size_t a = 0; a < placed.size(); ++a)
            for (const Box &x : placed[a])
                for (const Box &y : boxes)
                    if (x.el0 < y.el1 + gap - 1e-9 && y.el0 < x.el1 + gap - 1e-9) m = std::max(m, mids[a] + x.hi + gap - y.lo);
        for (const Box &b : boxes) out.width = std::max(out.width, m + b.hi);
        placed.push_back(boxes), mids.push_back(m);
        return m;
    };
    for (size_t c = 0; c <= cols.size(); ++c) {
        if (slot > 0 && int(c) == slotAt) out.slotMid = place({{-slot / 2, slot / 2, -1e9, 1e9}});
        if (c == cols.size()) break;
        out.stacks.push_back(Stack(cols[c], p, k));
        std::vector<Stacked> &st = out.stacks.back();
        double widest = 0;
        for (const Stacked &s : st) widest = std::max(widest, s.spanDeg / 2);
        const int l = c < lean.size() ? lean[c] : 0;
        std::vector<Box> boxes;
        for (Stacked &s : st) {
            s.offDeg = l * (widest - s.spanDeg / 2);
            boxes.push_back({s.offDeg - s.spanDeg / 2, s.offDeg + s.spanDeg / 2, s.el0, s.el1});
            out.top = std::max(out.top, s.el1);
        }
        out.mid.push_back(place(boxes));
    }
    return out;
}

// Which way each column's narrower panels lean (Pack's `lean`; user, 2026-11-17, of a narrow panel
// centred under a wide one: room either side of it that only a short neighbour can use). Each
// column with panels of different widths starts leaning toward the strip's middle (what room is
// left goes to the strip's outside), and then takes whichever way, a column at a time, packs the
// strip narrower (a short neighbour tucked under the wide one's other side), till none does.
inline std::vector<int> Lean(const std::vector<Column> &cols, const Params &p) {
    std::vector<int> lean(cols.size(), 0);
    const Packed centred = Pack(cols, p, 1, 0, -1);
    std::vector<size_t> uneven;
    for (size_t c = 0; c < cols.size(); ++c) {
        double lo = 1e9, hi = 0;
        for (const Stacked &s : centred.stacks[c]) lo = std::min(lo, s.spanDeg), hi = std::max(hi, s.spanDeg);
        if (hi - lo < 1e-6) continue;
        const double d = centred.mid[c] - centred.width / 2;
        lean[c] = d < -1e-6 ? 1 : d > 1e-6 ? -1 : 0;
        uneven.push_back(c);
    }
    double best = Pack(cols, p, 1, 0, -1, lean).width;
    for (int sweep = 0; sweep < 3; ++sweep) {   // (each sweep only ever narrower: a few at most)
        bool better = false;
        for (const size_t c : uneven)
            for (const int way : {-1, 0, 1}) {
                if (way == lean[c]) continue;
                std::vector<int> trial = lean;
                trial[c] = way;
                const double w = Pack(cols, p, 1, 0, -1, trial).width;
                if (w < best - 1e-6) best = w, lean = trial, better = true;
            }
        if (!better) break;
    }
    return lean;
}

// The strip packed at a size that fits: in a turn, with a gap where its ends would meet behind
// you, and under kMaxTopDeg. 1, or the largest smaller one that fits.
struct Fit {
    Packed packed;
    double k = 1;
};
inline Fit FitStrip(const std::vector<Column> &cols, const Params &p, double slot, int slotAt, const std::vector<int> &lean) {
    const auto fits = [&](const Packed &q, double k) { return q.width + p.gapDeg * k <= p.maxSpanDeg && q.top <= kMaxTopDeg; };
    Fit f;
    f.packed = Pack(cols, p, 1, slot, slotAt, lean);
    if (cols.empty() || fits(f.packed, 1)) return f;
    double lo = 0, hi = 1;   // (everything goes to nothing with k but the slot, at most half a turn: one fits)
    for (int i = 0; i < 24; ++i) {   // (to within 1e-7 of the size)
        const double mid = (lo + hi) / 2;
        (fits(Pack(cols, p, mid, slot, slotAt, lean), mid) ? lo : hi) = mid;
    }
    f.k = std::max(lo, 1e-3);
    f.packed = Pack(cols, p, f.k, slot, slotAt, lean);
    return f;
}

// `split`: the columns left of Steam's slot (the grid's: -1 for one not chosen yet).
inline Layout Arrange(const std::vector<Column> &cols, const Params &p, int split = -1) {
    Layout out;
    // Steam's slot: with a split, always; without, when the strip centred in front of you would
    // reach it (a gap's width short of it), with the split that puts the strip's middle nearest in
    // front of you (even columns round a slot in front: half each side; an odd one: the extra one right)
    const double slot = std::min(p.reserveDeg, p.maxSpanDeg / 2);
    const double slotAz = WrapNear(p.reserveAzDeg, 0);
    const std::vector<int> lean = Lean(cols, p);
    Fit f = FitStrip(cols, p, 0, -1, lean);
    const bool reserve =
        slot > 0 && !cols.empty() && (split >= 0 || std::fabs(slotAz) - slot / 2 < f.packed.width / 2 + p.gapDeg * f.k);
    int left = -1;
    if (reserve && split >= 0) {
        left = std::min(split, int(cols.size()));
        f = FitStrip(cols, p, slot, left, lean);
    } else if (reserve) {
        double best = 1e9;
        for (int j = 0; j <= int(cols.size()); ++j) {
            Fit g = FitStrip(cols, p, slot, j, lean);
            const double off = std::fabs(slotAz - g.packed.slotMid + g.packed.width / 2);
            if (off < best - 1e-9) best = off, left = j, f = std::move(g);
        }
    }
    const Packed &pk = f.packed;
    out.shrink = f.k;
    out.az0 = reserve ? slotAz - pk.slotMid : -pk.width / 2;   // (the slot where Steam's menu is, or the strip centred)
    out.az1 = out.az0 + pk.width;
    if (reserve) out.split = left, out.slotAz = slotAz;
    out.el0 = out.el1 = p.deskDeg;
    for (size_t c = 0; c < cols.size(); ++c) {
        const double mid = out.az0 + pk.mid[c];
        const std::vector<Stacked> &st = pk.stacks[c];
        for (size_t r = 0; r < st.size(); ++r) {
            Placed pl;
            pl.id = cols[c].tiles[r].id, pl.col = int(c), pl.row = int(r);
            pl.el0 = st[r].el0, pl.el1 = st[r].el1;
            pl.azDeg = mid + st[r].offDeg;
            pl.az0 = pl.azDeg - st[r].spanDeg / 2, pl.az1 = pl.azDeg + st[r].spanDeg / 2;
            pl.elDeg = (pl.el0 + pl.el1) / 2;
            pl.widthM = st[r].widthM, pl.heightM = st[r].heightM;
            CornersOnSphere(p.radius, &pl);
            out.el1 = std::max(out.el1, pl.el1);
            out.tiles.push_back(pl);
        }
    }
    return out;
}

// The layout turned `deg` round you, all of it (dashtiles.h's Board keeps the strip where it was
// after a change this way: Arrange always centres it).
inline void Turn(Layout *l, double deg) {
    for (Placed &p : l->tiles) p.azDeg += deg, p.az0 += deg, p.az1 += deg;
    l->az0 += deg, l->az1 += deg, l->slotAz += deg;
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
    // the column of the panel it's over (a gap: the nearest one's, round you and up; packed columns
    // can share an azimuth at different heights)
    int c = 0;
    double best = 1e9;
    double a0 = 0, a1 = 0;
    for (const Placed &p : l.tiles) {
        const double dAz = az < p.az0 ? p.az0 - az : az > p.az1 ? az - p.az1 : 0;
        const double dEl = el < p.el0 ? p.el0 - el : el > p.el1 ? el - p.el1 : 0;
        if (dAz + dEl < best) best = dAz + dEl, c = p.col, a0 = p.az0, a1 = p.az1;
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
