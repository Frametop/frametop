// Where on the room's panels the others look (together/local/glancesums.h: the sums;
// glances.h: what this Frame's programs are told of it, and how often). No sockets.
#include "../local/glances.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static bool near(double a, double b, double by = 1e-6) { return std::fabs(a - b) <= by; }
static double rad(double degrees) { return degrees * M_PI / 180; }

// A turn of `degrees` about the way up: one of 90 takes the way a thing faces from +z to +x.
static void about(float q[4], double degrees) { q[0] = float(std::cos(rad(degrees) / 2)), q[1] = 0, q[2] = float(std::sin(rad(degrees) / 2)), q[3] = 0; }

// A panel 16 wide to 9 tall with its middle at (x, 1.5, z), facing +z turned `degrees`.
static proto::Panel panel(uint8_t id, double x, double z, double degrees = 0, float metres = 2, float curve = 0) {
    proto::Panel p;
    p.id = id, p.width = 1920, p.height = 1080, p.metres = metres, p.curve = curve;
    p.at[0] = float(x), p.at[1] = 1.5f, p.at[2] = float(z);
    about(p.turn, degrees);
    return p;
}

// Someone with their eyes at (x, 1.5, z), their head turned `degrees` to their left from facing
// -z, who looks `left` and `up` degrees from the way it faces.
static proto::Pose pose(double x, double z, double degrees = 0, double left = 0, double up = 0) {
    proto::Pose p;
    p.eyes[0] = float(x), p.eyes[1] = 1.5f, p.eyes[2] = float(z);
    about(p.head, degrees);
    p.looks = true, p.gaze[0] = float(left), p.gaze[1] = float(up);
    return p;
}

static bool lands(const proto::Panel &p, const proto::Pose &who, double x, double y, double far) {
    local::Spot spot;
    return local::Lands(p, who, spot) && near(spot.x, x) && near(spot.y, y) && near(spot.far, far, 1e-5);
}
static bool misses(const proto::Panel &p, const proto::Pose &who) {
    local::Spot spot;
    return !local::Lands(p, who, spot);
}

static void flat() {
    const proto::Panel p = panel(9, 0, -2);   // 2 m wide, 1.125 tall, 2 m ahead of the room's middle
    check("someone who faces a panel's middle and looks ahead looks at its middle", lands(p, pose(0, 0), 0.5, 0.5, 2));
    check("to their left is toward the picture's left", lands(p, pose(0, 0, 0, 10), 0.5 - 2 * std::tan(rad(10)) / 2, 0.5, 2 / std::cos(rad(10))));
    check("...to their right toward its right", lands(p, pose(0, 0, 0, -10), 0.5 + std::tan(rad(10)), 0.5, 2 / std::cos(rad(10))));
    check("up is toward its top", lands(p, pose(0, 0, 0, 0, 10), 0.5, 0.5 - 2 * std::tan(rad(10)) / 1.125, 2 / std::cos(rad(10))));
    check("...down toward its bottom", lands(p, pose(0, 0, 0, 0, -10), 0.5, 0.5 + 2 * std::tan(rad(10)) / 1.125, 2 / std::cos(rad(10))));
    check("left and up at once", lands(p, pose(0, 0, 0, 10, 10), 0.5 - std::tan(rad(10)), 0.5 - 2 * std::tan(rad(10)) / std::cos(rad(10)) / 1.125,
                                       2 / (std::cos(rad(10)) * std::cos(rad(10)))));
    check("the eyes turn in a head that is turned: the two add up", lands(p, pose(0, 0, 20, -20), 0.5, 0.5, 2) && lands(p, pose(0, 0, 10, 10), 0.5 - std::tan(rad(20)), 0.5, 2 / std::cos(rad(20))));
    check("from a step to the right, straight ahead is right of the middle", lands(p, pose(0.5, 0), 0.75, 0.5, 2));
    check("from nearer, the same look lands nearer the middle", lands(p, pose(0, -1, 0, 10), 0.5 - std::tan(rad(10)) / 2, 0.5, 1 / std::cos(rad(10))));
    check("just inside an edge is on it", lands(p, pose(0, 0, 0, 26), 0.5 - std::tan(rad(26)), 0.5, 2 / std::cos(rad(26))) && !misses(p, pose(0, 0, 0, 0, 15)) && !misses(p, pose(0, 0, 0, 0, -15)) &&
                                              !misses(p, pose(0, 0, 0, -26)));
    check("past an edge is past the panel", misses(p, pose(0, 0, 0, 27)) && misses(p, pose(0, 0, 0, -27)) && misses(p, pose(0, 0, 0, 0, 16)) && misses(p, pose(0, 0, 0, 0, -16)));
    local::Spot slant;
    check("from far to its side, a look that meets it aslant still meets it",
          local::Lands(p, pose(3, -1.5, 0, std::atan2(3, 0.5) * 180 / M_PI), slant) && near(slant.x, 0.5, 1e-4) && near(slant.y, 0.5) && near(slant.far, std::sqrt(9.25), 1e-4));
    check("someone who faces away from it doesn't look at it", misses(p, pose(0, 0, 180)));
    check("...nor someone behind it, who sees its back", misses(p, pose(0, -4, 180)) && misses(p, pose(0, -4)));
    check("...nor someone who looks along it", misses(p, pose(0, 0, 90)) && misses(p, pose(-3, -2, -90)));

    const proto::Panel side = panel(17, 3, 0, -90);   // on the room's +x side, facing its middle
    check("a panel stands where the room says, turned as it says", lands(side, pose(0, 0, -90), 0.5, 0.5, 3) && misses(side, pose(0, 0)) && misses(side, pose(0, 0, 90)));
    check("...and its picture's left is its own left", lands(side, pose(0, 0, -90, 10), 0.5 - 3 * std::tan(rad(10)) / 2, 0.5, 3 / std::cos(rad(10))));
    proto::Panel tall = panel(9, 0, -2, 0, 1);
    tall.width = 1080, tall.height = 1920;
    check("a tall picture's panel is as tall as its shape", lands(tall, pose(0, 0, 0, 0, -20), 0.5, 0.5 + 2 * std::tan(rad(20)) / (1920.0 / 1080), 2 / std::cos(rad(20))) && misses(tall, pose(0, 0, 0, 15)));
    proto::Panel high = panel(9, 0, -2);
    high.at[1] = 1.75f;
    check("...and stands as high as the room says", lands(high, pose(0, 0), 0.5, 0.5 + 0.25 / 1.125, 2) && misses(high, pose(0, 0, 0, 0, -10)) && !misses(high, pose(0, 0, 0, 0, 20)));
}

