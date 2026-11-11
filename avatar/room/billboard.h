// Someone else in the room, as you see them: the sums. SteamVR shows flat pictures in the room
// (below.h tells why that works), so a person is drawn on one: a sheet standing where they are,
// upright and facing you, with each eye's view of their body drawn through it as through a
// window (mirror::Window), so the body has its own depth in front of the sheet and behind it.
//
// The sheet is no bigger than what is drawn of them needs, so the picture's pixels go on the
// person: for the abstract body that is a head and two hands, wherever the hands are.
//
// Header only, no GPU or SteamVR calls (the tests use it).
#pragma once

#include "../bodyfile.h"
#include "../view/mirror.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace billboard {

using avatar::Vec;

constexpr double kSmallest = 0.4;   // a sheet's width, metres, at the least
constexpr double kStep = 0.1;       // ...and it grows by this much at a time, so it doesn't shimmer as a hand moves
constexpr double kEdge = 0.02;      // clear of what is drawn, all round
constexpr double kTooNear = 0.12;   // a part of them this near an eye isn't what the sheet is sized for: you are all but inside it

// What hangs from one bone of a body at rest: a ball that holds it.
struct Ball {
    int role = 0;
    Vec centre;
    double radius = 0;
};

// The balls about a body's mesh at rest, one for each bone something hangs from.
inline std::vector<Ball> Balls(const bodyfile::Body &b) {
    Vec lo[bodyfile::kRoles], hi[bodyfile::kRoles];
    bool any[bodyfile::kRoles] = {};
    const auto at = [&b](int v) { return Vec{double(b.pos[3 * v]), double(b.pos[3 * v + 1]), double(b.pos[3 * v + 2])} * double(b.unit); };
    const auto each = [&b](int v, const auto &fn) {
        for (int k = 0; k < 4; ++k)
            if (b.weights[4 * v + k]) fn(int(b.bones[4 * v + k]));
    };
    for (int v = 0; v < b.verts(); ++v) {
        const Vec p = at(v);
        each(v, [&](int role) {
            if (!any[role]) lo[role] = hi[role] = p, any[role] = true;
            lo[role] = {std::min(lo[role].x, p.x), std::min(lo[role].y, p.y), std::min(lo[role].z, p.z)};
            hi[role] = {std::max(hi[role].x, p.x), std::max(hi[role].y, p.y), std::max(hi[role].z, p.z)};
        });
    }
    std::vector<Ball> balls;
    int index[bodyfile::kRoles];
    for (int r = 0; r < bodyfile::kRoles; ++r) {
        index[r] = any[r] ? int(balls.size()) : -1;
        if (any[r]) balls.push_back({r, (lo[r] + hi[r]) * 0.5, 0});
    }
    for (int v = 0; v < b.verts(); ++v) {
        const Vec p = at(v);
        each(v, [&](int role) { balls[size_t(index[role])].radius = std::max(balls[size_t(index[role])].radius, (p - balls[size_t(index[role])].centre).norm()); });
    }
    return balls;
}

// The balls where a pose puts them (`bones`: its skinning matrices, which scale the body too).
inline std::vector<Ball> Posed(const std::vector<Ball> &rest, const float bones[bodyfile::kRoles][12]) {
    std::vector<Ball> out = rest;
    for (Ball &ball : out) {
        const float *m = bones[ball.role];
        const Vec c = ball.centre;
        ball.centre = {m[0] * c.x + m[1] * c.y + m[2] * c.z + m[3], m[4] * c.x + m[5] * c.y + m[6] * c.z + m[7], m[8] * c.x + m[9] * c.y + m[10] * c.z + m[11]};
        ball.radius *= Vec{m[0], m[4], m[8]}.norm();
    }
    return out;
}

// A person's sheet: where it stands, and how far in front of it and behind it they reach.
struct Sheet {
    mirror::Glass glass;
    double front = 0, back = 0;   // metres from the sheet toward its face (the viewer) and away
};

// The sheet for someone whose mesh is within `balls` (the room), seen by eyes at `eyes` (left,
// right): upright through their middle, facing the viewer, `aspect` times as wide as tall, and
// holding all of them as either eye sees them through it, but for a part right up against an
// eye (kTooNear). False when there is nothing to draw or all of them is that near.
inline bool Around(const std::vector<Ball> &balls, const Vec eyes[2], double aspect, Sheet &out) {
    if (balls.empty() || !(aspect > 0.05 && aspect < 20)) return false;
    Vec lo = balls[0].centre, hi = lo;
    for (const Ball &b : balls) {
        lo = {std::min(lo.x, b.centre.x - b.radius), std::min(lo.y, b.centre.y - b.radius), std::min(lo.z, b.centre.z - b.radius)};
        hi = {std::max(hi.x, b.centre.x + b.radius), std::max(hi.y, b.centre.y + b.radius), std::max(hi.z, b.centre.z + b.radius)};
    }
    const Vec middle = (lo + hi) * 0.5, viewer = (eyes[0] + eyes[1]) * 0.5;
    Vec toward{viewer.x - middle.x, 0, viewer.z - middle.z};
    mirror::Glass &g = out.glass;
    g.out = toward.norm() > 1e-3 ? toward.unit() : Vec{0, 0, 1};
    g.up = {0, 1, 0};
    g.right = g.up.cross(g.out);
    // each ball as each eye sees it on the sheet's plane: what is in front of the plane looks larger
    // there. One right up against an eye is left out: it is drawn as far as the sheet goes.
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    out.front = out.back = 0;
    for (const Ball &b : balls) {
        const Vec c = b.centre - middle;
        const double bx = c.dot(g.right), by = c.dot(g.up), bz = c.dot(g.out);
        double on[2][3];   // where it is on the plane for each eye, and how big
        bool near = false;
        for (int e = 0; e < 2; ++e) {
            const Vec eye = eyes[e] - middle;
            const double ex = eye.dot(g.right), ey = eye.dot(g.up), ez = eye.dot(g.out);
            near = near || !(ez - bz - b.radius > kTooNear);
            const double k = ez / (ez - bz);
            on[e][0] = ex + (bx - ex) * k, on[e][1] = ey + (by - ey) * k, on[e][2] = b.radius * ez / (ez - bz - b.radius);
        }
        if (near) continue;
        out.front = std::max(out.front, bz + b.radius), out.back = std::max(out.back, b.radius - bz);
        for (const auto &o : on) x0 = std::min(x0, o[0] - o[2]), x1 = std::max(x1, o[0] + o[2]), y0 = std::min(y0, o[1] - o[2]), y1 = std::max(y1, o[1] + o[2]);
    }
    if (x1 < x0) return false;
    const double need = std::max({x1 - x0 + 2 * kEdge, (y1 - y0 + 2 * kEdge) * aspect, kSmallest});
    g.width = std::ceil(need / kStep - 1e-9) * kStep;
    g.height = g.width / aspect;
    g.centre = middle + g.right * ((x0 + x1) / 2) + g.up * ((y0 + y1) / 2);
    return true;
}

// What an eye sees of them through the sheet (mirror::Window: it winds the other way). False
// when the eye is at the sheet's plane or behind it.
inline bool EyeView(const Sheet &s, const Vec &eye, viewmath::Mat4 &clip) {
    const double ez = (eye - s.glass.centre).dot(s.glass.out);
    return mirror::Window(s.glass, eye, std::max(0.03, ez - s.front - kEdge), ez + s.back + kEdge, clip);
}

// Toward the light they are lit by: the room's, from above and a little to one side, the same
// from wherever you look at them.
inline Vec Light() { return Vec{0.25, 0.92, 0.3}.unit(); }

}  // namespace billboard
