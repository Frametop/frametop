// The dashboard tiles' policy (screens/vrtiles.cpp does the OpenVR side): which panels are
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
// The group frame: at your head when the dashboard opened, level, -Z the way you faced (GroupFrame).
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilegrid.h"
#include "tilememory.h"
#include "tilestate.h"
#include "tiling.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
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

// The group frame in the room, from the head's pose: at the head, level, its -Z the way the head
// faces, so the strip is in front of you wherever Steam put its bar (the palm menu's Steam menu
// opens it low and to the side). The way is the forward's, level (right whatever the roll). Near
// straight up or down, where the forward's level part goes to nothing, the up's level part joins
// it, eased in from kUpFromDeg to kUpFullDeg of pitch (fwd - up * fwd.y: the forward with the pitch
// taken out, right without roll; eased, the way turns smoothly with the pitch, no jump), so looking
// straight down at a palm or straight up still faces the same way. `headFwd`, `headUp`: the head's
// -Z and +Y. A pose that isn't finite, or has no way at all, faces -Z.
constexpr double kUpFromDeg = 75, kUpFullDeg = 88;
inline void GroupFrame(const double head[3], const double headFwd[3], const double headUp[3], double out[3][4]) {
    const double s0 = std::sin(tiling::Rad(kUpFromDeg)), s1 = std::sin(tiling::Rad(kUpFullDeg));
    const double t = std::clamp((std::fabs(headFwd[1]) - s0) / (s1 - s0), 0.0, 1.0), w = t * t * (3 - 2 * t);
    double fx = headFwd[0] - w * headUp[0] * headFwd[1], fz = headFwd[2] - w * headUp[2] * headFwd[1];
    const double n = std::hypot(fx, fz);
    if (!(n >= 1e-9) || !std::isfinite(n)) fx = 0, fz = -1;
    else fx /= n, fz /= n;
    const double x[3] = {-fz, 0, fx}, y[3] = {0, 1, 0}, z[3] = {-fx, 0, -fz};
    for (int r = 0; r < 3; ++r) out[r][0] = x[r], out[r][1] = y[r], out[r][2] = z[r], out[r][3] = head[r];
}

