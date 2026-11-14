// The pictures ft-pcsend plays when no streaming host hands it any: a loop that was encoded
// earlier, as a Frame's encoding program hands pictures over (local/mediawire.h: 'U' messages
// one after another), the first a keyframe.
//
// Reads a file. Header only.
#pragma once

#include "../local/mediawire.h"

#include <cstdio>
#include <vector>

namespace pcsend {

constexpr size_t kMostLoop = 64u << 20;   // a loop's file

struct Picture {
    bool key = false;
    std::vector<uint8_t> unit;
};

// The loop's pictures out of its file. None when it isn't one.
inline std::vector<Picture> ReadLoop(const char *path) {
    std::vector<Picture> loop;
    FILE *f = std::fopen(path, "rb");
    if (!f) return loop;
    local::MediaReader reader;
    uint8_t buf[1 << 16], kind = 0;
    std::vector<uint8_t> body;
    size_t total = 0;
    bool ok = true;
    for (size_t n; ok && (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        ok = (total += n) <= kMostLoop && reader.Feed(buf, n);
        while (ok && reader.Next(kind, body)) {
            ok = kind == local::kMediaUnit && body.size() > 1 && !(body[0] & ~1);
            if (ok) loop.push_back({bool(body[0] & 1), {body.begin() + 1, body.end()}});
        }
    }
    std::fclose(f);
    if (!ok || reader.held() || loop.empty() || !loop[0].key) loop.clear();
    return loop;
}

}  // namespace pcsend
