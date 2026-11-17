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
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
