// The dashboard tiles' policy (screens/vrtiles.cpp does the OpenVR side): which panels are
// anchored to the SteamVR dashboard, which of those tile (tilegrid.h keeps their order and sizes,
// tiling.h places them) and which are satellites, which stay shown with the dashboard closed, what
// the tiling keys do, and each app's or display's last state (Memory).
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
// The group frame: at your head when the dashboard opened, level, -Z toward Steam's bar (AtBar) or the
// way you faced (GroupFrame).
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilegrid.h"
#include "tilememory.h"
#include "tilepack.h"
#include "tilefit.h"
#include "tilestate.h"
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

// What the host says about a panel: its natural size at the wall (its pixels, tiling::MetresPerPx each).
struct Info {
    double widthM = 1.6, heightM = 0.9;
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

// The wall at Steam's bar (user, 2026-11-18: "the same distance that the Steam menu is ... the
// middle display should be positioned right above where the Steam menu bar is"; DASH_TILE_AT_BAR,
// on unless "0"): the group frame at the head, level, facing the bar, so the strip is centred over
// it; the wall (the tiles' corners) kBehindBarM further from the head than the bar is (user,
// 2026-11-18: "move the display's distance a little bit back ... I can't interact with the Steam
// menu because it's covered by one of the displays": a flat tile's middle comes nearer than its
// corners, and the laser starts at the hand, below the head, so a tile at the bar's distance stood
// between the hand and the bar); and the desk line `gapDeg` above the bar's top edge, an angle, so
// it stays clear of the bar however far back the wall is. `bar`: its middle, `barUp`: its +Y (a unit vector: Steam tilts it back,
// so the top edge is a little further away too), `barHeightM` (0: not known, its middle counts).
// Into the frame g, *radius (kWallMinM..kWallMaxM) and *deskDeg (kDeskMinDeg..kDeskMaxDeg), the
// ranges DASH_TILE_RADIUS and DASH_TILE_DESK_DEG have. False, nothing set, with the bar under
// kMinFlatM round from straight under or over the head, or anything not finite.
constexpr double kWallMinM = 0.8, kWallMaxM = 6, kDeskMinDeg = -40, kDeskMaxDeg = 20, kMinFlatM = 0.2, kBehindBarM = 0.3;
// The desk line's lift over the bar's top edge (the `gapDeg` callers pass): the bar draws over the
// tiles whatever their depth, and at the tiles' gap (0.8 degrees) the front tile's bottom edge stood
// under it (user, 2026-11-18: "it is just slightly above the dashboard bar and somewhat gets covered.
// So we just want to rise that up just a little bit").
constexpr double kOverBarDeg = 4;
inline bool AtBar(const double head[3], const double bar[3], const double barUp[3], double barHeightM, double gapDeg,
                  double g[3][4], double *radius, double *deskDeg) {
    const double d[3] = {bar[0] - head[0], bar[1] - head[1], bar[2] - head[2]};
    const double flat = std::hypot(d[0], d[2]), dist = std::sqrt(flat * flat + d[1] * d[1]);
    const double half = std::isfinite(barHeightM) && barHeightM > 0 ? barHeightM / 2 : 0;
    const double top[3] = {d[0] + barUp[0] * half, d[1] + barUp[1] * half, d[2] + barUp[2] * half};
    const double el = tiling::Deg(std::atan2(top[1], std::hypot(top[0], top[2]))) + gapDeg;
    if (!(flat >= kMinFlatM) || !std::isfinite(dist + el)) return false;
    const double fwd[3] = {d[0] / flat, 0, d[2] / flat}, up[3] = {0, 1, 0};
    GroupFrame(head, fwd, up, g);
    *radius = std::clamp(dist + kBehindBarM, kWallMinM, kWallMaxM);
    *deskDeg = std::clamp(el, kDeskMinDeg, kDeskMaxDeg);
    return true;
}

// A column `height` tall from the desk line (ctx: the board's tiling::Params): no taller than
// tilepack::kColumnDeg as your eyes see it.
inline bool EyeFits(const void *ctx, double height) {
    const tiling::Params &p = *static_cast<const tiling::Params *>(ctx);
    return tiling::EyeElDeg(p, p.deskDeg + height) - tiling::EyeElDeg(p, p.deskDeg) <= tilepack::kColumnDeg + 1e-9;
}
// ...with `under` of it hanging under the desk line.
inline bool EyeFitsRound(const tiling::Params &p, double under, double height) {
    return tiling::EyeElDeg(p, p.deskDeg - under + height) - tiling::EyeElDeg(p, p.deskDeg - under) <= tilepack::kColumnDeg + 1e-9;
}

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
    void Anchor(int id, int focused, dashtiles::Stay stay, int leftOf = -1, const Info *info = nullptr) {
        if (Anchored(id)) return;
        spots_[id].stay = stay;
        Recentre();
        if (const tilegrid::Where w = grid_.Find(leftOf); w.ok()) return grid_.Insert(id, w.col, 0, true);
        if (const int c = info ? OnTopOf(*info) : -1; c >= 0) return grid_.Insert(id, c, 0, false);
        if (grid_.Has(focused)) return grid_.Add(id, focused);
        // none: the end that brings the strip's middle nearer in front (its left one when it leans
        // right; centred, the right one, however the sums round)
        grid_.AddAtEnd(id, last_.split >= 0 && tiling::WrapNear((last_.az0 + last_.az1) / 2, 0) > 1e-6);
    }
    // Not anchored any more (or gone). False if it wasn't.
    bool Unanchor(int id) {
        Forget(id);
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
        tiling::Layout l = params.fit ? tilefit::Fit(cols, params) : tiling::Arrange(cols, params, Split());
        if (l.split < 0) tiling::Turn(&l, pan_);   // (as you see it)
        return Without(grid_, id, tiling::DropAt(l, cols, az, el));
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
        moved_ = id, movedAz_ = az, recentre_ = false;   // (where you let it go wins)
        if (d.kind == tiling::Drop::Lift) {
            EndBatch();
            spots_[id].satellite = false;
            Lift(id, d.align);
            return;
        }
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
    }

