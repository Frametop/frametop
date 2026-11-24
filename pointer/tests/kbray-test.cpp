// pointer/helper/kbray.h: the pointer's ray on Full Keyboard's panel, as "kbpoke" lines.
#include "pointer/helper/kbray.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using kbray::Forward;
    using kbray::Point;
    Forward f;
    std::string line;
    const Point a{0.1, 1.2, -0.5}, b{0.12, 1.2, -0.5};
    // Nothing until the ray is on it; off a panel it never was on, nothing.
    assert(f.Aim(0.0, false, a, "right").empty());
    assert(!f.Press(0.0, false, a, "right", &line));   // (SteamVR's laser takes that press)
    assert(!f.Release(0.0, &line));
    // On it: a hover, again only on a move or after kKeepS.
    assert(f.Aim(1.0, true, a, "right") == "kbpoke right hover 0.1000 1.2000 -0.5000");
    assert(f.Aim(1.1, true, a, "right").empty());
    assert(f.Aim(1.3, true, a, "right") == "kbpoke right hover 0.1000 1.2000 -0.5000");
    assert(f.Aim(1.31, true, b, "right") == "kbpoke right hover 0.1200 1.2000 -0.5000");
    // A press there is the keyboard's; moves while held, and a still press kept alive.
    assert(f.Press(1.4, true, b, "right", &line) && line == "kbpoke right press 0.1200 1.2000 -0.5000");
    assert(f.Pressed());
    assert(f.Aim(1.45, true, a, "right") == "kbpoke right move 0.1000 1.2000 -0.5000");
    assert(f.Aim(1.5, true, a, "right").empty());
    // Off the panel while held: still the press's, at its last point, kept alive.
    assert(f.Aim(1.6, false, b, "right").empty());
    assert(f.Aim(1.8, false, b, "right") == "kbpoke right move 0.1000 1.2000 -0.5000");
    assert(f.Release(1.9, &line) && line == "kbpoke right up 0.1000 1.2000 -0.5000");
    assert(!f.Pressed() && !f.Release(2.0, &line));
    // Leaving it ends the hover, once.
    assert(f.Aim(2.0, false, a, "right") == "kbpoke right end");
    assert(f.Aim(2.1, false, a, "right").empty());
    // The other hand's ray: the old hover ends first.
    assert(f.Aim(3.0, true, a, "left") == "kbpoke left hover 0.1000 1.2000 -0.5000");
    assert(f.Aim(3.01, true, a, "right") == "kbpoke left end\nkbpoke right hover 0.1000 1.2000 -0.5000");
    assert(f.Press(3.1, true, a, "left", &line) && line == "kbpoke right end\nkbpoke left press 0.1000 1.2000 -0.5000");
    assert(f.Release(3.2, &line) && line == "kbpoke left up 0.1000 1.2000 -0.5000");
    // A point the relay couldn't take is clamped.
    Forward g;
    assert(g.Aim(0.0, true, Point{1e6, -1e6, std::nan("")}, "right").find("999.0000 -999.0000") != std::string::npos);
    std::printf("kbray-test: ok\n");
}
