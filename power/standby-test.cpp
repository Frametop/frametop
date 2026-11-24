// power/standby.h: ft-powerd's forced standby, without SteamVR.
#include "standby.h"

#include <cstdio>

using ft_power::Standby;

static int failed = 0;
#define CHECK(what) \
    do { \
        if (!(what)) { \
            std::printf("FAIL line %d: %s\n", __LINE__, #what); \
            ++failed; \
        } \
    } while (0)

int main() {
    {  // Asked, taken, ended by use.
        Standby s;
        CHECK(!s.Active());
        CHECK(s.Start(100, true, false) == Standby::Begin::Ask);
        CHECK(s.Active() && !s.Holding());
        CHECK(s.Start(101, true, false) == Standby::Begin::Busy);  // one "not worn" per attempt
        CHECK(s.Tick(102, false) == Standby::Step::None);
        CHECK(s.Tick(106, true) == Standby::Step::Holds);
        CHECK(s.Holding());
        CHECK(s.Tick(107, true) == Standby::Step::None);  // Holds is told once
        CHECK(s.Start(108, true, false) == Standby::Begin::Busy);
        CHECK(s.Use(5000));  // a key: write "worn"
        CHECK(!s.Active());
        CHECK(!s.Use(5001));  // nothing left to write
        CHECK(s.Tick(5002, false) == Standby::Step::None);  // SteamVR waking after our write isn't "by itself"
        CHECK(s.Start(6000, true, false) == Standby::Begin::Ask);
    }
    {  // Use before SteamVR has the headset as off: still a "worn" to write.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Use(2));
        CHECK(!s.Active());
        CHECK(s.Tick(8, true) == Standby::Step::None);  // away later is SteamVR's own business
    }
    {  // SteamVR never goes away: failed after kAnswerSecs, and the next timeout tries again.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Tick(Standby::kAnswerSecs - 0.1, false) == Standby::Step::None);
        CHECK(s.Tick(Standby::kAnswerSecs, false) == Standby::Step::Failed);
        CHECK(!s.Active());
        CHECK(s.Start(400, true, false) == Standby::Begin::Ask);
    }
    {  // Away at the last moment still holds.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Tick(Standby::kAnswerSecs, true) == Standby::Step::Holds);
    }
    {  // One under way comes before the setting, the setting before the testbench, the testbench
       // before a standby that didn't hold.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Start(1, false, true) == Standby::Begin::Busy);
        CHECK(s.Tick(6, true) == Standby::Step::Holds);
        CHECK(s.Tick(30, false) == Standby::Step::Woke);
        CHECK(s.Start(31, false, true) == Standby::Begin::Disabled);
        CHECK(s.Start(31, true, true) == Standby::Begin::Testing);
        CHECK(s.Start(31, true, false) == Standby::Begin::Unheld);
    }
    {  // The write couldn't be sent.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        s.Abort();
        CHECK(!s.Active());
        CHECK(s.Start(1, true, false) == Standby::Begin::Ask);
    }
    {  // Off, or the testbench fakes a wearer: the backlight.
        Standby s;
        CHECK(s.Start(0, false, false) == Standby::Begin::Disabled);
        CHECK(s.Start(0, true, true) == Standby::Begin::Testing);
        CHECK(!s.Active());
    }
    {  // SteamVR woke by itself and nobody used the headset: the backlight until someone does.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Tick(6, true) == Standby::Step::Holds);
        CHECK(s.Tick(30, false) == Standby::Step::Woke);
        CHECK(!s.Active());
        CHECK(s.Start(330, true, false) == Standby::Begin::Unheld);
        CHECK(s.Start(630, true, false) == Standby::Begin::Unheld);
        CHECK(!s.Use(640));  // use: nothing to write, but standby is tried again
        CHECK(s.Start(940, true, false) == Standby::Begin::Ask);
    }
    {  // Use right after such a wake is tracking settling, not a person.
        Standby s;
        CHECK(s.Start(0, true, false) == Standby::Begin::Ask);
        CHECK(s.Tick(6, true) == Standby::Step::Holds);
        CHECK(s.Tick(30, false) == Standby::Step::Woke);
        CHECK(!s.Use(30 + Standby::kSettleSecs - 0.1));
        CHECK(s.Start(330, true, false) == Standby::Begin::Unheld);
        CHECK(!s.Use(30 + Standby::kSettleSecs));
        CHECK(s.Start(331, true, false) == Standby::Begin::Ask);
    }
    if (failed) return 1;
    std::printf("standby-test: ok\n");
    return 0;
}
