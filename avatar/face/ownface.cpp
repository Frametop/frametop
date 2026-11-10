// The face of the avatar you wear yourself (ownface.h).
#include "ownface.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace ownface {

void Face::After(double dt, bool hasBlink, float weights[face::kWeights]) {
    float heard[micear::kRate / 4];   // a draw's worth is a few hundred; after a pause in drawing, the newest quarter second
    const size_t n = ear_.Take(heard, sizeof heard / sizeof heard[0]);
    mouth_.Hear(heard, n, micear::kRate);
    mouth_.After(dt, weights);
    face::Lids(blink_.After(dt), hasBlink, weights);
}

int Hear(double seconds) {
    Face f;
    f.Listen("Avatar mouth (check)");
    setvbuf(stdout, nullptr, _IOLBF, 0);
    float w[face::kWeights] = {};
    bool heard = false;
    const double tick = 1.0 / 45;
    for (int i = 0; i < int(seconds / tick); ++i) {
        std::this_thread::sleep_for(std::chrono::duration<double>(tick));
        f.After(tick, true, w);
        heard = heard || f.hearing();
        if (i % 11 == 10)
            std::printf("%s %6.1f dB, room %6.1f dB | aa %.2f ih %.2f ou %.2f ee %.2f oh %.2f\n", f.hearing() ? "hearing" : "no microphone", f.mouth().levelDb(),
                        f.mouth().floorDb(), w[bodyfile::Shape::kAa], w[bodyfile::Shape::kIh], w[bodyfile::Shape::kOu], w[bodyfile::Shape::kEe], w[bodyfile::Shape::kOh]);
    }
    return heard ? 0 : 1;
}

}  // namespace ownface
