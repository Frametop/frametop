// handshape.h: the joint numbers, finger extension, the four- and three-finger openness, and the
// palm's normal for each side, on made-up hands.
#include "../handshape.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

struct FakeHand {
    float p[21][3] = {};
    bool right = true;
};

int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) std::printf("FAILED: %s\n", what), ++failures;
}
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// A flat hand in the x-y plane: the wrist at 0, each knuckle 0.1 m up (y) and its tip `reach`
// metres from the wrist, straight out from it.
FakeHand Flat(const double reach[4], bool right) {
    FakeHand h;
    h.right = right;
    for (int f = 0; f < 4; ++f) {
        const int mcp = handshape::kIndexMcp + 4 * f;
        const float x = 0.02f * (f - 1.5f);
        h.p[mcp][0] = x, h.p[mcp][1] = 0.1f;
        const double l = std::hypot(x, 0.1);
        h.p[mcp + handshape::kTipFromMcp][0] = float(x / l * reach[f]);
        h.p[mcp + handshape::kTipFromMcp][1] = float(0.1 / l * reach[f]);
    }
    return h;
}

}  // namespace

int main() {
    using namespace handshape;
    Check(kWrist == 0 && kThumbTip == 4 && kIndexMcp == 5 && kIndexTip == 8 && kMiddleMcp == 9 &&
              kMiddleTip == 12 && kRingMcp == 13 && kRingTip == 16 && kLittleMcp == 17 && kLittleTip == 20 &&
              kJoints == 21,
          "MediaPipe's joint numbers");
    for (int mcp : {kIndexMcp, kMiddleMcp, kRingMcp, kLittleMcp}) Check(mcp + kTipFromMcp == mcp + 3, "tip = mcp + 3");

    const float a[3] = {0, 0, 0}, b[3] = {3, 4, 12};
    Check(Near(Dist3(a, b), 13), "Dist3 3-4-12-13");

    // Extensions 1.8 (index), 1.6, 1.4, 1.2: openness is their mean, Curl3 the last three's.
    double e[4], reach[4];
    for (int f = 0; f < 4; ++f) e[f] = 1.8 - 0.2 * f;
    FakeHand ref = Flat(e, true);
    for (int f = 0; f < 4; ++f) {
        const int mcp = kIndexMcp + 4 * f;
        reach[f] = e[f] * Dist3(ref.p[mcp], ref.p[kWrist]);
    }
    const FakeHand h = Flat(reach, true);
    for (int f = 0; f < 4; ++f) Check(std::fabs(FingerExtension(h, kIndexMcp + 4 * f) - e[f]) < 1e-5, "extension");
    Check(std::fabs(Openness(h) - 1.5) < 1e-5, "Openness: four fingers' mean");
    Check(std::fabs(Curl3(h) - 1.4) < 1e-5, "Curl3: middle, ring and little only");

    // A wrist on a knuckle: no division by zero.
    FakeHand flat{};
    Check(std::isfinite(FingerExtension(flat, kIndexMcp)), "degenerate hand: finite");

    // The palm's normal: wrist->index x wrist->little is +z or -z here, flipped for the left hand.
    double n[3], m[3];
    PalmNormal(h, n);
    FakeHand left = h;
    left.right = false;
    PalmNormal(left, m);
    Check(Near(std::hypot(n[0], n[1], n[2]), 1), "a unit normal");
    Check(Near(n[0], 0) && Near(n[1], 0) && Near(std::fabs(n[2]), 1), "the flat palm's normal is along z");
    Check(Near(n[2], -m[2]), "the left hand's is turned round");
    PalmNormal(flat, n);
    Check(n[0] == 0 && n[1] == 0 && n[2] == 0, "degenerate palm: zero");

    if (failures) return EXIT_FAILURE;
    std::printf("handshape: ok\n");
    return 0;
}
