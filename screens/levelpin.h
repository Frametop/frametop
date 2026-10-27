// A level head pin's frame (SCREEN_PIN_LEVEL, vr.cpp LevelFrame): on the floor under the head,
// upright, turned with the head. The turn is the head's forward laid flat: looking up or down only
// shortens it, and tilting the head doesn't change it. Looking nearly straight up or down, where
// forward has no direction left, a little of the head's right vector (turned to forward) takes
// over smoothly, so the frame never jumps. Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include <cmath>

namespace levelpin {

constexpr double kRightWeight = 0.1;   // how much the right vector counts next to forward (length 1 when level)

// head: a 3x4 pose (HmdMatrix34_t's layout); out: the level frame, same layout. False if the
// head's pose gives no direction (it can't, for a real rotation).
inline bool Frame(const float head[3][4], float out[3][4]) {
    // forward is -Z; the right vector (X) turned to forward is (X.z, -X.x)
    const double fx = -head[0][2] + kRightWeight * head[2][0], fz = -head[2][2] - kRightWeight * head[0][0];
    const double n = std::hypot(fx, fz);
    if (n < 1e-6) return false;
    const double x = -fz / n, z = fx / n;   // the level right vector
    const float m[3][4] = {{float(x), 0, float(-z), head[0][3]},   // right, up, back (right x up)
                           {0, 1, 0, 0},
                           {float(z), 0, float(x), head[2][3]}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return true;
}

}  // namespace levelpin
