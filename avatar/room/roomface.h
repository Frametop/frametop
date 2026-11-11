// The face of someone in a shared room, in the avatar they sent: its mouth is as their pose
// tells it (together/proto/posewire.h: their own Frame makes it of their voice), and its eyes
// blink here, on their own (../face/face.h). The weights are a model file's face shapes
// (bodyfile::Shape::Id), for bodygpu::Avatar::Shape.
//
// The pose is a stranger's: whatever its five bytes say, the mouth is never opened past whole.
//
// The sums only. Header only.
#pragma once

#include "../../together/proto/posewire.h"
#include "../face/face.h"

#include <algorithm>
#include <cstdint>

namespace roomface {

class Face {
public:
    // Two faces with different seeds don't blink together.
    explicit Face(uint32_t seed = 1) : blink_(seed) {}

    // Their face `dt` seconds on from the call before, with the mouth their pose told, into
    // the weights. `hasBlink`: the body has the shape that shuts both eyes (face::Lids).
    void After(const uint8_t mouth[proto::kMouthShapes], double dt, bool hasBlink, float weights[face::kWeights]) {
        static_assert(int(bodyfile::Shape::kOh) - int(bodyfile::Shape::kAa) + 1 == proto::kMouthShapes, "a pose's mouth is the five vowels, in Shape::Id's order");
        std::fill(weights, weights + face::kWeights, 0.0f);
        int sum = 0;
        for (int k = 0; k < proto::kMouthShapes; ++k) sum += mouth[k];
        const float whole = float(std::max(sum, 255));
        for (int k = 0; k < proto::kMouthShapes; ++k) weights[bodyfile::Shape::kAa + k] = float(mouth[k]) / whole;
        face::Lids(blink_.After(dt), hasBlink, weights);
    }

private:
    face::Blink blink_;
};

}  // namespace roomface
