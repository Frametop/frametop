// Round a super wide display (screens/tilewide.h, tilepack.h's squeeze): how many panels stand over
// and under it, where a drop puts one, and the tiling keys there. At the live wall's numbers: Steam's
// bar about 1.4 m away, the arc 2 m behind your eyes, the desk line 24 degrees down, 70 degrees wide.
#include "../dashtiles.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// A desk of panels by their pixels; 1 is the display.
struct Desk {
    dashtiles::Board b;
    std::map<int, std::pair<double, double>> px;
    double mpp = 0;
    Desk(double w = 5120, double h = 1440) {
        const double r = 1.4;
        b.params.pxPerDeg = 48;
        tiling::FromEyes({r, -24, 0.8, 70}, 2.0, &b.params);
        tiling::Params at = b.params;
        at.radius = r, mpp = tiling::MetresPerPx(at);
        px[1] = {w, h};
        b.Anchor(1, -1, dashtiles::Stay::On);
        Lay();
    }
    auto Info() {
        return [this](int id) { return dashtiles::Info{px[id].first * mpp, px[id].second * mpp}; };
    }
    const tiling::Layout &Lay() { return b.Arrange(Info()); }
    // Opened (the desk's full: it goes where there's room).
    void Open(int id, double w, double h) {
        px[id] = {w, h};
        const auto i = Info()(id);
        b.Anchor(id, 1, dashtiles::Stay::On, -1, &i);
        Lay();
    }
    // Anchored beside, then let go at (az, el).
    tiling::Drop DropAt(int id, double w, double h, double az, double el) {
        px[id] = {w, h};
        if (!b.Anchored(id)) b.Anchor(id, 1, dashtiles::Stay::On);
        Lay();
        return Again(id, az, el);
    }
    tiling::Drop Again(int id, double az, double el) {
        const auto d = b.DropFor(id, az, el, Info());
        b.Drop(id, d, az);
        Lay();
        return d;
    }
    bool Key(int id, dashtiles::Key k) {
        const bool did = b.Press(id, k);
        Lay();
        return did;
    }
    const tiling::Placed &At(int id) const { return *b.PlacedOf(id); }
    size_t Cols() const { return b.Grid().Cols().size(); }
    bool Over(int id) const { return At(id).col == At(1).col && At(id).el0 >= At(1).el1; }
    bool Under(int id) const { return At(id).col == At(1).col && At(id).el1 <= At(1).el0; }
    // Every panel a gap from every other, none past the display's ends.
    bool Tidy(double slack = 1e-6) const {
        const auto &t = b.Last().tiles;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i].col == At(1).col && (t[i].az0 < At(1).az0 - 0.01 || t[i].az1 > At(1).az1 + 0.01)) return false;
            for (size_t j = i + 1; j < t.size(); ++j) {
                const double gap = b.params.gapDeg - slack;
                const bool apart = t[i].az1 + gap <= t[j].az0 || t[j].az1 + gap <= t[i].az0 || t[i].el1 + gap <= t[j].el0 || t[j].el1 + gap <= t[i].el0;
                if (!apart) return false;
            }
        }
        return true;
    }
    // These, left to right, on one shelf.
    bool Row(const std::vector<int> &ids) const {
        for (size_t i = 1; i < ids.size(); ++i)
            if (At(ids[i]).shelf != At(ids[0]).shelf || At(ids[i]).below != At(ids[0]).below || At(ids[i]).azDeg <= At(ids[i - 1]).azDeg) return false;
        return true;
    }
    double WideMid() const { return At(1).azDeg; }
    double WideW() const { return At(1).az1 - At(1).az0; }
};

