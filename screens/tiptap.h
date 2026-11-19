// A fingertip tapping a small flat target of ours that hand input doesn't see as a panel (the reset
// question, a row of the grab bar's menu): the index tip coming through its face from in front, within
// its edges. Pure (screens/tests/tiptap-test.cpp); vr.cpp and vrbarmenu.cpp give it the tip each hands
// frame in the target's own frame.
#pragma once

#include <cmath>

namespace tiptap {

class Tap {
public:
    // One hands frame. `l`: the tip in the target's frame (x right, y up, z out of its face, metres), or
    // null with no hand. `halfW`, `halfH`: the target's half size. True as the tip comes through.
    bool Step(const double *l, double halfW, double halfH) {
        double z = 1;   // (far, or off it)
        if (l && std::fabs(l[0]) <= halfW + kMargin && std::fabs(l[1]) <= halfH + kMargin && l[2] > -kBehind) z = l[2];
        // in front of it (not already through, not far off) last frame, at or past its face now
        const bool through = z_ > kFront && z_ < kReach && z <= kFace;
        z_ = z;
        return through;
    }
    // Shown afresh: a tip already through it doesn't count until it's been out in front.
    void Reset() { z_ = 1; }
    // The tip near its face (lit under it), this frame.
    bool Near() const { return z_ < kReach; }

    static constexpr double kMargin = 0.01;   // past its edges, still on it
    static constexpr double kBehind = 0.03;   // behind it farther: off it
    static constexpr double kFront = 0.008;   // last frame at least this far in front
    static constexpr double kReach = 0.08;    // ...and no farther
    static constexpr double kFace = 0.003;    // this frame within this of its face, or through

private:
    double z_ = 1;
};

}  // namespace tiptap
