// The dashboard tiles' order and sizes (tiling.h places them): columns left to right, each with its
// panels top to bottom (the last one on the desk), and each panel's size, times its natural one.
// What the tiling keys, a drag and the anchor button change (vr.cpp's dashboard tiling). Panels
// are their ft-screens numbers (0-based); it doesn't know what kind each is.
//
// The keys (user, 2026-11-16, like Omarchy's, rebindable; Meta+Shift+arrows since 2026-11-20, here
// called Meta+arrows): Meta+Left/Right moves the focused panel to the next column that way (alone in
// its column: the columns swap; with others: it joins the next one, or at the end, a new column of
// its own), Meta+Up/Down moves it within its column (alone in it: dashtiles.h's Board), and
// Meta+Ctrl+arrows make it bigger or smaller (dashtiles.h). A panel anchored gets a column of its
// own right of the focused one (or goes on top of one: dashtiles.h's Board).
//
// Steam's menu is a slot in the strip (tiling.h): a split is how many columns are left of it, one
// for each place the menu opens (kSplits: in front, as the Steam button opens it, and off to the
// right or the left, as the palm menu does; slotsplit.h's KeyOf), so a palm-menu open leaves
// the front one as it was. A column stays on its side of the slot shown: a new one next to the
// focused one goes on its side (of each split), a drop on the side it was let go, and
// Meta+Left/Right steps a panel across the slot (alone in its column: the column goes; with
// others: out into a column of its own) rather than past it. A swap of two columns round another
// place's slot moves them both across it: splits count columns, so one order can't keep them all.
// -1: not chosen yet (the tiling chooses one; Board keeps it, or chooses again while it's only
// a default); a grid emptied by Remove has none.
//
// A column has a base row, the one on the desk line (user, 2026-11-19: "displays go from the center
// row down or up"): the panels before it stand above it, the ones after it hang under the desk line.
// Meta+Down on the lowest panel, the base, hangs it under the one above it; Meta+Up on the top one,
// the base, stands it on the one under it. A panel narrower than its column can lean against its left
// or right edge (`align`; user: "a 600 pixel wide display that I can either set on the right, or the
// left of below the 1400"; 0: as the tiling packs the strip narrowest, tiling.h's Lean).
//
// A pinned panel is always its column's base (Pin; user, 2026-11-27: "We want the desk line to be the
// display that is in the main slot"): the centre display you chose. Every move keeps it on the desk
// line, so what's put under it hangs under it and what's put over it stands on it; it never rises
// or drops because of another panel. Meta+Up/Down on it does nothing, and one beside it crosses it.
//
// Header only, so its test runs anywhere.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace tilegrid {

constexpr double kStep = 1.1;            // one key press: 10% bigger or smaller
constexpr double kMinScale = 0.25, kMaxScale = 6;   // a panel's size, times its natural one
constexpr int kSplits = 3;            // a split round Steam's menu for each place it opens

struct Cell {
    int id = 0;
    double scale = 1;   // its size, times its natural one
    int align = 0;      // -1 against its column's left edge, +1 its right, 0 as the tiling chooses
    bool row = false;   // starts a shelf, though there's room on the one before (round a super wide display: tilewide.h)
};
struct Col {
    std::vector<Cell> cells;
    int base = 0;       // the row on the desk line: the ones after it hang under it
    bool Below(int row) const { return row > base; }
};

// Where a panel is: its column and row, or -1s.
struct Where {
    int col = -1, row = -1;
    bool ok() const { return col >= 0; }
};

class Grid {
public:
    const std::vector<Col> &Cols() const { return cols_; }
    bool Empty() const { return cols_.empty(); }

    Where Find(int id) const {
        for (size_t c = 0; c < cols_.size(); ++c)
            for (size_t r = 0; r < cols_[c].cells.size(); ++r)
                if (cols_[c].cells[r].id == id) return {int(c), int(r)};
        return {};
    }
    bool Has(int id) const { return Find(id).ok(); }

    // `id` always its column's base from now on (-1: none pinned).
    void Pin(int id) {
        pin_ = id;
        KeepPin();
    }
    int Pinned() const { return pin_; }

