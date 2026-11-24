// One curve for the centre display, another beside it and another over and under it (screens/tilecurve.h): where the tiles go.
#include "../tilecurve.h"

#include <cstdio>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

struct V { double x, y, z; };
// A placed tile's point, `u` across and `v` up from its middle, halves of its size.
static V At(const double m[3][4], const tiling::Placed &p, double u, double v) {
    const double a = u * p.widthM / 2, b = v * p.heightM / 2;
    return {m[0][0] * a + m[0][1] * b + m[0][3], m[1][0] * a + m[1][1] * b + m[1][3], m[2][0] * a + m[2][1] * b + m[2][3]};
}
static double Dist(V a, V b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

int main() {
    using namespace tilecurve;
    tiling::Params p;
    tiling::FromEyes({1.4, -12, 0.8, 70}, 1.6, &p);   // (the centre's curve 3 m, 1.4 m away)
    p.reserveDeg = 0;
    // a wide centre with two over it, and two columns each side, one of them two high
    std::vector<tiling::Column> cols(5);
    int id = 0;
    const auto tile = [&](double w, double h) { return tiling::Tile{id++, w, h}; };
    cols[0].tiles = {tile(0.7, 0.45)};
    cols[1].tiles = {tile(0.6, 0.4), tile(0.8, 0.5)};
    cols[2].tiles = {tile(0.8, 0.4), tile(0.8, 0.4), tile(1.7, 0.6)};
    cols[3].tiles = {tile(0.8, 0.5)};
    cols[4].tiles = {tile(0.6, 0.4), tile(0.7, 0.45)};
    const int centre = 5;
    tiling::Layout l = tiling::Arrange(cols, p);
    const auto of = [&](int i) { for (const auto &t : l.tiles) if (t.id == i) return t; return tiling::Placed{}; };
    double mid = of(centre).azDeg;
    tiling::Turn(&l, -mid);

    const auto pose = [&](const Wall &s, int i, double m[3][4]) { const tiling::Placed t = of(i); Place(s, t, t.distM, m); };
    const auto same = [&](const Wall &a, const Wall &b, double eps) {
        bool ok = true;
        for (const auto &t : l.tiles) {
            double m[3][4], n[3][4];
            pose(a, t.id, m), pose(b, t.id, n);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) ok = ok && near(m[r][c], n[r][c], eps);
        }
        return ok;
    };
    const Wall off = For(l, centre, p, 0, 0);
    check("not set: one curve, the sphere's own poses", !off.On() && !For(l, centre, p, 0, p.radius).On());
    check("the same as the centre's: one curve", !For(l, centre, p, p.radius, 0).On());
    check("next to the same: next to the same poses", same(off, For(l, centre, p, p.radius * 0.9999, 0), 1e-3));

    const Wall s = For(l, centre, p, 1.2, 0);
    check("the centre's section: its column and half a gap", s.On() && near(s.a0, -s.a1) && s.a1 > of(centre).az1 && s.a1 < of(centre).az1 + p.gapDeg);
    bool still = true;
    for (int i : {3, 4, 5}) {
        double m[3][4], n[3][4];
        pose(s, i, m), pose(off, i, n);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) still = still && near(m[r][c], n[r][c]);
    }
    check("the centre and the tiles over it stay on its curve", still);
    // the sides face you more: the angle between a tile's front and the way to your eyes (0, 0, -backM)
    const auto off_deg = [&](const Wall &q, int i) {
        double m[3][4];
        pose(q, i, m);
        const V e{-m[0][3], -m[1][3], -p.backM - m[2][3]};
        return Deg(std::acos((e.x * m[0][2] + e.y * m[1][2] + e.z * m[2][2]) / std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z)));
    };
    check("the sides face you better than with one curve", off_deg(s, 0) < off_deg(off, 0) - 5 && off_deg(s, 2) < off_deg(off, 2) - 3 &&
                                                               off_deg(s, 6) < off_deg(off, 6) - 3);
    // neighbours along the desk: corner to corner, about the gap apart, and no two in each other
    const auto gap = [&](const Wall &q, int a, int b) {
        double m[3][4], n[3][4];
        pose(q, a, m), pose(q, b, n);
        return Dist(At(m, of(a), 1, -1), At(n, of(b), -1, -1));
    };
    for (double rs : {0.8, 1.2, 2.0, 5.0, 12.0}) {
        const Wall q = For(l, centre, p, rs, 0);
        bool ok = true;
        for (auto [a, b] : {std::pair{0, 2}, {2, 5}, {5, 6}, {6, 8}}) ok = ok && gap(q, a, b) > 0.005 && gap(q, a, b) < 0.14;
        char what[96];
        std::snprintf(what, sizeof what, "sides %.1f m: neighbours' lower corners meet, a gap apart", rs);
        check(what, ok);
        bool apart = true, size = true;
        for (const auto &t : l.tiles) {   // (each tile's own share of the turn: its corners within it)
            const int side = q.SideOf(t.azDeg);
            if (!side) continue;
            double m[3][4], x, z;
            pose(q, t.id, m);
            detail::Axis(q, side, &x, &z);
            for (int u : {-1, 1})
                for (int v : {-1, 1}) {
                    const V c = At(m, t, u, v);
                    const double h = tiling::WrapNear(Deg(std::atan2(c.x - x, -(c.z - z))), q.Heading(t.azDeg));
                    apart = apart && std::fabs(h - q.Heading(t.azDeg)) <= (t.az1 - t.az0) / 2 * q.rc / q.rs + 1e-6;
                }
            size = size && Dist(At(m, t, -1, 0), At(m, t, 1, 0)) > t.widthM - 1e-9;
        }
        std::snprintf(what, sizeof what, "sides %.1f m: each tile its own size, within its own share of the turn", rs);
        check(what, apart && size);
        bool rows = true;
        for (auto [low, high] : {std::pair{2, 1}, {8, 7}}) {   // (the row over: not in the one under it, nor far over it)
            double m[3][4], n[3][4];
            pose(q, low, m), pose(q, high, n);
            const V a = At(m, of(low), 0, 1), b = At(n, of(high), 0, -1);
            rows = rows && b.y > a.y - 1e-3 && b.y - a.y < 0.08;
        }
        std::snprintf(what, sizeof what, "sides %.1f m: the rows over stand on the ones under, neither in them nor off them", rs);
        check(what, rows);
        double worst = 0;
        for (const auto &t : l.tiles) {   // (let go where it was: the same place)
            double m[3][4], az, el;
            pose(q, t.id, m);
            const double pt[3] = {m[0][3], m[1][3], m[2][3]};
            Direction(q, pt, &az, &el);
            worst = std::max({worst, std::fabs(az - t.azDeg), std::fabs(el - t.elDeg)});
        }
        std::snprintf(what, sizeof what, "sides %.1f m: a tile's place back to its azimuth and elevation (within %.2f degrees)", rs, worst);
        check(what, worst < 1.5);
        const double join = q.a1;   // the wall's own line either side of the join
        double m[3][4], n[3][4];
        tiling::Placed t;
        t.widthM = t.heightM = 1e-6, t.az0 = join - 1e-5, t.az1 = join + 1e-5;
        t.azDeg = join - 1e-5, Place(q, t, q.rc, m);
        t.azDeg = join + 1e-5, t.az1 = t.azDeg + 1e-5, t.az0 = t.azDeg - 1e-5;
        t.widthM = 2 * q.rc * std::tan(Rad(1e-5)), Place(q, t, q.rc, n);
        check("...no step and no corner where the curves join", Dist({m[0][3], m[1][3], m[2][3]}, {n[0][3], n[1][3], n[2][3]}) < 1e-2 &&
                                                                    near(m[0][2], n[0][2], 1e-3) && near(m[2][2], n[2][2], 1e-3));
    }
    // the rows over and under: their own curve up and down
    check("rows next to the same curve: next to the same poses", same(off, For(l, centre, p, 0, p.radius * 0.9999), 2e-3));
{
        const Wall q = For(l, centre, p, 1.2, 1.0);
        double m[3][4], n[3][4];
        pose(q, 3, m), pose(q, 1, n);
        check("rows at tight sides lean toward you too, near as much as the centre's", n[1][2] < -0.05 && n[1][2] < 0.5 * m[1][2]);
    }
    for (double rs : {0.0, 1.2, 5.0})
        for (double rh : {0.5, 0.8, 1.2, 2.0, 12.0}) {
            const Wall q = For(l, centre, p, rs, rh), base = For(l, centre, p, rs, 0);
            char what[128];
            const auto say = [&](const char *about) { std::snprintf(what, sizeof what, "sides %.1f m, rows %.1f m: %s", rs, rh, about); return what; };
            double m[3][4], n[3][4];
            pose(q, centre, m), pose(base, centre, n);
            check(say("the centre display stays, its own size"), near(m[1][3], n[1][3]) && near(m[2][3], n[2][3]) && near(m[1][2], n[1][2]) && Fit(q, of(centre)) == 1);
            bool leans = true;
            for (int i : {3, 4, 1, 7}) {   // (front down toward you: more with a tighter curve, less with a flatter)
                pose(q, i, m), pose(base, i, n);
                leans = leans && (rh < p.radius ? m[1][2] < n[1][2] - 1e-4 : m[1][2] > n[1][2] + 1e-4 || rs > 0);
            }
            check(say("the rows over lean toward you more, or less"), leans);
            bool fits = true, within = true, touch = true;
            for (const auto &t : l.tiles) {
                const double f = Fit(q, t);
                fits = fits && f <= 1 && f >= 0.7;
                tiling::Placed g = t;
                g.widthM *= f, g.heightM *= f;
                pose(q, t.id, m);
                const int side = q.SideOf(t.azDeg);
                double x, z;
                detail::Axis(q, side, &x, &z);
                for (int u : {-1, 1})
                    for (int v : {-1, 1}) {
                        const V c = At(m, g, u, v);
                        const double h = tiling::WrapNear(Deg(std::atan2(c.x - x, -(c.z - z))), q.Heading(t.azDeg));
                        within = within && std::fabs(h - q.Heading(t.azDeg)) <= (t.az1 - t.az0) / 2 * q.rc / q.Radius(side) + 1e-6;
                    }
            }
            check(say("no tile grows, none under its smallest"), fits);
            check(say("each within its own share of the turn"), within);
            for (auto [low, high] : {std::pair{2, 1}, {8, 7}, {5, 3}, {5, 4}}) {   // (a row's bottom edge at the top of the one under: in it or off it)
                tiling::Placed a = of(low), b = of(high);
                a.widthM *= Fit(q, a), a.heightM *= Fit(q, a), b.widthM *= Fit(q, b), b.heightM *= Fit(q, b);
                pose(q, low, m), pose(q, high, n);
                const V at = At(m, a, 0, 1), bt = At(n, b, 0, -1);
                const double ax = std::hypot(at.x, at.z), bx = std::hypot(bt.x, bt.z);
                touch = touch && bt.y - at.y > -0.005 && bt.y - at.y < 0.1 && (low == 5 || std::fabs(ax - bx) < 0.15);
            }
            check(say("each row stands on the one under, not in it"), touch);
            double worst = 0;
            for (const auto &t : l.tiles) {
                double az, el;
                pose(q, t.id, m);
                const double pt[3] = {m[0][3], m[1][3], m[2][3]};
                Direction(q, pt, &az, &el);
                worst = std::max({worst, std::fabs(az - t.azDeg), std::fabs(el - t.elDeg)});
            }
            std::snprintf(what, sizeof what, "sides %.1f m, rows %.1f m: places back to theirs (within %.2f degrees)", rs, rh, worst);
            check(what, worst < 2);
        }
    {   // the desk well under your eyes (the centre's top edge too): the rows lean in from that edge, not from eye height
        tiling::Params low;
        tiling::FromEyes({1.6, -23, 0.8, 70}, 1.3, &low);
        tiling::Layout k = tiling::Arrange(cols, low);
        tiling::Turn(&k, -tiling::Layout(k).tiles[0].azDeg);
        const auto at = [&](int i) { for (const auto &t : k.tiles) if (t.id == i) return t; return tiling::Placed{}; };
        const Wall q = For(k, centre, low, 0.8, 0.8), b = For(k, centre, low, 0.8, 0);
        double m[3][4], n[3][4];
        Place(q, at(3), at(3).distM, m), Place(b, at(3), at(3).distM, n);
        check("the desk under your eyes: its rows lean from the centre's top edge", at(centre).el1 < 0 && m[1][2] < n[1][2] - 0.1);
        Place(q, at(centre), at(centre).distM, m), Place(b, at(centre), at(centre).distM, n);
        check("...the centre as it was", near(m[1][2], n[1][2]) && near(m[1][3], n[1][3]));
        const tiling::Placed lo = at(centre), hi = at(3);
        Place(q, lo, lo.distM, m), Place(q, hi, hi.distM, n);
        tiling::Placed small = hi;
        small.widthM *= Fit(q, hi), small.heightM *= Fit(q, hi);
        const double up = At(n, small, 0, -1).y - At(m, lo, 0, 1).y;
        check("...and the row stands on it", up > -0.005 && up < 0.08);
        double az, el;
        const double pt[3] = {n[0][3], n[1][3], n[2][3]};
        Direction(q, pt, &az, &el);
        check("...and its place back to its own", std::fabs(tiling::WrapNear(az, hi.azDeg) - hi.azDeg) < 1 && std::fabs(el - hi.elDeg) < 2);
    }
    check("a tight side curve stops before it closes behind you", For(l, centre, p, 0.2, 0).rs > 0.2 &&
              For(l, centre, p, 0.2, 0).Heading(l.az1) <= kMaxHeadingDeg + 1e-6 && For(l, centre, p, 0.2, 0).Heading(l.az0) >= -kMaxHeadingDeg - 1e-6);
    check("no centre display: the sides part straight ahead", near(For(l, 99, p, 1.2, 0).a0, 0) && near(For(l, 99, p, 1.2, 0).a1, 0));
    p.fit = true;
    check("no columns: the centre's own span", near(For(l, 0, p, 1.2, 0).a0, of(0).az0 - p.gapDeg / 2));
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
