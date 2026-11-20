// The dashboard tiles' policy (screens/vrtiles.cpp does the OpenVR side): which panels are
// anchored to the SteamVR dashboard, which of those tile (tilegrid.h keeps their order and sizes,
// tiling.h places them) and which are satellites, which stay shown with the dashboard closed, the
// tiling keys (tilekeys.h says what each does to the grid; tileview.h, what fits where as laid out
// last), and each app's or display's last state (Memory).
//
// Anchored (user, 2026-11-16): a panel rides with the dashboard. Dragged on the grid, it moves to
// where it's let go; pulled well off it, it goes back to its place (user, 2026-11-19: tiled mode
// "should always re-orient in the tiled and no longer allow you to pull displays out of the
// tiling"). A satellite (TearOff: off the grid at its own pose in the group's frame) is only an
// older memory's or a direct call's now; a drop or a restore puts it on the grid. Anchored panels stay shown with the dashboard closed as DASH_TILE_STAY says
// (`stayDefault`; on unless set, user 2026-11-18: a click in the room closes the dashboard), unless
// the anchor button's long press set one the other way. Each app or display remembers what it was last: anchored or not,
// its place on the grid or as a satellite, and whether it stays, for the next time it opens.
//
// The group frame (tileframe.h): at your head when the dashboard opened, level, -Z toward Steam's bar
// (AtBar) or the way you faced (GroupFrame).
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilefill.h"
#include "tilegap.h"
#include "tilegrid.h"
#include "tileframe.h"
#include "tilekeys.h"
#include "tilememory.h"
#include "tilepack.h"
#include "tilefit.h"
#include "tilestate.h"
#include "tileview.h"
#include "tiling.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dashtiles {

struct Spot {
    Stay stay = Stay::Default;   // shown with the dashboard closed too (tilestate.h)
    bool satellite = false;   // off the grid, at `rel`
    double rel[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // a satellite's: group frame -> panel
};

class Board {
public:
    tiling::Params params;
    // Tiled panels laid out at no size, so the others close up their gap: Steam's window with the
    // dashboard closed, and any panel not shown (user, 2026-11-19: desktopless mode hid the desktop's
    // screen and a PC's display wasn't live, and their columns left "a massive gap in the middle").
    // A reset packs them at their own size (Repack), so one shown again comes back where it was.
    std::set<int> absent;
    bool stayDefault = false;   // DASH_TILE_STAY: a panel whose stay is Default stays shown

