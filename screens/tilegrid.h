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
// Header only, so its test runs anywhere.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tilegrid {

constexpr double kStep = 1.1;         // one key press: 10% wider, taller, narrower or shorter
constexpr double kMinWeight = 0.2, kMaxWeight = 5;
constexpr double kMinWidthDeg = 12, kMaxWidthDeg = 160;
constexpr double kMinScale = 0.3;     // a fixed panel at least this much of its row (never more than all of it)

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

    // A new column of its own, right of `after`'s (none, or not in the grid: at the right end).
    void Add(int id, int after = -1, double widthDeg = 0) {
        if (Has(id)) return;
        const Where w = Find(after);
        const int at = w.ok() ? w.col + 1 : int(cols_.size());
        cols_.insert(cols_.begin() + at, Col{{Cell{id, 1}}, widthDeg});
    }
    // Into column `col` at row `row` (clamped), or a new column at `col` when `newColumn` or no such column.
    void Insert(int id, int col, int row, bool newColumn, double weight = 1, double widthDeg = 0, double scale = 1) {
        if (Has(id)) Remove(id);
        col = std::clamp(col, 0, int(cols_.size()));
        if (newColumn || col == int(cols_.size())) {
            cols_.insert(cols_.begin() + col, Col{{Cell{id, Clamp(weight), Sc(scale)}}, widthDeg});
            return;
        }
        auto &cells = cols_[col].cells;
        row = std::clamp(row, 0, int(cells.size()));
        cells.insert(cells.begin() + row, Cell{id, Clamp(weight), Sc(scale)});
    }
    // Out of the grid (its column goes when it was the last in it). False if it wasn't in it.
    bool Remove(int id) {
        const Where w = Find(id);
        if (!w.ok()) return false;
        auto &cells = cols_[w.col].cells;
        cells.erase(cells.begin() + w.row);
        if (cells.empty()) cols_.erase(cols_.begin() + w.col);
        return true;
    }

    // Meta+Left (-1) / Meta+Right (+1). False when nothing changed.
    bool MoveSide(int id, int dir) {
        const Where w = Find(id);
        if (!w.ok() || dir == 0) return false;
        dir = dir < 0 ? -1 : 1;
        Col &col = cols_[w.col];
        const int next = w.col + dir;
        if (col.cells.size() == 1) {
            if (next < 0 || next >= int(cols_.size())) return false;
            std::swap(cols_[w.col], cols_[next]);
            return true;
        }
        const Cell cell = col.cells[w.row];
        col.cells.erase(col.cells.begin() + w.row);
        if (next < 0 || next >= int(cols_.size())) {
            // at the end: out into a column of its own that side
            cols_.insert(cols_.begin() + (next < 0 ? 0 : int(cols_.size())), Col{{Cell{cell.id, 1, cell.scale}}, 0});
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

private:
    static double Clamp(double w) { return std::clamp(w, kMinWeight, kMaxWeight); }
    static double Sc(double s) { return std::clamp(s, kMinScale, 1.0); }
    std::vector<Col> cols_;
};

}  // namespace tilegrid
