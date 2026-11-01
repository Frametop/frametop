// A head pin's floor frame (SCREEN_PIN_LEVEL, vr.cpp FloorFrame). Mode 1, Level: on the floor under the head,
// upright, turned with the head. The turn is the head's forward laid flat: looking up or down only
// shortens it, and tilting the head doesn't change it. Looking nearly straight up or down, where
// forward has no direction left, a little of the head's right vector (turned to forward) takes
// over smoothly, so the frame never jumps. Mode 2, Walk (DEE-208: "a ball that follows you around
// and you're in the middle of it but it does not rotate, just move"): on the floor under the head,
// with the room's axes, so a pinned screen follows you walking but not turning, looking or crouching.
// Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include <cmath>
#include <cstring>

namespace levelpin {

// SCREEN_PIN_LEVEL: how a screen pinned to the head follows it. Rigid rides on the headset (SteamVR's own pin).
enum Mode : int { Rigid = 0, Level = 1, Walk = 2 };

// The setting's value: "1" Level, "2" Walk, anything else Rigid (the default, and what "0" was).
inline Mode ModeOf(const char *v) {
    if (!v) return Rigid;
    if (!std::strcmp(v, "1") || !std::strcmp(v, "on")) return Level;
    if (!std::strcmp(v, "2") || !std::strcmp(v, "walk")) return Walk;
    return Rigid;
}
inline const char *ModeName(Mode m) { return m == Level ? "level" : m == Walk ? "walking" : "rigid"; }

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

// A walking pin's frame: the head's floor position (x, 0, z) with the room's axes. Its height and
// turn stay out, so the pin's offset keeps the screen's height and facing.
inline bool WalkFrame(const float head[3][4], float out[3][4]) {
    const float m[3][4] = {{1, 0, 0, head[0][3]}, {0, 1, 0, 0}, {0, 0, 1, head[2][3]}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[r][c];
    return true;
}

// The frame for `mode` (Level or Walk); false for Rigid, which has none (it rides on the headset).
inline bool FloorFrame(Mode mode, const float head[3][4], float out[3][4]) {
    return mode == Level ? Frame(head, out) : mode == Walk ? WalkFrame(head, out) : false;
}

}  // namespace levelpin
