// A PC app window's scale (docs/remote-displays.md): the steps its bar menu offers, and Meta+scroll's
// wheel through them. Pure: no OpenVR (screens/tests/appscale-test.cpp). vrappscale.cpp asks ft-layout
// for them.
#pragma once

#include <cmath>
#include <string>

namespace appscale {

constexpr int kPercents[] = {100, 125, 150, 160, 175, 200, 225, 250};
constexpr double kLeast = 0.5, kMost = 4.0;   // what a PC takes (layout/ft_windowscale.py)

inline int Percent(double scale) { return int(std::lround(scale * 100)); }
inline std::string Label(int percent) { return std::to_string(percent) + "%"; }
inline bool Same(double a, double b) { return std::fabs(a - b) < 0.005; }   // (167% is 166.67%)

// The step `by` steps from `scale` (a scale between two steps goes to the one beside it). Past the
// ends it stays: a scale bigger than the steps isn't made smaller by one asking for bigger.
inline double Step(double scale, int by) {
    for (; by > 0; --by) {
        double next = scale;
        for (int p : kPercents)
            if (p / 100.0 > scale && !Same(p / 100.0, scale)) { next = p / 100.0; break; }
        scale = next;
    }
    for (; by < 0; ++by) {
        double next = scale;
        for (int p : kPercents)
            if (p / 100.0 < scale && !Same(p / 100.0, scale)) next = p / 100.0;
        scale = next;
    }
    return scale;
}

// Wheel notches as steps: a smooth wheel's parts add up; a turn the other way starts over.
struct Wheel {
    double part = 0;
    int Add(double notches) {
        if (part * notches < 0) part = 0;
        part += notches;
        const int whole = int(part);
        part -= whole;
        return whole;
    }
};

}  // namespace appscale
