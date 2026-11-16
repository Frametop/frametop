// The dashboard tiles' policy (vr.cpp's "Dashboard tiles" does the OpenVR side): which panels are
// anchored to the SteamVR dashboard, which of those tile (tilegrid.h keeps their order and sizes,
// tiling.h places them) and which are satellites, which stay shown with the dashboard closed, what
// the tiling keys do, and each app's or display's last state (Memory).
//
// Anchored (user, 2026-11-16): a panel rides with the dashboard. Dragged on the grid, it moves to
// where it's let go; pulled well off it, it tears off and stays anchored as a satellite at its own
// pose in the group's frame (it comes along when the dashboard opens somewhere else) until its
// anchor is turned off. Anchored panels show only with the dashboard unless they "stay" (the
// anchor button's long press). Each app or display remembers what it was last: anchored or not,
// its place on the grid or as a satellite, and whether it stays, for the next time it opens.
//
// The group frame: at your head when the dashboard opened, level, -Z towards the dashboard (the
// middle of the strip is over it). Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "tilegrid.h"
#include "tiling.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace dashtiles {

struct Spot {
    bool stay = false;        // shown with the dashboard closed too
    bool satellite = false;   // off the grid, at `rel`
    double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // a satellite's: group frame -> panel
};

// What the host says about a panel: its shape now, and whether it keeps it (a desktop screen or a
// remote display: its resolution is the other machine's) or takes the cell's (a floating window).
struct Info {
    double aspect = 16.0 / 9;
    bool fixed = true;
};

enum class Key { Left, Right, Up, Down, Wider, Narrower, Taller, Shorter };
constexpr const char *kKeyNames[] = {"left", "right", "up", "down", "wider", "narrower", "taller", "shorter"};
inline bool ParseKey(const char *word, Key *k) {
    for (int i = 0; i < 8; ++i)
        if (std::strcmp(word, kKeyNames[i]) == 0) return *k = Key(i), true;
    return false;
}

// The group frame in the room, from the head's position and the bar's (Steam's, under the
// dashboard): at the head, level, its -Z towards the bar. `barBack`: the bar's +Z (for a bar
// right over or under the head). `barElDeg`: the bar's elevation from the head.
inline void GroupFrame(const double head[3], const double bar[3], const double barBack[3], double out[3][4],
                       double *barElDeg) {
    double d[3] = {bar[0] - head[0], bar[1] - head[1], bar[2] - head[2]};
    double flat = std::hypot(d[0], d[2]);
    double fx = d[0], fz = d[2];
    if (flat < 0.05) fx = -barBack[0], fz = -barBack[2];
    const double n = std::hypot(fx, fz);
    if (n < 1e-9) fx = 0, fz = -1;
    else fx /= n, fz /= n;
    const double x[3] = {-fz, 0, fx}, y[3] = {0, 1, 0}, z[3] = {-fx, 0, -fz};
    for (int r = 0; r < 3; ++r) out[r][0] = x[r], out[r][1] = y[r], out[r][2] = z[r], out[r][3] = head[r];
    *barElDeg = tiling::Deg(std::atan2(d[1], std::max(flat, 1e-9)));
}

class Board {
public:
    tiling::Params params;

    bool Anchored(int id) const { return spots_.count(id) != 0; }
    bool Tiled(int id) const { return grid_.Has(id); }
    bool Stay(int id) const {
        const auto it = spots_.find(id);
        return it != spots_.end() && it->second.stay;
    }
    bool Satellite(int id) const {
        const auto it = spots_.find(id);
        return it != spots_.end() && it->second.satellite;
    }
    const Spot *SpotOf(int id) const {
        const auto it = spots_.find(id);
        return it == spots_.end() ? nullptr : &it->second;
    }
    std::vector<int> Ids() const {
        std::vector<int> ids;
        for (const auto &[id, spot] : spots_) ids.push_back(id);
        return ids;
    }
    const tilegrid::Grid &Grid() const { return grid_; }

