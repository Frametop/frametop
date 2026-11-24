// A PC app window's scale (docs/remote-displays.md): the steps its bar menu offers, and Meta+scroll's
// wheel through them. Pure: no OpenVR (screens/tests/appscale-test.cpp). vrappscale.cpp asks ft-layout
// for them.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace appscale {

constexpr int kPercents[] = {100, 125, 150, 160, 175, 200, 225, 250};
constexpr int kNudge = 16;   // pixels a side moves at most to take a scale (layout/ft_windowscale.py)

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

// A panel's scale as its menu says it, and what's asked of ft-layout for it. A spin of the wheel is one
// change on the PC: the last step, once it rests. One ask is out at a time, so they come in order, and an
// answer never takes back a scale picked since.
struct Asking {
    static constexpr int64_t kRestMs = 220;
    static constexpr int64_t kAnswerMs = 30000;   // an ask with no answer (ft-layout waits 25 s for the PC)

    double scale = 1;
    Wheel wheel;
    int64_t askAt = -1;     // ms: ask for it then (-1: nothing to ask)
    int64_t askedAt = -1;   // an ask is out since then

    void Pick(double to, int64_t now) {
        if (!Same(to, scale)) scale = to, askAt = now;
    }
    void Scroll(double notches, int64_t now) {
        const double to = Step(scale, wheel.Add(notches));
        if (!Same(to, scale)) scale = to, askAt = now + kRestMs;
    }
    // True once when it's time to ask for `scale`.
    bool Due(int64_t now) {
        if (askAt < 0 || now < askAt || (askedAt >= 0 && now - askedAt < kAnswerMs)) return false;
        askAt = -1, askedAt = now;
        return true;
    }
    // ft-layout said it: the PC's scale now.
    void Said(double s) {
        askedAt = -1;
        if (askAt < 0) scale = s;
    }
};

// A size that takes a scale, for one asked for (a corner dragged to it): a few pixels from it.
inline bool Near(int w, int h, int askedW, int askedH) {
    return std::abs(w - askedW) <= kNudge && std::abs(h - askedH) <= kNudge;
}

}  // namespace appscale
