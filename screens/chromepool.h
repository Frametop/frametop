// Which panels have controls (the bar and its buttons, the resize tab) right now.
//
// SteamVR allows k_unMaxOverlayCount (128) overlays for every app together, SteamVR's own
// included, and a panel with controls of its own took nine of them: desktopless mode ran out
// at six PC window panels next to three display panels (2026-11-15). The controls only show
// while a laser, the 3D mouse, a hand or a drag is at a panel (vr.cpp's UpdateControls), so
// a few sets of them go round instead: a panel takes a set when something comes near and
// gives it back once its controls have faded. A panel in use (dragged, hovered, its layout
// carried) keeps its set; one whose controls are only fading gives it up to a panel that
// wants one; when every set is in use, the newcomer waits.
//
// The choosing only: vr.cpp makes the sets' overlays and moves them. Header only.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace chromepool {

constexpr int kSets = 4;   // two controllers, the 3D mouse and a hand at different panels

struct Panel {
    int screen = -1;
    bool busy = false;       // dragged, hovered, carried by its layout button: keeps its set
    bool wants = false;      // something is near it (or it's busy)
    bool fading = false;     // its controls are still showing, fading out
    int64_t nearAt = 0;      // when something last came near: the latest goes first
};

// A set changing hands: from a screen (-1: it was free) to a screen (-1: it's free now).
struct Move {
    int set, from, to;
    bool operator==(const Move &o) const { return set == o.set && from == o.from && to == o.to; }
};

class Pool {
public:
    Pool() { owner_.fill(-1); }

    int Owner(int set) const { return owner_[set]; }
    int SetOf(int screen) const {
        for (int k = 0; k < kSets; ++k)
            if (owner_[k] == screen) return k;
        return -1;
    }

    // A panel going away now (its overlays destroyed): its set, now free, or -1.
    int Release(int screen) {
        const int k = SetOf(screen);
        if (k >= 0) owner_[k] = -1;
        return screen < 0 ? -1 : k;
    }

    // One tick, from the panels that may have controls (the shown ones; any other gives its set
    // back). The moves in the order to make them: a set's old owner lets go before the new one takes it.
    std::vector<Move> Step(const std::vector<Panel> &panels) {
        std::map<int, const Panel *> by;
        for (const Panel &p : panels) by[p.screen] = &p;
        std::vector<Move> moves;
        auto give = [&](int k, int to) {
            moves.push_back({k, owner_[k], to});
            owner_[k] = to;
        };
        for (int k = 0; k < kSets; ++k) {
            if (owner_[k] < 0) continue;
            const auto it = by.find(owner_[k]);
            if (it == by.end() || (!it->second->busy && !it->second->wants && !it->second->fading)) give(k, -1);
        }
        std::vector<const Panel *> waiting;
        for (const Panel &p : panels)
            if ((p.busy || p.wants) && SetOf(p.screen) < 0) waiting.push_back(&p);
        std::stable_sort(waiting.begin(), waiting.end(), [](const Panel *a, const Panel *b) {
            return a->busy != b->busy ? a->busy : a->nearAt > b->nearAt;
        });
        for (const Panel *p : waiting) {
            int k = SetOf(-1);
            if (k < 0) {
                // one only fading out, the one longest since anything was near it
                for (int j = 0; j < kSets; ++j) {
                    const Panel *o = by.at(owner_[j]);
                    if (o->busy || o->wants) continue;
                    if (k < 0 || o->nearAt < by.at(owner_[k])->nearAt) k = j;
                }
            }
            if (k < 0) continue;   // every set is in use: it waits
            give(k, p->screen);
        }
        return moves;
    }

private:
    std::array<int, kSets> owner_;
};

}  // namespace chromepool
