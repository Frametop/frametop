#pragma once
// A CPU list as ft-hands and ft-handreplay take it ("5,6,7"): the numbers up to the first one that doesn't parse.
#include <cstdlib>
#include <vector>

inline std::vector<int> parse_cpus(const char *p) {
    std::vector<int> out;
    while (*p) {
        char *end;
        const long c = std::strtol(p, &end, 10);
        if (end == p) break;
        out.push_back(int(c));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}
