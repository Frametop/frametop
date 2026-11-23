// The centre display's look (screens/centreglow.h): a drop right on it, its width round the arc, its glow.
#include "../centreglow.h"

#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

int main() {
    using namespace centreglow;
    check("over: right on it", Over(1, -2, 0, 0));
    check("over: off to one side, or up", !Over(5, 0, 0, 0) && !Over(0, 4.5, 0, 0));
    check("over: across behind you (179 and -179 are 2 apart)", Over(179, 0, -179, 0));
    // round the arc at the radius it's flat at: as wide; further out, wider by as much; the same span either way
    check("arc: at its own distance, a little narrower (the flat one's edges were further)",
          ArcWidth(1.6, 2, 2) < 1.6 && ArcWidth(1.6, 2, 2) > 1.5);
    check("arc: the same span round the arc", near(ArcWidth(1.6, 2, 4) / 4, 2 * std::atan2(0.8, 2)));
    check("arc: no distance: as it is", near(ArcWidth(1.6, 0, 3), 1.6));
    check("band: about 1.3 degrees, within bounds", near(Band(2), 0.044) && near(Band(0.1), 0.02) && near(Band(10), 0.08));
    const int w = 64, h = 40, mx = 8, my = 6;
    const std::vector<uint8_t> px = Texture(w, h, mx, my, kHotPeak);
    const auto alpha = [&](int x, int y) { return px[(size_t(y) * w + x) * 4 + 3]; };
    check("texture: clear over the panel", alpha(w / 2, h / 2) == 0 && alpha(mx, my) == 0);
    check("texture: blue round it, fading outward", alpha(mx - 1, h / 2) > alpha(mx - 4, h / 2) && alpha(mx - 4, h / 2) > 0 &&
                                                       alpha(0, h / 2) < alpha(mx - 4, h / 2));
    check("texture: hotter is brighter", Texture(w, h, mx, my, kPeak)[(size_t(h / 2) * w + mx - 1) * 4 + 3] < alpha(mx - 1, h / 2));
    check("texture: blue", px[(size_t(h / 2) * w + 2) * 4 + 2] == 255 && px[(size_t(h / 2) * w + 2) * 4] == 70);
    std::printf(failed ? "%d FAILED\n" : "all ok\n", failed);
    return failed ? 1 : 0;
}
