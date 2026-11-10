// The views ft-avatars draws a body from: 4x4 matrices in GL's order (column-major, a point on
// the right), room -> clip space. Header only, no GPU calls (the tests use it).
#pragma once

#include "../vec.h"

namespace viewmath {

using avatar::Vec;

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// b first, then a.
inline Mat4 Mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += double(a.m[k * 4 + row]) * b.m[c * 4 + k];
            r.m[c * 4 + row] = float(v);
        }
    return r;
}

// Where a point lands: clip space divided through (x and y in -1..1 on screen, y up).
inline Vec Project(const Mat4 &m, const Vec &p) {
    const double x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12], y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13];
    const double z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14], w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    return {x / w, y / w, z / w};
}

// A camera at `eye` looking at `target`: the room -> the camera's own space (-z ahead, y up).
inline Mat4 LookAt(const Vec &eye, const Vec &target, const Vec &up) {
    const Vec f = (target - eye).unit(), s = f.cross(up).unit(), u = s.cross(f);
    Mat4 r;
    const double m[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -s.dot(eye), -u.dot(eye), f.dot(eye), 1};
    for (int i = 0; i < 16; ++i) r.m[i] = float(m[i]);
    return r;
}

// A lens `fovY` radians tall, `aspect` times as wide, seeing from `nearM` to `farM` metres.
inline Mat4 Perspective(double fovY, double aspect, double nearM, double farM) {
    const double f = 1 / std::tan(fovY / 2);
    Mat4 r;
    const double m[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (farM + nearM) / (nearM - farM), -1, 0, 0, 2 * farM * nearM / (nearM - farM), 0};
    for (int i = 0; i < 16; ++i) r.m[i] = float(m[i]);
    return r;
}

// The room as a mirror through `point` facing along `normal` shows it. What it shows winds the
// other way: draw it with the front faces turned.
inline Mat4 Mirror(const Vec &point, const Vec &normal) {
    const Vec n = normal.unit();
    const double d = 2 * n.dot(point);
    Mat4 r;
    const double m[16] = {1 - 2 * n.x * n.x, -2 * n.y * n.x, -2 * n.z * n.x, 0, -2 * n.x * n.y, 1 - 2 * n.y * n.y, -2 * n.z * n.y, 0,
                          -2 * n.x * n.z, -2 * n.y * n.z, 1 - 2 * n.z * n.z, 0, d * n.x, d * n.y, d * n.z, 1};
    for (int i = 0; i < 16; ++i) r.m[i] = float(m[i]);
    return r;
}

}  // namespace viewmath
