// The palm menu's battery line (user, 2026-11-24: "also show the battery percentage and charging status
// (slow, fast, etc.. )"): how full the headset's battery is, and what the charger does for it, from
// /sys/class/power_supply. What counts is the power that reaches the battery, not what the charger could
// give: worn, the headset takes most of it, and a weak charger loses charge while plugged in.
// Pure: the words (screens/tests/battery-test.cpp); vrstats.cpp reads the files, vrquick.cpp draws them.
#pragma once

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace battery {

// A supply of the headset's own: a Bluetooth keyboard's or controller's battery says scope "Device".
inline bool Headset(const char *scope) { return !scope || std::strncmp(scope, "Device", 6) != 0; }
inline bool IsBattery(const char *type) { return type && std::strncmp(type, "Battery", 7) == 0; }

enum class State { Unknown, OnBattery, Losing, Holding, Slow, Normal, Fast, ToppingOff, Full };

struct Reading {
    int percent = -1;       // -1: no battery read
    bool plugged = false;   // a charger is online
    double watts = NAN;     // into the battery (negative: out of it); NAN unknown
    State state = State::Unknown;   // Settle's
};

// Net watts from the gauge's microvolts and microamps (positive: charging).
inline double Watts(double microvolts, double microamps) { return microvolts * microamps / 1e12; }

// The gauge's average trails by about 6 s: just plugged in or out it still says the old way. So far from
// what flows now (kJumpW), what flows now counts instead.
constexpr double kJumpW = 5;
inline double Net(double averageW, double nowW) {
    if (std::isnan(averageW)) return nowW;
    return std::fabs(nowW - averageW) > kJumpW ? nowW : averageW;   // (nowW NAN: the average)
}

// The Frame's battery is 20 Wh, and gets 10 W at most from a 30 W charger (upower's history, 2026-11-24:
// the top tenth of its charging at 50 to 90% is 8 W and up). Fast: full in about two and a half hours or
// less; slow: more than five. From kTopOff % the charger holds the voltage and tapers the current
// (half of that history is under 2 W there), which isn't a slow charger.
constexpr double kFastW = 8, kSlowW = 4, kHoldW = 0.7;
// From kFull % next to nothing flowing means it's done (the gauge's own status says Full at 100% whatever
// flows, so it isn't asked).
constexpr int kTopOff = 90, kFull = 98;

inline State Of(const Reading &r) {
    if (r.percent < 0) return State::Unknown;
    if (!r.plugged) return State::OnBattery;
    if (std::isnan(r.watts)) return r.percent >= 100 ? State::Full : State::Normal;
    if (r.watts <= -kHoldW) return State::Losing;
    if (r.percent >= 100) return State::Full;
    if (r.watts < kHoldW) return r.percent >= kFull ? State::Full : State::Holding;
    if (r.watts >= kFastW) return State::Fast;
    if (r.watts >= kSlowW) return State::Normal;
    return r.percent >= kTopOff ? State::ToppingOff : State::Slow;
}

// What to show after `was` (Unknown: the first look): `was` while the power is within kStickW of it, so a
// reading by an edge doesn't flip the word back and forth.
constexpr double kStickW = 0.6;
inline State Settle(const Reading &r, State was) {
    for (double by : {-kStickW, kStickW}) {
        Reading near = r;
        near.watts += by;
        if (was != State::Unknown && Of(near) == was) return was;
    }
    return Of(r);
}

// One of /sys/class/power_supply's folders: its files' text (empty: not there).
struct Supply {
    std::string type, scope, online, capacity, microvolts, average, now;
};

// The headset's battery (the first, should there ever be two) and whether any charger of its own is online
// (1, or 2 at a voltage it picked).
inline Reading From(const std::vector<Supply> &supplies, State was) {
    Reading r;
    for (const Supply &s : supplies) {
        if (!Headset(s.scope.c_str())) continue;
        if (!IsBattery(s.type.c_str())) {
            r.plugged |= std::atoi(s.online.c_str()) > 0;
        } else if (r.percent < 0 && !s.capacity.empty()) {
            r.percent = std::clamp(std::atoi(s.capacity.c_str()), 0, 100);
            auto watts = [&](const std::string &ua) {
                return s.microvolts.empty() || ua.empty() ? NAN : Watts(std::atof(s.microvolts.c_str()), std::atof(ua.c_str()));
            };
            r.watts = Net(watts(s.average), watts(s.now));
        }
    }
    r.state = Settle(r, was);
    return r;
}

inline const char *Name(State s) {
    switch (s) {
        case State::OnBattery: return "On battery";
        case State::Losing: return "Losing charge";   // (plugged in, but the headset takes more)
        case State::Holding: return "Not charging";
        case State::Slow: return "Slow charging";
        case State::Normal: return "Charging";
        case State::Fast: return "Fast charging";
        case State::ToppingOff: return "Topping off";
        case State::Full: return "Full";
        case State::Unknown: break;
    }
    return "";
}

// Getting charge: the picture's fill is green.
inline bool Charging(State s) {
    return s == State::Slow || s == State::Normal || s == State::Fast || s == State::ToppingOff || s == State::Full;
}
// Nearly empty with nothing coming in: red.
constexpr int kLow = 15;
inline bool Low(const Reading &r) { return r.percent >= 0 && r.percent <= kLow && !Charging(r.state); }

inline std::string Say(const Reading &r) {
    if (r.percent < 0) return "";
    return std::to_string(r.percent) + "%  " + Name(r.state);
}

}  // namespace battery
