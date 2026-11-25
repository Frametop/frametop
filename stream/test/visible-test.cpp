// Which part of a decoded buffer is shown (stream/visible.h), as each SteamOS's driver says it.
#include "visible.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static bool is(const VisibleRect &r, uint32_t w, uint32_t h, int left = 0, int top = 0) {
    return r.width == w && r.height == h && r.left == left && r.top == top;
}

int main() {
    const VisibleRect hd{0, 0, 1920, 1080};
    check("the 0.5.10 driver: the crop from before the format was set", is(Visible(hd, {0, 0, 1920, 1088}, 1920, 1088), 1920, 1080));
    check("...across too", is(Visible({0, 0, 3440, 1440}, {0, 0, 3456, 1440}, 3456, 1440), 3440, 1440));
    check("the driver before it: the one from after", is(Visible(hd, hd, 1920, 1088), 1920, 1080));
    check("...whatever it said before", is(Visible({0, 0, 1280, 720}, hd, 1920, 1088), 1920, 1080) && is(Visible({}, hd, 1920, 1088), 1920, 1080));
    check("a size with nothing to crop", is(Visible({0, 0, 2560, 1600}, {0, 0, 2560, 1600}, 2560, 1600), 2560, 1600));
    check("an earlier stream's crop isn't taken", is(Visible({0, 0, 1280, 720}, {0, 0, 1920, 1088}, 1920, 1088), 1920, 1088) &&
                                                     is(Visible(hd, {0, 0, 1280, 736}, 1280, 736), 1280, 736));
    check("a crop that isn't at the corner is kept", is(Visible({0, 4, 1920, 1080}, {0, 0, 1920, 1088}, 1920, 1088), 1920, 1080, 0, 4));
    check("no crop at all: the whole buffer", is(Visible({}, {}, 1920, 1088), 1920, 1088));
    check("a crop outside the buffer: the whole buffer", is(Visible({0, 0, 4000, 1080}, {-2, 0, 1920, 1088}, 1920, 1088), 1920, 1088));
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
