// The dashboard tiles' order and sizes (tiling.h places them): columns left to right, each with its
// panels top to bottom, and each panel's share of its column's height. What the tiling keys, a
// drag and the anchor button change (vr.cpp's dashboard tiling). Panels are their ft-screens
// numbers (0-based); it doesn't know what kind each is.
//
// The keys (user, 2026-11-16, like Omarchy's, rebindable): Meta+Left/Right moves the focused panel
// to the next column that way (alone in its column: the columns swap; with others: it joins the
// next one, or at the end, a new column of its own), Meta+Up/Down moves it within its column,
// Meta+Ctrl+Left/Right makes its column narrower or wider, Meta+Ctrl+Up/Down its row taller or
// shorter. A panel anchored gets a column of its own right of the focused one.
//
// Steam's menu is a slot in the strip (tiling.h): a split is how many columns are left of it, one
// for each place the menu opens (kSplits: in front, as the Steam button opens it, and off to the
// right or the left, as the palm menu does; dashtiles.h's SplitKey), so a palm-menu open leaves
// the front one as it was. A column stays on its side of the slot shown: a new one next to the
// focused one goes on its side (of each split), a drop on the side it was let go, and
// Meta+Left/Right steps a panel across the slot (alone in its column: the column goes; with
// others: out into a column of its own) rather than past it. A swap of two columns round another
// place's slot moves them both across it: splits count columns, so one order can't keep them all.
// -1: not chosen yet (the tiling chooses one, Board keeps it); a grid emptied by Remove has none.
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

constexpr double kStep = 1.1;         // one key press: 10% wider, taller, narrower or shorter
constexpr double kMinWeight = 0.2, kMaxWeight = 5;
constexpr double kMinWidthDeg = 12, kMaxWidthDeg = 160;
constexpr double kMinScale = 0.3;     // a fixed panel at least this much of its row (never more than all of it)
constexpr int kSplits = 3;            // a split round Steam's menu for each place it opens

struct Cell {
    int id = 0;
    double weight = 1;
    double scale = 1;   // a fixed panel's size in its row (tiling::Tile::scale)
};
struct Col {
    std::vector<Cell> cells;
    double widthDeg = 0;   // 0: the default width
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

    // A new column of its own, right of `after`'s, on its side of each split (none, or not in the
    // grid: at the right end).
    void Add(int id, int after = -1, double widthDeg = 0) {
        if (Has(id)) return;
        const Where w = Find(after);
        if (!w.ok()) return AddAtEnd(id, false, widthDeg);
        InsertCol(w.col + 1, Col{{Cell{id, 1}}, widthDeg}, true);   // (at a split: `after` is left of it)
    }
    // A new column of its own at the left end or the right one.
    void AddAtEnd(int id, bool left, double widthDeg = 0) {
        if (Has(id)) return;
        InsertCol(left ? 0 : int(cols_.size()), Col{{Cell{id, 1}}, widthDeg}, left);
    }
    // Into column `col` at row `row` (clamped), or a new column at `col` when `newColumn` or no such
    // column (at the split: left of Steam's slot when `leftOfSlot`).
    void Insert(int id, int col, int row, bool newColumn, double weight = 1, double widthDeg = 0, double scale = 1,
                bool leftOfSlot = false) {
        Take(id);   // (moved: the splits stay, even with the grid empty for a moment)
        col = std::clamp(col, 0, int(cols_.size()));
        if (newColumn || col == int(cols_.size())) {
            InsertCol(col, Col{{Cell{id, Clamp(weight), Sc(scale)}}, widthDeg}, leftOfSlot);
            return;
        }
        auto &cells = cols_[col].cells;
        row = std::clamp(row, 0, int(cells.size()));
        cells.insert(cells.begin() + row, Cell{id, Clamp(weight), Sc(scale)});
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
        const Cell cell = col.cells[w.row];
        col.cells.erase(col.cells.begin() + w.row);
        if (across) {
            // across the slot: into a column of its own, the slot's nearest that side
            InsertCol(split, Col{{Cell{cell.id, 1, cell.scale}}, 0}, dir < 0);
            return true;
        }
        if (next < 0 || next >= int(cols_.size())) {
            // at the end: out into a column of its own that side
            InsertCol(next < 0 ? 0 : int(cols_.size()), Col{{Cell{cell.id, 1, cell.scale}}, 0}, next < 0);
            return true;
        }
        auto &into = cols_[next].cells;
        into.insert(into.begin() + std::min(w.row, int(into.size())), Cell{cell.id, 1, cell.scale});
        return true;
    }
    // Meta+Up (-1) / Meta+Down (+1): one row up or down its column.
    bool MoveUpDown(int id, int dir) {
        const Where w = Find(id);
        if (!w.ok() || dir == 0) return false;
        auto &cells = cols_[w.col].cells;
        const int to = w.row + (dir < 0 ? -1 : 1);
        if (to < 0 || to >= int(cells.size())) return false;
        std::swap(cells[w.row], cells[to]);
        return true;
    }
    // Its column `factor` times as wide, from `nowDeg` (its width as laid out).
    bool Widen(int id, double factor, double nowDeg) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        const double to = std::clamp(nowDeg * factor, kMinWidthDeg, kMaxWidthDeg);
        if (std::fabs(to - nowDeg) < 1e-6) return false;
        cols_[w.col].widthDeg = to;
        return true;
    }
    // Its row `factor` times as tall within its column (the others share the rest). False alone in it.
    bool Heighten(int id, double factor) {
        const Where w = Find(id);
        if (!w.ok() || cols_[w.col].cells.size() < 2) return false;
        double &wt = cols_[w.col].cells[w.row].weight;
        const double to = Clamp(wt * factor);
        if (std::fabs(to - wt) < 1e-9) return false;
        wt = to;
        return true;
    }

    // A fixed panel's size in its row `factor` times (at most all of it). False when nothing changed.
    bool Scale(int id, double factor) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        double &sc = cols_[w.col].cells[w.row].scale;
        const double to = std::clamp(sc * factor, kMinScale, 1.0);
        if (std::fabs(to - sc) < 1e-9) return false;
        sc = to;
        return true;
    }
    // A panel's cell, or null.
    const Cell *CellOf(int id) const {
        const Where w = Find(id);
        return w.ok() ? &cols_[w.col].cells[w.row] : nullptr;
    }
    // Its column's width as asked (0: the default).
    double WidthOf(int id) const {
        const Where w = Find(id);
        return w.ok() ? cols_[w.col].widthDeg : 0;
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
    static double Clamp(double w) { return std::clamp(w, kMinWeight, kMaxWeight); }
    static double Sc(double s) { return std::clamp(s, kMinScale, 1.0); }
    // Out of its cell (its column too when it was the last in it), the splits counting columns.
    bool Take(int id) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        auto &cells = cols_[w.col].cells;
        cells.erase(cells.begin() + w.row);
        if (cells.empty()) {
            cols_.erase(cols_.begin() + w.col);
            for (int &s : split_) s -= w.col < s;
        }
        return true;
    }
    // A new column at `at` (at a split: left of its slot when `left`).
    void InsertCol(int at, Col c, bool left) {
        cols_.insert(cols_.begin() + at, std::move(c));
        for (int &s : split_)
            if (s >= 0 && (at < s || (at == s && left))) ++s;
    }
    std::vector<Col> cols_;
    std::array<int, kSplits> split_ = {-1, -1, -1};
};

}  // namespace tilegrid
