// A dashboard pin's frame (vr.cpp UpdateDashAnchor): where Steam's bar under the SteamVR dashboard
// (valve.steam.gamepadui.bar) is, from reading its absolute transform every tick. OpenVR won't let
// another process read the dashboard's own transform (a DashboardTab overlay: WrongTransformType),
// but the bar is an Absolute overlay Steam places rigidly with the dashboard: dragged, closed and
// opened again in front of you, it goes with it (probed on the Frame 2026-11-16, docs/design.md).
// Steam draws the bar scaled (its axes about 0.369 long on SteamOS 0.4.5, 2026-11-16): the scale is
// taken out, so the frame is the bar's position and turn. A read counts only while the bar is shown,
// finite, a rotation at one scale and not the identity (the pose a hidden bar reports), and only
// from the second shown read in a row (the first read after
// the dashboard opens was seen to be off: a nan, or a stale pose, on panels beside it). While
// the bar is hidden the last good frame stays. Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include <cmath>

namespace dashanchor {

constexpr int kSettleReads = 2;       // shown reads in a row before one counts
constexpr double kMoveEps = 1e-5;     // a frame this close to the last is the same (no re-placing)
constexpr double kAxisTol = 0.02;     // a rotation's axes, scale taken out: length 1 and at right angles, within this
constexpr double kMinScale = 1e-3;    // axes shorter than this: no rotation to take out

inline bool Finite(const float m[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(m[r][c])) return false;
    return true;
}

inline bool Identity(const float m[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(m[r][c] - (r == c ? 1.f : 0.f)) > 1e-6f) return false;
    return true;
}

// Its 3x3 part is a rotation: unit axes at right angles.
inline bool Rotation(const float m[3][4]) {
    for (int a = 0; a < 3; ++a)
        for (int b = a; b < 3; ++b) {
            double d = 0;
            for (int r = 0; r < 3; ++r) d += double(m[r][a]) * m[r][b];
            if (std::fabs(d - (a == b ? 1 : 0)) > kAxisTol) return false;
        }
    return true;
}

// m with its scale taken out into out: the axes divided by their mean length. False when they're
// too short to have a direction. Unequal axes stay unequal (Rotation then rejects them).
inline bool Unscaled(const float m[3][4], float out[3][4]) {
    double len = 0;
    for (int c = 0; c < 3; ++c)
        len += std::sqrt(double(m[0][c]) * m[0][c] + double(m[1][c]) * m[1][c] + double(m[2][c]) * m[2][c]) / 3;
    if (!(len > kMinScale)) return false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r][c] = float(m[r][c] / len);
        out[r][3] = m[r][3];
    }
    return true;
}

struct Anchor {
    float frame[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // the last good one (valid once `ever`)
    bool ever = false;      // a good read came this run: dashboard pins have a frame
    int shownReads = 0;     // good reads in a row (up to kSettleReads)
    const char *why = "no read yet";   // the last read's verdict: why it didn't count, or nullptr (it did)

    // One tick's read: whether the bar is shown, whether its transform could be read (and in the
    // standing universe), and the transform. True when the frame changed (the pins move).
    bool Feed(bool shown, bool read, const float raw[3][4]) {
        float m[3][4];
        why = !shown ? "hidden" : !read ? "unreadable" : !Finite(raw) ? "not finite" : Identity(raw) ? "identity"
            : !Unscaled(raw, m) || !Rotation(m) ? "not a rotation" : nullptr;
        if (why) {
            shownReads = 0;
            return false;
        }
        if (shownReads < kSettleReads) ++shownReads;
        if (shownReads < kSettleReads) return why = "settling", false;
        why = nullptr;
        bool same = ever;
        for (int r = 0; r < 3 && same; ++r)
            for (int c = 0; c < 4 && same; ++c) same = std::fabs(frame[r][c] - m[r][c]) <= kMoveEps;
        if (same) return false;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) frame[r][c] = m[r][c];
        ever = true;
        return true;
    }
};

}  // namespace dashanchor
