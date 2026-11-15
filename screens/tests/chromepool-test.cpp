// The panels' shared controls (screens/chromepool.h): who gets a set, who keeps it, who waits.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/chromepool-test screens/tests/chromepool-test.cpp && /tmp/chromepool-test
#include "../chromepool.h"

#include <cassert>
#include <cstdio>

using chromepool::Move;
using chromepool::Panel;
using chromepool::Pool;

static Panel Near(int screen, int64_t at) { return {screen, false, true, false, at}; }
static Panel Busy(int screen, int64_t at = 0) { return {screen, true, true, false, at}; }
static Panel Fading(int screen, int64_t at = 0) { return {screen, false, false, true, at}; }
static Panel Idle(int screen) { return {screen, false, false, false, 0}; }

int main() {
    {   // nothing near: no sets taken
        Pool pool;
        assert(pool.Step({Idle(1), Idle(2)}).empty());
        assert(pool.SetOf(1) < 0 && pool.SetOf(2) < 0);
    }
    {   // a laser comes near: a set; it fades, keeping it; faded, it's free again
        Pool pool;
        assert((pool.Step({Near(1, 5), Idle(2)}) == std::vector<Move>{{0, -1, 1}}));
        assert(pool.Step({Busy(1, 6)}).empty());
        assert(pool.Step({Fading(1, 6)}).empty());
        assert((pool.Step({Idle(1)}) == std::vector<Move>{{0, 1, -1}}));
        assert(pool.SetOf(1) < 0 && pool.Owner(0) == -1);
    }
    {   // a hidden or gone panel gives its set back, even mid-drag
        Pool pool;
        pool.Step({Busy(3)});
        assert((pool.Step({}) == std::vector<Move>{{0, 3, -1}}));
    }
    {   // more panels wanting than sets: the latest first, the rest wait
        Pool pool;
        std::vector<Panel> p;
        for (int s = 1; s <= chromepool::kSets + 2; ++s) p.push_back(Near(s, s));
        pool.Step(p);
        assert(pool.SetOf(1) < 0 && pool.SetOf(2) < 0);
        for (int s = 3; s <= chromepool::kSets + 2; ++s) assert(pool.SetOf(s) >= 0);
        // still wanted: nobody's set is taken from them
        assert(pool.Step(p).empty());
    }
    {   // a fading panel gives its set to a newcomer, the one longest without a laser first
        Pool pool;
        std::vector<Panel> p;
        for (int s = 1; s <= chromepool::kSets; ++s) p.push_back(Near(s, s));
        pool.Step(p);
        for (auto &q : p) q = Fading(q.screen, q.nearAt);
        p[2].wants = true;   // screen 3 still has something near
        p.push_back(Near(9, 100));
        const int was = pool.SetOf(1);
        assert((pool.Step(p) == std::vector<Move>{{was, 1, 9}}));
        assert(pool.SetOf(9) == was && pool.SetOf(1) < 0);
    }
    {   // a busy panel never loses its set, and goes before a near one
        Pool pool;
        std::vector<Panel> p;
        for (int s = 1; s <= chromepool::kSets; ++s) p.push_back(Busy(s));
        pool.Step(p);
        p.push_back(Near(9, 100));
        assert(pool.Step(p).empty());
        assert(pool.SetOf(9) < 0);
        Pool fresh;
        std::vector<Panel> q;
        for (int s = 1; s <= chromepool::kSets; ++s) q.push_back(Near(s, 50 + s));
        q.push_back(Busy(20, 0));
        fresh.Step(q);
        assert(fresh.SetOf(20) >= 0 && fresh.SetOf(1) < 0);
    }
    {   // a set freed and taken in one tick: the old owner lets go first
        Pool pool;
        pool.Step({Near(1, 1)});
        const auto moves = pool.Step({Idle(1), Near(2, 2)});
        assert((moves == std::vector<Move>{{0, 1, -1}, {0, -1, 2}}));
    }
    {   // a panel destroyed mid-drag: its set is free at once, for the next one
        Pool pool;
        pool.Step({Busy(5)});
        assert(pool.Release(5) == 0 && pool.Owner(0) == -1);
        assert(pool.Release(5) == -1 && pool.Release(-1) == -1);
        assert((pool.Step({Near(6, 1)}) == std::vector<Move>{{0, -1, 6}}));
    }
    std::puts("chromepool-test: ok");
}