    // Off the grid, and the gap it leaves filled (user, 2026-11-20: a PC window's panel opening or
    // closing, "have all of our displays adjust to fill in the gap"): a panel that stood in a stack
    // leaves its column short, so a neighbouring column's only panel that fits in its place (no
    // wider than the column, no taller than it was) goes up there, and that column closes up. Not
    // Unanchor's other callers (Restore puts it back). False if it wasn't anchored.
    bool Leave(int id) {
        const tilegrid::Where w = grid_.Find(id);
        const tiling::Placed *gone = PlacedOf(id);
        const bool stacked = !params.fit && w.ok() && gone && !gone->below && grid_.Cols()[size_t(w.col)].cells.size() > 1;
        const tiling::Placed was = gone ? *gone : tiling::Placed{};
        if (!Unanchor(id)) return false;
        if (!stacked) return true;
        double span = 0;
        for (const tiling::Placed &p : last_.tiles)
            if (p.col == w.col && p.id != id) span = std::max(span, p.az1 - p.az0);
        int best = -1;
        double bestArea = 0;
        for (const int c : {w.col - 1, w.col + 1}) {
            if (c < 0 || c >= int(grid_.Cols().size()) || grid_.Cols()[size_t(c)].cells.size() != 1) continue;
            const int other = grid_.Cols()[size_t(c)].cells.front().id;
            const tiling::Placed *q = PlacedOf(other);
            if (!q || !Pullable(other) || q->az1 - q->az0 > span + kFitSlackDeg || q->el1 - q->el0 > was.el1 - was.el0 + kFitSlackDeg) continue;
            const double area = (q->az1 - q->az0) * (q->el1 - q->el0);
            if (area > bestArea) best = c, bestArea = area;
        }
        if (best >= 0) slot_.Move(grid_, grid_.Cols()[size_t(best)].cells.front().id, [&] { return grid_.PullTop(best, w.col, 0); });
        return true;
    }
    int steam = -1;   // Steam's window (vrtiles.cpp): never pulled into another column

