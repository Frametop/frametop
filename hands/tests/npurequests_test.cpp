// track/npurequests.cpp: one hand's two views share a request at crops (0, 1) or (2, 3) with that pair's bit, other
// views go alone, and no frame takes more requests than its views need.
//   build/npurequests-test
#include "../track/npuclient.h"
#include "../track/npurequests.h"

#include <cstdio>
#include <set>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

// every view exactly once, every pair bit over two views of one known hand, no hand paired twice, and the fewest
// requests
static void sane(const std::vector<int> &hands, const std::vector<NpuRequest> &rs) {
    std::multiset<int> seen;
    std::set<int> paired;
    for (const NpuRequest &r : rs) {
        CHECK(r.n >= 1 && r.n <= NpuClient::kMaxCrops);
        for (int c = 0; c < r.n; ++c) seen.insert(r.views[c]);
        for (int p = 0; p < 2; ++p)
            if (r.pairs & (p ? NpuClient::kPair1 : NpuClient::kPair0)) {
                CHECK(2 * p + 1 < r.n);
                const int a = hands[r.views[2 * p]], b = hands[r.views[2 * p + 1]];
                CHECK(a > 0 && a == b);
                CHECK(paired.insert(a).second);
            }
    }
    CHECK(seen.size() == hands.size());
    for (int i = 0; i < int(hands.size()); ++i) CHECK(seen.count(i) == 1);
    CHECK(rs.size() == (hands.size() + 3) / 4);
}

int main() {
    // one-view: in order, no bits
    auto rs = npu_requests({3, 3, 4, 4, 5}, false);
    sane({3, 3, 4, 4, 5}, rs);
    CHECK(rs.size() == 2 && rs[0].pairs == 0 && rs[0].views[0] == 0 && rs[1].views[0] == 4);
    // two hands, two views each, interleaved as the tracker's has_lm sort may leave them
    const std::vector<int> two = {7, 9, 7, 9};
    rs = npu_requests(two, true);
    sane(two, rs);
    CHECK(rs.size() == 1 && rs[0].pairs == (NpuClient::kPair0 | NpuClient::kPair1));
    CHECK(rs[0].views[0] == 0 && rs[0].views[1] == 2 && rs[0].views[2] == 1 && rs[0].views[3] == 3);
    // unknown hands (search and palm views) are never paired, even with each other
    const std::vector<int> unknown = {0, 0, 0};
    rs = npu_requests(unknown, true);
    sane(unknown, rs);
    CHECK(rs[0].pairs == 0);
    // a hand seen once, a pair, and a third view of the pair's hand: one pair, the rest alone
    const std::vector<int> mixed = {5, 2, 2, 2, 0, 8, 8};
    rs = npu_requests(mixed, true);
    sane(mixed, rs);
    CHECK(rs.size() == 2 && rs[0].pairs == (NpuClient::kPair0 | NpuClient::kPair1) && rs[1].pairs == 0);
    // three pairs: the third lands at the second request's crops (0, 1)
    const std::vector<int> six = {1, 1, 2, 2, 3, 3};
    rs = npu_requests(six, true);
    sane(six, rs);
    CHECK(rs.size() == 2 && rs[1].n == 2 && rs[1].pairs == NpuClient::kPair0);
    // one hand seen four times (replays' --views 4): its first two views pair, the other two go alone
    const std::vector<int> four = {5, 5, 5, 5};
    rs = npu_requests(four, true);
    sane(four, rs);
    CHECK(rs.size() == 1 && rs[0].pairs == NpuClient::kPair0 && rs[0].views[0] == 0 && rs[0].views[1] == 1);
    // the tracker's step-local ids (-1, -2) are not hands yet
    const std::vector<int> local = {-1, -1, -2, -2};
    rs = npu_requests(local, true);
    sane(local, rs);
    CHECK(rs[0].pairs == 0);
    // five views, two pairs: the fifth view alone in a second request
    const std::vector<int> five = {1, 2, 1, 2, 3};
    rs = npu_requests(five, true);
    sane(five, rs);
    CHECK(rs.size() == 2 && rs[1].n == 1 && rs[1].pairs == 0 && rs[1].views[0] == 4);
    CHECK(npu_requests({}, true).empty());
    std::printf(failures ? "npurequests: %d failures\n" : "npurequests: ok\n", failures);
    return failures != 0;
}