static void bent() {
    const proto::Panel p = panel(9, 0, -2, 0, 3, 2);   // 3 m of a cylinder of radius 2 about the room's middle line
    check("the middle of a bent panel is where the flat one's is", lands(p, pose(0, 0), 0.5, 0.5, 2) && lands(p, pose(0, -1), 0.5, 0.5, 1));
    check("from its axis every look is as far, and lands as far round as it is turned", lands(p, pose(0, 0, 0, 20), 0.5 - rad(20) * 2 / 3, 0.5, 2) && lands(p, pose(0, 0, 0, -20), 0.5 + rad(20) * 2 / 3, 0.5, 2));
    check("...up to its edge, and no further", !misses(p, pose(0, 0, 0, 42)) && misses(p, pose(0, 0, 0, 44)) && !misses(p, pose(0, 0, 0, -42)) && misses(p, pose(0, 0, 0, -44)));
    check("up and down are as on a flat one, from that far", lands(p, pose(0, 0, 0, 0, 10), 0.5, 0.5 - 2 * std::tan(rad(10)) / (3 * 9.0 / 16), 2 / std::cos(rad(10))));
    // from 1 m in front of its middle, 30 degrees to the left: the line meets the circle where sin(a + 30) = 1/4
    check("from off its axis, the look lands where its line leaves the cylinder", lands(p, pose(0, -1, 0, 30), 0.5 + (std::asin(0.25) - rad(30)) * 2 / 3, 0.5, 2 * std::sin(rad(30) - std::asin(0.25)) / std::sin(rad(30))));
    check("from beyond its axis it is still its inside that is seen", lands(p, pose(0, 1), 0.5, 0.5, 3));
    check("someone behind it sees its back", misses(p, pose(0, -3, 180)) && misses(p, pose(0, -3)));
    check("...and someone who faces away from it, nothing of it", misses(p, pose(0, 0, 180)));
    proto::Pose down = pose(0, 0);
    down.gaze[0] = 0, down.gaze[1] = 0, down.head[0] = float(std::cos(rad(45))), down.head[1] = float(-std::sin(rad(45))), down.head[2] = 0, down.head[3] = 0;   // the head tipped to face straight down
    check("a look straight down meets no cylinder", misses(p, down) && misses(panel(9, 0, -2), down));
}

