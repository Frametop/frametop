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
// the bar is hidden the last good frame stays. Before any read has counted (ft-screens started
// with the dashboard closed: the hidden bar reads the identity), the dashboard tiles may ride on a
// guess from the head instead (Guess), where SteamVR opens the dashboard; the first read that
// counts replaces it. When standing jumps while the bar is hidden (SteamVR lost tracking and came
// back in a new universe: universefix.h), the kept frame goes with the jump (Carry), so what rides
// on it stays where you saw it. Header only, no OpenVR, so its test runs anywhere.
#pragma once

#include <cmath>

namespace dashanchor {

constexpr int kSettleReads = 2;       // shown reads in a row before one counts
constexpr double kMoveEps = 1e-5;     // a frame this close to the last is the same (no re-placing)
constexpr double kAxisTol = 0.02;     // a rotation's axes, scale taken out: length 1 and at right angles, within this
constexpr double kMinScale = 1e-3;    // axes shorter than this: no rotation to take out
// Where SteamVR opens its dashboard's bar from your eyes (seen on the Frame 2026-11-22, SteamOS
// 0.5.9: 0.96 to 0.97 m ahead, 0.52 to 0.54 m down, facing back at you, over three openings)
constexpr double kGuessAheadM = 0.96;
constexpr double kGuessBelowM = 0.53;

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

// m with its scale taken out into out: the axes divided by their mean length (into `scale`). False
// when they're too short to have a direction. Unequal axes stay unequal (Rotation then rejects them).
inline bool Unscaled(const float m[3][4], float out[3][4], double *scale = nullptr) {
    double len = 0;
    for (int c = 0; c < 3; ++c)
        len += std::sqrt(double(m[0][c]) * m[0][c] + double(m[1][c]) * m[1][c] + double(m[2][c]) * m[2][c]) / 3;
    if (!(len > kMinScale)) return false;
    if (scale) *scale = len;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r][c] = float(m[r][c] / len);
        out[r][3] = m[r][3];
    }
    return true;
}

// The bar's frame a guess puts it at, from the head's pose: level, kGuessAheadM ahead of the head
// round where it faces (its right axis, so looking down still has a facing), kGuessBelowM down,
// its +Z back at you. False with no facing (the head on its side).
inline bool GuessFrame(const float head[3][4], float out[3][4]) {
    const double rx = head[0][0], rz = head[2][0], len = std::sqrt(rx * rx + rz * rz);
    if (!(len > 0.1)) return false;
    const double x[3] = {rx / len, 0, rz / len}, z[3] = {-rz / len, 0, rx / len};   // (right, and back: right x up)
    for (int r = 0; r < 3; ++r) {
        out[r][0] = float(x[r]), out[r][1] = r == 1 ? 1.f : 0.f, out[r][2] = float(z[r]);
        out[r][3] = float(head[r][3] - z[r] * kGuessAheadM - (r == 1 ? kGuessBelowM : 0));
    }
    return true;
}

struct Anchor {
    float frame[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};   // the last good one (valid once `ever`)
    double scale = 1;       // the bar's scale in the last read that counted (Steam draws it scaled)
    bool ever = false;      // a good read came this run: dashboard pins have a frame
    bool guessed = false;   // no good read yet, the frame a guess from the head (Guess): the tiles ride on it
    bool shown = false;     // the bar was shown at the last read (Feed)
    int shownReads = 0;     // good reads in a row (up to kSettleReads)
    const char *why = "no read yet";   // the last read's verdict: why it didn't count, or nullptr (it did)

    // One tick's read: whether the bar is shown, whether its transform could be read (and in the
    // standing universe), and the transform. True when the frame changed (the pins move).
    bool Feed(bool shown, bool read, const float raw[3][4]) {
        float m[3][4];
        double sc = 1;
        this->shown = shown;
        why = !shown ? "hidden" : !read ? "unreadable" : !Finite(raw) ? "not finite" : Identity(raw) ? "identity"
            : !Unscaled(raw, m, &sc) || !Rotation(m) ? "not a rotation" : nullptr;
        if (why) {
            shownReads = 0;
            return false;
        }
        if (shownReads < kSettleReads) ++shownReads;
        if (shownReads < kSettleReads) return why = "settling", false;
        why = nullptr;
        scale = sc;
        guessed = false;
        bool same = ever;
        for (int r = 0; r < 3 && same; ++r)
            for (int c = 0; c < 4 && same; ++c) same = std::fabs(frame[r][c] - m[r][c]) <= kMoveEps;
        if (same) return false;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) frame[r][c] = m[r][c];
        ever = true;
        return true;
    }

    // A frame to ride on: a read's, or a guess's.
    bool Framed() const { return ever || guessed; }

    // Standing jumped by `jump` (old standing -> new): the kept frame goes with it. Only while the
    // bar is hidden: shown, Steam's own place for it is the frame, and the next read gives it.
    // True when the frame went with it (the pins move); the caller passes only a jump that moved.
    bool Carry(const float jump[3][4]) {
        if (!Framed() || shown) return false;
        float m[3][4];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                m[r][c] = jump[r][0] * frame[0][c] + jump[r][1] * frame[1][c] + jump[r][2] * frame[2][c] + (c == 3 ? jump[r][3] : 0);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) frame[r][c] = m[r][c];
        return true;
    }

    // Before any read counts: the frame guessed from the head's pose (GuessFrame). True when it's
    // set (the pins move); false once a read has counted, or with no facing.
    bool Guess(const float head[3][4]) {
        float m[3][4];
        if (ever || !GuessFrame(head, m)) return false;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) frame[r][c] = m[r][c];
        guessed = true;
        return true;
    }
};

}  // namespace dashanchor
