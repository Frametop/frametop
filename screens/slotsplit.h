// Which of the grid's splits round Steam's menu is in use, and when one is chosen again
// (dashtiles.h's Board lays the tiles out with it; tilegrid.h keeps the splits themselves).
//
// The grid keeps a split for each place the menu opens (KeyOf: in front, as the Steam button
// opens it, or off to the right or the left, as the palm menu does). Until the strip centred in
// front of you reaches the menu, it stays centred and has no slot. Once it does, the slot stays
// for this open, with the split for where the menu opened: the one kept from an earlier open
// there, else the tiling's choice, kept from then on. A split you set (a drop or Meta+Left/Right
// that took a panel across the menu, or onto the grid) is never replaced (council, 2026-11-16).
// One only chosen for you is looked at again when the dashboard opens: chosen again when a fresh
// choice puts the strip's middle more than kResplitDeg nearer in front (new panels all went one
// side, or some went). Both are laid out round the menu where the split was chosen, not where it
// is now, so where the menu happens to open never moves a panel across it.
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
            tiling::Layout probe = tiling::Arrange(cols, p);
            if (probe.split < 0) return probe;
            inUse_ = true;
            const int kept = g.Split(key_);
            if (kept >= 0 && !userSet_[k]) {
                tiling::Params then = p;
                then.reserveAzDeg = chosenAz_[k];
                const tiling::Layout fresh = tiling::Arrange(cols, then);
                if (fresh.split >= 0 && Off(tiling::Arrange(cols, then, kept)) > Off(fresh) + kResplitDeg)
                    g.SetSplit(key_, fresh.split);
            }
        }
        tiling::Layout l = tiling::Arrange(cols, p, Split(g));
        if (l.split >= 0 && Split(g) < 0) g.SetSplit(key_, l.split), userSet_[k] = false, chosenAz_[k] = p.reserveAzDeg;
        return l;
    }

    // Which side of the slot in use a panel is on (-1 left, +1 right; 0: no slot, or not tiled).
    int SideOf(const tilegrid::Grid &g, int id) const {
        const tilegrid::Where w = g.Find(id);
        const int s = Split(g);
        return !w.ok() || s < 0 ? 0 : w.col < s ? -1 : 1;
    }
    // A panel moved from side `was`: across the slot (or onto the grid), its split is yours now.
    void Crossed(const tilegrid::Grid &g, int id, int was) {
        const int now = SideOf(g, id);
        if (now != 0 && now != was) userSet_[size_t(key_)] = true;
    }

private:
    int key_ = 0;           // KeyOf Steam's menu now
    bool inUse_ = false;    // the slot is in the strip, this open
    std::array<bool, tilegrid::kSplits> userSet_{};      // each split, set by you (not just chosen for you)
    std::array<double, tilegrid::kSplits> chosenAz_{};   // where Steam's menu was when each was chosen
};

}  // namespace slotsplit
