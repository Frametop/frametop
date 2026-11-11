// Where the others in the room are, as ft-together has it from their poses
// (together/local/whereabouts.h): their eyes, the way they face, level, and who is forgotten.
#include "../local/whereabouts.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A head turned `yaw` about the way up, then tipped `pitch` up.
static void turned(proto::Pose &p, double yaw, double pitch) {
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    p.head[0] = float(cy * cp), p.head[1] = float(cy * sp), p.head[2] = float(sy * cp), p.head[3] = float(-sy * sp);
}

int main() {
    local::Whereabouts w;
    check("with no poses nobody is anywhere", w.Ask(0) == "ok 0");
    proto::Pose p;
    p.eyes[0] = 1.5f, p.eyes[1] = 1.625f, p.eyes[2] = -2.25f;
    w.Saw(2, p, 10);
    check("someone whose pose came is where it said, facing -z when not turned", w.Ask(10.5) == "ok 1 2:1.500:1.625:-2.250:0.0000");
    turned(p, 0.5, 0);
    w.Saw(2, p, 11);
    check("turned to the left, they face that way", w.Ask(11) == "ok 1 2:1.500:1.625:-2.250:0.5000");
    turned(p, -2.5, 0.6);
    w.Saw(2, p, 11.5);
    check("looking up or down doesn't change the way they face", w.Ask(11.5) == "ok 1 2:1.500:1.625:-2.250:-2.5000");
    turned(p, 1, M_PI / 2);
    w.Saw(2, p, 11.6);
    char facing[16];
    std::snprintf(facing, sizeof facing, "%.1f", local::Whereabouts::Facing(p.head));
    check("looking straight up they still face a way, and it is a number", std::string(facing) != "nan" && std::string(facing) != "-nan");
    proto::Pose q;
    q.eyes[0] = -3;
    w.Saw(0, q, 12);
    check("each of the others is told, in the order of their places", w.Ask(12).rfind("ok 2 0:-3.000:0.000:0.000:0.0000 2:", 0) == 0);
    check("someone whose poses stopped is forgotten", w.Ask(13.7) == "ok 1 0:-3.000:0.000:0.000:0.0000" && w.Ask(14.1) == "ok 0");
    w.Saw(-1, q, 15), w.Saw(proto::kPeople, q, 15);
    check("a place that isn't one of the room's is nobody's", w.Ask(15) == "ok 0");
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
