// A dashboard pin's frame (screens/dashanchor.h): which reads of Steam's bar count, the settling
// read after it shows, and the last good frame kept while it's hidden.
#include "../dashanchor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A pose turned by yaw about +Y, then pitch about its X, at (x, y, z).
static void pose(float m[3][4], double yaw, double pitch, float x, float y, float z) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const double Y[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double P[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += Y[i][k] * P[k][j];
            m[i][j] = float(v);
        }
    m[0][3] = x, m[1][3] = y, m[2][3] = z;
}
static bool same(const float a[3][4], const float b[3][4]) { return std::memcmp(a, b, sizeof(float) * 12) == 0; }

int main() {
    using dashanchor::Anchor;
    float a[3][4], b[3][4], id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    pose(a, 1.97, 0, -1.205f, 1.148f, 0.644f);   // the probe's bar, dashboard open
    pose(b, 0.92, -0.3, 0.4f, 1.3f, -0.9f);      // reopened elsewhere, tilted

    {
        Anchor d;
        check("hidden: no frame", !d.Feed(false, true, a) && !d.ever);
        check("first shown read only settles", !d.Feed(true, true, a) && !d.ever);
        check("second shown read counts", d.Feed(true, true, a) && d.ever && same(d.frame, a));
        check("the same frame again: no move", !d.Feed(true, true, a));
        check("dragged: moves", d.Feed(true, true, b) && same(d.frame, b));
        check("hidden: keeps the last frame", !d.Feed(false, true, id) && d.ever && same(d.frame, b));
        check("reopened: the first read settles again", !d.Feed(true, true, a) && same(d.frame, b));
        check("reopened: then jumps there", d.Feed(true, true, a) && same(d.frame, a));
    }
    {
        Anchor d;
        d.Feed(true, true, a), d.Feed(true, true, a);
        float n[3][4];
        std::memcpy(n, b, sizeof n);
        n[0][3] = std::nanf("");
        check("nan: ignored", !d.Feed(true, true, n) && same(d.frame, a));
        check("identity while shown: ignored", !d.Feed(true, true, id) && same(d.frame, a));
        float s[3][4];
        std::memcpy(s, b, sizeof s);
        for (int r = 0; r < 3; ++r) s[r][0] *= 2;
        check("stretched (one axis longer): ignored", !d.Feed(true, true, s) && same(d.frame, a));
        float z[3][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        check("no axes: ignored", !d.Feed(true, true, z) && same(d.frame, a));
        check("a failed read: ignored", !d.Feed(true, false, b) && same(d.frame, a));
        check("a bad read breaks the settling run", !d.Feed(true, true, b) && d.Feed(true, true, b));
    }
    {
        Anchor d;
        d.Feed(true, true, a), d.Feed(true, true, a);
        float t[3][4];
        std::memcpy(t, a, sizeof t);
        t[1][3] += 5e-6f;
        check("a change under kMoveEps: no move", !d.Feed(true, true, t));
        t[1][3] += 1e-3f;
        check("a change over it: moves", d.Feed(true, true, t));
    }
    {
        // Steam's bar is drawn scaled (axes 0.369 long on the Frame): the scale comes out, the turn
        // and position stay
        Anchor d;
        float s[3][4];
        std::memcpy(s, b, sizeof s);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] *= 0.369f;
        d.Feed(true, true, s);
        check("scaled the same on every axis: counts", d.Feed(true, true, s) && d.ever && !d.why);
        bool close = true;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) close = close && std::fabs(d.frame[r][c] - b[r][c]) < 1e-5;
        check("...with the scale taken out", close);
        check("...and kept (the bar's width in the room)", std::fabs(d.scale - 0.369) < 1e-5);
    }
    {
        // tilt is kept: the frame is the bar's whole pose
        Anchor d;
        d.Feed(true, true, b), d.Feed(true, true, b);
        check("tilted frame kept whole", same(d.frame, b) && std::fabs(d.frame[1][1] - std::cos(-0.3)) < 1e-5);
    }
    {
        // ft-screens started with the dashboard closed: the tiles ride on a guess from the head
        // until the bar's first good read
        Anchor d;
        float head[3][4];
        pose(head, 0.6, -0.4, 0.2f, 1.6f, -0.1f);   // turned left, looking down
        d.Feed(false, true, id);
        check("a guess from the head: framed, not read", d.Guess(head) && d.Framed() && d.guessed && !d.ever);
        const double ahead = std::hypot(d.frame[0][3] - head[0][3], d.frame[2][3] - head[2][3]);
        check("...kGuessAheadM ahead and kGuessBelowM down", std::fabs(ahead - dashanchor::kGuessAheadM) < 1e-5 &&
              std::fabs(head[1][3] - d.frame[1][3] - dashanchor::kGuessBelowM) < 1e-5);
        // (facing as the head does round you: its -Z level, from its right axis despite the pitch)
        const double fx = d.frame[0][3] - head[0][3], fz = d.frame[2][3] - head[2][3];
        check("...where the head faces, level, facing back at it",
              std::fabs(std::atan2(-fx, -fz) - 0.6) < 1e-5 && std::fabs(d.frame[1][1] - 1) < 1e-6 &&
              std::fabs(d.frame[0][2] * fx + d.frame[2][2] * fz + ahead) < 1e-5);
        check("hidden reads keep the guess", !d.Feed(false, true, id) && d.guessed);
        d.Feed(true, true, a);
        check("the first good read replaces it", d.Feed(true, true, a) && d.ever && !d.guessed && same(d.frame, a));
        check("no guessing after a read", !d.Guess(head) && same(d.frame, a));
        float side[3][4] = {{0, -1, 0, 0}, {1, 0, 0, 1.6f}, {0, 0, 1, 0}};   // rolled 90 degrees: right axis up
        Anchor e;
        check("no facing: no guess", !e.Guess(side) && !e.Framed());
    }
    {
        // Standing jumps while the dashboard is closed (universefix.h): the kept frame goes with
        // it. The jump of 2026-11-23, XRService started again: turned 164 degrees, 0.2 m off.
        float jump[3][4], want[3][4];
        pose(jump, -2.86, 0, 0.15f, -0.006f, 0.13f);
        const double c = std::cos(-2.86), s = std::sin(-2.86);
        Anchor d;
        check("no frame: nothing to carry", !d.Carry(jump));
        d.Feed(true, true, a), d.Feed(true, true, a);
        check("shown: Steam places it, not carried", !d.Carry(jump) && same(d.frame, a));
        d.Feed(false, true, id);
        // the bar's place turned about the vertical and moved, its facing turned the same
        pose(want, 1.97 - 2.86, 0, float(c * a[0][3] + s * a[2][3] + 0.15), a[1][3] - 0.006f, float(-s * a[0][3] + c * a[2][3] + 0.13));
        bool near = d.Carry(jump);
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 4; ++k) near = near && std::fabs(d.frame[r][k] - want[r][k]) < 1e-5;
        check("hidden: the frame goes with the jump", near && d.ever);
        check("reopened: a read replaces the carried frame", !d.Feed(true, true, b) && d.Feed(true, true, b) && same(d.frame, b));
        Anchor g;
        float head[3][4];
        pose(head, 0.4, 0, 0.1f, 1.6f, 0.2f);
        check("a guess is carried too", g.Guess(head) && g.Carry(jump) && g.guessed);
        // two jumps in a row (the dummy universe, then the new one): each moves it, here there and back
        float back[3][4], was[3][4];
        pose(back, 2.86, 0, float(-(c * 0.15 - s * 0.13)), 0.006f, float(-(s * 0.15 + c * 0.13)));
        d.Feed(false, true, id);   // (closed again)
        std::memcpy(was, d.frame, sizeof was);
        near = d.Carry(jump) && std::fabs(d.frame[0][3] - was[0][3]) > 0.1 && d.Carry(back);
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 4; ++k) near = near && std::fabs(d.frame[r][k] - was[r][k]) < 1e-4;
        check("the jump and its inverse leave the frame where it was", near);
    }
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
