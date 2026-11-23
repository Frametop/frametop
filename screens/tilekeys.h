// The tiling keys on the dashboard tiles' grid (Meta+Shift+arrows: move; Meta+Ctrl+arrows: size):
// what each does to it, asking the layout as it was last (tileview.h) what fits. The board
// (dashtiles.h) owns the grid and the strip's pan; these change only the grid and Steam's slot.
// Header only, no OpenVR, so its test (tests/dashtiles-test.cpp) runs anywhere.
#pragma once

#include "slotsplit.h"
#include "tilegrid.h"
#include "tileview.h"
#include "tiling.h"

#include <cstring>

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
// Key `k` on tiled panel `id`. Meta+Ctrl+Right or Up: bigger; Left or Down: smaller (its natural
// shape kept). *moved: the panel the others keep their places round (it, or the one Lift pulled
// in). False when nothing changed.
inline bool Press(tilegrid::Grid &g, slotsplit::Policy &slot, const View &v, int id, Key k, int *moved) {
    *moved = id;
    switch (k) {
        case Key::Left:
        case Key::Right: {
            // narrower than its column and not against that edge yet: it goes there first (user,
            // 2026-11-19: "a 600 pixel wide display that I can either set on the right, or the left")
            const int dir = k == Key::Left ? -1 : +1;
            if (const tiling::Placed *p = v.PlacedOf(id); p && p->narrow && p->lean != dir) return g.SetAlign(id, dir);
            return slot.Move(g, id, [&] { return g.MoveSide(id, dir, slot.Key()); });
        }
        case Key::Up: return g.MoveUpDown(id, -1) || (v.Alone(id) && Lift(g, v, id, -1, moved));
        case Key::Down: return g.MoveUpDown(id, +1) || (v.Alone(id) && Hang(g, v, id));
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
