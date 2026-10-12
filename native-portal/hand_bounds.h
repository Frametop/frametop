#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace hand_bounds {
constexpr unsigned kGroups = 8;
inline void range(unsigned count, unsigned group, unsigned &begin, unsigned &end) {
    count = std::min(count, 64u);
    const unsigned split = (count + 1) / 2;
    const unsigned first = group < 4 ? 0 : split, last = group < 4 ? split : count;
    const unsigned stride = (last - first + 3) / 4;
    begin = std::min(last, first + (group % 4) * stride);
    end = std::min(last, begin + stride);
}
// Four conservative spheres per contiguous half of the capsule list.
// Every endpoint sphere is inside its bound; their tapered convex hull is too.
// Union coverage is independent of capsule order or hand ownership.
inline void compute(const float *capsules, unsigned count, float output[kGroups * 4]) {
    count = std::min(count, 64u);
    for (unsigned group = 0; group < kGroups; ++group) {
        unsigned begin, end;
        range(count, group, begin, end);
        float *sphere = output + group * 4;
        std::fill(sphere, sphere + 4, 0.f);
        if (begin == end) continue;
        double low[3]{1e100, 1e100, 1e100}, high[3]{-1e100, -1e100, -1e100};
        for (unsigned i = begin; i < end; ++i)
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
                const float *point = capsules + i * 8 + endpoint * 4;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], double(point[axis]));
                    high[axis] = std::max(high[axis], double(point[axis]));
                }
            }
        for (unsigned axis = 0; axis < 3; ++axis) sphere[axis] = float((low[axis] + high[axis]) / 2);
        double radius = 0;
        for (unsigned i = begin; i < end; ++i)
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
                const float *point = capsules + i * 8 + endpoint * 4;
                double distance = 0;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const double delta = double(point[axis]) - sphere[axis];
                    distance += delta * delta;
                }
                radius = std::max(radius, std::sqrt(distance) + point[3]);
            }
        // Outward rounding plus 1 mm covers float shader error near tangencies.
        sphere[3] = std::nextafter(float(radius + .001), std::numeric_limits<float>::infinity());
    }
}
}
