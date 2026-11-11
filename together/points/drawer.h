// What draws the marks of where the others point. SteamVR does (dots.h); for the tests, nothing
// does, and what would be drawn is read from the program's `state`.
#pragma once

#include "marks.h"
#include "panelkeys.h"

namespace points {

class Drawer {
public:
    virtual ~Drawer() = default;
    // The mark of the person at `place` goes to `mark` on `panel`, and shows. False when it
    // can't now (no SteamVR, the panel isn't shown here): then it doesn't show.
    virtual bool Show(int place, const PanelKey &panel, const Mark &mark, double now) = 0;
    virtual void Hide(int place) = 0;
    // Every pass of the loop.
    virtual void Tick(double now) = 0;
};

// Draws nothing, and says it did.
class NoDrawer : public Drawer {
public:
    bool Show(int, const PanelKey &, const Mark &, double) override { return true; }
    void Hide(int) override {}
    void Tick(double) override {}
};

}  // namespace points
