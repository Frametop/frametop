// Unit tests for hands hanging at the side (pointer/helper/hanging.h): the eye 1.2 m up, hands
// where the 2026-11-22 narrated take had them (knuckles under the eyes, forearm angle).
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/hanging-test.cpp).
#include "pointer/helper/hanging.h"

#include <cmath>
#include <cstdio>
#include <utility>

using namespace hanging;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

namespace {

const V3 kEye{0, 1.2, 0};

// The knuckles below the eyes and the forearm at deg below level (a 0.27 m forearm).
Check At(double below, double deg, const Params &p = {}, bool hasElbow = true) {
    const V3 knuckle{0.2, kEye.y - below, -0.15};
    const double r = deg * M_PI / 180, len = 0.27;
    const V3 elbow{knuckle.x, knuckle.y + len * std::sin(r), knuckle.z + len * std::cos(r)};
    return Test(p, kEye, knuckle, elbow, hasElbow);
}

}  // namespace

int main() {
    // the take's false taps: hanging
    for (auto [below, deg] : {std::pair{0.76, 40.6}, {0.70, 60.8}, {0.84, 73.5}, {0.65, 39.8}, {1.30, 57.6}}) {
        const Check c = At(below, deg);
        CHECK(c.hanging, "%.2f m, %.1f deg: hanging", below, deg);
        CHECK(std::fabs(c.below - below) < 1e-9 && std::fabs(c.forearmDeg - deg) < 1e-6, "numbers: %.3f %.2f", c.below, c.forearmDeg);
    }
    // the wearer's taps (raised forearm) and typing (about level): not
    for (auto [below, deg] : {std::pair{0.28, -46.7}, {0.33, -31.8}, {0.38, -29.4}, {0.30, -15.5}, {0.53, 6.4}, {0.50, 5.3}})
        CHECK(!At(below, deg).hanging, "%.2f m, %.1f deg: a tap", below, deg);
    // both needed: low with a level arm, or a dropped arm held high
    CHECK(!At(0.70, 10).hanging, "low, arm level");
    CHECK(!At(0.50, 45).hanging, "arm down, not low");
    CHECK(At(0.59, 21).hanging, "just past both");
    CHECK(!At(0.57, 60).hanging, "just short of the height");
    // no elbow, or off
    CHECK(!At(0.80, 60, {}, false).hanging, "no elbow");
    Params off;
    off.below = 0;
    CHECK(!At(0.80, 60, off).hanging, "off");
    // the room's up, not the head's: only heights and the horizontal matter (a forearm straight down)
    {
        const Check c = Test({}, kEye, {0.25, 0.4, 0}, {0.25, 0.67, 0}, true);
        CHECK(c.hanging && std::fabs(c.forearmDeg - 90) < 1e-9, "straight down: %.1f", c.forearmDeg);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hanging-test: all passed\n");
    return 0;
}
