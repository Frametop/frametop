// What someone lets the video of their panels have at the most (docs/together.md, "What a
// guest sees", Quality): the bits a second a panel's bitrate may climb to (pace.h), and the
// pictures a second it is encoded at while someone has it focused (attention.h). Everyone picks
// for their own panels, and it is Balanced until they do.
//
// The names and the numbers only. Header only.
#pragma once

#include <string>

namespace flow {

struct Quality {
    const char *name;
    int bits;       // a second, a panel
    int pictures;   // a second
};

inline constexpr Quality kLow{"low", 1'000'000, 15}, kBalanced{"balanced", 3'000'000, 30}, kHigh{"high", 8'000'000, 60};

// The one of that name, or none.
inline const Quality *QualityNamed(const std::string &name) {
    for (const Quality *q : {&kLow, &kBalanced, &kHigh})
        if (name == q->name) return q;
    return nullptr;
}

}  // namespace flow
