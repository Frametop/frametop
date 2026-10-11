// Unit tests for the hands feed's age (pointer/helper/feedage.h): the helper's loop reads its
// clock at the top and ft-hands' files later in the pass, so a frame can be newer than "now".
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/feedage-test.cpp).
#include "pointer/helper/feedage.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

namespace {

constexpr uint64_t kMs = 1'000'000ull, kLive = 1'500 * kMs;

// The check before the fix, for the replay below.
bool OldLive(uint64_t nowNs, uint64_t publishNs) { return publishNs && nowNs - publishNs < kLive; }

// The helper's loop against ft-hands publishing every periodMs: each pass reads the clock (tnow),
// then the hands file readMs later, then asks Live(tnow). Counts the passes that read the feed as
// stale while ft-hands kept publishing. live: the check under test.
template <class F>
int StalePasses(F live, uint64_t periodMs, uint64_t readMs, int passes) {
    const uint64_t t0 = 1'000'000 * kMs;  // CLOCK_MONOTONIC some 17 minutes after boot
    int stale = 0;
    for (int k = 0; k < passes; ++k) {
        const uint64_t tnow = t0 + uint64_t(k) * 8 * kMs + uint64_t(k % 7) * 300'000;  // 8 ms, jittered
        const uint64_t readAt = tnow + readMs * kMs / 4 + uint64_t(k % 5) * 250'000;    // later in the pass
        // the newest frame ft-hands wrote by readAt (it has run for a while: frames at
        // base + n * period, base 10 s before t0 plus 0.7 ms)
        const uint64_t base = t0 - 10'000 * kMs + 700'000;
        const uint64_t published = base + (readAt - base) / (periodMs * kMs) * (periodMs * kMs);
        if (!live(tnow, published)) ++stale;
    }
    return stale;
}

}  // namespace

int main() {
    using hin::AgeNs;
    using hin::Fresh;

    // ages
    CHECK(AgeNs(2'000 * kMs, 1'000 * kMs) == 1'000 * kMs, "1 s old");
    CHECK(AgeNs(1'000 * kMs, 1'000 * kMs) == 0, "published now: age 0");
    CHECK(AgeNs(1'000 * kMs, 1'000 * kMs + 300'000) == 0, "published 0.3 ms after now: age 0, not 584 years");

    // fresh
    CHECK(!Fresh(5'000 * kMs, 0, kLive), "nothing published yet");
    CHECK(Fresh(5'000 * kMs, 5'000 * kMs - 1'499 * kMs, kLive), "1.499 s old: fresh");
    CHECK(!Fresh(5'000 * kMs, 5'000 * kMs - 1'500 * kMs, kLive), "1.5 s old: stale");
    CHECK(!Fresh(500'000 * kMs, 440'000 * kMs, kLive), "ft-hands stopped a minute ago");
    CHECK(Fresh(5'000 * kMs, 5'000 * kMs + 2 * kMs, kLive), "a frame written after the loop read its clock");

    // The 2026-10-10 flicker: the old check calls the feed stale on passes whose file read caught a
    // frame newer than tnow; the new one never does, at 30 Hz, 15 Hz or ft-hands' idle 1 Hz.
    const auto newLive = [](uint64_t now, uint64_t pub) { return Fresh(now, pub, kLive); };
    const int passes = 7 * 60 * 125;  // 7 minutes at 8 ms
    for (uint64_t period : {33ull, 66ull, 995ull}) {
        for (uint64_t readMs : {1ull, 4ull}) {
            const int before = StalePasses(OldLive, period, readMs, passes);
            const int after = StalePasses(newLive, period, readMs, passes);
            std::printf("publish every %llu ms, file read %.2f ms into the pass: stale passes before %d, after %d\n",
                        (unsigned long long)period, double(readMs) / 4, before, after);
            CHECK(after == 0, "period %llu ms: %d stale passes", (unsigned long long)period, after);
        }
    }
    CHECK(StalePasses(OldLive, 33, 4, passes) > 0, "the replay reproduces the old flicker");

    // ft-hands stopping still reads as stale, 1.5 s after its last frame.
    {
        const uint64_t last = 10'000 * kMs;
        CHECK(Fresh(last + 1'400 * kMs, last, kLive), "1.4 s after the last frame: still live");
        CHECK(!Fresh(last + 1'600 * kMs, last, kLive), "1.6 s after the last frame: stopped");
    }

    if (failures) return std::printf("%d failures\n", failures), 1;
    std::printf("feedage: ok\n");
    return 0;
}