// Steam's menu as a slot in the strip (tiling.h's reserve), from the group frame `g`, the bar's
// position and its width in metres (the menu over it is about as wide): the slot's middle round
// you (`azDeg`, in the group frame: also set when there's no slot) and its width (`spanDeg`, at
// most kMaxSlotDeg). Wherever it is round you (the Steam button opens it in front, the palm menu
// off to the side): the strip keeps the slot only when it would reach it (tiling.h). False, no
// slot, when its width isn't known, it's over your head, or anything isn't finite.
constexpr double kMaxSlotDeg = 90;
inline bool SteamSlot(const double g[3][4], const double bar[3], double barWidthM, double *azDeg, double *spanDeg) {
    const double d[3] = {bar[0] - g[0][3], bar[1] - g[1][3], bar[2] - g[2][3]};
    const double x = d[0] * g[0][0] + d[1] * g[1][0] + d[2] * g[2][0], z = d[0] * g[0][2] + d[1] * g[1][2] + d[2] * g[2][2];
    const double flat = std::hypot(x, z);
    const double az = tiling::Deg(std::atan2(x, -z));
    *azDeg = std::isfinite(az) ? az : 0;
    if (!(barWidthM > 0) || !std::isfinite(barWidthM) || !(flat >= 0.2) || !std::isfinite(az)) return false;
    *spanDeg = std::min(kMaxSlotDeg, 2 * tiling::Deg(std::atan2(barWidthM / 2, flat)));
    return true;
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

    // Anchored (already: nothing): a column of its own right of `focused`'s (not tiled: at the
    // strip's end that brings its middle nearer in front of you, the left one when Steam's slot has
    // pushed it right).
    void Anchor(int id, int focused, bool stay) {
        if (Anchored(id)) return;
        spots_[id].stay = stay;
        if (grid_.Has(focused)) return grid_.Add(id, focused);
        // none: the end that brings the strip's middle nearer in front (its left one when it leans right)
        grid_.AddAtEnd(id, last_.split >= 0 && tiling::WrapNear((last_.az0 + last_.az1) / 2, 0) > 0);
    }
    // Not anchored any more (or gone). False if it wasn't.
    bool Unanchor(int id) {
        Forget(id);
        grid_.Remove(id);
        return spots_.erase(id) != 0;
    }
    void SetStay(int id, bool on) {
        if (!Anchored(id)) return;
        spots_[id].stay = on;
        batch_.SetStay(id, on);
    }
    // Off the grid, a satellite at `rel` (the group frame's -> the panel's).
    void TearOff(int id, const double rel[3][4]) {
        if (!Anchored(id)) return;
        EndBatch();
        grid_.Remove(id);
        Spot &s = spots_[id];
        s.satellite = true;
        std::memcpy(s.rel, rel, sizeof s.rel);
    }
    // Where a drop at (az, el) goes: aimed at the grid as you see it (the dragged panel's place
    // still in it: the others don't move while you drag), as indices in the grid without it.
    template <typename InfoOf>
    tiling::Drop DropFor(int id, double az, double el, InfoOf info) const {
        const auto cols = Columns(info);
        return Without(grid_, id, tiling::DropAt(tiling::Arrange(cols, params, Split()), cols, az, el));
    }
    // Let go: where DropFor said (a column's indices are the grid's without it), or torn off at `rel`.
    void Drop(int id, const tiling::Drop &d, const double rel[3][4]) {
        if (!Anchored(id)) return;
        if (d.kind == tiling::Drop::TearOff) return TearOff(id, rel);
        const tilegrid::Cell *c = grid_.CellOf(id);
        const double scale = c ? c->scale : 1;
        EndBatch();
        spots_[id].satellite = false;
        slot_.Move(grid_, id, [&] {   // (Insert moves it)
            grid_.Insert(id, d.col, d.row, d.kind == tiling::Drop::NewColumn, 1, 0, scale, d.leftOfSlot);
            return true;
        });
    }

    // A tiling key on a tiled panel (a satellite: nothing). False when nothing changed. A fixed
    // panel fills its row's height already, so wider and narrower size it in its row instead.
    bool Press(int id, Key k, const Info &info) {
        if (!grid_.Has(id)) return false;
        return Changed(Pressed(id, k, info));
    }
    // Resized by its corner on the grid: `fw`, `fh` times as wide and tall as its tile was.
    bool Resized(int id, double fw, double fh, const Info &info) {
        if (!grid_.Has(id) || !(fw > 0) || !(fh > 0)) return false;
        if (info.fixed) return Changed(grid_.Scale(id, fw));
        const bool w = grid_.Widen(id, fw, NowWidth(id));
        const bool h = grid_.Heighten(id, fh);
        return Changed(w || h);
    }

private:
    bool Pressed(int id, Key k, const Info &info) {
        const bool grow = k == Key::Wider || k == Key::Taller;
        const double f = grow ? tilegrid::kStep : 1 / tilegrid::kStep;
        switch (k) {
            case Key::Left:
            case Key::Right: {
                return slot_.Move(grid_, id, [&] { return grid_.MoveSide(id, k == Key::Left ? -1 : +1, slot_.Key()); });
            }
            case Key::Up: return grid_.MoveUpDown(id, -1);
            case Key::Down: return grid_.MoveUpDown(id, +1);
            case Key::Wider:
            case Key::Narrower: return info.fixed ? grid_.Scale(id, f) : grid_.Widen(id, f, NowWidth(id));
            case Key::Taller:
            case Key::Shorter: return grid_.Heighten(id, f) || (info.fixed && grid_.Scale(id, f));
        }
        return false;
    }
    bool Changed(bool changed) {
        if (changed) EndBatch();
        return changed;
    }

public:

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
    // Laid out, round Steam's slot as slotsplit.h says.
    template <typename InfoOf>
    const tiling::Layout &Arrange(InfoOf info) {
        return last_ = slot_.Arrange(grid_, Columns(info), params);
    }
    // Steam's menu is where params.reserveAzDeg says now (the dashboard opened again).
    void SlotMoved() { slot_.Moved(params.reserveAzDeg); }
    // The split in use now (-1: no slot in the strip, or not chosen yet).
    int Split() const { return slot_.Split(grid_); }
    const tiling::Layout &Last() const { return last_; }
    const tiling::Placed *PlacedOf(int id) const {
        for (const auto &p : last_.tiles)
            if (p.id == id) return &p;
        return nullptr;
    }

    // A panel's state, to remember it (tilestate.h's text).
    std::string Remember(int id) const { return Text(StateOf(id)); }
    State StateOf(int id) const {
        State st;
        const Spot *s = SpotOf(id);
        if (!s) return st;
        st.stay = s->stay;
        if (s->satellite) {
            st.kind = State::Kind::Sat;
            std::memcpy(st.rel, s->rel, sizeof st.rel);
            return st;
        }
        const tilegrid::Where w = grid_.Find(id);
        if (!w.ok()) return State{};
        const tilegrid::Col &col = grid_.Cols()[size_t(w.col)];
        const tilegrid::Cell &cell = col.cells[size_t(w.row)];
        st.kind = State::Kind::Tile, st.col = w.col, st.row = w.row, st.alone = col.cells.size() == 1;
        st.weight = cell.weight, st.scale = cell.scale, st.width = col.widthDeg;
        return st;
    }
    // Back as it was remembered (`focused`: a new tile's neighbour when its column is gone). False
    // (and nothing changed) for a line that isn't one.
    bool Restore(int id, const std::string &line, int focused) {
        State s;
        return Parse(line, &s) && (Restore(id, s, focused), true);
    }
    void Restore(int id, const State &st, int focused) {
        Unanchor(id);
        if (st.kind == State::Kind::Off) return;
        Spot &s = spots_[id];
        s.stay = st.stay;
        if (st.kind == State::Kind::Sat) {
            s.satellite = true;
            std::memcpy(s.rel, st.rel, sizeof s.rel);
            return;
        }
        if (!st.Placed()) return grid_.Add(id, focused);
        const double width = st.width > 0 ? std::clamp(st.width, tilegrid::kMinWidthDeg, tilegrid::kMaxWidthDeg) : 0;
        grid_.Insert(id, st.col, st.row, st.alone, st.weight, width, st.scale);   // (the width: if it makes the column)
    }
    // The profile put back last is done with: an add after this goes after the other tiles.
    void EndRestore() { batch_.End(); }

    // A profile's tiles back, each (panel, Remember's line) in the order given (by column and
    // row): its columns first, as it saved them, then the tiles it doesn't name, each column as it
    // was. The saved column numbers count panels the profile doesn't have (a floating window, a
    // remote display that isn't on): its columns close up, stacks kept. `more`: the rest of the same
    // profile (one too long for a command goes in several): the whole of it is put back again, as
    // if it had come at once (minus its panels that went); after a tile is moved, resized or torn
    // off, an add puts its tiles after the others. A line that isn't one leaves that panel as it is. The
    // number of lines taken.
    int RestoreAll(const std::vector<std::pair<int, std::string>> &saved, bool more = false) {
        const bool after = more && batch_.mode() != Batch::Mode::First;   // an add on its own: after the others
        std::vector<std::pair<int, State>> good;
        if (more)
            for (const auto &[id, s] : batch_.States())
                if (Anchored(id)) good.push_back({id, s});   // (the ones gone since: left out)
        int taken = 0;
        for (const auto &[id, line] : saved) {
            State s;
            if (!Parse(line, &s)) continue;
            ++taken;
            for (auto g = good.begin(); g != good.end();) g = g->first == id ? good.erase(g) : std::next(g);
            if (s.kind == State::Kind::Off) Unanchor(id);   // (now, and not kept: anchoring it again later stays)
            else good.push_back({id, s});
        }
        tilegrid::Grid rest = grid_;
        for (const auto &[id, s] : good) rest.Remove(id);
        grid_ = tilegrid::Grid{};
        if (after) {
            for (const tilegrid::Col &c : rest.Cols()) AppendColumn(grid_, c);
            grid_.KeepSplits(rest);   // (the add goes after them: they keep their sides)
        }
        // saved column -> its column now (placed tiles only); the states by column and row, however they came
        std::map<int, int> cols;
        for (const auto &[id, s] : good)
            if (s.Placed()) cols[s.col] = 0;
        int rank = after ? int(grid_.Cols().size()) : 0;
        for (auto &[col, now] : cols) now = rank++;
        std::vector<std::pair<int, State>> sorted = good;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto &a, const auto &b) { return RestoreOrder(a.second) < RestoreOrder(b.second); });
        for (auto [id, s] : sorted) {
            if (s.Placed()) s.col = cols.at(s.col);
            Restore(id, s, -1);
        }
        if (!after)
            for (const tilegrid::Col &c : rest.Cols()) AppendColumn(grid_, c);
        batch_.Start(std::move(good), after ? Batch::Mode::After : Batch::Mode::First);   // (after: Restore forgets)
        return taken;
    }

