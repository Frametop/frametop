// pointer/helper/twohand.h: a two-hand turn of a panel a hand ray is carrying. Pure math, no OpenVR.
//
// While one hand's ray drags Full Keyboard by its handle or a floating window by its bar, the other
// hand's pinch held as well turns the panel as if both hands held it (user, 2026-11-21: "If you drag
// using hand tracking with one hand and then tap with the other, you should be able to rotate as if
// you were manipulating the keyboard or window"). The line between the hands turns it: one hand
// forward, a yaw; one up, a roll. Both wrists twisting about that line (the palms' normals around
// it) tilts it, a pitch. The midpoint carries it.
//   Like a playing card held at arm's length, however far off the panel is (user, 2026-11-21: "almost
// more like you're manipulating it directly in your hands ... even if it's further away ... basically
// telekinesis"): it turns about its own centre, not about the hands (a far panel swung round you), and
// the midpoint's movement is scaled by the panel's distance over the hands' (a lever).
//
// Turn::Now gives the turn R since Begin and the move t: the panel's owner turns its pose at the
// begin about its own centre c by R and moves it by t (rotation R * R0, centre c + t), standing-universe
// coordinates.
#pragma once

#include <cmath>
#include <cstdio>

#include "../common/vec3.h"

namespace twohand {

using md::Vec3;

struct Hand {
    Vec3 at;     // the pinch point (the index and middle knuckles)
    Vec3 palm;   // unit, out of the palm
};

// A turn and a move: rows of the rotation, and the translation in the last column.
struct Move {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

constexpr double kMinSpan = 0.06;      // hands closer than this: no line to turn by (m)
constexpr double kMinPalmAcross = 0.3; // a palm nearly along the line says nothing of the twist

// The signed angle (radians) from a to b about the unit axis, both projected across it; false if
// either is too near the axis.
inline bool AngleAbout(Vec3 a, Vec3 b, Vec3 axis, double *out) {
    a = a - axis * md::Dot(a, axis), b = b - axis * md::Dot(b, axis);
    if (md::Length(a) < kMinPalmAcross || md::Length(b) < kMinPalmAcross) return false;
    *out = std::atan2(md::Dot(md::Cross(a, b), axis), md::Dot(a, b));
    return true;
}

class Turn {
public:
    // gain: how far the panel moves per metre the hands' midpoint does.
    void Begin(const Hand &a, const Hand &b, double gain = 1) { a0_ = a, b0_ = b, gain_ = gain; }

    // The move since Begin for the hands now; false when they're too close together to tell.
    bool Now(const Hand &a, const Hand &b, Move *out) const {
        const Vec3 v0 = b0_.at - a0_.at, v = b.at - a.at;
        if (md::Length(v0) < kMinSpan || md::Length(v) < kMinSpan) return false;
        const Vec3 u0 = md::Normalize(v0), u = md::Normalize(v);
        // the line's own turn (shortest arc), then the twist about it
        Vec3 arcAxis = md::Cross(u0, u);
        const double arcSin = md::Length(arcAxis), arc = std::atan2(arcSin, md::Dot(u0, u));
        const bool hasArc = arcSin > 1e-9;
        if (hasArc) arcAxis = arcAxis * (1.0 / arcSin);
        auto byArc = [&](Vec3 x) { return hasArc ? md::RotateAbout(x, arcAxis, arc) : x; };
        double twist = 0, ta, tb;
        int n = 0;
        if (AngleAbout(byArc(a0_.palm), a.palm, u, &ta)) twist += ta, ++n;
        if (AngleAbout(byArc(b0_.palm), b.palm, u, &tb)) twist += tb, ++n;
        if (n) twist /= n;
        auto turn = [&](Vec3 x) { return md::RotateAbout(byArc(x), u, twist); };
        const Vec3 cx = turn({1, 0, 0}), cy = turn({0, 1, 0}), cz = turn({0, 0, 1});
        const Vec3 t = ((a.at + b.at) * 0.5 - (a0_.at + b0_.at) * 0.5) * gain_;
        const double rows[3][4] = {{cx.x, cy.x, cz.x, t.x}, {cx.y, cy.y, cz.y, t.y}, {cx.z, cy.z, cz.z, t.z}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) out->m[r][c] = rows[r][c];
        return true;
    }

private:
    Hand a0_, b0_;
    double gain_ = 1;
};

// The lever: the panel's distance from the eye over the hands' midpoint's, within [1, kMaxGain].
constexpr double kMaxGain = 6;
inline double Gain(double panelDistance, Vec3 eye, const Hand &a, const Hand &b) {
    const double hands = md::Length((a.at + b.at) * 0.5 - eye);
    return hands > 0.05 ? std::fmin(std::fmax(panelDistance / hands, 1.0), kMaxGain) : 1.0;
}

// The move as words for a "turn" line: twelve numbers, row by row.
template <typename Out>
inline void Words(const Move &mv, Out &out) {
    char b[24];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            std::snprintf(b, sizeof b, " %.5f", mv.m[r][c]);
            out += b;
        }
}

}  // namespace twohand
