// The panels' copy into their buffers (screens/overlaypanel.h): premultiplied, only the area
// asked for (inside the picture) when the buffer has the whole picture it changed from, and all
// of it otherwise.
#define OVERLAYPANEL_PURE
#include "../overlaypanel.h"

#include <cstdio>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool same(overlaypanel::Area a, int x, int y, int w, int h) {
    return a.x == x && a.y == y && a.w == w && a.h == h;
}

int main() {
    using overlaypanel::Area;
    using overlaypanel::ToCopy;
    const Area in{2, 3, 4, 5};
    check("no area: the whole picture", same(ToCopy(nullptr, true, 10, 8), 0, 0, 10, 8));
    check("an area, buffer has the picture: the area", same(ToCopy(&in, true, 10, 8), 2, 3, 4, 5));
    check("an area, buffer doesn't have it: the whole picture", same(ToCopy(&in, false, 10, 8), 0, 0, 10, 8));
    const Area past{8, 6, 5, 5}, before{-3, -1, 4, 4}, outside{12, 9, 3, 3};
    check("an area past the right and bottom edges is cut there", same(ToCopy(&past, true, 10, 8), 8, 6, 2, 2));
    check("an area before the left and top edges starts at 0, its size kept",
          same(ToCopy(&before, true, 10, 8), 0, 0, 4, 4));
    check("an area outside the picture is empty", same(ToCopy(&outside, true, 10, 8), 10, 8, 0, 0));

    // 3 x 2 pixels into a buffer 16 bytes a row (4 bytes of padding).
    const int pw = 3, ph = 2;
    const uint32_t stride = 16;
    const std::vector<uint8_t> px = {
        200, 100, 50, 255,  200, 100, 50, 128,  200, 100, 50, 0,
        255, 255, 255, 51,  10, 20, 30, 255,    90, 60, 30, 200,
    };
    std::vector<uint8_t> buf(stride * ph, 7);
    overlaypanel::CopyPremultiplied(buf.data(), stride, px.data(), pw, ToCopy(nullptr, true, pw, ph));
    const uint8_t want[2][16] = {
        {200, 100, 50, 255, 100, 50, 25, 128, 0, 0, 0, 0, 7, 7, 7, 7},
        {51, 51, 51, 51, 10, 20, 30, 255, 70, 47, 23, 200, 7, 7, 7, 7},
    };
    bool all = true;
    for (int y = 0; y < ph; ++y)
        for (int i = 0; i < 16; ++i) all = all && buf[size_t(y) * stride + size_t(i)] == want[y][i];
    check("the whole picture premultiplied, alpha kept, the row padding untouched", all);

    std::vector<uint8_t> part(stride * ph, 7);
    const Area one{1, 1, 1, 1};
    overlaypanel::CopyPremultiplied(part.data(), stride, px.data(), pw, ToCopy(&one, true, pw, ph));
    bool only = true;
    for (size_t i = 0; i < part.size(); ++i) {
        const bool in1 = i >= stride + 4 && i < stride + 8;
        only = only && (in1 ? part[i] == want[1][i - stride] : part[i] == 7);
    }
    check("an area: only its pixels are written", only);

    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
