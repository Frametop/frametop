// gstheater.h: a window lent to SteamVR's theater comes back once SteamVR has shown it there and
// stops, or if SteamVR never shows it there in time; never twice.
//
//   g++ -std=c++17 -Iscreens screens/tests/gstheater-test.cpp
#include "gstheater.h"

#include <cstdio>

using namespace gsapps;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

int main() {
    // the app id in an app's key
    CHECK(SteamAppOf("steam:2868840") == 2868840);
    CHECK(SteamAppOf("x11:XTerm") == 0 && SteamAppOf("steam:") == 0 && SteamAppOf("steam:12x") == 0);
    CHECK(SteamAppOf("steam:0") == 0 && SteamAppOf("") == 0 && SteamAppOf("steam:99999999999") == 0);

    // lent; SteamVR not yet showing it: kept (the theater takes a moment)
    {
        Theater t;
        t.Lent(7, 100, 0);
        t.Heard(0);
        CHECK(t.Due(1000).empty() && t.IsLent(7) && !t.Showing());
        // shown in the theater: kept, and showing
        t.Heard(100);
        CHECK(t.Due(kTheaterWaitMs + 1).empty() && t.Showing());
        // another game's in the theater now, or none: it's back, once
        t.Heard(0);
        const auto back = t.Due(kTheaterWaitMs + 2);
        CHECK(back.size() == 1 && back[0].win == 7 && !t.IsLent(7) && !t.Showing());
        CHECK(t.Due(kTheaterWaitMs + 3).empty());
    }
    // never shown in the theater: back after kTheaterWaitMs, not before
    {
        Theater t;
        t.Lent(7, 100, 1000);
        t.Heard(0), t.Heard(200);
        CHECK(t.Due(1000 + kTheaterWaitMs - 1).empty());
        const auto back = t.Due(1000 + kTheaterWaitMs);
        CHECK(back.size() == 1 && back[0].win == 7);
    }
    // two games lent: each on its own word
    {
        Theater t;
        t.Lent(7, 100, 0), t.Lent(8, 200, 0);
        t.Heard(100);
        CHECK(t.Due(10).empty());
        t.Heard(200);   // 200's in the theater now: 100's left it
        const auto back = t.Due(20);
        CHECK(back.size() == 1 && back[0].win == 7 && t.IsLent(8) && t.Showing());
        t.Clear();
        CHECK(!t.IsLent(8) && !t.Showing());
    }

    if (failures) return std::printf("gstheater-test: %d failed\n", failures), 1;
    std::printf("gstheater-test: ok\n");
    return 0;
}
