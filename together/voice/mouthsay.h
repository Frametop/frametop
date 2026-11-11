// The wearer's mouth, from their voice on its way out (docs/together.md, "Avatars"): each 20 ms
// the microphone gives is boiled down to the weights of the mouth's five shapes, as the mirror
// does for the avatar you wear yourself (avatar/face/face.h), and those are told to ft-together
// as a mouth datagram (local/datagrams.h), which sends them in the wearer's poses. The others
// see the mouth of the avatar you sent move as you speak.
//
// A shut mouth is told once and then not again, so nothing is said while the wearer is quiet.
// Nothing of the voice is kept.
//
// The sums only: no microphone or sockets here. Header only.
#pragma once

#include "../../avatar/face/face.h"
#include "../local/datagrams.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace voice {

class MouthSay {
public:
    // The voice as it comes: `n` mono samples at `rate` a second. True when ft-together has
    // something to hear: `out` is the datagram for it.
    bool Hear(const float *samples, size_t n, double rate, uint8_t out[local::kMouthBytes]) {
        float weights[face::kWeights] = {};
        mouth_.Hear(samples, n, rate);
        mouth_.After(double(n) / rate, weights);
        return Tell(weights, out);
    }
    // The microphone closed: the mouth shuts, and what it knew of the room's noise is
    // forgotten. True when ft-together has to hear of it.
    bool Shut(uint8_t out[local::kMouthBytes]) {
        const float shut[face::kWeights] = {};
        mouth_ = face::Mouth();
        return Tell(shut, out);
    }

private:
    static_assert(int(bodyfile::Shape::kOh) - int(bodyfile::Shape::kAa) + 1 == proto::kMouthShapes, "a pose's mouth is the five vowels, in Shape::Id's order");

    bool Tell(const float weights[face::kWeights], uint8_t out[local::kMouthBytes]) {
        bool open = false;
        out[0] = local::kMouthToTogether;
        for (int k = 0; k < proto::kMouthShapes; ++k) {
            const float w = weights[bodyfile::Shape::kAa + k];
            out[1 + k] = uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, std::isfinite(w) ? w : 0.0f)) * 255));
            open = open || out[1 + k];
        }
        const bool tell = open || open_;
        open_ = open;
        return tell;
    }

    face::Mouth mouth_;
    bool open_ = false;   // as ft-together was last told
};

}  // namespace voice
