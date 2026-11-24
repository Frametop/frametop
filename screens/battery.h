// The palm menu's battery line (user, 2026-11-24: "also show the battery percentage and charging status
// (slow, fast, etc.. )"): how full the headset's battery is, and what the charger does for it, from
// /sys/class/power_supply. What counts is the power that reaches the battery, not what the charger could
// give: worn, the headset takes most of it, and a weak charger loses charge while plugged in.
// Pure: the words (screens/tests/battery-test.cpp); vrstats.cpp reads the files, vrquick.cpp draws them.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace battery {

// A supply of the headset's own: a Bluetooth keyboard's or controller's battery says scope "Device".
inline bool Headset(const char *scope) { return !scope || std::strncmp(scope, "Device", 6) != 0; }
inline bool IsBattery(const char *type) { return type && std::strncmp(type, "Battery", 7) == 0; }

struct Reading {
    int percent = -1;       // -1: no battery read
    bool plugged = false;   // a charger is online
    bool full = false;      // the battery's status is Full (the charger's own goes on saying Charging)
    double watts = NAN;     // into the battery (negative: out of it), the gauge's average; NAN unknown
};

// Net watts from the gauge's microvolts and microamps (positive: charging).
inline double Watts(double microvolts, double microamps) { return microvolts * microamps / 1e12; }

enum class State { Unknown, OnBattery, Losing, Holding, Slow, Normal, Fast, ToppingOff, Full };

// The Frame's battery is 20 Wh, and gets 10 W at most from a 30 W charger (upower's history, 2026-11-24:
// the top tenth of its charging at 50 to 90% is 8 W and up). Fast: full in about two and a half hours or
// less; slow: more than five. From kTopOff % the charger holds the voltage and tapers the current
// (half of that history is under 2 W there), which isn't a slow charger.
constexpr double kFastW = 8, kSlowW = 4, kHoldW = 0.7;
constexpr int kTopOff = 90;

inline State Of(const Reading &r) {
    if (r.percent < 0) return State::Unknown;
    if (!r.plugged) return State::OnBattery;
    if (r.full) return State::Full;
    if (std::isnan(r.watts)) return State::Normal;
    if (r.watts <= -kHoldW) return State::Losing;
    if (r.percent >= kTopOff && r.watts < kSlowW) return State::ToppingOff;
    if (r.watts < kHoldW) return State::Holding;
    return r.watts >= kFastW ? State::Fast : r.watts < kSlowW ? State::Slow : State::Normal;
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
inline bool Low(const Reading &r) { return r.percent >= 0 && r.percent <= kLow && !Charging(Of(r)); }

inline std::string Say(const Reading &r) {
    if (r.percent < 0) return "";
    return std::to_string(r.percent) + "%  " + Name(Of(r));
}

}  // namespace battery
