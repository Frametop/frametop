// Whether tiles put by hand stay as the dashboard opens (screens/tilekeep.h).
#include "../tilekeep.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

struct M {  // as vr::HmdMatrix34_t
    float m[3][4];
};
using namespace posemath;

// A head at (x, y, z) turned `yawDeg` to the left, looking `pitchDeg` up.
static M headAt(double x, double y, double z, double yawDeg = 0, double pitchDeg = 0) {
    return PanelPose<M>(x, y, z, yawDeg, pitchDeg, 0);
}

int main() {
    const double wall = 3;   // the tiles' middle from the group's centre
    const M group = PanelPose<M>(1, 1.6, 2, 30, 0, 0);   // anywhere in the room, turned any way
    const auto in = [&](const M &rel) { return Mul(group, rel); };
    tilekeep::Put put;
    check("nothing put: they come round you", !put.Holds(in(headAt(0, 0, 0)), group, wall));
    // the head between the group's centre and the wall, as it is (tiling.h: the arc's centre behind your eyes)
    const M spot = in(headAt(0.2, 0, -1.8));
    put.At(spot, group);
    check("put, the head still there: they stay", put.Holds(spot, group, wall));
    check("a metre off: they stay", put.Holds(in(headAt(0.2, 0, -0.8)), group, wall));
    check("two metres off: they come", !put.Holds(in(headAt(0.2, 0, 0.3)), group, wall));
    check("stood up or lay down (higher, lower): they stay",
          put.Holds(in(headAt(0.2, 0.6, -1.8)), group, wall) && put.Holds(in(headAt(0.2, -1.9, -1.8)), group, wall));
    check("turned 60 degrees either way: they stay",
          put.Holds(in(headAt(0.2, 0, -1.8, 60)), group, wall) && put.Holds(in(headAt(0.2, 0, -1.8, -60)), group, wall));
    check("turned your back: they come", !put.Holds(in(headAt(0.2, 0, -1.8, 180)), group, wall));
    check("turned 120 degrees: they come",
          !put.Holds(in(headAt(0.2, 0, -1.8, 120)), group, wall) && !put.Holds(in(headAt(0.2, 0, -1.8, -120)), group, wall));
    check("looking at the ceiling or the desk: the way you face is the same",
          put.Holds(in(headAt(0.2, 0, -1.8, 0, 85)), group, wall) && put.Holds(in(headAt(0.2, 0, -1.8, 0, -85)), group, wall) &&
              !put.Holds(in(headAt(0.2, 0, -1.8, 180, 85)), group, wall));
    // moved off to the side of where you face: put there on purpose, so it's where they are that counts
    const M aside = in(headAt(1.0, 0, -2.6, -100));
    tilekeep::Put side;
    side.At(aside, group);
    check("put beside you: they stay while you face them", side.Holds(in(headAt(1.0, 0, -2.6, 20)), group, wall));
    // the group moved since (a dragged dashboard, a universe's jump), the head with it
    const M moved = PanelPose<M>(-2, 1.2, 0.5, -80, 0, 0);
    check("the room's frame changed, the head with it: they stay", put.Holds(Mul(moved, headAt(0.2, 0, -1.8)), moved, wall));
    check("the dashboard dragged off: they come", !put.Holds(spot, moved, wall));
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