private:
    Batch batch_;   // RestoreAll's (a panel unanchored since: forgotten)
    void EndBatch() { batch_.End(); }
    void Forget(int id) { batch_.Forget(id); }
    // A state's place for restoring in order: a placed tile's (column, row); others after, as they came.
    static std::pair<int, int> RestoreOrder(const State &s) {
        return s.Placed() ? std::pair<int, int>{s.col, s.row} : std::pair<int, int>{INT_MAX, 0};
    }

    static void AppendColumn(tilegrid::Grid &g, const tilegrid::Col &c) {
        const int at = int(g.Cols().size());
        for (size_t r = 0; r < c.cells.size(); ++r)
            g.Insert(c.cells[r].id, at, int(r), r == 0, c.cells[r].weight, r == 0 ? c.widthDeg : 0, c.cells[r].scale);
    }
    // A drop aimed at `g` as it is, as indices once `id` is out of it.
    static tiling::Drop Without(const tilegrid::Grid &g, int id, tiling::Drop d) {
        const tilegrid::Where w = g.Find(id);
        if (!w.ok() || d.kind == tiling::Drop::TearOff) return d;
        const bool alone = g.Cols()[size_t(w.col)].cells.size() == 1;   // (its column goes with it)
        if (d.kind == tiling::Drop::Into && d.col == w.col) {
            if (alone) return d.kind = tiling::Drop::NewColumn, d;   // onto itself: back where it was
            if (w.row < d.row) --d.row;
            return d;
        }
        if (alone && w.col < d.col) --d.col;
        return d;
    }
    // A tiled panel's column width now (as laid out, before any shrink), in degrees.
    double NowWidth(int id) const {
        if (const tiling::Placed *p = PlacedOf(id)) return (p->az1 - p->az0) / std::max(1e-6, last_.shrink);
        const double w = grid_.WidthOf(id);
        return w > 0 ? w : params.columnDeg;
    }

    std::map<int, Spot> spots_;
    tilegrid::Grid grid_;
    tiling::Layout last_;
    slotsplit::Policy slot_;   // which split round Steam's menu, and when it's chosen again
};

}  // namespace dashtiles
