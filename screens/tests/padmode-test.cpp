// padmode.h: gamepad mode for a panel starts at the pill's click (after the Steam menu closes, when it
// was open), holds the lasers off while it lasts, and ends on the relay's word, the Steam menu, its
// panel going, a pause or a VR game; our lasers get a moment's grace after it.
//
//   g++ -std=c++17 -Iscreens screens/tests/padmode-test.cpp
#include "padmode.h"

#include <cstdio>
#include <cstring>

using namespace padmode;

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond), ++failures; \
    } while (0)

static bool Is(const char *why, const char *part) { return why && std::strstr(why, part); }

int main() {
    // off: nothing held, no panel, no grace, and a word to leave is no news
    {
        Mode m;
        CHECK(!m.Holds() && m.Panel() == -1 && m.For() == -1 && !m.Grace(0));
        CHECK(m.Tick(false, false, true, 0) == nullptr);
        CHECK(!m.Leave(0) && !m.Grace(1));
    }
    // clicked with the Steam menu closed: on at once, and it stays on
    {
        Mode m;
        CHECK(!m.Click(4, false, 100));
        CHECK(m.Holds() && m.Panel() == 4 && m.For() == 4);
        CHECK(m.Tick(false, true, false, 5000) == nullptr && m.Panel() == 4);
        // both grips (the relay's word): over, once, with grace for the lasers
        CHECK(m.Leave(6000));
        CHECK(!m.Holds() && m.Panel() == -1 && m.Grace(6000) && m.Grace(6000 + kLaserGraceMs - 1));
        CHECK(!m.Grace(6000 + kLaserGraceMs));
        CHECK(!m.Leave(6001));
    }
    // clicked with the Steam menu open: the lasers are held, but no panel has the pad until it closes
    {
        Mode m;
        CHECK(m.Click(2, true, 0));
        CHECK(m.Holds() && m.Panel() == -1 && m.For() == 2);
        CHECK(m.Tick(true, true, false, kCloseWaitMs - 1) == nullptr && m.Panel() == -1);
        CHECK(m.Tick(false, true, false, kCloseWaitMs - 1) == nullptr && m.Panel() == 2);
        // ...and the menu opening again (the system button) ends it
        CHECK(Is(m.Tick(true, true, false, 9000), "opened"));
        CHECK(!m.Holds() && m.Grace(9000));
    }
    // the Steam menu never closes (no ft-dashpatch): given up, not held for ever
    {
        Mode m;
        m.Click(2, true, 0);
        CHECK(Is(m.Tick(true, true, false, kCloseWaitMs), "stayed open"));
        CHECK(!m.Holds() && m.Panel() == -1);
    }
    // its panel goes (the game quit), Frametop pauses, a VR game starts
    {
        Mode m;
        m.Click(7, false, 0);
        CHECK(Is(m.Tick(false, false, false, 10), "panel"));
        m.Click(7, false, 20);
        CHECK(!m.Grace(20));   // on again: no grace left over from the last one
        CHECK(Is(m.Tick(false, true, true, 30), "paused"));
        CHECK(m.Tick(false, true, false, 40) == nullptr && !m.Holds());
    }
    // a second click moves it to that panel
    {
        Mode m;
        m.Click(1, false, 0);
        m.Click(3, false, 10);
        CHECK(m.Panel() == 3);
    }
    if (failures) return std::printf("padmode-test: %d failed\n", failures), 1;
    std::printf("padmode-test: ok\n");
    return 0;
}
