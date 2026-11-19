// A fingertip tapping a small flat target of ours that hand input doesn't see as a panel (the reset
// question, a row of the grab bar's menu): the index tip coming through its face from in front, within
// its edges. Pure (screens/tests/panelmenu-test.cpp, at poke speeds from a slow 5 cm/s to 2 m/s, at hand
// tracking's 15 and 30 frames a second); vr.cpp and vrbarmenu.cpp give it the tip each hands frame in the
// target's own frame.
#pragma once

#include <cmath>

namespace tiptap {

class Tap {
public:
    // One hands frame. `l`: the tip in the target's frame (x right, y up, z out of its face, metres), or
    // null with no hand. `halfW`, `halfH`: the target's half size. True as the tip comes through: it was
    // seen in front of it (within kArm, past kFront) since it last came through, and it's at its face or
    // past it now, however many frames it took (a slow, careful poke) and up to kDeep past it in one
    // (a quick one).
    bool Step(const double *l, double halfW, double halfH) {
        if (!l || std::fabs(l[0]) > halfW + kMargin || std::fabs(l[1]) > halfH + kMargin || l[2] > kArm) {
            armed_ = false, z_ = 1;   // (off it, or far in front)
            return false;
        }
        const double z = l[2];
        z_ = z < -kBehind ? 1 : z;
        if (z < -kDeep) {   // (a jump in tracking, or a hand reaching through it: not a tap)
            armed_ = false;
            return false;
        }
        if (z <= kFace) {
            const bool through = armed_;
            armed_ = false;
            return through;
        }
        if (z > kFront) armed_ = true;
        return false;
    }
    // Shown afresh: a tip already through it doesn't count until it's been out in front.
    void Reset() { armed_ = false, z_ = 1; }
    // The tip near its face (lit under it), this frame.
    bool Near() const { return z_ < kNear; }

    static constexpr double kMargin = 0.01;   // past its edges, still on it
    static constexpr double kArm = 0.15;      // in front of it no farther than this, it's on its way in
    static constexpr double kNear = 0.08;     // ...and this near, it lights what it's in front of
    static constexpr double kFront = 0.008;   // in front of it at least this far: out (and on its way in again)
    static constexpr double kFace = 0.003;    // within this of its face, or through: a tap
    static constexpr double kBehind = 0.03;   // behind it farther: not near it
    static constexpr double kDeep = 0.18;     // ...and farther than this in one frame: no tap (2 m/s at 15 Hz is 13 cm)

private:
    bool armed_ = false;
    double z_ = 1;
};

}  // namespace tiptap
