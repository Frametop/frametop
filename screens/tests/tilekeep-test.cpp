// Whether the tiles stay as the dashboard opens, put by hand or brought round you, and when the head
// is steady enough to bring them round (screens/tilekeep.h).
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
    check("1.4 metres off: they stay", put.Holds(in(headAt(0.2, 0, -0.4)), group, wall));
    check("1.6 metres off: they come", !put.Holds(in(headAt(0.2, 0, -0.2)), group, wall));
    check("stood up or lay down (higher, lower): they stay",
          put.Holds(in(headAt(0.2, 0.6, -1.8)), group, wall) && put.Holds(in(headAt(0.2, -1.9, -1.8)), group, wall));
    // (the spot is a little to the right of their middle: straight at it is 9 degrees to the left)
    check("turned 80 degrees either way from their middle: they stay",
          put.Holds(in(headAt(0.2, 0, -1.8, 89)), group, wall) && put.Holds(in(headAt(0.2, 0, -1.8, -71)), group, wall));
    check("turned your back: they come", !put.Holds(in(headAt(0.2, 0, -1.8, 180)), group, wall));
    check("turned 100 degrees: they come",
          !put.Holds(in(headAt(0.2, 0, -1.8, 109)), group, wall) && !put.Holds(in(headAt(0.2, 0, -1.8, -91)), group, wall));
    check("the head on its side: no way faced to tell by, they stay", put.Holds(in(PanelPose<M>(0.2, 0, -1.8, 0, 0, 90)), group, wall));
    tilekeep::Put middle;
    middle.At(in(headAt(0, 0, -wall)), group);
    check("in their middle: no way to it to tell by, they stay", middle.Holds(in(headAt(0, 0, -wall, 180)), group, wall));
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
    // an opening that left them holds them a moment
    check("put: not held yet", !put.Settling(5000));
    put.Kept(5000);
    check("left by an opening: held for a second", put.Settling(5000) && put.Settling(5999) && !put.Settling(6000));
    put.At(spot, group);
    check("put again meanwhile: the hold is the old opening's", !put.Settling(5500));
    put.Kept(5000);
    put.Clear();
    check("a reset or another wall: not held, and the next opening brings them", !put.Settling(5500) && !put.Holds(spot, group, wall));
    put.Kept(5000);
    check("nothing put: nothing held", !put.Settling(5500));
    // brought round you (an opening, a reset): a narrower spot than put by hand
    tilekeep::Put round, hand;
    round.At(spot, group, false);
    hand.At(spot, group);
    check("brought round you: not by hand", round.on && !round.byHand && hand.byHand);
    check("brought round you, the head still there: they stay", round.Holds(spot, group, wall));
    check("brought round you, 0.9 metres off: they stay", round.Holds(in(headAt(0.2, 0, -0.9)), group, wall));
    check("brought round you, 1.1 metres off: they come (by hand they'd stay)",
          !round.Holds(in(headAt(0.2, 0, -0.7)), group, wall) && hand.Holds(in(headAt(0.2, 0, -0.7)), group, wall));
    check("brought round you, turned 50 degrees either way: they stay",
          round.Holds(in(headAt(0.2, 0, -1.8, 59)), group, wall) && round.Holds(in(headAt(0.2, 0, -1.8, -41)), group, wall));
    check("brought round you, turned 70 degrees: they come (by hand they'd stay)",
          !round.Holds(in(headAt(0.2, 0, -1.8, 79)), group, wall) && !round.Holds(in(headAt(0.2, 0, -1.8, -61)), group, wall) &&
              hand.Holds(in(headAt(0.2, 0, -1.8, 79)), group, wall));
    round.At(spot, group);
    check("then put by hand: the wider spot", round.byHand && round.Holds(in(headAt(0.2, 0, -0.7)), group, wall));

    // a steady head: worn kWornMs, still kStillMs
    tilekeep::Steady steady;
    const double here[3] = {0.1, 1.6, -0.2};
    check("nothing yet: not steady", !steady.Ready(0));
    steady.Tick(1000, true, true, here);
    check("just put on: not steady", !steady.Ready(1000) && !steady.Ready(2999));
    steady.Tick(3000, true, true, here);
    check("worn two seconds, still: steady", steady.Ready(3000));
    const double nudged[3] = {0.18, 1.62, -0.25};   // (a head turning moves the eyes a little)
    steady.Tick(3100, true, true, nudged);
    check("a head turning: still steady", steady.Ready(3100));
    const double lifted[3] = {0.1, 1.85, -0.2};   // (the headset coming off, a pose's jump)
    steady.Tick(3200, true, true, lifted);
    check("the head 0.25 m up: not steady", !steady.Ready(3200) && !steady.Ready(3499));
    steady.Tick(3500, true, true, lifted);
    check("still there 0.3 s: steady again", steady.Ready(3500));
    steady.Tick(3600, false, true, lifted);
    check("taken off: not steady", !steady.Ready(3600));
    steady.Tick(4000, true, true, lifted);
    steady.Tick(5900, true, true, lifted);
    check("put on again: not steady for two seconds", !steady.Ready(5900));
    steady.Tick(6000, true, true, lifted);
    check("...then steady", steady.Ready(6000));
    steady.Tick(6100, true, false, lifted);
    check("the head's pose lost: not steady", !steady.Ready(6100));
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
