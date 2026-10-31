// A hand's shape from its 21 joints (MediaPipe's landmark order, as handcut::Hand's `p`), shared
// by handinput, palmmenu and grabthat. Header-only and templated on the hand (anything with
// `float p[21][3]` and `bool right`), so tests need neither handcut.h nor OpenVR.
#pragma once

#include <algorithm>
#include <cmath>

namespace handshape {

// MediaPipe's joints. A finger's joints follow its knuckle (MCP): +1 PIP, +2 DIP, +3 tip.
enum Joint : int {
    kWrist = 0,
    kThumbCmc = 1, kThumbMcp = 2, kThumbIp = 3, kThumbTip = 4,
    kIndexMcp = 5, kIndexPip = 6, kIndexDip = 7, kIndexTip = 8,
    kMiddleMcp = 9, kMiddlePip = 10, kMiddleDip = 11, kMiddleTip = 12,
    kRingMcp = 13, kRingPip = 14, kRingDip = 15, kRingTip = 16,
    kLittleMcp = 17, kLittlePip = 18, kLittleDip = 19, kLittleTip = 20,
    kJoints = 21,
};
constexpr int kTipFromMcp = 3;

inline double Dist3(const float a[3], const float b[3]) {
    return std::sqrt((double(a[0]) - b[0]) * (double(a[0]) - b[0]) + (double(a[1]) - b[1]) * (double(a[1]) - b[1]) +
                     (double(a[2]) - b[2]) * (double(a[2]) - b[2]));
}

// A finger's extension: its tip's distance from the wrist over its knuckle's (`mcp`: kIndexMcp,
// kMiddleMcp, kRingMcp or kLittleMcp). About 1 curled, 1.7-1.9 straight.
template <class H> double FingerExtension(const H &h, int mcp) {
    return Dist3(h.p[mcp + kTipFromMcp], h.p[kWrist]) / std::max(1e-6, Dist3(h.p[mcp], h.p[kWrist]));
}

// How open a hand is: its four fingers' mean extension (a fist about 1, a pointing hand about
// 1.3, an open one 1.7-1.9).
template <class H> double Openness(const H &h) {
    return (FingerExtension(h, kIndexMcp) + FingerExtension(h, kMiddleMcp) + FingerExtension(h, kRingMcp) +
            FingerExtension(h, kLittleMcp)) / 4;
}

// The middle, ring and little fingers' mean extension, without the index: a pointing hand's are
// curled, an open one's straight. The palm menu's "open" is this one, on purpose: its thresholds
// were set on it, and a hand turned palm-up toward the eyes often has its index bent.
template <class H> double Curl3(const H &h) {
    return (FingerExtension(h, kMiddleMcp) + FingerExtension(h, kRingMcp) + FingerExtension(h, kLittleMcp)) / 3;
}

// The palm's unit normal, out of the palm (wrist to index knuckle, cross wrist to little knuckle,
// turned round for the left hand). Zero for a degenerate hand.
template <class H> void PalmNormal(const H &h, double out[3]) {
    double a[3], b[3];
    for (int i = 0; i < 3; ++i)
        a[i] = double(h.p[kIndexMcp][i]) - h.p[kWrist][i], b[i] = double(h.p[kLittleMcp][i]) - h.p[kWrist][i];
    const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (h.right ? 1 : -1);
    for (int i = 0; i < 3; ++i) out[i] = l != 0 ? n[i] / l : 0;
}

}  // namespace handshape