    bool Anchored(int id) const { return spots_.count(id) != 0; }
    bool Tiled(int id) const { return grid_.Has(id); }
    // Shown with the dashboard closed: as set for it, else as stayDefault says.
    bool Stay(int id) const {
        const auto it = spots_.find(id);
        if (it == spots_.end()) return false;
        return it->second.stay == dashtiles::Stay::Default ? stayDefault : it->second.stay == dashtiles::Stay::On;
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
    // pushed it right). On a full desk (OnTopOf, with its `info`): on top of the lowest column.
    // With `leftOf` tiled: a column of its own just left of its.
    // The strip centred again with it (user, 2026-11-19: "the tiling doesn't keep itself centered";
    // a panel that came back after the dashboard opened grew the strip off to one side).
    // `sized`: `info` is its own size, not a guess (no picture yet: Settle checks it once it has one).
    void Anchor(int id, int focused, dashtiles::Stay stay, int leftOf = -1, const Info *info = nullptr, bool sized = true) {
        if (Anchored(id)) return;
        spots_[id].stay = stay;
        Recentre();
        if (const tilegrid::Where w = grid_.Find(leftOf); w.ok()) return grid_.Insert(id, w.col, 0, true);
        if (const int c = info ? Laid().OnTopOf(*info) : -1; c >= 0) {
            if (!sized) unsized_.insert(id);
            stacked_.insert(id);
            return grid_.Insert(id, c, 0, false);
        }
        if (grid_.Has(focused)) return grid_.Add(id, focused);
        // none: the end that brings the strip's middle nearer in front (its left one when it leans
        // right; centred, the right one, however the sums round)
        grid_.AddAtEnd(id, last_.split >= 0 && tiling::WrapNear((last_.az0 + last_.az1) / 2, 0) > 1e-6);
    }
    // Not anchored any more (or gone). False if it wasn't.
    bool Unanchor(int id) {
        Forget(id);
        gaps_.Forget(id);
        Unmark(id);
        grid_.Remove(id);
        return spots_.erase(id) != 0;
    }
    // Shown with the dashboard closed or not, set for it: as the default is, it goes back to
    // following the default (so turning DASH_TILE_STAY over turns it over too).
    void SetStay(int id, bool on) {
        if (!Anchored(id)) return;
        const dashtiles::Stay stay = on == stayDefault ? dashtiles::Stay::Default : on ? dashtiles::Stay::On : dashtiles::Stay::Off;
        spots_[id].stay = stay;
        batch_.SetStay(id, stay);
    }
    // Off the grid, a satellite at `rel` (the group frame's -> the panel's).
    void TearOff(int id, const double rel[3][4]) {
        if (!Anchored(id)) return;
        EndBatch(), Unmark(id);
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
        tiling::Layout l = params.fit ? tilefit::Fit(cols, params) : tiling::Arrange(cols, params, Split());
        if (l.split < 0) tiling::Turn(&l, pan_);   // (as you see it)
        return Without(grid_, id, tiling::DropAt(l, cols, az, el, id));
    }
    // Let go at `az` round you: where DropFor said (a column's indices are the grid's without it), or
    // torn off at `rel`. The strip then goes where it keeps it nearest `az` (Arrange; NAN: the others
    // where they were, it as it falls).
    void Drop(int id, const tiling::Drop &d, double az = NAN) {
        if (!Anchored(id)) return;
        if (d.kind == tiling::Drop::TearOff) {   // (pulled off: a tile stays where it was, a satellite joins the grid)
            if (Satellite(id)) EndBatch(), spots_[id].satellite = false, grid_.Add(id, -1);
            return;
        }
        if (d.kind == tiling::Drop::Lift) {   // (refused: nothing changed, nothing forgotten)
            if (int moved; tilekeys::Lift(grid_, Laid(), id, d.align, &moved))
                EndBatch(), spots_[id].satellite = false, recentre_ = false, moved_ = moved, movedAz_ = NAN,   // (the strip's pan: it's the one that moved)
                    ByHand(id), ByHand(moved);
            return;
        }
        moved_ = id, movedAz_ = az, recentre_ = false;   // (where you let it go wins)
        const tilegrid::Cell *c = grid_.CellOf(id);
        const double scale = c ? c->scale : 1;
        EndBatch();
        spots_[id].satellite = false;
        slot_.Move(grid_, id, [&] {   // (Insert moves it)
            const bool into = d.kind == tiling::Drop::Into;
            grid_.Insert(id, d.col, d.row, !into, scale, d.leftOfSlot, into && d.below);
            grid_.SetAlign(id, into ? d.align : 0);
            return true;
        });
        ByHand(id);   // (where you put it stays, and the stack it's in is yours)
    }

    // Off the grid, its gap filled later (FillGap): a panel that stood in a stack (at or above the
    // desk line) leaves its column short. `key`: its app's or display's (Restore's, to undo the
    // fill when it comes back). Not Unanchor's other callers (Restore puts it back). False if it
    // wasn't anchored.
    bool Leave(int id, int64_t nowMs, const std::string &key = "") {
        const tilegrid::Where w = grid_.Find(id);
        const tiling::Placed *gone = PlacedOf(id);
        tilegap::Gap gap{id, key, -1, gone ? Laid().Unshrunk(gone->el1 - gone->el0) : 0, {}};
        if (!params.fit && w.ok() && gone && !gone->below) {
            const tilegrid::Col &col = grid_.Cols()[size_t(w.col)];
            for (int r = 0; r < int(col.cells.size()); ++r)
                if (col.cells[size_t(r)].id != id && !col.Below(r)) gap.stayed = col.cells[size_t(r)].id, gap.stack.push_back(gap.stayed);
        }
        if (!Unanchor(id)) return false;
        gaps_.Went(gap, nowMs);
        return true;
    }
    // The gap Leave left, filled once no other panel has gone for tilegap::kQuietMs (user,
    // 2026-11-20: a PC window's panel opening or closing, "have all of our displays adjust to fill
    // in the gap"): a neighbouring column's only panel that fits in its place (no wider than the
    // column, no taller than the one that went, on its side of Steam's slot) goes up there, and
    // that column closes up. None when more than one went (a desktop stopping or restarting: they
    // come back where they were). Laid out (Arrange) since the Leave. False when nothing changed.
    bool FillGap(int64_t nowMs) {
        const tilegap::Gap g = gaps_.Due(nowMs);
        const tilegrid::Where w = grid_.Find(g.stayed);
        if (g.stayed < 0 || !w.ok() || params.fit || (g.key.empty() && Anchored(g.gone))) return false;
        if (tilefill::Newcomer(Laid(), g, w.col)) return gaps_.Take(g), false;
        if (!tilefill::Open(Laid(), g, w.col)) return false;
        const tilefill::Pick pick = tilefill::For(Laid(), g, w.col);
        if (pick.col < 0) return false;
        const int other = grid_.Cols()[size_t(pick.col)].cells.front().id;
        grid_.PullTop(pick.col, w.col, 0);   // (on its side of the slot: the split stays a default)
        stacked_.insert(other);   // (on that stack for you: one coming back to it is checked)
        gaps_.Filled({g.key, other, g.stayed, pick.side});
        return true;
    }
    int steam = -1;   // Steam's window (vrtiles.cpp): never pulled into another column (View::Pullable)

    // A panel put on a stack before its size was known (Anchor's guess for one with no picture yet,
    // or Restore's place in a stack another panel took its gap in), shown and laid out since: out
    // into a column of its own beside that stack when the stack's too tall with it (council,
    // 2026-11-20). Otherwise left as it is, as any stack made by hand is. False when nothing changed.
    bool Settle() {
        if (params.fit) return unsized_.clear(), false;
        for (auto it = unsized_.begin(); it != unsized_.end();) {
            const int id = *it;
            const tilegrid::Where w = grid_.Find(id);
            if (!w.ok()) {
                it = unsized_.erase(it);
                continue;
            }
            const View v = Laid();
            if (absent.count(id) || !v.LaidOut(w.col)) {   // (its size not known yet)
                ++it;
                continue;
            }
            const bool fits = grid_.Cols()[size_t(w.col)].cells.size() < 2 || v.ColumnFits(w.col, fills_[size_t(w.col)].height);
            if (fits && !v.AllShown(w.col)) {   // (fits so far, a stackmate's size not known yet: one shown can only add)
                ++it;
                continue;
            }
            it = unsized_.erase(it);
            if (fits) continue;
            moved_ = id, movedAz_ = NAN;   // (the others keep their places)
            gaps_.Forget(id), stacked_.erase(id);   // (a gap it stayed in goes with it: nothing climbs onto it)
            grid_.Insert(id, w.col + 1, 0, true, grid_.CellOf(id)->scale, Laid().SideOf(w.col) < 0);
            grid_.SetAlign(id, 0);
            return true;
        }
        return false;
    }

    // A tiling key on a tiled panel (a satellite: nothing). False when nothing changed.
    bool Press(int id, Key k) {
        if (!grid_.Has(id)) return false;
        int moved = id;
        if (!Changed(tilekeys::Press(grid_, slot_, Laid(), id, k, &moved))) return false;   // (refused: nothing changed, nothing forgotten)
        moved_ = moved, movedAz_ = NAN, recentre_ = false;   // (moved by a key: the others keep their places, not it)
        return ByHand(id), ByHand(moved), true;   // (a lift's: the one pulled in too)
    }
    // Resized by its corner on the grid: `factor` times as big as its tile was.
    bool Resized(int id, double factor) {
        if (!grid_.Has(id) || !(factor > 0)) return false;
        return Changed(grid_.Scale(id, factor)) && (ByHand(id), true);
    }

    // Every tile packed into columns again (tilepack.h), sizes kept: a reset's (user, 2026-11-19:
    // "Tiling looks way broken ... There's a massive gap in the middle").
    template <typename InfoOf>
    void Repack(InfoOf info) {
        std::vector<tilepack::Item> items;
        std::map<int, double> scales;
        for (const tilegrid::Col &col : grid_.Cols())
            for (const tilegrid::Cell &cell : col.cells) {
                const Info i = absent.count(cell.id) ? Info{0, 0} : info(cell.id);   // (not shown: a column of its own)
                items.push_back({cell.id, SpanDeg(params, i.widthM * cell.scale), SpanDeg(params, i.heightM * cell.scale)});
                scales[cell.id] = cell.scale;
            }
        if (items.empty()) return;
        EndBatch();
        const std::vector<std::vector<int>> cols = tilepack::Pack(items, params.gapDeg, EyeFits, &params);
        std::map<int, tilepack::Item> byId;
        for (const tilepack::Item &it : items) byId[it.id] = it;
        grid_ = tilegrid::Grid{}, stacked_.clear();   // (packed afresh: none on a stack for you now)
        for (size_t c = 0; c < cols.size(); ++c) {
            std::vector<tilepack::Item> deskUp;
            for (const int id : cols[c]) deskUp.push_back(byId[id]);
            const tilepack::Sides s = tilepack::UpAndDown(deskUp, params.gapDeg);   // (round the desk line: up, then down)
            for (size_t r = 0; r < s.up.size(); ++r)   // (row 0 is the top: each one up goes in over the rest)
                grid_.Insert(s.up[r], int(c), 0, r == 0, scales[s.up[r]]);
            for (const int id : s.down) grid_.Insert(id, int(c), INT_MAX, false, scales[id], false, true);
        }
        Recentre();
    }

private:
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
            c.base = gc.base;
            for (const tilegrid::Cell &cell : gc.cells) {
                const Info i = absent.count(cell.id) ? Info{0, 0} : info(cell.id);
                c.tiles.push_back({cell.id, i.widthM * cell.scale, i.heightM * cell.scale, cell.align});
            }
            cols.push_back(c);
        }
        return cols;
    }
    // Laid out, round Steam's slot as slotsplit.h says, and without one turned to keep the strip
    // where it was (Pan).
    template <typename InfoOf>
    const tiling::Layout &Arrange(InfoOf info) {
        const std::vector<tiling::Column> cols = Columns(info);
        fills_.clear();
        for (const tiling::Column &c : cols) {   // (each column's shelves so far, from the desk up: OnTopOf)
            std::vector<tilepack::Item> deskUp;
            for (int r = c.Base(); r >= 0; --r) deskUp.push_back({c.tiles[size_t(r)].id, SpanDeg(params, c.tiles[size_t(r)].widthM), SpanDeg(params, c.tiles[size_t(r)].heightM)});
            fills_.push_back(tilepack::FillOf(deskUp, params.gapDeg));
        }
        tiling::Layout l = params.fit ? tilefit::Fit(cols, params) : slot_.Arrange(grid_, cols, params);
        pan_ = l.split >= 0 || params.fit ? 0 : Pan(l);   // (fitted: always round the middle)
        if (pan_ != 0) tiling::Turn(&l, pan_);
        moved_ = -1, recentre_ = false;
        return last_ = std::move(l);
    }
    // The dashboard opened: the strip centred again on the next Arrange (its middle straight ahead),
    // or with panel `on`'s middle straight ahead (Steam's page over its bar), not where it was.
    void Recentre(int on = -1) { recentre_ = true, centreOn_ = on; }
    double PanDeg() const { return pan_; }
    // Steam's menu is where params.reserveAzDeg says now (the dashboard opened again).
    void SlotMoved() { slot_.Moved(params.reserveAzDeg); }
    // The split in use now (-1: no slot in the strip, or not chosen yet).
    int Split() const { return slot_.Split(grid_); }
    const tiling::Layout &Last() const { return last_; }
    const tiling::Placed *PlacedOf(int id) const { return Laid().PlacedOf(id); }
    // The board as laid out last, to ask what fits where (tileview.h).
    View Laid() const { return {params, grid_, last_, fills_, absent, slot_, steam}; }

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
        st.scale = cell.scale, st.below = col.Below(w.row), st.align = cell.align;
        return st;
    }
    // Back as it was remembered (`focused`: a new tile's neighbour when its column is gone). False
    // (and nothing changed) for a line that isn't one.
    // `key`: its app's or display's (a fill Leave's gap had, undone).
    bool Restore(int id, const std::string &line, int focused, const std::string &key = "") {
        State s;
        return Parse(line, &s) && (Restore(id, s, focused, key), true);
    }
    void Restore(int id, const State &st, int focused, const std::string &key = "") {
        const tilegap::Fill fill = gaps_.Back(id, key);   // (before Unanchor forgets its panel)
        const bool taken = gaps_.Took(key);
        Unanchor(id);
        if (st.kind == State::Kind::Off) return;
        Recentre();   // (as Anchor)
        Spot &s = spots_[id];
        s.stay = st.stay;
        if (st.kind == State::Kind::Sat) return grid_.Add(id, focused);   // (no satellites in tiled mode: on the grid)
        if (!st.Placed()) return grid_.Add(id, focused);
        int col = st.col;   // (back in the stack it left: the panel pulled into its gap goes back first)
        if (!st.alone && fill.pulled >= 0 && tilegap::Undo(grid_, fill, Laid().SideOf(grid_.Find(fill.stayed).col) < 0))
            col = grid_.Find(fill.stayed).col, stacked_.erase(fill.pulled);   // (back in its own column)
        grid_.Insert(id, col, st.row, st.alone, st.scale, false, st.below);
        grid_.SetAlign(id, st.align);
        if (!st.alone && (taken || AutoStacked(grid_.Find(id).col))) unsized_.insert(id);   // (its stack isn't as it left it: Settle)
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
        gaps_.Clear(), stacked_.clear();   // (the profile's stacks are yours)
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
    void EndBatch() { batch_.End(), gaps_.Changed(); }   // (the grid changed by hand)
    void Forget(int id) { batch_.Forget(id); }
    // A state's place for restoring in order: a placed tile's (column, row); others after, as they came.
    static std::pair<int, int> RestoreOrder(const State &s) {
        return s.Placed() ? std::pair<int, int>{s.col, s.row} : std::pair<int, int>{INT_MAX, 0};
    }

    // The turn for `l` (laid out centred: Arrange): Recentre's; else, after a change (user,
    // 2026-11-18: a panel moved from the right to the left "should put that monitor roughly where I
    // put it, but not shift all monitors around"; Arrange centres the strip, so it shifted them by the
    // moved one's width), the one most of the others want to stay where they were: moved to an end,
    // they stay; into the middle of two, both make room. The one moved counts only
    // when there are no others (it where it was let go; by a key, not at all: it pulled them a fifth
    // of its width when it did). Always with the strip still reaching straight ahead (what it's
    // centred round: Steam's bar, or where you faced).
    double Pan(const tiling::Layout &l) const {
        double pan = pan_;
        if (recentre_) {
            pan = 0;
            for (const tiling::Placed &p : l.tiles)
                if (p.id == centreOn_) pan = -p.azDeg;
        } else {
            // (a vote a column, its widest panel's, which leans against nothing; the median: most
            // columns stay exactly where they were. A mean let one odd panel pull the rest: a narrower
            // one flips the way it leans, Lean, or one left alone by the wide one it leaned on lands
            // somewhere new, and took the others 6 to 9 degrees. A vote a panel let a stacked column
            // outvote the one across from it: let go between them, it went 33 degrees off.)
            std::map<int, std::pair<double, double>> vote;   // column -> (its widest's width, how far it wants turning)
            double alone = NAN;
            for (const tiling::Placed &p : l.tiles) {
                if (p.id == moved_) {
                    if (!std::isnan(movedAz_)) alone = tiling::WrapNear(movedAz_ - p.azDeg, 0);
                    continue;
                }
                for (const tiling::Placed &q : last_.tiles)
                    if (q.id == p.id) {
                        auto [it, added] = vote.try_emplace(p.col, -1.0, 0.0);
                        if (p.az1 - p.az0 > it->second.first) it->second = {p.az1 - p.az0, tiling::WrapNear(q.azDeg - p.azDeg, 0)};
                    }
            }
            std::vector<double> want;
            for (const auto &[col, v] : vote) want.push_back(v.second);
            if (const size_t n = want.size()) {   // (the median; of an even count, the middle two's mean: into
                std::sort(want.begin(), want.end());   // the middle of two, both sides make room)
                pan = (want[(n - 1) / 2] + want[n / 2]) / 2;
            } else if (!std::isnan(alone)) pan = alone;
        }
        return std::clamp(pan, -l.az1, -l.az0);
    }
    double pan_ = 0;              // the strip turned this far from centred
    bool recentre_ = false;
    int centreOn_ = -1;
    int moved_ = -1;              // the panel just moved (the next Arrange's), and where it was let go (NAN: by a key, or not said)
    double movedAz_ = NAN;

    static void AppendColumn(tilegrid::Grid &g, const tilegrid::Col &c) {
        const int at = int(g.Cols().size());
        for (size_t r = 0; r < c.cells.size(); ++r) {
            g.Insert(c.cells[r].id, at, int(r), r == 0, c.cells[r].scale, false, c.Below(int(r)));
            g.SetAlign(c.cells[r].id, c.cells[r].align);
        }
    }
    // A drop aimed at `g` as it is, as indices once `id` is out of it.
    static tiling::Drop Without(const tilegrid::Grid &g, int id, tiling::Drop d) {
        const tilegrid::Where w = g.Find(id);
        if (!w.ok() || d.kind == tiling::Drop::TearOff) return d;
        const bool alone = g.Cols()[size_t(w.col)].cells.size() == 1;   // (its column goes with it)
        if (d.kind == tiling::Drop::Into && d.col == w.col) {
            if (d.over && w.row == 0) return d.kind = tiling::Drop::Lift, d;   // over itself: up (the grid's indices, with it)
            if (alone) return d.kind = tiling::Drop::NewColumn, d;   // onto itself: back where it was
            if (w.row < d.row) --d.row;
            return d;
        }
        if (alone && w.col < d.col) --d.col;
        return d;
    }
    std::map<int, Spot> spots_;
    tilegrid::Grid grid_;
    tiling::Layout last_;
    std::vector<tilepack::Fill> fills_;   // each column as laid out last (OnTopOf)
    slotsplit::Policy slot_;   // which split round Steam's menu, and when it's chosen again
    tilegap::Book gaps_;       // the gaps panels left (Leave, FillGap), and the fills to undo
    std::set<int> unsized_;    // on a stack before their size was known (Settle)
    std::set<int> stacked_;    // put on a stack for you (Anchor's OnTopOf, FillGap), that stack not changed by hand since
    // Off the grid: nothing to check for it.
    void Unmark(int id) { unsized_.erase(id), stacked_.erase(id); }
    // Moved or sized by hand where it is now: that's yours, and so is the stack it's in (council,
    // 2026-11-20: one built on a stack made for you, then back in it, stays where you put it).
    void ByHand(int id) {
        Unmark(id);
        if (const tilegrid::Where w = grid_.Find(id); w.ok())
            for (const tilegrid::Cell &c : grid_.Cols()[size_t(w.col)].cells) stacked_.erase(c.id);
    }
    // Column `col` holds a panel put on it for you: one coming back to its stack there may not fit.
    bool AutoStacked(int col) const {
        for (const tilegrid::Cell &c : grid_.Cols()[size_t(col)].cells)
            if (stacked_.count(c.id)) return true;
        return false;
    }
};

}  // namespace dashtiles
