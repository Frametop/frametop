// Small vector math shared by the pointer helper, the layout tool, and the probe, plus
// ScanPanel: measuring a floating dashboard panel, whose transform OpenVR won't give out.
// Header-only. Standing-universe coordinates unless a name says otherwise.
// The OpenVR-free part (Vec3, Basis, rotations, pose matrix math) is in vec3.h.
#pragma once

#include <openvr.h>

#include <cmath>

#include "vec3.h"

namespace md {

// Rotation part of a pose matrix applied to a vector, its transpose, and the position (vec3.h).
inline Vec3 Rotate(const vr::HmdMatrix34_t &m, Vec3 v) { return RotateBy(m, v); }
inline Vec3 RotateInverse(const vr::HmdMatrix34_t &m, Vec3 v) { return RotateInverseBy(m, v); }
inline Vec3 Position(const vr::HmdMatrix34_t &m) { return PositionOf(m); }

// A panel measured by ScanPanel: centre, size, and frame (x right, y up, z out of the front).
struct Panel {
    bool found = false;
    Vec3 center;
    double width = 0, height = 0;
    Basis basis;
    int hits = 0;
};

// Cast rays from `from` over the whole sphere (step in degrees) at one overlay, and fit
// point = origin + u*U + v*V to the hits (least squares). ComputeOverlayIntersection
// works on floating dashboard panels, whose transforms aren't readable, and returns the
// texture coordinates of each hit; v runs bottom to top. It's local and fast: a 0.5-degree
// scan (about 230,000 rays) takes 0.1 s.
inline Panel ScanPanel(vr::VROverlayHandle_t h, Vec3 from, double step = 1.0) {
    Panel p;
    double ata[3][3] = {}, atb[3][3] = {};  // normal equations for [1 u v] -> (x, y, z)
    for (double pitch = -80; pitch <= 80; pitch += step)
        for (double yaw = -180; yaw < 180; yaw += step) {
            const Vec3 d = Direction(yaw, pitch);
            vr::VROverlayIntersectionParams_t params{};
            params.vSource = {float(from.x), float(from.y), float(from.z)};
            params.vDirection = {float(d.x), float(d.y), float(d.z)};
            params.eOrigin = vr::TrackingUniverseStanding;
            vr::VROverlayIntersectionResults_t hit{};
            if (!vr::VROverlay()->ComputeOverlayIntersection(h, &params, &hit)) continue;
            ++p.hits;
            const double row[3] = {1, hit.vUVs.v[0], hit.vUVs.v[1]};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    ata[i][j] += row[i] * row[j];
                    atb[i][j] += row[i] * hit.vPoint.v[j];
                }
        }
    if (p.hits < 6) return p;
    auto det3 = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det3(ata);
    if (std::fabs(d) < 1e-12) return p;
    double x[3][3];  // x[k][j]: coefficient k (1, u, v) of coordinate j, by Cramer's rule
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
            double t[3][3];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) t[r][c] = c == k ? atb[r][j] : ata[r][c];
            x[k][j] = det3(t) / d;
        }
    const Vec3 O{x[0][0], x[0][1], x[0][2]}, U{x[1][0], x[1][1], x[1][2]}, V{x[2][0], x[2][1], x[2][2]};
    p.found = true;
    p.center = O + U * 0.5 + V * 0.5;
    p.width = Length(U);
    p.height = Length(V);
    const Vec3 bx = Normalize(U), bz = Normalize(Cross(U, V));
    p.basis = {bx, Cross(bz, bx), bz};
    return p;
}

}  // namespace md
