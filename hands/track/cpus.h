#pragma once
// A CPU list as ft-hands and ft-handreplay take it: "5,6,7", ranges as "5-7", or both ("0,5-7"). Anything else
// (junk, an empty item, a reversed range, a CPU < 0 or >= CPU_SETSIZE, which CPU_SET can't hold) gives an empty
// list, so the caller reports it or falls back, never pins to half of what was asked.
#include <sched.h>

#include <cerrno>
#include <cstdlib>
#include <vector>

inline std::vector<int> parse_cpus(const char *p) {
    std::vector<int> out;
    auto num = [](const char *&q, long &v) {
        if (*q < '0' || *q > '9') return false;     // no sign, no space: strtol would take them
        char *end;
        errno = 0;
        v = std::strtol(q, &end, 10);
        if (errno || v >= CPU_SETSIZE) return false;
        q = end;
        return true;
    };
    while (*p) {
        long a, b;
        if (!num(p, a)) return {};
        b = a;
        if (*p == '-' && (++p, !num(p, b) || b < a)) return {};
        for (long c = a; c <= b; ++c) out.push_back(int(c));
        if (*p == ',') {
            if (!*++p) return {};                   // a trailing comma
        } else if (*p) {
            return {};                              // anything but a comma after an item
        }
    }
    return out;
}
