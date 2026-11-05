// The pointer device's poses as ft-pointer sends them (pointer/helper/devpose.h): a drag's tilt
// about the grab point, the "posq" line in raw space, and when a plain "pose" is due.
#include "pointer/helper/devpose.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace md;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
static bool near(Vec3 a, Vec3 b, double eps = 1e-9) { return Length(a - b) < eps; }
static bool orthonormal(const Basis &b) {
    return near(Length(b.x), 1) && near(Length(b.y), 1) && near(Length(b.z), 1) && near(Dot(b.x, b.y), 0) &&
           near(Dot(b.y, b.z), 0) && near(Dot(b.x, b.z), 0) && near(Cross(b.x, b.y), b.z);
}

struct Mat34 {  // as OpenVR's HmdMatrix34_t: rows, the position in column 3
    float m[3][4];
};

int main() {
    using std::chrono::milliseconds;
    const Basis ahead = AimBasis({0, 0, -1});
    check("aiming ahead is the identity basis", near(ahead.x, {1, 0, 0}) && near(ahead.y, {0, 1, 0}) && near(ahead.z, {0, 0, 1}));

    // Tilt
    const Vec3 pivot{0.2, 1.5, -1};
    DevicePose t = TiltAbout(pivot, {0.2, 1.5, 0}, ahead, 0, 0);
    check("no tilt leaves the pose", near(t.origin, {0.2, 1.5, 0}) && near(t.basis.z, ahead.z) && near(t.basis.x, ahead.x));
    t = TiltAbout(pivot, pivot + Vec3{1, 0, 0}, ahead, 90, 0);
    check("yaw 90 turns left about up, around the pivot", near(t.origin, pivot + Vec3{0, 0, -1}) && orthonormal(t.basis) &&
                                                             near(t.basis.x, {0, 0, -1}));
    t = TiltAbout(pivot, pivot + Vec3{0, 0, -1}, ahead, 0, 90);
    check("pitch 90 turns up about the basis's side", near(t.origin, pivot + Vec3{0, 1, 0}) && near(t.basis.x, ahead.x) &&
                                                         near(t.basis.z, {0, -1, 0}));
    const Basis side = AimBasis({1, 0, 0});
    t = TiltAbout({0, 0, 0}, {1, 0, 0}, side, 0, 90);
    check("pitch follows that basis's side, not the world's x",
          near(side.x, {0, 0, 1}) && near(t.origin, {0, 1, 0}) && near(t.basis.x, side.x) && orthonormal(t.basis));
    t = TiltAbout(pivot, {1, 2, 3}, AimBasis({0.3, -0.2, -1}), 37, -12);
    check("a tilted basis stays orthonormal and the distance to the pivot stays",
          orthonormal(t.basis) && near(Length(t.origin - pivot), Length(Vec3{1, 2, 3} - pivot)));

    // posq
    const Mat34 standing{{{1, 0, 0, 0}, {0, 1, 0, 1.6f}, {0, 0, 1, 0}}};
    double v[7] = {};
    auto scan = [&](const std::string &line) {
        return std::sscanf(line.c_str(), "posq %lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7;
    };
    check("posq: the same standing and raw pose pass the pose through",
          scan(PosqLine(standing, standing, {1, 2, 3}, ahead)) && near(v[0], 1, 1e-5) && near(v[1], 2, 1e-5) &&
              near(v[2], 3, 1e-5) && near(v[3], 1, 1e-6) && near(v[4], 0, 1e-6) && near(v[5], 0, 1e-6) && near(v[6], 0, 1e-6));
    // Raw space: the headset at (1, 0, 0), turned half around.
    const Mat34 raw{{{-1, 0, 0, 1}, {0, 1, 0, 0}, {0, 0, -1, 0}}};
    check("posq: a point 1 m ahead of the head lands ahead of it in raw space, turned with it",
          scan(PosqLine(standing, raw, {0, 1.6, -1}, ahead)) && near(v[0], 1, 1e-5) && near(v[1], 0, 1e-5) &&
              near(v[2], 1, 1e-5) && near(std::fabs(v[5]), 1, 1e-6) && near(v[3], 0, 1e-6));
    check("posq: the line's format",
          PosqLine(standing, standing, {0.5, 1.25, -2}, ahead) == "posq 0.50000 1.25000 -2.00000 1.000000 0.000000 0.000000 0.000000");

    // pose: when it's due, and its line
    const auto t0 = std::chrono::steady_clock::time_point{} + milliseconds(5000);
    PoseSent last;
    check("pose: due when none went out", PoseDue(last, t0, {0, 0, 0}, 0, 0));
    last = {true, t0, {0.1, 1.6, -0.5}, 10, -3};
    check("pose: not due for the same pose right away", !PoseDue(last, t0 + milliseconds(99), last.origin, 10, -3));
    check("pose: due again after 100 ms", PoseDue(last, t0 + milliseconds(100), last.origin, 10, -3));
    check("pose: due after a 0.21 mm move, not 0.19 mm (the line is 0.2 mm)",
          PoseDue(last, t0, last.origin + Vec3{0.00021, 0, 0}, 10, -3) &&
              !PoseDue(last, t0, last.origin + Vec3{0.00019, 0, 0}, 10, -3));
    check("pose: due after 0.05 degrees of yaw or pitch, not 0.03 (the line is 0.04)",
          PoseDue(last, t0, last.origin, 10.05, -3) && PoseDue(last, t0, last.origin, 10, -3.05) &&
              !PoseDue(last, t0, last.origin, 10.03, -3.03));
    last.yaw = 179.99;
    check("pose: yaw wraps (179.99 to -179.99 is 0.02 degrees)", !PoseDue(last, t0, last.origin, -179.99, -3));
    check("pose: the line's format", PoseLine({0.1, 1.6, -0.5}, 12.34567, -3) == "pose 0.10000 1.60000 -0.50000 12.3457 -3.0000");
    return failed ? 1 : 0;
}
