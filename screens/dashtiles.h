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
// The centre display (user, 2026-11-23: "a center display that the tiling justifies around ... it's
// really easy to end up with multiple curved displays that you never have one that's looking directly
// at you"): the strip, recentred (the dashboard opening, a panel anchored or back, a new centre), turns
// to put it straight ahead, not the middle of the strip (two as wide: the seam between them); moves by
// hand keep it where they leave it, as they keep the others. It's the one you chose (a drop right onto the
// one before: TakeCentre), else Steam's window while it's a tile (it was over its bar), else the
// widest. It alone may curve (ToggleArc: round the tiles' arc, vrtiles.cpp). Any tile may be nudged
// up or down on its own (Key::NudgeUp, NudgeDown), the others staying where they are. All three are
// in its remembered state, so per app and in profiles.
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
#include "tilemarks.h"
#include "tilememory.h"
#include "tilepack.h"
#include "tilefit.h"
#include "tilestate.h"
#include "tileview.h"
#include "tiledrop.h"
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
    // pushed it right). On a full desk (RoomFor, with its `info`): on top of the lowest column.
    // With `leftOf` tiled: a column of its own just left of its.
    // The strip centred again with it (user, 2026-11-19: "the tiling doesn't keep itself centered";
    // a panel that came back after the dashboard opened grew the strip off to one side).
    // `sized`: `info` is its own size, not a guess (no picture yet: Settle checks it once it has one).
    void Anchor(int id, int focused, dashtiles::Stay stay, int leftOf = -1, const Info *info = nullptr, bool sized = true) {
        if (Anchored(id)) return;
        spots_[id].stay = stay;
        Recentre();
        if (const tilegrid::Where w = grid_.Find(leftOf); w.ok()) return grid_.Insert(id, w.col, 0, true);
        if (const View::Room r = info ? Laid().RoomFor(*info) : View::Room{}; r.col >= 0) {
            if (!sized) marks_.Unsized(id);
            marks_.Stacked(id);
            return grid_.Insert(id, r.col, r.under ? INT_MAX : 0, false, 1, false, r.under);
        }
        if (grid_.Has(focused)) return grid_.Add(id, focused);
        // none: the end that brings the strip's middle nearer in front (its left one when it leans
        // right; centred, the right one, however the sums round)
        grid_.AddAtEnd(id, last_.split >= 0 && tiling::WrapNear((last_.az0 + last_.az1) / 2, 0) > 1e-6);
    }
    // Not anchored any more (or gone). False if it wasn't.
    bool Unanchor(int id) {
        arc_.erase(id);
        if (centre_ == id) centre_ = -1;
        return TakeOff(id);
    }
    // Taken off by hand (the pin button, In the Room): anchored again, it's the centre display and
    // curved as it was, unless you chose another since (user, 2026-11-23: an ultrawide pulled off
    // and put back wasn't the centre any more: "off to my side on a reset and doesnt let me curve").
    bool TakeOff(int id) {
        Forget(id);
        nudge_.erase(id);
        gaps_.Forget(id);
        marks_.Unmark(id);
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
        EndBatch(), marks_.Unmark(id);
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
            const std::vector<int> mates = Marks::Mates(grid_, id);
            if (int moved; tilekeys::Lift(grid_, Laid(), id, d.align, &moved))
                EndBatch(), spots_[id].satellite = false, recentre_ = false, moved_ = moved, movedAz_ = NAN,   // (the strip's pan: it's the one that moved)
                    ByHand(id, mates);   // (the one pulled in: a new stackmate)
            return;
        }
        moved_ = id, movedAz_ = az, recentre_ = false;   // (where you let it go wins)
        const tilegrid::Cell *c = grid_.CellOf(id);
        const double scale = c ? c->scale : 1;
        EndBatch();
        spots_[id].satellite = false;
        const std::vector<int> mates = Marks::Mates(grid_, id);
        const std::string was = Remember(id);
        const int split = Split();
        slot_.Move(grid_, id, [&] {   // (Insert moves it)
            const bool into = d.kind == tiling::Drop::Into;
            grid_.Insert(id, d.col, d.row, !into, scale, d.leftOfSlot, into && d.below);
            grid_.SetAlign(id, into ? d.align : 0);
            grid_.SetRow(id, into && d.ownRow);
            if (into) grid_.SetRow(d.before, false);
            return true;
        });
        // Where you put it stays, and a stack you put it in is yours. Let go where it was: nothing.
        if (Remember(id) != was || Split() != split) ByHand(id, mates);
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
        marks_.Stacked(other);   // (on that stack for you: one coming back to it is checked)
        gaps_.Filled({g.key, other, g.stayed, pick.side});
        return true;
    }
    // The centre display the profile in use saved, by its memory's key (empty: none): while it's the
    // centre, a panel that comes back remembered as the centre doesn't take it (user, 2026-11-23:
    // "profile saved choices should take priority"), and it takes it when it comes back itself.
    std::string profileCentre;
    int steam = -1;   // Steam's window (vrtiles.cpp): never pulled into another column (View::Pullable)

    // A panel put on a stack before its size was known (Anchor's guess for one with no picture yet,
    // or Restore's place in a stack another panel took its gap in), shown and laid out since: out
    // into a column of its own beside that stack when the stack's too tall with it (council,
    // 2026-11-20). Otherwise left as it is, as any stack made by hand is. False when nothing changed.
    bool Settle() {
        if (params.fit) return marks_.ClearUnsized(), false;
        const std::set<int> unsized = marks_.Unsized();   // (Sized as it goes)
        for (const int id : unsized) {
            const tilegrid::Where w = grid_.Find(id);
            if (!w.ok()) {
                marks_.Sized(id);
                continue;
            }
            const View v = Laid();
            if (absent.count(id) || !v.LaidOut(w.col)) continue;   // (its size not known yet)
            // Too tall with it? Laid out with its hidden stackmates at no size, and as it'll be with them
            // shown (Neon, 2026-11-20: not always shorter, as a wide one hidden at the bottom
            // lets the narrow ones over it stand side by side no more).
            const bool stack = grid_.Cols()[size_t(w.col)].cells.size() >= 2;
            const bool fits = !stack || v.ColumnFits(w.col, fills_[size_t(w.col)].height);
            const bool fitsShown = !stack || v.ColumnFits(w.col, shownFills_[size_t(w.col)].height);
            // (a stackmate not shown: out early only when too tall both ways. Fitting isn't sure till
            // it's shown: one hanging under the desk line isn't in those heights, and one with no
            // picture yet has only a guess for a size.)
            if (!v.AllShown(w.col) && (fits || fitsShown)) continue;
            marks_.Sized(id);
            if (fits) continue;
            moved_ = id, movedAz_ = NAN;   // (the others keep their places)
            gaps_.Forget(id), marks_.NotStacked(id);   // (a gap it stayed in goes with it: nothing climbs onto it)
            grid_.Insert(id, w.col + 1, 0, true, grid_.CellOf(id)->scale, v.SideOf(w.col) < 0);
            grid_.SetAlign(id, 0);
            return true;
        }
        return false;
    }

    // A tiling key on a tiled panel (a satellite: nothing). False when nothing changed.
    bool Press(int id, Key k) {
        if (!grid_.Has(id)) return false;
        if (k == Key::NudgeUp || k == Key::NudgeDown) {   // (it alone: the grid and the others as they are)
            const double was = Nudge(id), to = std::clamp(was + (k == Key::NudgeUp ? kNudgeStepDeg : -kNudgeStepDeg), -kNudgeMaxDeg, kNudgeMaxDeg);
            if (to == was) return false;
            EndBatch();
            if (std::fabs(to) < 1e-9) nudge_.erase(id);
            else nudge_[id] = to;
            moved_ = -1, movedAz_ = NAN, recentre_ = false;
            return true;
        }
        int moved = id;
        const std::vector<int> mates = Marks::Mates(grid_, id);
        if (!Changed(tilekeys::Press(grid_, slot_, Laid(), id, k, &moved))) return false;   // (refused: nothing changed, nothing forgotten)
        moved_ = moved, movedAz_ = NAN, recentre_ = false;   // (moved by a key: the others keep their places, not it)
        return ByHand(id, mates), true;   // (a lift's: the one pulled in, a new stackmate)
    }
    static constexpr double kNudgeStepDeg = 1, kNudgeMaxDeg = 30;
    double Nudge(int id) const {
        const auto it = nudge_.find(id);
        return it == nudge_.end() ? 0 : it->second;
    }
    // The centre display as laid out last (-1: none), and whether it's curved round the arc now.
    int Centre() const { return centreNow_; }
    // ...the one the wall's middle section is (tilecurve.h): it, or while it's hidden (no size) the shown
    // one nearest straight ahead.
    int WallCentre() const {
        if (!absent.count(centreNow_)) return centreNow_;
        int best = -1;
        double off = INFINITY;
        for (const tiling::Placed &p : last_.tiles)
            if (!absent.count(p.id) && std::fabs(p.azDeg) < off - 1e-9) off = std::fabs(p.azDeg), best = p.id;
        return best;
    }
    bool ArcNow(int id) const { return id >= 0 && id == centreNow_ && arc_.count(id) && !absent.count(id); }
    bool Arc(int id) const { return arc_.count(id) != 0; }
    // The centre display's curve round the tiles' arc on or off (kept if another takes the centre:
    // it curves again when it's the centre again). False for any other panel.
    bool ToggleArc(int id) {
        if (id < 0 || id != centreNow_ || !grid_.Has(id)) return false;
        EndBatch();
        if (!arc_.erase(id)) arc_.insert(id);
        return true;
    }
    // `id` the centre display: let go right on the one before, the two trade places (each keeps its
    // size). False when it isn't a tile or already is the centre.
    bool TakeCentre(int id) {
        if (!grid_.Has(id) || (id == centreNow_ && id == centre_)) return false;
        EndBatch();
        if (centreNow_ >= 0 && centreNow_ != id) grid_.Swap(id, centreNow_);
        centre_ = id, centreNow_ = id, centreOfProfile_ = false;
        moved_ = -1, movedAz_ = NAN;
        Recentre();   // (it straight ahead)
        return true;
    }

    // Resized by its corner on the grid: `factor` times as big as its tile was.
    // Within kResizeSlack of 1: a corner let go where it was (vrtiles.cpp's size there is only kept
    // to 2 mm), nothing.
    static constexpr double kResizeSlack = 0.005;
    bool Resized(int id, double factor) {
        if (!grid_.Has(id) || !(factor > 0) || std::fabs(factor - 1) < kResizeSlack) return false;
        const std::vector<int> mates = Marks::Mates(grid_, id);
        return Changed(grid_.Scale(id, factor)) && (ByHand(id, mates), true);
    }

    // Every tile packed into columns again (tilepack.h), the centre display's in the middle (hidden
    // now or not), sizes kept: a reset's (user, 2026-11-19: "Tiling looks way broken ... There's a
    // massive gap in the middle").
    template <typename InfoOf>
    void Repack(InfoOf info) {
        std::vector<tilepack::Item> items;
        std::map<int, double> scales;
        for (const tilegrid::Col &col : grid_.Cols())
            for (const tilegrid::Cell &cell : col.cells) {
                const Info i = absent.count(cell.id) ? Info{0, 0} : info(cell.id);   // (not shown: a column of its own)
                items.push_back(ItemOf({cell.id, i.widthM * cell.scale, i.heightM * cell.scale}));
                scales[cell.id] = cell.scale;
            }
        if (items.empty()) return;
        EndBatch();
        const std::vector<std::vector<int>> cols = tilepack::Pack(items, params.gapDeg, EyeFits, &params, CentreOf(info));
        std::map<int, tilepack::Item> byId;
        for (const tilepack::Item &it : items) byId[it.id] = it;
        grid_ = tilegrid::Grid{}, marks_.ClearStacked();   // (packed afresh: none on a stack for you now)
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
    tilepack::Item ItemOf(const tiling::Tile &t) const {
        return {t.id, SpanDeg(params, t.widthM), SpanDeg(params, t.heightM), tiling::Wide(params, t.widthM, t.heightM), t.row};
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
            c.base = gc.base;
            for (const tilegrid::Cell &cell : gc.cells) {
                const Info i = absent.count(cell.id) ? Info{0, 0} : info(cell.id);
                c.tiles.push_back({cell.id, i.widthM * cell.scale, i.heightM * cell.scale, cell.align, cell.row});
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
        fills_.clear(), shownFills_.clear(), underFills_.clear();
        for (size_t ci = 0; ci < cols.size(); ++ci) {   // (each column's shelves so far, from the desk up and down: RoomFor)
            const tiling::Column &c = cols[ci];
            std::vector<tilepack::Item> deskUp, shown, deskDown;   // (shown: its hidden ones at their own size, Settle)
            const int wideRow = tiling::WideRow(c, params);   // (its display's shelves are as wide as it, over it and under it)
            const double widest = wideRow < 0 ? 0 : SpanDeg(params, c.tiles[size_t(wideRow)].widthM);
            for (int r = c.Base() + 1; r < int(c.tiles.size()); ++r)
                deskDown.push_back(ItemOf(c.tiles[size_t(r)]));
            for (int r = c.Base(); r >= 0; --r) {
                const tiling::Tile &t = c.tiles[size_t(r)];
                deskUp.push_back(ItemOf(t));
                shown.push_back(deskUp.back());
                if (absent.count(t.id)) {
                    const Info own = info(t.id);
                    const double scale = grid_.Cols()[ci].cells[size_t(r)].scale;   // (cells and tiles row for row)
                    shown.back() = ItemOf({t.id, own.widthM * scale, own.heightM * scale, t.align, t.row});
                }
            }
            fills_.push_back(tilepack::FillOf(deskUp, params.gapDeg, widest, wideRow >= 0));
            shownFills_.push_back(tilepack::FillOf(shown, params.gapDeg, widest, wideRow >= 0));
            underFills_.push_back(tilepack::FillOf(deskDown, params.gapDeg, widest, wideRow >= 0));
        }
        tiling::Layout l = params.fit ? tilefit::Fit(cols, params) : slot_.Arrange(grid_, cols, params);
        centreNow_ = CentreOf(info);
        pan_ = l.split >= 0 || params.fit ? 0 : Pan(l);   // (fitted: always round the middle)
        if (pan_ != 0) tiling::Turn(&l, pan_);
        for (tiling::Placed &p : l.tiles)   // (nudged: it alone, up or down)
            if (const double n = Nudge(p.id); n != 0) p.elDeg += n, p.el0 += n, p.el1 += n;
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
    View Laid() const { return {params, grid_, last_, fills_, underFills_, absent, slot_, steam}; }

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
        st.scale = cell.scale, st.below = col.Below(w.row), st.align = cell.align, st.ownRow = cell.row;
        st.centre = centre_ == id, st.arc = Arc(id), st.nudgeDeg = Nudge(id);
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
        if (st.nudgeDeg != 0) nudge_[id] = std::clamp(st.nudgeDeg, -kNudgeMaxDeg, kNudgeMaxDeg);
        if (st.arc) arc_.insert(id);
        const bool ofProfile = !profileCentre.empty() && key == profileCentre;
        if (st.centre && (ofProfile || !centreOfProfile_ || !grid_.Has(centre_))) centre_ = id, centreOfProfile_ = ofProfile;
        if (st.kind == State::Kind::Sat) return grid_.Add(id, focused);   // (no satellites in tiled mode: on the grid)
        if (!st.Placed()) return grid_.Add(id, focused);
        int col = st.col;   // (back in the stack it left: the panel pulled into its gap goes back first)
        if (!st.alone && fill.pulled >= 0 && tilegap::Undo(grid_, fill, Laid().SideOf(grid_.Find(fill.stayed).col) < 0))
            col = grid_.Find(fill.stayed).col, marks_.NotStacked(fill.pulled);   // (back in its own column)
        grid_.Insert(id, col, st.row, st.alone, st.scale, false, st.below);
        grid_.SetAlign(id, st.align), grid_.SetRow(id, st.ownRow);
        if (!st.alone && (taken || marks_.AutoStacked(grid_, grid_.Find(id).col))) marks_.Unsized(id);   // (its stack isn't as it left it: Settle)
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
        gaps_.Clear(), marks_.ClearStacked();   // (the profile's stacks are yours)
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
        for (const auto &[id, s] : good)
            if (s.centre) centre_ = id, centreOfProfile_ = true;
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
        if (recentre_) {   // (round the one asked for, else the centre display: one straight ahead,
            pan = 0;       // not the seam between two: user, 2026-11-23)
            const int on = centreOn_ >= 0 ? centreOn_ : absent.count(centreNow_) ? -1 : centreNow_;
            for (const tiling::Placed &p : l.tiles)
                if (p.id == on) pan = -p.azDeg;
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
    // The centre display: the one chosen (TakeCentre, or remembered) while it's a tile, else the
    // widest (its own size, shown or not: one hidden keeps its place as the centre), of ones as wide
    // the one nearest the middle column. Steam's window only when it's alone: gone with the dashboard
    // closed, it's at the strip's end, so straight ahead it put all the others to one side (user, 2026-11-23).
    template <typename InfoOf>
    int CentreOf(InfoOf info) const {
        if (grid_.Has(centre_)) return centre_;
        int best = grid_.Has(steam) ? steam : -1;
        double widest = 0, off = 0;
        const double mid = (double(grid_.Cols().size()) - 1) / 2;
        for (size_t c = 0; c < grid_.Cols().size(); ++c)
            for (const tilegrid::Cell &cell : grid_.Cols()[c].cells) {
                if (cell.id == steam) continue;
                const double w = info(cell.id).widthM * cell.scale, o = std::fabs(double(c) - mid);
                if (w > widest + 1e-6 || (w > widest - 1e-6 && o < off - 1e-9)) widest = w, off = o, best = cell.id;
            }
        return best;
    }
    int centre_ = -1;             // the centre display chosen (-1: none: CentreOf's default)
    bool centreOfProfile_ = false;   // ...by the profile in use (profileCentre)
    int centreNow_ = -1;          // ...the one laid out last
    std::set<int> arc_;           // curved round the arc while the centre
    std::map<int, double> nudge_; // moved up (down: negative), degrees
    double pan_ = 0;              // the strip turned this far from centred
    bool recentre_ = false;
    int centreOn_ = -1;
    int moved_ = -1;              // the panel just moved (the next Arrange's), and where it was let go (NAN: by a key, or not said)
    double movedAz_ = NAN;

    static void AppendColumn(tilegrid::Grid &g, const tilegrid::Col &c) {
        const int at = int(g.Cols().size());
        for (size_t r = 0; r < c.cells.size(); ++r) {
            g.Insert(c.cells[r].id, at, int(r), r == 0, c.cells[r].scale, false, c.Below(int(r)));
            g.SetAlign(c.cells[r].id, c.cells[r].align), g.SetRow(c.cells[r].id, c.cells[r].row);
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
    std::vector<tilepack::Fill> fills_;        // each column as laid out last (RoomFor), from the desk line up
    std::vector<tilepack::Fill> underFills_;   // ...and down
    std::vector<tilepack::Fill> shownFills_;   // ...with its hidden panels at their own size (Settle)
    slotsplit::Policy slot_;   // which split round Steam's menu, and when it's chosen again
    tilegap::Book gaps_;       // the gaps panels left (Leave, FillGap), and the fills to undo
    Marks marks_;              // the tiles Settle still checks (tilemarks.h)
    void ByHand(int id, const std::vector<int> &mates) { marks_.ByHand(grid_, absent, id, mates); }
};

}  // namespace dashtiles
