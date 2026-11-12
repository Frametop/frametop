// What draws the marks of where the others point, and of where they look. SteamVR does
// (dots.h); for the tests, nothing does, and what would be drawn is read from the program's
// `state`.
#pragma once

#include "marks.h"
#include "panelkeys.h"

namespace points {

// A person has two marks: where they point, and where they look.
enum Which { kPointing = 0, kLooking = 1 };

class Drawer {
public:
    virtual ~Drawer() = default;
    // That mark of the person at `place` goes to `mark` on `panel`, and shows. False when it
    // can't now (no SteamVR, the panel isn't shown here): then it doesn't show.
    virtual bool Show(Which which, int place, const PanelKey &panel, const Mark &mark, double now) = 0;
    virtual void Hide(Which which, int place) = 0;
    // Every pass of the loop.
    virtual void Tick(double now) = 0;
};

// Draws nothing, and says it did.
class NoDrawer : public Drawer {
public:
    bool Show(Which, int, const PanelKey &, const Mark &, double) override { return true; }
    void Hide(Which, int) override {}
    void Tick(double) override {}
};

}  // namespace points