int main() {
    using dashtiles::Key;
    // --- what's super wide
    {
        Desk d;
        check("wide: a 5120x1440", d.At(1).wide);
        check("wide: a 3440x1440", Desk(3440, 1440).At(1).wide);
        check("wide: not a 3840x2160", !Desk(3840, 2160).At(1).wide);
        check("wide: not a short wide window (1200x500)", !Desk(1200, 500).At(1).wide);
        check("wide: not an ultrawide made small", [] {
            Desk s;
            s.b.Press(1, Key::Narrower);
            for (int i = 0; i < 8; ++i) s.b.Press(1, Key::Narrower);
            s.Lay();
            return !s.At(1).wide;
        }());
        d.Open(2, 1280, 800);
        check("wide: its column's display, not the window over it", d.At(1).wide && !d.At(2).wide);
    }
    // --- shelves
    {
        using tilepack::Shelves;
        check("shelf: four 11s over 42: three fit as they are", Shelves({42, 11, 11, 11, 11}, 0.4) == std::vector<int>({0, 1, 1, 1, 2}));
        check("shelf: ...all four over a super wide display", Shelves({42, 11, 11, 11, 11}, 0.4, 0, true) == std::vector<int>({0, 1, 1, 1, 1}));
        check("shelf: ...not a fifth: too small", Shelves({42, 11, 11, 11, 11, 11}, 0.4, 0, true).back() == 2);
        check("shelf: never beside the display itself", Shelves({42, 3}, 0.4, 0, true) == std::vector<int>({0, 1}));
        check("shelf: nor a panel most as wide", Shelves({42, 35, 11}, 0.4, 0, true) == std::vector<int>({0, 1, 2}));
        tilepack::Fill f = tilepack::FillOf({{1, 42, 12, true}, {2, 16.5, 9}, {3, 16.5, 9}}, 0.4);
        check("fill: a third squeezes the shelf, lower", f.wide && tilepack::Grown(f, {4, 16.5, 9}, 0.4) < f.height);
        check("fill: ...at a cost", tilepack::Cost(f, {4, 16.5, 9}, 0.4) > 1);
    }
    // --- opened one after another
    {
        Desk d;
        for (int i = 2; i <= 9; ++i) d.Open(i, 1280, 800);
        check("open: eight 1280x800, all in its column", d.Cols() == 1 && d.Tidy());
        check("open: ...four over it, the rest under it", d.Row({2, 3, 4, 5}) && d.Over(2) && d.Row({6, 7, 8, 9}) && d.Under(6));
        check("open: ...across all of it", d.At(5).az1 - d.At(2).az0 > 0.97 * d.WideW());
        check("open: ...no smaller than three quarters", d.At(2).widthM > 0.75 * 1280 * d.mpp * 0.98);
        d.b.Repack(d.Info()), d.Lay();
        check("reset: as many, four a side", d.Cols() == 1 && d.Tidy() && d.b.Last().el1 < 12);
    }
    {
        Desk d;
        for (int i = 2; i <= 7; ++i) d.Open(i, 1920, 1080);
        check("open: six 1920x1080, all in its column, three a side",
              d.Cols() == 1 && d.Tidy() && d.Row({2, 3, 4}) && d.Over(2) && d.Row({5, 6, 7}) && d.Under(5));
        d.Open(8, 1920, 1080);
        const double top = tiling::EyeElDeg(d.b.params, d.b.Last().el1), low = tiling::EyeElDeg(d.b.params, d.b.Last().el0);
        check("open: ...a seventh: never taller than kWideColumnDeg", top - low <= tilepack::kWideColumnDeg + 1e-6);
    }
    {
        Desk d(3840, 2160);   // (not super wide: as before)
        d.Open(2, 1920, 1080);
        check("open: over a 16:9 display, no taller than kColumnDeg: a column of its own", d.Cols() == 2);
    }
    // --- drops
    {
        Desk d;
        const double w = d.WideW(), up = 6.4, low = -13.8;
        for (int k = 0; k < 4; ++k) d.DropAt(2 + k, 1280, 800, d.WideMid() - w / 2 + w * (k + 0.5) / 4, up);
        for (int k = 0; k < 4; ++k) d.DropAt(6 + k, 1280, 800, d.WideMid() - w / 2 + w * (k + 0.5) / 4, low);
        check("drop: four over it and four under it, each where it was let go",
              d.Cols() == 1 && d.Tidy() && d.Row({2, 3, 4, 5}) && d.Over(2) && d.Row({6, 7, 8, 9}) && d.Under(6));
        d.Again(5, d.At(2).azDeg + 1, d.At(2).elDeg);
        check("drop: along its shelf, between two", d.Row({2, 5, 3, 4}) && d.Cols() == 1);
        d.Again(5, d.At(2).azDeg - 1, d.At(2).elDeg);
        check("drop: ...and first", d.Row({5, 2, 3, 4}));
        d.Again(5, d.At(4).azDeg + 2, d.At(4).elDeg);
        check("drop: ...and last", d.Row({2, 3, 4, 5}));
        d.Again(9, d.At(3).azDeg - 1, d.At(3).elDeg);
        check("drop: from under it into a full shelf over it: the last one steps up", d.Over(9) && d.Row({2, 9, 3, 4}) && d.Over(5) && d.At(5).el0 > d.At(4).el1);
        d.Again(9, d.At(8).azDeg + 1, d.At(8).elDeg);
        check("drop: ...and back", d.Row({6, 7, 8, 9}) && d.Under(9) && d.Row({2, 3, 4, 5}) && d.Tidy());
    }
    {
        Desk d;
        const auto a = d.DropAt(2, 1280, 800, d.WideMid() + 12, d.At(1).elDeg + 1);
        check("drop: on the display's upper half: over it, at that side", a.kind == tiling::Drop::Into && d.Over(2) && near(d.At(2).az1, d.At(1).az1, 0.02));
        d.Again(2, d.WideMid() - 12, d.At(1).elDeg - 1);
        check("drop: on its lower half: under it, at that side", d.Under(2) && near(d.At(2).az0, d.At(1).az0, 0.02));
        d.Again(2, d.WideMid() + 1, d.At(1).el1 + 3);
        check("drop: over its middle: in the middle", d.Over(2) && near(d.At(2).azDeg, d.WideMid(), 0.02));
        d.Again(2, d.At(1).az1 - 1, d.At(1).elDeg);
        check("drop: on its end: a column of its own", d.Cols() == 2 && d.At(2).col == 1);
        d.Again(2, d.At(1).az1 - 1, d.At(1).el1 + 3);
        check("drop: over its end: over it", d.Cols() == 1 && d.Over(2));
        d.Again(2, d.At(1).az1 + 6, d.At(1).elDeg);
        check("drop: past its end: a column of its own", d.Cols() == 2);
        d.Open(3, 1280, 800);
        d.Again(3, d.At(2).azDeg, d.At(2).elDeg + 1);
        check("drop: on its neighbour: the neighbour's, as before", d.At(3).col == d.At(2).col && d.At(1).col != d.At(2).col);
    }
    {
        Desk d;
        for (int k = 0; k < 3; ++k) d.DropAt(2 + k, 1280, 800, d.WideMid() - 12 + 12 * k, 6.4);
        const double top = d.b.Last().el1;
        d.DropAt(5, 1280, 800, d.WideMid(), top + 3.4);
        check("drop: past the shelf over it, room on it or not: one further up", d.Over(5) && d.At(5).el0 > d.At(2).el1 && d.Row({2, 3, 4}));
        d.DropAt(6, 1280, 800, d.WideMid() - 3, d.At(5).elDeg);
        check("drop: ...and beside it, first: up there too", d.Row({6, 5}) && d.At(6).el0 > d.At(2).el1 && d.Row({2, 3, 4}) && d.Tidy());
        check("drop: the display itself isn't let go on itself", d.b.DropFor(1, d.WideMid(), 6.4, d.Info()).kind != tiling::Drop::TearOff);
    }
    // --- the keys
    {
        Desk d;
        for (int i = 2; i <= 5; ++i) d.Open(i, 1280, 800);
        check("key: right, along the shelf", d.Key(2, Key::Right) && d.Row({3, 2, 4, 5}));
        check("key: left, back", d.Key(2, Key::Left) && d.Row({2, 3, 4, 5}));
        check("key: down: past the display, under it", d.Key(3, Key::Down) && d.Under(3) && d.Row({2, 4, 5}) && d.Over(2));
        check("key: down again: nothing", !d.Key(3, Key::Down) && d.Under(3));
        check("key: another down: beside it", d.Key(4, Key::Down) && d.Under(4) && d.Row({3, 4}));
        check("key: up: back over it, where it was along it", d.Key(4, Key::Up) && d.Over(4) && d.Cols() == 1 && d.Tidy());
        check("key: up again: a shelf of its own", d.Key(4, Key::Up) && d.Over(4) && d.At(4).el0 > d.At(2).el1 && d.Tidy());
        check("key: ...and again: nothing", !d.Key(4, Key::Up));
        check("key: ...and down: back beside them", d.Key(4, Key::Down) && d.At(4).shelf == d.At(2).shelf && d.Over(4));
        check("key: right at the end of a shelf with room: the shelf to that end", d.Key(4, Key::Right) && d.Cols() == 1 && near(d.At(4).az1, d.At(1).az1, 0.02));
        check("key: ...and again: out beside the display", d.Key(4, Key::Right) && d.Cols() == 2 && d.At(4).col == 1);
        check("key: up, alone beside it: over it", d.Key(4, Key::Up) && d.Cols() == 1 && d.Over(4));
        check("key: ...at the near end", d.Row({2, 5, 4}));
        d.DropAt(6, 1280, 800, d.At(1).az1 + 6, d.At(1).elDeg);
        check("key: down, alone beside it: under it", d.Cols() == 2 && d.Key(6, Key::Down) && d.Cols() == 1 && d.Under(6) && d.Row({3, 6}));
        d.DropAt(6, 1280, 800, d.At(1).az1 + 6, d.At(1).elDeg);
        d.Open(7, 1280, 800);
        d.Again(7, d.At(6).azDeg, d.At(6).elDeg + 1);
        check("key: left, stacked beside it: over it (not under it, the display on it)",
              d.At(7).col == d.At(6).col && d.Key(6, Key::Left) && d.Over(6) && d.At(1).el0 < d.At(7).el1 && d.Cols() == 2);
        check("key: sizes as anywhere", d.Key(2, Key::Wider) && d.Key(2, Key::Narrower) && d.Tidy());
    }
    // --- remembered
    {
        Desk d, e;
        for (int i = 2; i <= 9; ++i) d.Open(i, 1280, 800), e.px[i] = {1280, 800};
        d.Key(4, Key::Down), d.Key(3, Key::Up);
        std::vector<std::pair<int, std::string>> saved;
        for (int i = 1; i <= 9; ++i) saved.push_back({i, d.b.Remember(i)});
        e.b.RestoreAll(saved), e.Lay();
        bool same = true;
        for (int i = 1; i <= 9; ++i) same = same && near(e.At(i).azDeg - e.WideMid(), d.At(i).azDeg - d.WideMid(), 1e-6) && near(e.At(i).elDeg, d.At(i).elDeg, 1e-6);
        check("profile: back as it was, a shelf of its own too", same && d.At(3).el0 > d.At(2).el1);
    }
    // --- the edges: every width, every place, nothing overlaps or is lost
    {
        bool tidy = true, kept = true, size = true;
        const std::vector<std::pair<double, double>> sizes = {{640, 480}, {800, 1200}, {1280, 800}, {1920, 1080}, {2560, 1440}, {1200, 500}};
        for (const auto &wide : {std::pair<double, double>{5120, 1440}, {3440, 1440}, {3840, 1080}})
            for (unsigned seed = 0; seed < 40; ++seed) {
                Desk d(wide.first, wide.second);
                unsigned r = seed * 7919 + 13;
                const auto rnd = [&](double lo, double hi) { return r = r * 1103515245 + 12345, lo + (hi - lo) * ((r >> 16) & 0x7fff) / 32767.0; };
                for (int i = 2; i <= 8; ++i) {
                    const auto &s = sizes[size_t(rnd(0, double(sizes.size()) - 0.01))];
                    d.DropAt(i, s.first, s.second, rnd(-25, 25), rnd(-20, 15));
                }
                for (int i = 0; i < 6; ++i) d.Again(2 + int(rnd(0, 6.99)), rnd(-25, 25), rnd(-20, 15));
                for (int i = 0; i < 6; ++i) d.Key(2 + int(rnd(0, 6.99)), Key(int(rnd(0, 3.99))));
                kept = kept && d.b.Last().tiles.size() == 8;
                const auto &t = d.b.Last().tiles;
                for (size_t i = 0; i < t.size(); ++i) {
                    for (size_t j = i + 1; j < t.size(); ++j)
                        if (t[i].az1 > t[j].az0 + 1e-6 && t[j].az1 > t[i].az0 + 1e-6 && t[i].el1 > t[j].el0 + 1e-6 && t[j].el1 > t[i].el0 + 1e-6) tidy = false;
                    const double natural = d.px[t[i].id].first * d.mpp * d.b.Last().shrink;
                    if (t[i].distM > 0 && t[i].widthM * d.b.params.radius / t[i].distM < (tilepack::kSqueeze - 1e-6) * natural) size = false;
                }
            }
        check("any: none over another", tidy);
        check("any: none lost", kept);
        check("any: none smaller than the squeeze", size);
    }
    // --- a shelf that fills the display's width: Meta+Right at its right end moves it out at once, not
    // a lean first that nobody sees (user, 2026-11-24: "it will sometimes require pressing it twice")
    {
        Desk d;
        for (int i = 2; i <= 5; ++i) d.Open(i, 1280, 800);
        const bool full = d.Row({2, 3, 4, 5}) && d.Over(2) && d.At(5).az1 > d.At(1).az1 - dashtiles::tilekeys::kLeanSeenDeg;
        check("keys: four over the display, their shelf as wide as it", full);
        check("keys: ...Right on the right one: a column of its own, in one press", d.Key(5, Key::Right) && d.Cols() == 2 && d.At(5).col > d.At(1).col);
        Desk e;
        e.Open(2, 1280, 800);
        const double was = e.At(2).azDeg;
        check("keys: one small one over it, room beside: Right leans it first", e.Key(2, Key::Right) && e.Cols() == 1 && e.At(2).azDeg > was + 1);
    }
    // --- dragged from over the display to beside it: its middle is half its width past the strip there
    // (user, 2026-11-24: "dragging the window from above Dodie to the side of Dodie doesn't work")
    {
        Desk d(3440, 1440);
        d.Open(2, 1600, 1100);
        const double halfW = (d.At(2).az1 - d.At(2).az0) / 2;
        const auto drop = d.Again(2, d.b.Last().az1 + halfW + 5, d.At(1).elDeg);
        check("drop: beside the display, its near edge 5 degrees off: a column there", halfW > tiling::kTearMarginDeg - 5 &&
                                                                                         drop.kind == tiling::Drop::NewColumn && d.Cols() == 2 && d.At(2).col > d.At(1).col);
        const auto far = d.Again(2, d.b.Last().az1 + halfW + tiling::kTearMarginDeg + 3, d.At(1).elDeg);
        check("drop: ...well past it: still torn off", far.kind == tiling::Drop::TearOff);
    }
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed != 0;
}
