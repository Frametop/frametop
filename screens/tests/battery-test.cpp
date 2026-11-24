// The palm card's battery line (battery.h): whose battery counts, what a charger does for it, the words,
// and its picture (batteryart.h).
#include "../batteryart.h"

#include <cstdio>

int main() {
    int failed = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        failed += !ok;
    };
    using namespace battery;
    auto at = [](int pct, double w, bool plugged = true, bool full = false) { return Reading{pct, plugged, full, w}; };

    check("the headset's own battery", IsBattery("Battery\n") && Headset("") && Headset(nullptr) && Headset("System\n"));
    check("a keyboard's isn't", !Headset("Device\n"));
    check("a charger isn't a battery", !IsBattery("Unknown\n") && !IsBattery("USB\n") && !IsBattery(nullptr));
    check("8.8 V at 157.5 mA: 1.4 W in", std::fabs(Watts(8807500, 157500) - 1.387) < 0.001);
    check("...and out", Watts(8000000, -2000000) == -16);

    check("no battery read", Of(at(-1, 5)) == State::Unknown && Say(at(-1, 5)).empty());
    check("unplugged", Of(at(63, -12, false)) == State::OnBattery && Say(at(63, -12, false)) == "63%  On battery");
    check("...even at 100%", Of(at(100, -12, false)) == State::OnBattery);
    check("10 W: fast", Of(at(50, 10)) == State::Fast && Say(at(50, 10)) == "50%  Fast charging");
    check("6 W: charging", Of(at(50, 6)) == State::Normal);
    check("2 W: slow", Of(at(50, 2)) == State::Slow);
    check("the steps' edges", Of(at(50, kFastW)) == State::Fast && Of(at(50, kSlowW)) == State::Normal &&
                                  Of(at(50, kHoldW)) == State::Slow && Of(at(50, -kHoldW)) == State::Losing);
    check("plugged in, next to nothing either way: not charging", Of(at(50, 0.2)) == State::Holding && Of(at(50, -0.2)) == State::Holding);
    check("plugged in but the headset takes more: losing charge", Of(at(50, -5)) == State::Losing && Of(at(97, -5)) == State::Losing);
    check("the taper by full isn't slow", Of(at(95, 1.8)) == State::ToppingOff && Of(at(99, -0.1)) == State::ToppingOff);
    check("...still strong there, its own word", Of(at(91, 9)) == State::Fast && Of(at(91, 5)) == State::Normal);
    check("full", Of(at(100, 0.3, true, true)) == State::Full && Say(at(100, 0.3, true, true)) == "100%  Full");
    check("no current read: just charging", Of(at(50, NAN)) == State::Normal);

    check("green while it gets charge", Charging(State::Slow) && Charging(State::Full) && !Charging(State::Losing) &&
                                            !Charging(State::Holding) && !Charging(State::OnBattery));
    check("red nearly empty", Low(at(15, -9, false)) && Low(at(10, -3)) && !Low(at(16, -9, false)) && !Low(at(5, 6)) && !Low(at(-1, 0)));

    using batteryart::FillWidth;
    check("the fill: none, a sliver, half, all", FillWidth(0) == 0 && FillWidth(1) == 2 && FillWidth(50) == 16 && FillWidth(100) == 32 &&
                                                    FillWidth(140) == 32 && FillWidth(-1) == 0);
    {
        constexpr int w = 60, h = 30;
        const uint8_t bg[4] = {24, 24, 30, 230};
        std::vector<uint8_t> px(size_t(w) * h * 4, 0);
        auto p = [&](int x, int y) { return &px[(size_t(y) * w + x) * 4]; };
        batteryart::Draw(px, w, h, 4, 15, at(50, 10), bg);
        check("half full, charging: green then the card's own", p(10, 15)[1] == 191 && p(30, 15)[1] == 24);
        check("the outline, and the terminal", p(20, 6)[0] == 225 && p(4 + batteryart::kW - 3, 15)[0] == 225);
        check("nothing past it", p(2, 15)[3] == 0 && p(4 + batteryart::kW + 1, 15)[3] == 0 && p(20, 3)[3] == 0);
        batteryart::Draw(px, w, h, 4, 15, at(10, -9, false), bg);
        check("low: red, and the green gone", p(9, 15)[0] == 222 && p(20, 15)[1] == 24);
    }

    std::printf(failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