static void nearest() {
    const std::vector<proto::Panel> room = {panel(17, 0, -4, 0, 6), panel(9, 0, -2), panel(3, 3, 0, -90)};
    uint8_t id = 200;
    local::Spot spot;
    check("of two panels one behind the other, the nearer is looked at", local::LooksAt(room, pose(0, 0), id, spot) && id == 9 && near(spot.far, 2) && near(spot.x, 0.5));
    check("...whichever the room lists first", local::LooksAt({room[1], room[0]}, pose(0, 0), id, spot) && id == 9 && near(spot.far, 2));
    check("past the nearer one's edge, the one behind it is", local::LooksAt(room, pose(0, 0, 0, 30), id, spot) && id == 17 && near(spot.x, 0.5 - 4 * std::tan(rad(30)) / 6));
    check("each is found where it stands", local::LooksAt(room, pose(0, 0, -90), id, spot) && id == 3);
    id = 200, spot = local::Spot();
    check("a look that meets none says so, and names none", !local::LooksAt(room, pose(0, 0, 90), id, spot) && id == 200 && spot.far == 0 && !local::LooksAt({}, pose(0, 0), id, spot));
}

// What was told of where someone looks: "-" nothing, else "PANEL:X:Y".
static std::string told(local::Glances &g, int place, const proto::Pose &who, const std::vector<proto::Panel> &room, double now) {
    std::vector<uint8_t> out = {7};
    if (!g.Saw(place, who, room, now, out)) return out == std::vector<uint8_t>{7} ? "-" : "- but something was written";
    proto::Point point;
    if (!proto::Read(out.data(), out.size(), point) || point.count != 0 || point.flags != 0) return "?";
    return std::to_string(point.panel) + ":" + std::to_string(point.x) + ":" + std::to_string(point.y);
}

static void rules() {
    const std::vector<proto::Panel> room = {panel(9, 0, -2)};
    proto::Pose blind = pose(0, 0);
    blind.looks = false;
    local::Glances g;
    check("someone who doesn't share where they look is told of to nobody", told(g, 1, blind, room, 0) == "-" && told(g, 1, blind, room, 1) == "-");
    check("one who does, and looks at a panel: the panel and where on it", told(g, 1, pose(0, 0), room, 2) == "9:32768:32768");
    check("...ten times a second at most", told(g, 1, pose(0, 0, 0, 5), room, 2.05) == "-" && told(g, 1, pose(0.5, 0), room, 2.125) == "9:49151:32768" && told(g, 1, pose(0.5, 0), room, 2.2) == "-" &&
                                                 told(g, 1, pose(0.5, 0), room, 2.25) == "9:49151:32768");
    local::Glances pace;
    check("...a tenth of a second after the last is soon enough", told(pace, 1, pose(0, 0), room, 0) == "9:32768:32768" && told(pace, 1, pose(0, 0), room, 0.0999) == "-" &&
                                                                     told(pace, 1, pose(0, 0), room, local::Glances::kEveryS) == "9:32768:32768");
    check("when they look away from it: no panel, once", told(g, 1, pose(0, 0, 90), room, 3) == "255:0:0" && told(g, 1, pose(0, 0, 90), room, 4) == "-" && told(g, 1, pose(0, 0, 90), room, 5) == "-");
    check("back on it, it is told again", told(g, 1, pose(0, 0), room, 6) == "9:32768:32768");
    check("when they stop sharing it: no panel, once", told(g, 1, blind, room, 7) == "255:0:0" && told(g, 1, blind, room, 8) == "-");
    check("a pose that says where they look without sharing it is no look", (blind.gaze[0] = 5, told(g, 1, blind, room, 9)) == "-");
    check("looking away isn't held back long either", told(g, 1, pose(0, 0), room, 10) == "9:32768:32768" && told(g, 1, pose(0, 0, 90), room, 10.05) == "-" && told(g, 1, pose(0, 0, 90), room, 10.125) == "255:0:0");
    check("a panel that left the room is looked at no more", told(g, 1, pose(0, 0), room, 11) == "9:32768:32768" && told(g, 1, pose(0, 0), {}, 12) == "255:0:0");

    local::Glances each;
    check("each person has their own", told(each, 0, pose(0, 0), room, 0) == "9:32768:32768" && told(each, 3, pose(0.5, 0), room, 0.01) == "9:49151:32768" && told(each, 0, pose(0, 0, 90), room, 0.125) == "255:0:0" &&
                                         told(each, 3, pose(0, 0, 90), room, 0.25) == "255:0:0" && told(each, 0, pose(0, 0, 90), room, 0.5) == "-");
    check("a place nobody can have is told of to nobody", told(each, -1, pose(0, 0), room, 1) == "-" && told(each, proto::kPeople, pose(0, 0), room, 1) == "-");
    check("a clock that went back holds nothing up", told(each, 2, pose(0, 0), room, 50) == "9:32768:32768" && told(each, 2, pose(0, 0), room, 40) == "9:32768:32768");
}

int main() {
    flat();
    bent();
    nearest();
    rules();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