    // Anchored (already: nothing): a column of its own right of `focused`'s.
    void Anchor(int id, int focused, bool stay) {
        if (Anchored(id)) return;
        spots_[id].stay = stay;
        grid_.Add(id, focused);
    }
    // Not anchored any more (or gone). False if it wasn't.
    bool Unanchor(int id) {
        grid_.Remove(id);
        return spots_.erase(id) != 0;
    }
    void SetStay(int id, bool on) {
        if (Anchored(id)) spots_[id].stay = on;
    }
    // Off the grid, a satellite at `rel` (the group frame's -> the panel's).
    void TearOff(int id, const double rel[3][4]) {
        if (!Anchored(id)) return;
        grid_.Remove(id);
        Spot &s = spots_[id];
        s.satellite = true;
        std::memcpy(s.rel, rel, sizeof s.rel);
    }
    // Where a drop at (az, el) goes: on the grid as it is without the dragged panel.
    template <typename InfoOf>
    tiling::Drop DropFor(int id, double az, double el, InfoOf info) const {
        Board without = *this;
        without.grid_.Remove(id);
        const auto cols = without.Columns(info);
        return tiling::DropAt(tiling::Arrange(cols, params), cols, az, el);
    }
    // Let go: where DropFor said (a column's indices are the grid's without it), or torn off at `rel`.
    void Drop(int id, const tiling::Drop &d, const double rel[3][4]) {
        if (!Anchored(id)) return;
        if (d.kind == tiling::Drop::TearOff) return TearOff(id, rel);
        const tilegrid::Cell *c = grid_.CellOf(id);
        const double scale = c ? c->scale : 1;
        grid_.Remove(id);
        spots_[id].satellite = false;
        grid_.Insert(id, d.col, d.row, d.kind == tiling::Drop::NewColumn, 1, 0, scale);
    }

    // A tiling key on a tiled panel (a satellite: nothing). False when nothing changed. A fixed
    // panel fills its row's height already, so wider and narrower size it in its row instead.
    bool Press(int id, Key k, const Info &info) {
        if (!grid_.Has(id)) return false;
        const bool grow = k == Key::Wider || k == Key::Taller;
        const double f = grow ? tilegrid::kStep : 1 / tilegrid::kStep;
        switch (k) {
            case Key::Left: return grid_.MoveSide(id, -1);
            case Key::Right: return grid_.MoveSide(id, +1);
            case Key::Up: return grid_.MoveUpDown(id, -1);
            case Key::Down: return grid_.MoveUpDown(id, +1);
            case Key::Wider:
            case Key::Narrower: return info.fixed ? grid_.Scale(id, f) : grid_.Widen(id, f, NowWidth(id));
            case Key::Taller:
            case Key::Shorter: return grid_.Heighten(id, f) || (info.fixed && grid_.Scale(id, f));
        }
        return false;
    }
    // Resized by its corner on the grid: `fw`, `fh` times as wide and tall as its tile was.
    bool Resized(int id, double fw, double fh, const Info &info) {
        if (!grid_.Has(id) || !(fw > 0) || !(fh > 0)) return false;
        if (info.fixed) return grid_.Scale(id, fw);
        const bool w = grid_.Widen(id, fw, NowWidth(id));
        const bool h = grid_.Heighten(id, fh);
        return w || h;
    }

    template <typename InfoOf>
    std::vector<tiling::Column> Columns(InfoOf info) const {
        std::vector<tiling::Column> cols;
        for (const tilegrid::Col &gc : grid_.Cols()) {
            tiling::Column c;
            c.widthDeg = gc.widthDeg;
            for (const tilegrid::Cell &cell : gc.cells) {
                const Info i = info(cell.id);
                c.tiles.push_back({cell.id, i.aspect, i.fixed, cell.weight, cell.scale});
            }
            cols.push_back(c);
        }
        return cols;
    }
    template <typename InfoOf>
    const tiling::Layout &Arrange(InfoOf info) {
        last_ = tiling::Arrange(Columns(info), params);
        return last_;
    }
    const tiling::Layout &Last() const { return last_; }
    const tiling::Placed *PlacedOf(int id) const {
        for (const auto &p : last_.tiles)
            if (p.id == id) return &p;
        return nullptr;
    }

    // The grid as text (tilegrid's: a profile's), and back. Anchored panels it leaves out keep a
    // column at the end; panels in it that aren't anchored are left out.
    std::string GridText() const { return grid_.Text(); }
    bool SetGrid(const std::string &text) {
        tilegrid::Grid g;
        if (!g.FromText(text, [this](int id) { return Anchored(id); })) return false;
        for (const auto &[id, spot] : spots_)
            if (!spot.satellite && !g.Has(id)) g.Add(id);
        for (auto &[id, spot] : spots_)
            if (g.Has(id)) spot.satellite = false;
        grid_ = g;
        return true;
    }

