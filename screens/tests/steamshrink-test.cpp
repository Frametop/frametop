// steamshrink.h: Steam's overlays shrunk out of the way and given back.
#include "steamshrink.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

static int g_failed = 0;
static void Check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failed;
}
static bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(b); }

int main() {
    using namespace steamshrink;
    const float page = 2.667f, menu = 1.0f, bar = 0.3f;
    for (const float w : {page, menu, bar}) {
        const float away = Away(w);
        Check(away > 0 && Shrunk(away), "a width of Steam's shrinks under a millimetre");
        Check(Near(Back(away), w), "and comes back as it was");
        Check(Away(away) == 0, "shrinking it again does nothing (the 1 s pass)");
    }
    Check(Back(page) == 0 && Back(menu) == 0, "a width of Steam's isn't given back (not ours)");
    Check(Away(0) == 0 && Back(0) == 0 && !Shrunk(0), "no width: left alone");
    Check(Away(-1) == 0 && Back(-1) == 0, "a bad width: left alone");
    // An ft-screens that crashed: a new one reads the width alone and puts it back.
    float w = page;
    for (int i = 0; i < 1000; ++i) w = Back(Away(w));
    Check(Near(w, page), "a thousand rounds don't drift");
    return g_failed ? 1 : 0;
}
