// The face of someone in a shared room (avatar/room/roomface.h): the mouth their pose tells,
// never opened past whole, and the blinks made where they are drawn. The sums only.
#include "../room/roomface.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static void faces() {
    using bodyfile::Shape;
    roomface::Face face(3);
    float w[face::kWeights];
    std::fill(w, w + face::kWeights, 9.0f);
    const uint8_t shut[proto::kMouthShapes] = {}, says[proto::kMouthShapes] = {255, 0, 51, 0, 0}, half[proto::kMouthShapes] = {0, 0, 0, 102, 0};
    face.After(shut, 0, true, w);
    bool none = true;
    for (float v : w) none = none && v == 0;
    check("someone who says nothing has a face at rest", none);
    face.After(half, 0, true, w);
    check("the mouth is as their pose tells it: each vowel's shape by its weight", std::fabs(w[Shape::kEe] - 0.4f) < 1e-6 && w[Shape::kAa] == 0 && w[Shape::kIh] == 0 &&
                                                                                    w[Shape::kOu] == 0 && w[Shape::kOh] == 0 && w[Shape::kBlink] == 0);
    face.After(says, 0, true, w);
    check("...and never opened past whole, whatever a pose says", std::fabs(w[Shape::kAa] + w[Shape::kOu] - 1) < 1e-6 && std::fabs(w[Shape::kAa] - 5 * w[Shape::kOu]) < 1e-5);
    const uint8_t all[proto::kMouthShapes] = {255, 255, 255, 255, 255};
    face.After(all, 0, true, w);
    float sum = 0;
    for (int id = Shape::kAa; id <= int(Shape::kOh); ++id) sum += w[id];
    check("...even with every shape whole", std::fabs(sum - 1) < 1e-5 && std::fabs(w[Shape::kIh] - 0.2f) < 1e-6);

    // their eyes blink on their own, here: thirty seconds as they are drawn
    int blinks = 0;
    bool shutNow = false, both = true, mouthKept = true;
    for (int i = 0; i < 900; ++i) {
        face.After(half, 1 / 30.0, true, w);
        both = both && w[Shape::kBlinkLeft] == 0 && w[Shape::kBlinkRight] == 0 && w[Shape::kBlink] >= 0 && w[Shape::kBlink] <= 1;
        mouthKept = mouthKept && std::fabs(w[Shape::kEe] - 0.4f) < 1e-6;
        const bool is = w[Shape::kBlink] > 0.5f;
        blinks += is && !shutNow, shutNow = is;
    }
    check("their eyes blink every few seconds, with the shape that shuts both", blinks >= 3 && blinks <= 20 && both);
    check("...while the mouth stays as it was told", mouthKept);
    bool each = false, never = true;
    for (int i = 0; i < 900; ++i) {
        face.After(shut, 1 / 30.0, false, w);
        each = each || (w[Shape::kBlinkLeft] > 0.5f && w[Shape::kBlinkLeft] == w[Shape::kBlinkRight]);
        never = never && w[Shape::kBlink] == 0;
    }
    check("a face without that shape shuts each eye by its own", each && never);
    roomface::Face a(1), b(2);
    float wa[face::kWeights], wb[face::kWeights];
    bool apart = false;
    for (int i = 0; i < 900; ++i) {
        a.After(shut, 1 / 30.0, true, wa), b.After(shut, 1 / 30.0, true, wb);
        apart = apart || ((wa[Shape::kBlink] > 0.5f) != (wb[Shape::kBlink] > 0.5f));
    }
    check("two people don't blink together", apart);
}

int main() {
    faces();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
