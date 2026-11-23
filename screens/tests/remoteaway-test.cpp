// Remote displays wait while the headset is off (screens/remoteaway.h): when away starts and ends.
#include "../remoteaway.h"

#include <cstdio>

static int failed = 0;

static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

int main() {
    using remoteaway::Watch;
    const std::optional<bool> none, away = true, back = false;
    {
        Watch w;
        check("worn at the start says nothing", w.Tick(0, false, false) == none && !w.Away());
        check("just off: not yet", w.Tick(1000, true, false) == none);
        check("a blink of the sensor ends the wait", w.Tick(1400, false, false) == none);
        check("off again: the wait starts over", w.Tick(2000, true, false) == none && w.Tick(11'900, true, false) == none);
        check("ten seconds off: away", w.Tick(12'000, true, false) == away && w.Away());
        check("said once", w.Tick(13'000, true, true) == none);
        check("back on at once", w.Tick(13'500, false, false) == back && !w.Away());
        check("said once", w.Tick(13'600, false, false) == none);
    }
    {
        Watch w;
        w.Tick(0, false, false);
        w.Tick(1000, true, false);
        check("standby before the ten seconds: away", w.Tick(6000, true, true) == away);
    }
    {
        Watch w;
        check("off at the start: away at once", w.Tick(500, true, false) == away);
        check("on: back", w.Tick(900, false, false) == back);
    }
    return failed != 0;
}