    // A new column of its own, right of `after`'s, on its side of each split (none, or not in the
    // grid: at the right end).
    void Add(int id, int after = -1) {
        if (Has(id)) return;
        const Where w = Find(after);
        if (!w.ok()) return AddAtEnd(id, false);
        InsertCol(w.col + 1, Col{{Cell{id, 1}}}, true);   // (at a split: `after` is left of it)
    }
    // A new column of its own at the left end or the right one.
    void AddAtEnd(int id, bool left) {
        if (Has(id)) return;
        InsertCol(left ? 0 : int(cols_.size()), Col{{Cell{id, 1}}}, left);
    }
    // Into column `col` at row `row` (clamped), or a new column at `col` when `newColumn` or no such
    // column (at the split: left of Steam's slot when `leftOfSlot`). `below`: hanging under the desk
    // line (its row clamped to those), else standing on it or above (a row past the base's: the new
    // base, the old one on it).
    void Insert(int id, int col, int row, bool newColumn, double scale = 1, bool leftOfSlot = false, bool below = false) {
        Take(id);   // (moved: the splits stay, even with the grid empty for a moment)
        col = std::clamp(col, 0, int(cols_.size()));
        if (newColumn || col == int(cols_.size())) {
            InsertCol(col, Col{{Cell{id, Sc(scale)}}}, leftOfSlot);
            return KeepPin();
        }
        Put(cols_[col], row, Cell{id, Sc(scale)}, below);
        KeepPin();
    }
    // Column `from`'s top panel into column `to` at `row`, on the desk line or above it (its column
    // goes when it was the last in it). False when there's no such move.
    bool PullTop(int from, int to, int row) {
        const int n = int(cols_.size());
        if (from == to || from < 0 || to < 0 || from >= n || to >= n || cols_[size_t(from)].cells.empty()) return false;
        Cell cell = cols_[size_t(from)].cells.front();
        cell.align = 0, cell.row = false;
        const bool gone = cols_[size_t(from)].cells.size() == 1;
        Take(cell.id);
        if (gone && from < to) --to;
        Put(cols_[size_t(to)], row, cell, false);
        KeepPin();
        return true;
    }
    // Panel `id` hung lowest under column `col`'s desk line, out of its own (which goes when it was
    // the last in it). False when there's no such move.
    bool HangUnder(int id, int col) {
        const Where w = Find(id);
        if (!w.ok() || id == pin_ || col == w.col || col < 0 || col >= int(cols_.size())) return false;   // (pinned: on the desk)
        Cell cell = cols_[size_t(w.col)].cells[size_t(w.row)];
        cell.align = 0, cell.row = false;
        const bool gone = cols_[size_t(w.col)].cells.size() == 1;
        Take(id);
        if (gone && w.col < col) --col;
        Put(cols_[size_t(col)], int(cols_[size_t(col)].cells.size()), cell, true);
        KeepPin();
        return true;
    }
    // Out of the grid (its column goes when it was the last in it; the splits with the last
    // panel). False if it wasn't in it.
    bool Remove(int id) {
        if (!Take(id)) return false;
        if (cols_.empty()) split_.fill(-1);
        return true;
    }