    // A tiling key on a tiled panel (a satellite: nothing). False when nothing changed.
    bool Press(int id, Key k) {
        if (!grid_.Has(id)) return false;
        return Changed(Pressed(id, k));
    }
    // Resized by its corner on the grid: `factor` times as big as its tile was.
    bool Resized(int id, double factor) {
        if (!grid_.Has(id) || !(factor > 0)) return false;
        return Changed(grid_.Scale(id, factor));
    }

private:
    // Meta+Ctrl+Right or Up: bigger; Left or Down: smaller (its natural shape kept).
    bool Pressed(int id, Key k) {
        moved_ = id, movedAz_ = NAN, recentre_ = false;   // (moved by a key: the others keep their places, not it)
        switch (k) {
            case Key::Left:
            case Key::Right: {
                // narrower than its column and not against that edge yet: it goes there first (user,
                // 2026-11-19: "a 600 pixel wide display that I can either set on the right, or the left")
                const int dir = k == Key::Left ? -1 : +1;
                if (const tiling::Placed *p = PlacedOf(id); p && p->narrow && p->lean != dir) return grid_.SetAlign(id, dir);
                return slot_.Move(grid_, id, [&] { return grid_.MoveSide(id, dir, slot_.Key()); });
            }
            case Key::Up: return grid_.MoveUpDown(id, -1);
            case Key::Down: return grid_.MoveUpDown(id, +1);
            case Key::Wider:
            case Key::Taller: return grid_.Scale(id, tilegrid::kStep);
            case Key::Narrower:
            case Key::Shorter: return grid_.Scale(id, 1 / tilegrid::kStep);
        }
        return false;
    }
    // The desk full (another column would take the strip past params.wallDeg): the column a new
    // panel `info` goes on top of (tilepack.h): where it adds the least height (beside others on its
    // top shelf), then the lowest, then the nearer in front, of those it fits under kColumnDeg and is
    // no wider than; -1 while there's room, before the grid has been laid out, or when none takes it
    // (a column of its own then).
    // A size in metres as degrees round you, at the wall.
    double Span(double m) const { return tiling::Deg(2 * std::atan2(m / 2, params.radius)); }
    int OnTopOf(const Info &info) const {
        if (grid_.Empty() || last_.tiles.empty() || fills_.size() != grid_.Cols().size()) return -1;
        const tilepack::Item it{-1, Span(info.widthM), Span(info.heightM)};
        if (last_.az1 - last_.az0 + params.gapDeg + it.wDeg <= params.wallDeg) return -1;
        std::map<int, double> front, under;   // (each column's nearest in front, and how far it hangs under the desk line)
        for (const tiling::Placed &p : last_.tiles) {
            const double off = std::fabs(tiling::WrapNear(p.azDeg, 0));
            if (!front.count(p.col) || off < front[p.col]) front[p.col] = off;
            under[p.col] = std::max(under[p.col], params.deskDeg - p.el0);
        }
        int best = -1;
        double bestAdd = 0;
        for (size_t c = 0; c < fills_.size(); ++c) {
            const tilepack::Fill &f = fills_[c];
            const double h = tilepack::Grown(f, it, params.gapDeg);
            if (h < 0 || !EyeFitsRound(params, under[int(c)], h + (under[int(c)] > 0 ? params.gapDeg + under[int(c)] : 0))) continue;
            const double add = h - f.height;
            const tilepack::Fill &b = best < 0 ? f : fills_[size_t(best)];
            const bool lower = f.height < b.height - 1e-6 || (f.height < b.height + 1e-6 && front[int(c)] < front[best]);
            if (best < 0 || add < bestAdd - 1e-9 || (add < bestAdd + 1e-9 && lower)) best = int(c), bestAdd = add;
        }
        return best;
    }
public:
    // Every tile packed into columns again (tilepack.h), sizes kept: a reset's (user, 2026-11-19:
    // "Tiling looks way broken ... There's a massive gap in the middle").
    template <typename InfoOf>
    void Repack(InfoOf info) {
        std::vector<tilepack::Item> items;
        std::map<int, double> scales;
        for (const tilegrid::Col &col : grid_.Cols())
            for (const tilegrid::Cell &cell : col.cells) {
                const Info i = absent.count(cell.id) ? Info{0, 0} : info(cell.id);   // (not shown: a column of its own)
                items.push_back({cell.id, Span(i.widthM * cell.scale), Span(i.heightM * cell.scale)});
                scales[cell.id] = cell.scale;
            }
        if (items.empty()) return;
        EndBatch();
        const std::vector<std::vector<int>> cols = tilepack::Pack(items, params.gapDeg, EyeFits, &params);
        std::map<int, tilepack::Item> byId;
        for (const tilepack::Item &it : items) byId[it.id] = it;
        grid_ = tilegrid::Grid{};
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
            for (int r = c.Base(); r >= 0; --r) deskUp.push_back({c.tiles[size_t(r)].id, Span(c.tiles[size_t(r)].widthM), Span(c.tiles[size_t(r)].heightM)});
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
        st.scale = cell.scale, st.below = col.Below(w.row), st.align = cell.align;
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
        Recentre();   // (as Anchor)
        Spot &s = spots_[id];
        s.stay = st.stay;
        if (st.kind == State::Kind::Sat) return grid_.Add(id, focused);   // (no satellites in tiled mode: on the grid)
        if (!st.Placed()) return grid_.Add(id, focused);
        grid_.Insert(id, st.col, st.row, st.alone, st.scale, false, st.below);
        grid_.SetAlign(id, st.align);
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
    static constexpr double kFitSlackDeg = 0.5;
    bool Pullable(int id) const { return id != steam && !absent.count(id); }
    // Let go over its own column, already its top: a neighbouring column's top panel in under it, the
    // one on side `side` first (none there that can move: nothing).
    void Lift(int id, int side) {
        const tilegrid::Where w = grid_.Find(id);
        if (!w.ok() || grid_.Cols()[size_t(w.col)].Below(w.row)) return;
        for (const int c : {w.col + (side < 0 ? -1 : 1), w.col + (side < 0 ? 1 : -1)}) {
            if (c < 0 || c >= int(grid_.Cols().size()) || !Pullable(grid_.Cols()[size_t(c)].cells.front().id)) continue;
            const int other = grid_.Cols()[size_t(c)].cells.front().id;
            slot_.Move(grid_, other, [&] { return grid_.PullTop(c, w.col, w.row + 1); });
            return;
        }
    }
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
};

}  // namespace dashtiles
