// One curve for the centre display and another beside it (screens/tilecurve.h): where the tiles go.
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

    const auto pose = [&](const Sides &s, int i, double m[3][4]) { const tiling::Placed t = of(i); Place(s, t, t.distM, m); };
    const auto same = [&](const Sides &a, const Sides &b, double eps) {
        bool ok = true;
        for (const auto &t : l.tiles) {
            double m[3][4], n[3][4];
            pose(a, t.id, m), pose(b, t.id, n);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) ok = ok && near(m[r][c], n[r][c], eps);
        }
        return ok;
    };
    const Sides off = For(l, centre, p, 0);
    check("not set: one curve, the sphere's own poses", !off.On());
    check("the same as the centre's: one curve", !For(l, centre, p, p.radius).On());
    check("next to the same: next to the same poses", same(off, For(l, centre, p, p.radius * 0.9999), 1e-3));

    const Sides s = For(l, centre, p, 1.2);
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
    const auto off_deg = [&](const Sides &q, int i) {
        double m[3][4];
        pose(q, i, m);
        const V e{-m[0][3], -m[1][3], -p.backM - m[2][3]};
        return Deg(std::acos((e.x * m[0][2] + e.y * m[1][2] + e.z * m[2][2]) / std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z)));
    };
    check("the sides face you better than with one curve", off_deg(s, 0) < off_deg(off, 0) - 5 && off_deg(s, 2) < off_deg(off, 2) - 3 &&
                                                               off_deg(s, 6) < off_deg(off, 6) - 3);
    // neighbours along the desk: corner to corner, about the gap apart, and no two in each other
    const auto gap = [&](const Sides &q, int a, int b) {
        double m[3][4], n[3][4];
        pose(q, a, m), pose(q, b, n);
        return Dist(At(m, of(a), 1, -1), At(n, of(b), -1, -1));
    };
    for (double rs : {0.8, 1.2, 2.0, 5.0, 12.0}) {
        const Sides q = For(l, centre, p, rs);
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
    check("a tight side curve stops before it closes behind you", For(l, centre, p, 0.2).rs > 0.2 &&
              For(l, centre, p, 0.2).Heading(l.az1) <= kMaxHeadingDeg + 1e-6 && For(l, centre, p, 0.2).Heading(l.az0) >= -kMaxHeadingDeg - 1e-6);
    check("no centre display: the sides part straight ahead", near(For(l, 99, p, 1.2).a0, 0) && near(For(l, 99, p, 1.2).a1, 0));
    p.fit = true;
    check("no columns: the centre's own span", near(For(l, 0, p, 1.2).a0, of(0).az0 - p.gapDeg / 2));
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
