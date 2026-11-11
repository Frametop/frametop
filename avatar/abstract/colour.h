// The colour of each place in a shared room (docs/together.md): the abstract body's, and the
// mark of where that person points (together/points). Told apart at a glance, and by someone
// who doesn't see red from green.
//
// Header only.
#pragma once

namespace abstract {

constexpr int kColours = 4;   // one for each place in a room

// The colour of place `place` (any number: they go round), red, green and blue, 0 to 1.
inline const float *Colour(int place) {
    static const float kColour[kColours][3] = {{0.16f, 0.66f, 0.72f}, {0.94f, 0.58f, 0.20f}, {0.60f, 0.47f, 0.90f}, {0.90f, 0.40f, 0.56f}};
    return kColour[((place % kColours) + kColours) % kColours];
}

}  // namespace abstract
