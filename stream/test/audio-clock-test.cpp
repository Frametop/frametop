// The KWin window sound's clock (stream/audio-clock.h, utrack's): late sound is dropped, a lasting delay is caught up with.
#include "../audio-clock.h"
#include <cassert>

int main() {
    constexpr int64_t ms = 1'000'000, frame = 1'000'000'000, host = 1'000'000'000'000;
    WindowAudioClock clock;
    assert(clock.Ready(host, frame));
    assert(clock.Ready(host + 20 * ms, frame + 100 * ms));  // ordinary jitter
    assert(!clock.Ready(host + 40 * ms, frame + 900 * ms)); // queued before a stall
    assert(!clock.Ready(host + 60 * ms, frame + 901 * ms));
    assert(clock.Ready(host + 900 * ms, frame + 902 * ms)); // current audio catches up

    WindowAudioClock slower;
    assert(slower.Ready(host, frame));
    assert(!slower.Ready(host + 1000 * ms, frame + 1300 * ms));
    assert(!slower.Ready(host + 1480 * ms, frame + 1780 * ms));
    assert(slower.Ready(host + 1500 * ms, frame + 1800 * ms)); // lasting latency change
    assert(slower.Ready(host + 1520 * ms, frame + 1820 * ms));
    assert(slower.Ready(host + 2000 * ms, frame + 2000 * ms)); // latency falls again

    WindowAudioClock limits;
    assert(!limits.Ready(UINT64_MAX, 0));
    assert(!limits.Ready(0, -1));
    assert(limits.Ready(INT64_MAX, 0));
    assert(!limits.Ready(0, INT64_MAX)); // no signed overflow across extreme origins
}
