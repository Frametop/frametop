// Unit tests for the two-hand turn (pointer/helper/twohand.h): made-up hands holding a panel.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/twohand-test.cpp).
#include "pointer/helper/twohand.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using md::Vec3;
using twohand::Hand;
using twohand::Move;

static Vec3 Apply(const Move &mv, Vec3 p) {
    return {mv.m[0][0] * p.x + mv.m[0][1] * p.y + mv.m[0][2] * p.z + mv.m[0][3],
            mv.m[1][0] * p.x + mv.m[1][1] * p.y + mv.m[1][2] * p.z + mv.m[1][3],
            mv.m[2][0] * p.x + mv.m[2][1] * p.y + mv.m[2][2] * p.z + mv.m[2][3]};
}
static bool Near(Vec3 a, Vec3 b, double e = 1e-6) { return md::Length(a - b) < e; }

int main() {
    // Hands 40 cm apart in front at 1.2 m, palms facing each other.
    const Hand l{{-0.2, 1.2, -0.5}, {1, 0, 0}}, r{{0.2, 1.2, -0.5}, {-1, 0, 0}};
    twohand::Turn t;
    t.Begin(l, r);
    Move mv;
    // Still: no move.
    assert(t.Now(l, r, &mv));
    assert(Near(Apply(mv, {0.3, 1.0, -0.8}), {0.3, 1.0, -0.8}));
    // Both 10 cm up: it rides along.
    assert(t.Now({{-0.2, 1.3, -0.5}, {1, 0, 0}}, {{0.2, 1.3, -0.5}, {-1, 0, 0}}, &mv));
    assert(Near(Apply(mv, {0, 1.2, -0.6}), {0, 1.3, -0.6}));
    // The right hand forward, the left back: a yaw (left turn seen from above), about the midpoint.
    {
        const double a = 0.3;
        const Hand l2{{-0.2 * std::cos(a), 1.2, -0.5 + 0.2 * std::sin(a)}, md::RotateAbout({1, 0, 0}, {0, 1, 0}, a)};
        const Hand r2{{0.2 * std::cos(a), 1.2, -0.5 - 0.2 * std::sin(a)}, md::RotateAbout({-1, 0, 0}, {0, 1, 0}, a)};
        assert(t.Now(l2, r2, &mv));
        const Vec3 front = Apply(mv, {0, 1.2, -0.4}) - Apply(mv, {0, 1.2, -0.5});   // the panel's +Z
        assert(Near(front, md::RotateAbout({0, 0, 0.1}, {0, 1, 0}, a), 1e-6));
        assert(Near(Apply(mv, {0, 1.2, -0.5}), {0, 1.2, -0.5}));
    }
    // The right hand up: a roll about the front.
    {
        const double a = 0.25;
        const Hand r2{{0.2 * std::cos(a), 1.2 + 0.2 * std::sin(a) * 1, -0.5}, md::RotateAbout({-1, 0, 0}, {0, 0, 1}, a)};
        const Hand l2{{-0.2 * std::cos(a), 1.2 - 0.2 * std::sin(a), -0.5}, md::RotateAbout({1, 0, 0}, {0, 0, 1}, a)};
        assert(t.Now(l2, r2, &mv));
        const Vec3 up = Apply(mv, {0, 1.3, -0.5}) - Apply(mv, {0, 1.2, -0.5});
        assert(Near(up, md::RotateAbout({0, 0.1, 0}, {0, 0, 1}, a)));
    }
    // Both wrists twisting forward about the line, palms down a little: a pitch.
    {
        const double a = 0.4;
        twohand::Turn p;
        p.Begin({l.at, {0, 1, 0}}, {r.at, {0, 1, 0}});   // palms up
        const Vec3 axis{1, 0, 0};
        assert(p.Now({l.at, md::RotateAbout({0, 1, 0}, axis, a)}, {r.at, md::RotateAbout({0, 1, 0}, axis, a)}, &mv));
        const Vec3 up = Apply(mv, {0, 1.3, -0.5}) - Apply(mv, {0, 1.2, -0.5});
        assert(Near(up, md::RotateAbout({0, 0.1, 0}, axis, a)));
        // a palm along the line says nothing: the other hand's twist alone
        assert(p.Now({l.at, {1, 0, 0}}, {r.at, md::RotateAbout({0, 1, 0}, axis, a)}, &mv));
        assert(Near(Apply(mv, {0, 1.3, -0.5}) - Apply(mv, {0, 1.2, -0.5}), md::RotateAbout({0, 0.1, 0}, axis, a)));
    }
    // Hands together: nothing to tell.
    assert(!t.Now({{0, 1.2, -0.5}, {1, 0, 0}}, {{0.02, 1.2, -0.5}, {-1, 0, 0}}, &mv));
    // The words: twelve numbers.
    std::string w;
    twohand::Words(Move{}, w);
    assert(w == " 1.00000 0.00000 0.00000 0.00000 0.00000 1.00000 0.00000 0.00000 0.00000 0.00000 1.00000 0.00000");
    std::puts("twohand-test: ok");
}