    // Meta+Left (-1) / Meta+Right (+1), `key` the split whose slot is in the strip (-1: none). False
    // when nothing changed.
    bool MoveSide(int id, int dir, int key = -1) {
        const Where w = Find(id);
        if (!w.ok() || dir == 0) return false;
        dir = dir < 0 ? -1 : 1;
        Col &col = cols_[w.col];
        const int next = w.col + dir;
        int none = -1;
        int &split = key >= 0 && key < kSplits ? split_[size_t(key)] : none;
        const bool across = split >= 0 && w.col == (dir > 0 ? split - 1 : split);
        if (col.cells.size() == 1) {
            if (across) return split -= dir, true;   // its column steps across the slot
            if (next < 0 || next >= int(cols_.size())) return false;
            std::swap(cols_[w.col], cols_[next]);
            return true;
        }
        Cell cell = col.cells[w.row];
        cell.align = 0, cell.row = false;   // (its new column's own way)
        const bool below = col.Below(w.row);
        const int fromBase = w.row - col.base;   // (rows from the desk line: negative above)
        Erase(col, w.row);
        if (across) {
            // across the slot: into a column of its own, the slot's nearest that side
            InsertCol(split, Col{{cell}}, dir < 0);
            return KeepPin(), true;
        }
        if (next < 0 || next >= int(cols_.size())) {
            // at the end: out into a column of its own that side
            InsertCol(next < 0 ? 0 : int(cols_.size()), Col{{cell}}, next < 0);
            return KeepPin(), true;
        }
        Col &into = cols_[next];   // (as far from the desk line, on the same side of it)
        Put(into, below ? into.base + fromBase : into.base + 1 + fromBase, cell, below);
        return KeepPin(), true;
    }
    // Meta+Up (-1) / Meta+Down (+1): one row up or down its column. Across the desk line it keeps its
    // place and the line moves: the one on it, down under it (the one above it takes the desk); the
    // first under it, back up onto it. The only one on the desk swaps with the one under it, and the
    // top one, with nothing above it but the desk, stands on the one under it. In the pinned one's
    // column the line stays with it: one next to it trades rows with it, so goes from standing on
    // it to hanging under it or back; it itself doesn't move.
    bool MoveUpDown(int id, int dir) {
        const Where w = Find(id);
        if (!w.ok() || dir == 0 || id == pin_) return false;   // (pinned: it stays on the desk)
        Col &col = cols_[w.col];
        auto &cells = col.cells;
        const int n = int(cells.size());
        if (Find(pin_).col == w.col) {   // (round the pinned one: across it, it staying on the desk)
            const int to = w.row + (dir < 0 ? -1 : 1);
            if (to < 0 || to >= n) return false;
            std::swap(cells[w.row], cells[to]);
            return KeepPin(), true;
        }
        if (dir > 0 && w.row == col.base && col.base > 0) return --col.base, true;
        if (dir < 0 && w.row == col.base + 1) return ++col.base, true;
        if (dir < 0 && w.row == 0 && col.base == 0 && n >= 2) return col.base = 1, true;
        const int to = w.row + (dir < 0 ? -1 : 1);
        if (to < 0 || to >= n) return false;
        std::swap(cells[w.row], cells[to]);
        return true;
    }
    // Which way it leans in its column (Cell::align). False when nothing changed.
    bool SetAlign(int id, int align) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        int &a = cols_[w.col].cells[w.row].align;
        align = std::clamp(align, -1, 1);
        if (a == align) return false;
        a = align;
        return true;
    }
    // Starts a shelf of its own or not (Cell::row).
    void SetRow(int id, bool on) {
        if (const Where w = Find(id); w.ok()) cols_[w.col].cells[w.row].row = on;
    }
    // Its size `factor` times. False when nothing changed.
    bool Scale(int id, double factor) {
        const Where w = Find(id);
        if (!w.ok() || !(factor > 0)) return false;
        double &sc = cols_[w.col].cells[w.row].scale;
        const double to = Sc(sc * factor);
        if (std::fabs(to - sc) < 1e-9) return false;
        sc = to;
        return true;
    }
    // Two panels trade places, each keeping its own size (the centre display taken by a drop onto
    // it: dashtiles.h's Board::TakeCentre). False when either isn't on the grid, or they're one.
    bool Swap(int a, int b) {
        const Where wa = Find(a), wb = Find(b);
        if (!wa.ok() || !wb.ok() || a == b) return false;
        Cell &ca = cols_[wa.col].cells[wa.row], &cb = cols_[wb.col].cells[wb.row];
        std::swap(ca.id, cb.id), std::swap(ca.scale, cb.scale);
        return KeepPin(), true;
    }
    // A panel's cell, or null.
    const Cell *CellOf(int id) const {
        const Where w = Find(id);
        return w.ok() ? &cols_[w.col].cells[w.row] : nullptr;
    }
    // The columns left of Steam's slot where the menu opens at `key` (-1: not chosen yet).
    int Split(int key) const { return key >= 0 && key < kSplits ? split_[size_t(key)] : -1; }
    void SetSplit(int key, int split) {
        if (key >= 0 && key < kSplits) split_[size_t(key)] = split < 0 || cols_.empty() ? -1 : std::min(split, int(cols_.size()));
    }
    // Another grid's splits, for these columns (RestoreAll's: its columns, then more).
    void KeepSplits(const Grid &from) {
        for (int k = 0; k < kSplits; ++k) SetSplit(k, from.Split(k));
    }

private:
    static double Sc(double s) { return std::isfinite(s) ? std::clamp(s, kMinScale, kMaxScale) : 1; }
    // Into a column at `row`: under the desk line (`below`) or on it and above, the base kept.
    static void Put(Col &c, int row, const Cell &cell, bool below) {
        const int n = int(c.cells.size());
        if (n == 0) return c.cells.push_back(cell), void(c.base = 0);
        row = below ? std::clamp(row, c.base + 1, n) : std::clamp(row, 0, c.base + 1);
        c.cells.insert(c.cells.begin() + row, cell);
        if (!below) ++c.base;   // (the old base under it moved down a row, or it's the new base)
    }
    // Out of a column at `row`, the base kept: the one above it takes the desk line when it was the
    // base (none above: the one under it).
    static void Erase(Col &c, int row) {
        c.cells.erase(c.cells.begin() + row);
        if (row <= c.base) c.base = std::max(0, c.base - 1);
    }
    // Out of its cell (its column too when it was the last in it), the splits counting columns.
    bool Take(int id) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        auto &cells = cols_[w.col].cells;
        Erase(cols_[w.col], w.row);
        if (cells.empty()) {
            cols_.erase(cols_.begin() + w.col);
            for (int &s : split_) s -= w.col < s;
        }
        KeepPin();
        return true;
    }
    // The pinned panel its column's base again (its column's others where they are: up or under it).
    void KeepPin() {
        if (const Where w = Find(pin_); w.ok()) cols_[size_t(w.col)].base = w.row;
    }
    // A new column at `at` (at a split: left of its slot when `left`).
    void InsertCol(int at, Col c, bool left) {
        cols_.insert(cols_.begin() + at, std::move(c));
        for (int &s : split_)
            if (s >= 0 && (at < s || (at == s && left))) ++s;
    }
    std::vector<Col> cols_;
    std::array<int, kSplits> split_ = {-1, -1, -1};
    int pin_ = -1;   // always its column's base (Pin; -1: none)
};

}  // namespace tilegrid
