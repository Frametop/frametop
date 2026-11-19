// After a fingertip pressed a row of the grab bar's menu, or tapped its panel beside the menu to close
// it: that panel stays out of hand input until the tip has come back out in front of it (council,
// 2026-11-18: the same poke went on into the panel's content and clicked what was under the row).
// Pure (screens/tests/panelmenu-test.cpp, flat and curved); vrbarmenu.cpp gives it the tip each tick in
// the panel's frame, and it measures it on the panel's surface (posemath::OnPanel), so a curved
// panel's sides, which come forward of its flat centre plane, aren't taken for "out in front".
#pragma once

#include "posemath.h"

#include <cmath>
#include <cstdint>

namespace handsoff {

class Hold {
public:
    // `curve`: the panel's then (posemath::OnPanel's; 0 flat). `lift`: how far in front of the panel's
    // surface the tip pressed (the menu's posemath::CardLift; 0 beside it): out is kOut past that
    // (council, 2026-11-19: a tight curve lifts the menu past kOut, and the press itself counted as out).
    void Start(int panel, int side, double curve, int64_t nowMs, double lift = 0) {
        panel_ = panel, side_ = side, curve_ = curve, sinceMs_ = nowMs, lift_ = lift;
    }
    void Clear() { panel_ = -1; }
    // One tick. `q`: the tip in the panel's frame (x right, y up, z out of its front, metres), or null
    // when it isn't seen (the hand lost in the poke, or the panel's pose this tick: still held, as it
    // may be through the panel yet). `curve`: the panel's now. `halfW`, `halfH`: its half size. `there`:
    // the panel still is. Out in front counts on the surface it had then and the one it has now (the
    // row pressed may have been Curved: flattened, a curved panel's side drops back behind the tip).
    void Step(const double *q, double curve, double halfW, double halfH, int64_t nowMs, bool there) {
        if (panel_ < 0) return;
        bool out = false;
        if (q) {
            double then[3], now[3];
            posemath::OnPanel(curve_, q, then);
            posemath::OnPanel(curve, q, now);
            out = std::fmin(then[2], now[2]) > kOut + lift_ || std::fabs(now[0]) > halfW + kAside || std::fabs(now[1]) > halfH + kAside;
        }
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
    double curve_ = 0, lift_ = 0;
    int64_t sinceMs_ = 0;
};

}  // namespace handsoff
