// How NpuLandmarks groups a frame's views into ft-npud requests: up to 4 crops each, and for a two-view student
// (two_view.py mode f) each hand's first two views as a pair the daemon reads together.
#pragma once

#include <cstdint>
#include <vector>

// One ft-npud request: views[0..n) in crop order, and its pair bits (NpuClient::kPair0 / kPair1).
struct NpuRequest {
    int views[4]{};
    int n = 0;
    uint8_t pairs = 0;
};

// The requests for views of hands (0: none known), as few as the views allow. With two_view, each hand's first
// two views go together at crops (0, 1) or (2, 3) with that pair's bit; every other view is read alone. Without,
// the views in order.
std::vector<NpuRequest> npu_requests(const std::vector<int> &hands, bool two_view);
