// A head pin's floor frames (screens/levelpin.h): Level, the head's turn and floor position, nothing
// else; Walk, the floor position only; and the SCREEN_PIN_LEVEL values.
#include "../levelpin.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

// A head at (x, y, z) turned by yaw (left, about +Y), then pitch (up, about its X), then roll (about its Z).
static void head(float m[3][4], double yaw, double pitch, double roll, float x, float y, float z) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch),
                 cr = std::cos(roll), sr = std::sin(roll);
    const double Y[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double P[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const double R[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double YP[3][3] = {}, M[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) YP[i][j] += Y[i][k] * P[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) M[i][j] += YP[i][k] * R[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = float(M[i][j]);
    m[0][3] = x, m[1][3] = y, m[2][3] = z;
}
// The frame is upright, on the floor under the head, and turned by yaw (within `tol` radians).
static bool is(const float f[3][4], double yaw, float x, float z, double tol = 1e-4) {
    const double got = std::atan2(-f[2][0], f[0][0]);   // the right vector's turn, as head() builds it
    const double err = std::fabs(std::remainder(got - yaw, 2 * M_PI));
    return err < tol && near(f[1][1], 1) && near(f[0][1], 0) && near(f[2][1], 0) && near(f[1][0], 0) &&
           near(f[1][2], 0) && near(f[0][3], x) && near(f[1][3], 0) && near(f[2][3], z) &&
           near(f[0][0] * f[0][0] + f[2][0] * f[2][0], 1);
}

// Rigid 3x4 poses: a times b, and a's inverse (rotation transposed).
static void mul(const float a[3][4], const float b[3][4], float o[3][4]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            double v = c == 3 ? a[r][3] : 0;
            for (int k = 0; k < 3; ++k) v += a[r][k] * b[k][c];
            o[r][c] = float(v);
        }
    }
}
static void inv(const float a[3][4], float o[3][4]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) o[r][c] = a[c][r];
        o[r][3] = -(a[0][r] * a[0][3] + a[1][r] * a[1][3] + a[2][r] * a[2][3]);
    }
}
static bool same(const float a[3][4], const float b[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!near(a[r][c], b[r][c])) return false;
    return true;
}

int main() {
    const double d = M_PI / 180;
    float h[3][4], f[3][4];
    head(h, 30 * d, 0, 0, 0.5f, 1.6f, -0.2f);
    check("level head: its own yaw, on the floor", levelpin::Frame(h, f) && is(f, 30 * d, 0.5f, -0.2f));
    head(h, 30 * d, 50 * d, 0, 0.5f, 1.4f, -0.2f);
    check("looking up: the same frame", levelpin::Frame(h, f) && is(f, 30 * d, 0.5f, -0.2f));
    head(h, 30 * d, -70 * d, 0, 0.5f, 1.0f, -0.2f);
    check("looking down, crouched: the same frame", levelpin::Frame(h, f) && is(f, 30 * d, 0.5f, -0.2f));
    head(h, 60 * d, 0, 85 * d, 0, 1.6f, 0);
    check("tilted all the way over: the same yaw", levelpin::Frame(h, f) && is(f, 60 * d, 0, 0));
    head(h, -120 * d, 20 * d, 25 * d, 0, 1.6f, 0);
    check("looking up and tilted: within a degree", levelpin::Frame(h, f) && is(f, -120 * d, 0, 0, d));
    head(h, 45 * d, -90 * d, 0, 0, 1.6f, 0);
    check("straight down: the right vector keeps the yaw", levelpin::Frame(h, f) && is(f, 45 * d, 0, 0));
    float a[3][4], b[3][4];
    head(h, 10 * d, 87 * d, 20 * d, 0, 1.6f, 0);
    levelpin::Frame(h, a);
    head(h, 10 * d, 89 * d, 20 * d, 0, 1.6f, 0);
    levelpin::Frame(h, b);
    check("nearly straight up, tilted: no jump between 87 and 89 degrees",
          std::fabs(std::remainder(std::atan2(-a[2][0], a[0][0]) - std::atan2(-b[2][0], b[0][0]), 2 * M_PI)) < 10 * d);
    head(h, 170 * d, 0, 0, 1, 1.6f, 1);
    check("facing behind the origin", levelpin::Frame(h, f) && is(f, 170 * d, 1, 1));
    // Walk: the room's axes under the head, whatever it does.
    head(h, 30 * d, 50 * d, 20 * d, 0.5f, 1.2f, -0.2f);
    check("walk: no turn, look or tilt, on the floor under the head",
          levelpin::FloorFrame(levelpin::Walk, h, f) && is(f, 0, 0.5f, -0.2f));
    head(h, 170 * d, -80 * d, 0, -1, 0.7f, 2);
    check("walk: turned round and crouched, still the room's axes",
          levelpin::WalkFrame(h, f) && is(f, 0, -1, 2));
    {
        // A screen pinned with Walk keeps its height and facing and moves with the head's floor position.
        float h0[3][4], h1[3][4], f0[3][4], f1[3][4], fi[3][4], rel[3][4], got[3][4];
        head(h0, 0, 0, 0, 0, 1.6f, 0);
        float screen[3][4];
        head(screen, 20 * d, 10 * d, 0, 0.3f, 1.5f, -1.2f);   // in front, a little right, turned and tipped
        levelpin::WalkFrame(h0, f0);
        inv(f0, fi);
        mul(fi, screen, rel);
        head(h1, 90 * d, -30 * d, 15 * d, 1.0f, 1.1f, 2.0f);   // walked 1 m right, 2 m back, turned, crouched
        levelpin::WalkFrame(h1, f1);
        mul(f1, rel, got);
        float want[3][4];
        std::memcpy(want, screen, sizeof want);
        want[0][3] += 1.0f, want[2][3] += 2.0f;
        check("walk pin: moved with the walk, same height and facing after turning and crouching", same(got, want));
        // The same with Level turns it with the head.
        levelpin::Frame(h0, f0);
        inv(f0, fi);
        mul(fi, screen, rel);
        levelpin::Frame(h1, f1);
        mul(f1, rel, got);
        check("level pin: turned with the head (not the same as walk)", !same(got, want) && near(got[1][3], screen[1][3]));
    }
    check("rigid has no floor frame", !levelpin::FloorFrame(levelpin::Rigid, h, f));
    check("SCREEN_PIN_LEVEL values: 0 rigid, 1 level, 2 walk, junk rigid",
          levelpin::ModeOf("0") == levelpin::Rigid && levelpin::ModeOf("1") == levelpin::Level &&
              levelpin::ModeOf("2") == levelpin::Walk && levelpin::ModeOf("on") == levelpin::Level &&
              levelpin::ModeOf("3") == levelpin::Rigid && levelpin::ModeOf("") == levelpin::Rigid &&
              levelpin::ModeOf(nullptr) == levelpin::Rigid);
    return failed ? 1 : 0;
}
