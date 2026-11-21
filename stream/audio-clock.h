#pragma once
#include <algorithm>
#include <cstdint>

// Host and Frame monotonic clocks have different origins. Discard a backlog
// relative to the best observed transit time, but recover if transit time
// stays higher: a slow connection must not become permanently silent.
struct WindowAudioClock {
    int64_t offset = INT64_MAX;
    int64_t staleSince = -1;

    bool Ready(uint64_t source, int64_t now) {
        if (source > uint64_t(INT64_MAX) || now < 0) return false;
        const int64_t lag = now - int64_t(source);
        offset = std::min(offset, lag);
        // Unsigned subtraction also handles widely separated clock origins.
        if (uint64_t(lag) - uint64_t(offset) <= 200'000'000) {
            staleSince = -1;
            return true;
        }
        if (staleSince < 0) staleSince = now;
        if (now - staleSince < 500'000'000) return false;
        offset = lag;
        staleSince = -1;
        return true;
    }
};
