// The tracking watch (screens/trackwatch.h): what it logs, and what it leaves out.
#include "../trackwatch.h"

#include <cstdio>
#include <string>
#include <vector>

static std::vector<std::string> lines;
static int failed = 0;

static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool logged(const char *part) {
    for (auto &l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
static trackwatch::Pose at(float x, float y, float z) {
    return trackwatch::Pose{{{1, 0, 0, x}, {0, 1, 0, y}, {0, 0, 1, z}}};
}

int main() {
    trackwatch::Watch w;
    w.sink = [](const char *l) { lines.push_back(l); };

    check("other events aren't ours", !w.Event(700) && lines.empty());
    check("a standing reset is", w.Event(trackwatch::kStandingReset) && logged("StandingZeroPoseReset (808)"));

    lines.clear();
    w.Standing(at(0, 1.0f, 0), "poll");
    check("the first raw->standing is logged as the start", logged("at start"));
    lines.clear();
    w.Standing(at(0.005f, 1.0f, 0), "poll");
    check("a few millimetres aren't a move", lines.empty());
    w.Standing(at(0, 1.3f, 0), "event");
    check("a 30 cm rise is, with its height", logged("moved (event): 0.300 m (height +0.300 m)"));

    lines.clear();
    w.Universe(7);
    w.Universe(7);
    check("the universe at start, once", lines.size() == 1 && logged("universe 7 at start"));
    w.Universe(9);
    check("a new universe", logged("universe 7 -> 9"));

    lines.clear();
    w.Head(true, at(0, 1.6f, 0), 1);
    w.Head(true, at(0.02f, 1.6f, 0), 2);
    check("a head moving normally isn't logged", lines.empty());
    w.Head(true, at(0.02f, 2.0f, 0), 3);
    check("a 40 cm jump in one tick is", logged("head jumped 0.400 m (height +0.400 m)"));
    lines.clear();
    w.Head(true, at(0.02f, 1.6f, 0), 4);
    check("...and another right after waits (quiet ticks)", lines.empty());
    w.Head(false, at(0, 0, 0), 5);
    check("losing the head is logged", logged("head pose lost"));
    lines.clear();
    w.Head(true, at(1, 1.6f, 0), 400);
    check("coming back is, and a jump across the gap isn't", logged("back after 396 ticks") && !logged("jumped"));
    return failed ? 1 : 0;
}
