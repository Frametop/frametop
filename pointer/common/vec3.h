// Small vector math with no OpenVR in it: vectors, orthonormal frames, rotations about an
// axis, and quaternions. vrmath.h adds what reads OpenVR's matrices and overlays; this one is
// on its own so tests that have no OpenVR header can use it. Header-only. Standing-universe
// coordinates unless a name says otherwise.
#pragma once

#include <cmath>

namespace md {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
    const double n = Length(a);
    return n > 1e-9 ? a * (1.0 / n) : Vec3{0, 0, -1};
}

// yaw 0 = -Z (SteamVR forward), positive yaw turns left (about +Y), positive pitch looks up.
inline Vec3 Direction(double yawDeg, double pitchDeg) {
    const double y = yawDeg * M_PI / 180, p = pitchDeg * M_PI / 180;
    return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

// Rotation part of a 3x4 pose matrix (rows of m.m, as OpenVR's HmdMatrix34_t) applied to a
// vector, its transpose, and the matrix's position.
template <typename M>
inline Vec3 RotateBy(const M &m, Vec3 v) {
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z, m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}
template <typename M>
inline Vec3 RotateInverseBy(const M &m, Vec3 v) {
    return {m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z, m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
            m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}
template <typename M>
inline Vec3 PositionOf(const M &m) { return {m.m[0][3], m.m[1][3], m.m[2][3]}; }

// Rodrigues: v rotated by angle (radians) about a unit axis.
inline Vec3 RotateAbout(Vec3 v, Vec3 axis, double angle) {
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1 - c));
}

// An orthonormal frame: device poses (-Z forward) and panels (+X right, +Y up, +Z out of the front).
struct Basis {
    Vec3 x, y, z;
};
// Coordinates of v in the basis, and back.
inline Vec3 ToBasis(const Basis &b, Vec3 v) { return {Dot(v, b.x), Dot(v, b.y), Dot(v, b.z)}; }
inline Vec3 FromBasis(const Basis &b, Vec3 v) { return b.x * v.x + b.y * v.y + b.z * v.z; }

// Device basis for a pointing direction with no roll: -Z along aim, +X horizontal.
inline Basis AimBasis(Vec3 aim) {
    const Vec3 z = Normalize(aim * -1.0);
    const Vec3 x = Normalize(Cross({0, 1, 0}, z));
    return {x, Cross(z, x), z};
}

// Panel basis for a panel facing the direction (yaw, pitch) points to. The front (+Z)
// faces back along that direction, toward whoever looks along it. roll turns the panel
// about its front normal, counterclockwise as you see it (90: a rotated-left monitor).
inline Basis PanelBasis(double yawDeg, double pitchDeg, double rollDeg = 0) {
    const Vec3 z = Direction(yawDeg, pitchDeg) * -1.0;
    const Vec3 x = Normalize(Cross({0, 1, 0}, z)), y = Cross(z, x);
    const double r = rollDeg * M_PI / 180, c = std::cos(r), s = std::sin(r);
    return {x * c + y * s, y * c - x * s, z};
}

// Quaternion (w, x, y, z) of a rotation whose matrix columns are the basis vectors.
inline void BasisQuat(const Basis &b, double q[4]) {
    const double m[3][3] = {{b.x.x, b.y.x, b.z.x}, {b.x.y, b.y.y, b.z.y}, {b.x.z, b.y.z, b.z.z}};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const double s = 0.5 / std::sqrt(trace + 1);
        q[0] = 0.25 / s, q[1] = (m[2][1] - m[1][2]) * s, q[2] = (m[0][2] - m[2][0]) * s, q[3] = (m[1][0] - m[0][1]) * s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2 * std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]);
        q[0] = (m[2][1] - m[1][2]) / s, q[1] = 0.25 * s, q[2] = (m[0][1] + m[1][0]) / s, q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = 2 * std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]);
        q[0] = (m[0][2] - m[2][0]) / s, q[1] = (m[0][1] + m[1][0]) / s, q[2] = 0.25 * s, q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = 2 * std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]);
        q[0] = (m[1][0] - m[0][1]) / s, q[1] = (m[0][2] + m[2][0]) / s, q[2] = (m[1][2] + m[2][1]) / s, q[3] = 0.25 * s;
    }
}

}  // namespace md
