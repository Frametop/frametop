// A vertex's weights as a model file holds them (handpart.cpp, bodypart.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace importer {

// The four heaviest of `acc`'s `count` joints, out of 255 and adding up to it (the largest
// remainders rounded up). A vertex nothing moves hangs from joint 0.
inline void PackWeights(const float *acc, int count, uint8_t joints[4], uint8_t weights[4]) {
    int order[4] = {0, 0, 0, 0};   // the heaviest four, heaviest first; the earlier joint of two alike
    bool taken[256] = {};
    int found = 0;
    for (; found < 4 && found < count; ++found) {
        int best = -1;
        for (int j = 0; j < count; ++j)
            if (!taken[j] && (best < 0 || acc[j] > acc[best])) best = j;
        order[found] = best, taken[best] = true;
    }
    double total = 0;
    for (int k = 0; k < found; ++k) total += acc[order[k]];
    if (total <= 0) {
        const uint8_t j[4] = {0, 0, 0, 0}, w[4] = {255, 0, 0, 0};
        std::memcpy(joints, j, 4), std::memcpy(weights, w, 4);
        return;
    }
    double exact[4] = {};
    int q[4] = {}, sum = 0;
    for (int k = 0; k < found; ++k) exact[k] = 255.0 * acc[order[k]] / total, q[k] = int(exact[k]), sum += q[k];
    int byRest[4] = {0, 1, 2, 3};
    std::stable_sort(byRest, byRest + 4, [&](int a, int c) { return exact[a] - q[a] > exact[c] - q[c]; });
    for (int k = 0; k < 255 - sum; ++k) ++q[byRest[k % 4]];
    for (int k = 0; k < 4; ++k) weights[k] = uint8_t(q[k]), joints[k] = q[k] ? uint8_t(order[k]) : 0;
}

}  // namespace importer
