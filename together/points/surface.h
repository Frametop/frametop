// Where a point of a panel's picture is on the panel as SteamVR shows it, and how large a
// pointer's mark is there, or the mark of where someone looks.
//
// A panel is an overlay `metres` wide, as tall as its picture's shape makes it, flat or bent
// into a cylinder toward its front with its centre line where the flat one was. OpenVR's
// curvature is the part of a whole cylinder the width covers, so the radius is
// metres / (2 pi curvature); screens/vr.cpp's OnSurface is the same sum from the radius.
//
// Sums only. Header only.
#pragma once

#include <algorithm>
#include <cmath>

namespace points {

struct Surface {
    double metres = 1;      // across
    double curvature = 0;   // OpenVR's: 0 flat, 1 a whole cylinder
    int width = 16, height = 9;   // the picture's pixels: its shape
};

// Rows of a 3x4 transform: the turn, then the place.
struct Place {
    double m[3][4];
};

// The place on `s` of the point x across and y down its picture (each 0 to 1), `lift` metres in
// front of it and facing the way the surface does there, from the panel's own middle.
inline Place On(const Surface &s, double x, double y, double lift) {
    const double u = (std::clamp(x, 0.0, 1.0) - 0.5) * s.metres;
    const double v = (0.5 - std::clamp(y, 0.0, 1.0)) * s.metres * s.height / std::max(1, s.width);
    Place p = {{{1, 0, 0, u}, {0, 1, 0, v}, {0, 0, 1, lift}}};
    if (s.curvature <= 1e-6) return p;
    const double r = s.metres / (2 * M_PI * std::min(1.0, s.curvature)), a = u / r, c = std::cos(a), sn = std::sin(a);
    p.m[0][0] = c, p.m[0][2] = -sn, p.m[0][3] = (r - lift) * sn;
    p.m[2][0] = sn, p.m[2][2] = c, p.m[2][3] = r - (r - lift) * c;
    return p;
}

// a then b: where b's place is when b is told from a.
inline Place Then(const Place &a, const Place &b) {
    Place out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        out.m[i][3] += a.m[i][3];
    }
    return out;
}

// A mark's width on a panel `metres` wide: a small part of it, never too small to see from
// across a room nor large enough to hide what is pointed at.
inline double MarkMetres(double metres) { return std::clamp(metres * 0.012, 0.012, 0.04); }

// The width of the mark of where someone looks: several pointers wide, as an eye tracker is
// only so sure, and never a large part of the panel.
inline double GlanceMetres(double metres) { return std::clamp(metres * 0.06, 0.06, 0.18); }

}  // namespace points
