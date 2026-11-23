#include "npurequests.h"

#include "npuclient.h"

#include <algorithm>

std::vector<NpuRequest> npu_requests(const std::vector<int> &hands, bool two_view) {
    // the crop order: each pair at an even position, so no pair straddles a request (requests start every 4th)
    std::vector<int> order;
    std::vector<bool> used(hands.size(), false);
    if (two_view)
        for (size_t i = 0; i < hands.size(); ++i) {
            if (hands[i] == 0 || used[i]) continue;
            for (size_t j = i + 1; j < hands.size(); ++j)
                if (!used[j] && hands[j] == hands[i]) {
                    order.push_back(int(i)), order.push_back(int(j));
                    used[i] = used[j] = true;
                    break;
                }
        }
    const size_t paired = order.size();
    for (size_t i = 0; i < hands.size(); ++i)
        if (!used[i]) order.push_back(int(i));
    std::vector<NpuRequest> reqs;
    for (size_t at = 0; at < order.size(); at += NpuClient::kMaxCrops) {
        NpuRequest r;
        r.n = int(std::min<size_t>(NpuClient::kMaxCrops, order.size() - at));
        for (int c = 0; c < r.n; ++c) r.views[c] = order[at + c];
        if (at + 2 <= paired) r.pairs |= NpuClient::kPair0;
        if (at + 4 <= paired) r.pairs |= NpuClient::kPair1;
        reqs.push_back(r);
    }
    return reqs;
}
