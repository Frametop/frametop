// After a fingertip pressed a row of the grab bar's menu, or tapped its panel beside the menu to close
// it: that panel stays out of hand input until the tip has come back out in front of it (council,
// 2026-11-18: the same poke went on into the panel's content and clicked what was under the row).
// Pure (screens/tests/panelmenu-test.cpp, flat and curved); vrbarmenu.cpp gives it the tip each tick
// on the panel's surface (posemath::OnPanel), so a curved panel's sides, which come forward of its
// flat centre plane, aren't taken for "out in front".
#pragma once

#include <cmath>
#include <cstdint>

namespace handsoff {

class Hold {
public:
    void Start(int panel, int side, int64_t nowMs) { panel_ = panel, side_ = side, sinceMs_ = nowMs; }
    void Clear() { panel_ = -1; }
    // One tick. `uvd`: the tip on its panel ({u, v, d}: along the surface from its centre, up, out of
    // its front; metres), or null when the hand isn't seen (lost in the poke: still held, as it may
    // be through the panel yet). `halfW`, `halfH`: the panel's half size. `there`: the panel still is.
    void Step(const double *uvd, double halfW, double halfH, int64_t nowMs, bool there) {
        if (panel_ < 0) return;
        const bool out = uvd && (uvd[2] > kOut || std::fabs(uvd[0]) > halfW + kAside || std::fabs(uvd[1]) > halfH + kAside);
        if (out || !there || nowMs - sinceMs_ >= kMostMs) panel_ = -1;
    }
    bool Holds(int panel) const { return panel_ >= 0 && panel_ == panel; }
    int panel() const { return panel_; }
    int side() const { return side_; }

    static constexpr double kOut = 0.02;     // the tip this far in front of the panel's surface: it's out
    static constexpr double kAside = 0.05;   // ...or this far off its edges
    static constexpr int64_t kMostMs = 3000; // ...or held this long, whatever the tip does

private:
    int panel_ = -1, side_ = 0;
    int64_t sinceMs_ = 0;
};

}  // namespace handsoff