    // A panel's state, to remember it: "off", "tile STAY COL ROW ALONE WEIGHT SCALE WIDTH" or
    // "sat STAY" and its 12 numbers.
    std::string Remember(int id) const {
        const Spot *s = SpotOf(id);
        if (!s) return "off";
        char b[512];
        if (s->satellite) {
            int n = std::snprintf(b, sizeof b, "sat %d", s->stay ? 1 : 0);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) n += std::snprintf(b + n, sizeof b - n, " %.4f", s->rel[r][c]);
            return b;
        }
        const tilegrid::Where w = grid_.Find(id);
        if (!w.ok()) return "off";
        const tilegrid::Col &col = grid_.Cols()[w.col];
        const tilegrid::Cell &cell = col.cells[w.row];
        std::snprintf(b, sizeof b, "tile %d %d %d %d %.3f %.3f %.2f", s->stay ? 1 : 0, w.col, w.row,
                      col.cells.size() == 1 ? 1 : 0, cell.weight, cell.scale, col.widthDeg);
        return b;
    }
    // Back as it was remembered (`focused`: a new tile's neighbour when its column is gone). False
    // (and nothing changed) for a line that isn't one.
    bool Restore(int id, const std::string &line, int focused) {
        int stay = 0, col = 0, row = 0, alone = 1, n = 0;
        double weight = 1, scale = 1, width = 0;
        if (line == "off") return Unanchor(id), true;
        if (std::sscanf(line.c_str(), "tile %d %d %d %d %lf %lf %lf%n", &stay, &col, &row, &alone, &weight, &scale, &width,
                        &n) == 7 &&
            n == int(line.size())) {
            if (!std::isfinite(weight) || !std::isfinite(scale) || !std::isfinite(width)) return false;
            Unanchor(id);
            spots_[id].stay = stay != 0;
            if (col < 0 || row < 0) return grid_.Add(id, focused), true;
            if (width > 0) width = std::clamp(width, tilegrid::kMinWidthDeg, tilegrid::kMaxWidthDeg);
            else width = 0;
            grid_.Insert(id, col, row, alone != 0, weight, alone ? width : 0, scale);
            return true;
        }
        double m[3][4];
        if (std::sscanf(line.c_str(), "sat %d%n", &stay, &n) == 1) {
            const char *p = line.c_str() + n;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) {
                    int k = 0;
                    if (std::sscanf(p, " %lf%n", &m[r][c], &k) != 1 || !std::isfinite(m[r][c])) return false;
                    p += k;
                }
            if (*p) return false;
            Unanchor(id);
            Spot &s = spots_[id];
            s.stay = stay != 0;
            s.satellite = true;
            std::memcpy(s.rel, m, sizeof m);
            return true;
        }
        return false;
    }

private:
    // A tiled panel's column width now (as laid out, before any shrink), in degrees.
    double NowWidth(int id) const {
        if (const tiling::Placed *p = PlacedOf(id)) return (p->az1 - p->az0) / std::max(1e-6, last_.shrink);
        const double w = grid_.WidthOf(id);
        return w > 0 ? w : params.columnDeg;
    }

    std::map<int, Spot> spots_;
    tilegrid::Grid grid_;
    tiling::Layout last_;
};

// Each app's or display's last state (Board::Remember), by a key of the host's ("screen:2",
// "app:org.kde.dolphin", "remote:ID"): ft-screens' ~/.config/frametop-tiles.txt, a line each,
// "KEY<TAB>STATE".
class Memory {
public:
    static constexpr size_t kMax = 500;   // entries at most (the oldest key in order goes)

    // Spaces and control characters out (a key is one word), at most 200 bytes.
    static std::string Clean(const std::string &key) {
        std::string k;
        for (unsigned char c : key) k += c <= ' ' || c == 0x7f ? '_' : char(c);
        return k.substr(0, 200);
    }
    std::string Get(const std::string &key) const {
        const auto it = m_.find(Clean(key));
        return it == m_.end() ? std::string() : it->second;
    }
    // True when it changed.
    bool Set(const std::string &key, const std::string &state) {
        const std::string k = Clean(key);
        if (k.empty() || state.find_first_of("\t\n") != std::string::npos) return false;
        auto it = m_.find(k);
        if (it != m_.end() && it->second == state) return false;
        m_[k] = state;
        while (m_.size() > kMax) m_.erase(m_.begin());
        return true;
    }
    std::string Text() const {
        std::string s;
        for (const auto &[k, v] : m_) s += k + "\t" + v + "\n";
        return s;
    }
    void Load(const std::string &text) {
        m_.clear();
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(at, end - at);
            at = end + 1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) continue;
            Set(line.substr(0, tab), line.substr(tab + 1));
        }
    }
    size_t Size() const { return m_.size(); }

private:
    std::map<std::string, std::string> m_;
};

}  // namespace dashtiles
