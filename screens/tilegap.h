// The gap a tiled panel leaves in a stack when it goes (dashtiles.h's Board::Leave and FillGap; user,
// 2026-11-20: a PC window's panel opening or closing, "have all of our displays adjust to fill in
// the gap"). The gap is filled once nothing else has gone for kQuietMs, so a desktop stopping or
// restarting, which takes its panels one by one, fills none and leaves no shuffled layout to be
// saved. A fill is undone when the panel that left comes back while the stack is as the fill left
// it, so it gets its place back instead of landing on another column.
//
// Header only, so its test runs anywhere.
#pragma once

#include "tilegrid.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tilegap {

constexpr int64_t kQuietMs = 1500;   // nothing else gone for this long: the gap is filled

struct Gap {
    int gone = -1;          // the panel that went
    std::string key;        // ...its app or display (vrtiles.cpp's tile memory key; panel numbers are reused)
    int stayed = -1;        // one still standing in its stack (-1: it left no gap)
    double heightDeg = 0;   // how tall it was, unshrunk (tiling::Layout::shrink)
    std::vector<int> stack; // the panels left standing in its stack (another there by the fill: it took the gap)
};
// A neighbouring column's only panel pulled up into the gap app `key` left, on top of `stayed`'s stack.
struct Fill {
    std::string key;
    int pulled = -1, stayed = -1;
    int side = 0;   // where `pulled` came from: -1 the left, +1 the right
};

class Book {
public:
    // Panel `g.gone` went at `nowMs`.
    void Went(const Gap &g, int64_t nowMs) {
        if (went_ > 0 && nowMs - at_ >= kQuietMs) Cancel();   // (the last went long enough ago: Due not asked meanwhile)
        if (!g.key.empty()) fills_.erase(g.key);
        ++went_, last_ = g, at_ = nowMs;
    }
    // The gap to fill now: the only panel that went since the last one was due, once kQuietMs passed
    // with nothing else going. None (gone -1) before then, and when more than one went (they're
    // forgotten: a desktop stopping or restarting).
    Gap Due(int64_t nowMs) {
        if (went_ == 0 || nowMs - at_ < kQuietMs) return {};
        const Gap g = went_ == 1 ? last_ : Gap{};
        Cancel();
        return g;
    }
    // App `key` back, on panel `id`: the gap it left isn't filled, and its fill is taken to be undone
    // (none: an empty key). By panel number only when neither has a key (panel numbers are reused).
    Fill Back(int id, const std::string &key) {
        if ((key.empty() && last_.key.empty()) ? last_.gone == id : (!key.empty() && last_.key == key)) Void();
        const auto it = key.empty() ? fills_.end() : fills_.find(key);
        if (it == fills_.end()) return {};
        const Fill f = it->second;
        fills_.erase(it);
        return f;
    }
    void Filled(const Fill &f) {
        if (!f.key.empty()) fills_[f.key] = f;
    }
    // Panel `id` gone or off the grid: no fill it's in is undone, and no gap it stands in is filled.
    void Forget(int id) {
        if (last_.stayed == id) Void();
        for (auto it = fills_.begin(); it != fills_.end();)
            it = it->second.pulled == id || it->second.stayed == id ? fills_.erase(it) : std::next(it);
    }
    // The grid changed by hand (a drop, a key, a resize): the gap waiting isn't filled (Undo checks a
    // fill is still as it was left).
    void Changed() { Void(); }
    // A profile put back: nothing waits, nothing is undone.
    void Clear() { Cancel(), fills_.clear(); }

private:
    // The gap waiting isn't filled, but the panels gone still count: one more going within
    // kQuietMs is still more than one (council, 2026-11-20: Leave's own Unanchor forgets the panel
    // the gap stood on). Cancel: the count starts again.
    void Void() { last_ = {}; }
    void Cancel() { went_ = 0, last_ = {}; }
    int went_ = 0;
    int64_t at_ = 0;
    Gap last_;
    std::map<std::string, Fill> fills_;   // by the app that went
};

// Fill `f` undone on `g`: f.pulled out of f.stayed's stack, into a column of its own on the side it
// came from (`leftOfSlot`: as Grid::Insert's). False, and nothing changed, unless f.pulled is still
// on top of f.stayed's stack.
inline bool Undo(tilegrid::Grid &g, const Fill &f, bool leftOfSlot) {
    const tilegrid::Where p = g.Find(f.pulled), s = g.Find(f.stayed);
    if (!p.ok() || !s.ok() || p.col != s.col || p.row != 0) return false;
    const double scale = g.CellOf(f.pulled)->scale;
    g.Insert(f.pulled, f.side < 0 ? p.col : p.col + 1, 0, true, scale, leftOfSlot);
    return true;
}

}  // namespace tilegap
