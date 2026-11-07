// ft-screens' pose math (vr.cpp): rigid 3x4 poses, a panel's pose from its centre and the
// direction it's seen from, and a pose facing the head. M is any type with a float m[3][4]
// (vr::HmdMatrix34_t in vr.cpp). Header only, no OpenVR, so its test runs anywhere
// (screens/tests/posemath-test.cpp).
#pragma once

#include <cmath>

namespace posemath {

// How far up or down (degrees) a yaw/pitch aim may point: short of straight up or down, where
// yaw stops meaning anything. The pointer helper's aims and gaze hold use it.
constexpr double kPitchClampDeg = 85;

template <class M>
M Identity() {
    M m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    return m;
}

template <class M>
M Mul(const M &a, const M &b) {
    M r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    }
    return r;
}

template <class M>
M Inverse(const M &a) {  // rigid: R^T, -R^T t
    M r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

template <class M>
M Translation(double x, double y, double z) {
    M m = Identity<M>();
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}

inline double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

template <class M>
void Column(const M &m, int c, double out[3]) { out[0] = m.m[0][c], out[1] = m.m[1][c], out[2] = m.m[2][c]; }

// A panel pose from a centre and the direction its front is seen from (yaw, pitch; see
// layout: the front faces back along that direction), turned by roll.
template <class M>
M PanelPose(double x, double y, double z, double yawDeg, double pitchDeg, double rollDeg) {
    const double yw = yawDeg * M_PI / 180, pt = pitchDeg * M_PI / 180, rl = rollDeg * M_PI / 180;
    const double fx = -std::sin(yw) * std::cos(pt), fy = std::sin(pt), fz = -std::cos(yw) * std::cos(pt);
    const double Z[3] = {-fx, -fy, -fz};  // the front
    double X[3] = {Z[2], 0, -Z[0]};       // up x Z: horizontal right
    const double n = std::sqrt(X[0] * X[0] + X[2] * X[2]) + 1e-12;
    X[0] /= n, X[2] /= n;
    const double Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    const double c = std::cos(rl), s = std::sin(rl);
    M m{};
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = float(X[i] * c + Y[i] * s);
        m.m[i][1] = float(Y[i] * c - X[i] * s);
        m.m[i][2] = float(Z[i]);
    }
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}

// A roll about a pose's own front axis (z), counterclockwise as you see its front: Mul(p, RollZ(rad)).
template <class M>
M RollZ(double rad) {
    M m = Identity<M>();
    m.m[0][0] = m.m[1][1] = float(std::cos(rad));
    m.m[1][0] = float(std::sin(rad)), m.m[0][1] = float(-std::sin(rad));
    return m;
}

// The roll (radians, as RollZ takes it) that levels pose p where it is, facing the same way: its right
// edge horizontal and its top up, so Mul(p, RollZ(*rad)) has no tilt (DEE-337, the roll button's tap).
// The shortest way round: a screen upside down turns half a turn. False facing straight up or down
// (no level to find).
template <class M>
bool LevelRoll(const M &p, double *rad) {
    const double xUp = p.m[1][0], yUp = p.m[1][1];   // how far up its right edge and its top point
    if (std::hypot(xUp, yUp) < 1e-3) return false;
    *rad = std::atan2(-xUp, yUp);
    return true;
}

// A pose at pt facing the head (upright).
template <class M>
M FacingPose(const double pt[3], const M &head) {
    double z[3] = {head.m[0][3] - pt[0], head.m[1][3] - pt[1], head.m[2][3] - pt[2]};
    const double zl = std::sqrt(Dot3(z, z)) + 1e-9;
    for (double &v : z) v /= zl;
    double x[3] = {z[2], 0, -z[0]};  // up x z
    const double xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    M m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = float(y[i]), m.m[i][2] = float(z[i]), m.m[i][3] = float(pt[i]);
    return m;
}

// How pose p is turned from facing the head at its centre: p = FacingPose(centre, head) * turn, with
// turn a rotation only. A panel facing you, level, has the identity.
template <class M>
M FacingTurn(const M &p, const M &head) {
    const double c[3] = {p.m[0][3], p.m[1][3], p.m[2][3]};
    M t = Mul(Inverse(FacingPose(c, head)), p);
    t.m[0][3] = t.m[1][3] = t.m[2][3] = 0;
    return t;
}

// Pose p turned about its own point (kx, ky, 0) (a corner, in metres from its centre) so it meets the
// head at the angle `turn` (FacingTurn) at its new centre, the point staying where it was: *out. A panel
// that grew from a corner kept still slid in its own plane, off the sphere round the head, and is
// turned away from you; this turns it back (DEE-336). False (out untouched) within about 25 degrees of
// straight above or below the head, at the old centre or the new: facing there has no steady level
// (FacingPose's right edge swings round with a few centimetres of movement), so the panel would spin.
template <class M>
bool FaceAbout(const M &p, double kx, double ky, const M &turn, const M &head, M *out) {
    auto steep = [&](const double c[3]) {
        const double d[3] = {head.m[0][3] - c[0], head.m[1][3] - c[1], head.m[2][3] - c[2]};
        return std::fabs(d[1]) > 0.9 * std::sqrt(Dot3(d, d));
    };
    const double c0[3] = {p.m[0][3], p.m[1][3], p.m[2][3]};
    if (steep(c0)) return false;
    const double K[3] = {p.m[0][0] * kx + p.m[0][1] * ky + p.m[0][3], p.m[1][0] * kx + p.m[1][1] * ky + p.m[1][3],
                         p.m[2][0] * kx + p.m[2][1] * ky + p.m[2][3]};
    M r = p;
    // The centre depends on the turn and the turn on the centre: each round takes the error down by
    // about the corner's distance from the centre over the head's, so a few rounds settle it.
    for (int round = 0; round < 48; ++round) {
        double c[3], moved = 0;
        for (int i = 0; i < 3; ++i) c[i] = K[i] - (r.m[i][0] * kx + r.m[i][1] * ky);
        if (steep(c)) return false;
        r = Mul(FacingPose(c, head), turn);
        for (int i = 0; i < 3; ++i) {
            const double at = K[i] - (r.m[i][0] * kx + r.m[i][1] * ky);
            moved = std::fmax(moved, std::fabs(at - c[i]));
            r.m[i][3] = float(at);
        }
        if (moved < 1e-5) {   // (10 microns)
            *out = r;
            return true;
        }
    }
    return false;
}

}  // namespace posemath
