// The tiling keys on the dashboard tiles' grid (Meta+Shift+arrows: move; Meta+Ctrl+arrows: size):
// what each does to it, asking the layout as it was last (tileview.h) what fits. The board
// (dashtiles.h) owns the grid and the strip's pan; these change only the grid and Steam's slot.
// Header only, no OpenVR, so its test (tests/dashtiles-test.cpp) runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilegrid.h"
#include "tileview.h"
#include "tilewide.h"
#include "tiling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace dashtiles {

// NudgeUp and NudgeDown move one tile up or down a degree, nothing else with it (dashtiles.h's
// Board::Press; user, 2026-11-23: "move a display up or down by degrees without it re-orienting the
// entire desktop").
enum class Key { Left, Right, Up, Down, Wider, Narrower, Taller, Shorter, NudgeUp, NudgeDown };
constexpr const char *kKeyNames[] = {"left", "right", "up", "down", "wider", "narrower", "taller", "shorter", "nudgeup", "nudgedown"};
inline bool ParseKey(const char *word, Key *k) {
    for (int i = 0; i < int(sizeof kKeyNames / sizeof *kKeyNames); ++i)
        if (std::strcmp(word, kKeyNames[i]) == 0) return *k = Key(i), true;
    return false;
}

namespace tilekeys {

constexpr double kLeanSeenDeg = 1;   // a shelf's lean that moves it less than this isn't seen


// Let go over its own column, already its top (or Meta+Shift+Up alone in its column, user
// 2026-11-20: "tile it up"): a neighbouring column's top panel in under it, the one on side `side`
// first, if it fits (none that can: nothing). It stays where it is; the one pulled in moves
// (*moved: the strip's pan keeps the others' places round it). False when nothing changed.
inline bool Lift(tilegrid::Grid &g, const View &v, int id, int side, int *moved) {
    const tilegrid::Where w = g.Find(id);
    if (!w.ok() || g.Cols()[size_t(w.col)].Below(w.row)) return false;
    for (const int dir : {side < 0 ? -1 : 1, side < 0 ? 1 : -1}) {
        const int c = v.Neighbour(w.col, dir);
        if (c < 0) continue;
        const int other = g.Cols()[size_t(c)].cells.front().id;
        if (!v.Pullable(other) || !v.FitsUnder(w.col, other)) continue;
        if (!g.PullTop(c, w.col, w.row + 1)) return false;
        return *moved = other, true;
    }
    return false;
}
// Meta+Shift+Down alone in its column ("tile it down"): it hangs under the neighbouring column,
// the left one first, lowest there (its own column goes, as a drop under that column does). Not
// under Steam's window or a column with nothing shown, nor across Steam's slot.
inline bool Hang(tilegrid::Grid &g, const View &v, int id) {
    const tilegrid::Where w = g.Find(id);
    if (!w.ok()) return false;
    for (const int dir : {-1, +1}) {
        const int c = v.Neighbour(w.col, dir);
        if (c < 0) continue;
        const tilegrid::Col &col = g.Cols()[size_t(c)];
        if (v.Pullable(col.cells[size_t(col.base)].id)) return g.HangUnder(id, c);
    }
    return false;
}
// Over or under a super wide display (tilewide.h; the display itself moves as any panel): Left and
// Right trade places with the panel beside it on its shelf (none there: as anywhere), Up and Down go
// to the shelf that way, past the display too; past the last one, a shelf of its own (alone on it
// already: nothing). `changed`: what it came to. False: not one of these.
inline bool RoundWide(tilegrid::Grid &g, const View &v, int id, Key k, bool *changed) {
    const tilegrid::Where w = g.Find(id);
    const tiling::Placed *p = v.PlacedOf(id);
    const int wideId = w.ok() ? v.WideId(w.col) : -1;
    const tiling::Placed *wide = v.PlacedOf(wideId);
    if (!p || !wide || wideId == id || !v.LaidOut(w.col)) return false;
    const auto shelves = tiling::WideShelves(v.last, w.col, -1);
    *changed = false;
    if (k == Key::Left || k == Key::Right) {
        const tiling::Placed *next = nullptr;
        for (const auto &s : shelves)
            for (const tiling::Placed *q : s)
                if (q->below == p->below && q->shelf == p->shelf && q->id != id && (q->azDeg > p->azDeg) == (k == Key::Right) &&
                    (!next || std::fabs(q->azDeg - p->azDeg) < std::fabs(next->azDeg - p->azDeg)))
                    next = q;
        return next && (*changed = g.Swap(id, next->id));
    }
    const bool up = k == Key::Up;
    const std::vector<const tiling::Placed *> *to = nullptr;   // (the nearest shelf that way, on its side of the display)
    const bool over = p->elDeg > wide->elDeg;
    for (const auto &s : shelves) {
        const double el = s[0]->elDeg;
        if ((s[0]->below == p->below && s[0]->shelf == p->shelf) || (el > p->elDeg) != up || (el > wide->elDeg) != over) continue;
        if (!to || std::fabs(el - p->elDeg) < std::fabs((*to)[0]->elDeg - p->elDeg)) to = &s;
    }
    const auto mate = [&](const tiling::Placed &q) { return q.col == p->col && q.id != id && q.below == p->below && q.shelf == p->shelf; };
    if (!to && over == up) {   // (the last one out: one further, if it leaves one there)
        if (std::none_of(v.last.tiles.begin(), v.last.tiles.end(), mate)) return true;
    }
    const double az = p->azDeg;
    tiling::WideSpot s;
    if (to) s = tiling::OnShelf(*to, az);
    else s = tiling::SpotOnWide(v.last, *wide, int(g.Cols()[size_t(w.col)].cells.size()), az,
                                over == up ? (up ? 1e9 : -1e9) : up ? wide->el1 : wide->el0, 0, id);
    if (w.row < s.row) --s.row;   // (it goes first)
    const double scale = g.CellOf(id)->scale;
    g.Insert(id, w.col, s.row, false, scale, false, s.below);
    g.SetAlign(id, s.align), g.SetRow(id, s.own), g.SetRow(s.before, false);
    return *changed = true;
}
// Into the next column, a super wide display's: over it (`up`) or under it, at the near end of the
// shelf that's there (`side` -1 or +1: that way only; 0: the left one first). False when there's
// none, or it doesn't fit.
inline bool OntoWide(tilegrid::Grid &g, const View &v, int id, bool up, int side = 0) {
    const tilegrid::Where w = g.Find(id);
    const tiling::Placed *p = v.PlacedOf(id);
    if (!w.ok() || !p) return false;
    const bool alone = v.Alone(id);
    for (const int dir : {-1, +1}) {
        if (side != 0 && dir != side) continue;
        const int c = v.Neighbour(w.col, dir);
        const tiling::Placed *wide = c < 0 || !v.LaidOut(c) ? nullptr : v.PlacedOf(v.WideId(c));
        if (!wide || !v.Pullable(wide->id) || p->az1 - p->az0 > wide->az1 - wide->az0 || !v.FitsRound(c, id, !up)) continue;
        const tiling::WideSpot s =
            tiling::SpotOnWide(v.last, *wide, int(g.Cols()[size_t(c)].cells.size()), dir < 0 ? 1e9 : -1e9, up ? wide->el1 : wide->el0, 0, id);
        const double scale = g.CellOf(id)->scale;
        g.Insert(id, alone && c > w.col ? c - 1 : c, s.row, false, scale, false, s.below);   // (alone: its own column goes)
        return g.SetAlign(id, 0), g.SetRow(id, false), true;
    }
    return false;
}
// Key `k` on tiled panel `id`. Meta+Ctrl+Right or Up: bigger; Left or Down: smaller (its natural
// shape kept). *moved: the panel the others keep their places round (it, or the one Lift pulled
// in). False when nothing changed.
inline bool Press(tilegrid::Grid &g, slotsplit::Policy &slot, const View &v, int id, Key k, int *moved) {
    *moved = id;
    bool changed = false;
    if (k <= Key::Down && RoundWide(g, v, id, k, &changed)) return changed;
    switch (k) {
        case Key::Left:
        case Key::Right: {
            // narrower than its column and not against that edge yet: it goes there first (user,
            // 2026-11-19: "a 600 pixel wide display that I can either set on the right, or the left").
            // As good as there already (its shelf fills the column): the key moves it on, not a press with
            // nothing to see (user, 2026-11-24: "it will sometimes require pressing it twice to move").
            const int dir = k == Key::Left ? -1 : +1;
            if (const tiling::Placed *p = v.PlacedOf(id); p && p->narrow && p->lean != dir && v.LeanRoom(*p, dir) > kLeanSeenDeg)
                return g.SetAlign(id, dir);
            if (const tiling::Placed *p = v.PlacedOf(id); p && !v.Alone(id) && OntoWide(g, v, id, !p->below, dir)) return true;
            return slot.Move(g, id, [&] { return g.MoveSide(id, dir, slot.Key()); });
        }
        case Key::Up: return g.MoveUpDown(id, -1) || (v.Alone(id) && (OntoWide(g, v, id, true) || Lift(g, v, id, -1, moved)));
        case Key::Down: return g.MoveUpDown(id, +1) || (v.Alone(id) && (OntoWide(g, v, id, false) || Hang(g, v, id)));
        case Key::Wider:
        case Key::Taller: return g.Scale(id, tilegrid::kStep);
        case Key::Narrower:
        case Key::Shorter: return g.Scale(id, 1 / tilegrid::kStep);
        case Key::NudgeUp:
        case Key::NudgeDown: return false;   // (not the grid's: Board::Press)
    }
    return false;
}

}  // namespace tilekeys
}  // namespace dashtiles
