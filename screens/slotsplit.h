// Which of the grid's splits round Steam's menu is in use, and when one is chosen again
// (dashtiles.h's Board lays the tiles out with it; tilegrid.h keeps the splits themselves).
//
// The grid keeps a split for each place the menu opens (KeyOf: in front, as the Steam button
// opens it, or off to the right or the left, as the palm menu does). Until the strip centred in
// front of you reaches the menu, it stays centred and has no slot. Once it does, the slot stays
// for this open, with the split for where the menu opened: the one kept from an earlier open
// there, else the tiling's choice, kept from then on. A split you set (a drop or Meta+Left/Right
// that took a tiled panel across the menu) is never replaced (council, 2026-11-16). One only
// chosen for you is looked at again when the dashboard opens: chosen again when a fresh choice
// puts the strip's middle more than kResplitDeg nearer in front (new panels all went one side,
// or some went) with the menu where it is now and where it was the open before, both. So a menu
// that opens one place, then another, and back never moves a panel across it, and one that has
// moved for good (or a strip that has) is followed from the second open there (council,
// 2026-11-17).
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include "tilegrid.h"
#include "tiling.h"

#include <array>
#include <cmath>
#include <vector>

namespace slotsplit {

constexpr double kFrontDeg = 45;     // a menu this near in front: the Steam button's
constexpr double kResplitDeg = 30;   // a split only chosen for you: chosen again this much nearer in front

// Which of the grid's splits a menu `azDeg` round opens with: 0 in front, 1 right, 2 left.
inline int KeyOf(double azDeg) {
    azDeg = tiling::WrapNear(azDeg, 0);
    return std::fabs(azDeg) < kFrontDeg ? 0 : azDeg > 0 ? 1 : 2;
}
// How far a strip's middle is from in front of you, in degrees.
inline double Off(const tiling::Layout &l) { return std::fabs(tiling::WrapNear((l.az0 + l.az1) / 2, 0)); }

class Policy {
public:
    // Steam's menu opened at `azDeg` (the dashboard opened again).
    void Moved(double azDeg) { key_ = KeyOf(azDeg), inUse_ = false; }
    // The split key in use now (-1: no slot in the strip).
    int Key() const { return inUse_ ? key_ : -1; }
    // The split in use now (-1: no slot in the strip, or not chosen yet).
    int Split(const tilegrid::Grid &g) const { return inUse_ ? g.Split(key_) : -1; }

    // `cols` (the grid's) laid out with `p`, the split chosen, chosen again or kept in `g`.
    tiling::Layout Arrange(tilegrid::Grid &g, const std::vector<tiling::Column> &cols, const tiling::Params &p) {
        const size_t k = size_t(key_);
        if (!inUse_) {
            const tiling::Layout probe = tiling::Arrange(cols, p);
            if (probe.split < 0) return probe;
            inUse_ = true;
            const int kept = g.Split(key_);
            if (kept >= 0 && !userSet_[k] && probe.split != kept) {
                tiling::Params before = p;
                before.reserveAzDeg = lastAz_[k];
                if (Nearer(cols, p, kept, probe.split) && Nearer(cols, before, kept, probe.split)) g.SetSplit(key_, probe.split);
            }
            lastAz_[k] = p.reserveAzDeg;
        }
        tiling::Layout l = tiling::Arrange(cols, p, Split(g));
        if (l.split >= 0 && Split(g) < 0) g.SetSplit(key_, l.split), userSet_[k] = false;
        return l;
    }
    // Panel `id` moved by `move` (its result returned): a move across the slot makes the split yours.
    template <typename F>
    bool Move(tilegrid::Grid &g, int id, F move) {
        const int was = SideOf(g, id);
        const bool r = move();
        const int now = SideOf(g, id);
        if (was != 0 && now != 0 && now != was) userSet_[size_t(key_)] = true;   // (back from a satellite: a new one)
        return r;
    }

private:
    // Split `to` puts the strip's middle more than kResplitDeg nearer in front than `from`, with `p`.
    static bool Nearer(const std::vector<tiling::Column> &cols, const tiling::Params &p, int from, int to) {
        return Off(tiling::Arrange(cols, p, from)) > Off(tiling::Arrange(cols, p, to)) + kResplitDeg;
    }
    // Which side of the slot in use a panel is on (-1 left, +1 right; 0: no slot, or not tiled).
    int SideOf(const tilegrid::Grid &g, int id) const {
        const tilegrid::Where w = g.Find(id);
        const int s = Split(g);
        return !w.ok() || s < 0 ? 0 : w.col < s ? -1 : 1;
    }

    int key_ = 0;           // KeyOf Steam's menu now
    bool inUse_ = false;    // the slot is in the strip, this open
    std::array<bool, tilegrid::kSplits> userSet_{};      // each split, set by you (not just chosen for you)
    std::array<double, tilegrid::kSplits> lastAz_{};   // where Steam's menu was the open before, at each
    // (A split's value is the grid's, its flags these: a split only goes from none to one in
    // Arrange, which clears userSet_, or in Board::RestoreAll's KeepSplits, which copies the same
    // board's. Splits put back from somewhere else would need their flags too.)
};

}  // namespace slotsplit
