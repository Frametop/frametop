// screens/glyphs.h, offline: UTF-8 to codepoints, a font file that isn't there, and text laid out by
// glyphs::Font against stb_truetype called directly, as the panels did before they shared it
// (same widths, same baseline, same pixel coverage at the same places), at several sizes.
// Needs Noto Sans (fc-match, as the panels find it); without it only the font-free checks run.
// (No check feeds it bytes that aren't a font: stb_truetype trusts its input, and the panels
// only read the system's font.)
//
//   just test-c (build/include gets the pinned stb_truetype.h from scripts/stb.sh)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "screens/glyphs.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            ++failures;                                                                     \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                    \
            std::printf(__VA_ARGS__);                                                       \
            std::printf("\n");                                                              \
        }                                                                                   \
    } while (0)

using Pixels = std::map<std::pair<int, int>, std::vector<int>>;  // (x, y) -> coverages, in order

// The panels' text before glyphs.h, uncached: each glyph straight from stb_truetype.
struct Direct {
    stbtt_fontinfo font{};
    std::vector<unsigned char> data;

    int Advance(uint32_t cp, float scale) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, int(cp), &adv, &lsb);
        return int(std::lround(adv * scale));
    }
    int Width(const std::vector<uint32_t> &cps, int size) {
        const float scale = stbtt_ScaleForPixelHeight(&font, float(size));
        int w = 0;
        for (uint32_t cp : cps) w += Advance(cp, scale);
        return w;
    }
    int Draw(const std::vector<uint32_t> &cps, int size, int x, int cy, Pixels &out) {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
        const float scale = stbtt_ScaleForPixelHeight(&font, float(size));
        const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
        for (uint32_t cp : cps) {
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char *b = stbtt_GetCodepointBitmap(&font, 0, scale, int(cp), &w, &h, &xoff, &yoff);
            for (int gy = 0; b && gy < h; ++gy)
                for (int gx = 0; gx < w; ++gx) out[{x + xoff + gx, baseline + yoff + gy}].push_back(b[gy * w + gx]);
            if (b) stbtt_FreeBitmap(b, nullptr);
            x += Advance(cp, scale);
        }
        return x;
    }
};

static void TestCodepoints() {
    using V = std::vector<uint32_t>;
    CHECK(glyphs::Codepoints("") == V{}, "empty");
    CHECK(glyphs::Codepoints("Ab 1") == (V{'A', 'b', ' ', '1'}), "ASCII");
    CHECK(glyphs::Codepoints("é") == V{0xE9}, "two bytes");
    CHECK(glyphs::Codepoints("…✓") == (V{0x2026, 0x2713}), "three bytes");
    CHECK(glyphs::Codepoints("😀x") == (V{0x1F600, 'x'}), "four bytes");
    // Cut short: the sequence ends where its continuation bytes stop.
    CHECK(glyphs::Codepoints("\xC3" "A") == (V{0x03, 'A'}), "a lead byte with no continuation");
    CHECK(glyphs::Codepoints("\xE2\x80") == V{0x80}, "a three-byte sequence cut at the end");
    CHECK(glyphs::Codepoints(std::string("a\0b", 3)) == V{'a'}, "stops at a NUL, as the C strings did");
}

static void TestLoadFailures() {
    glyphs::Font f;
    CHECK(!f.ok(), "a new font isn't loaded");
    CHECK(!f.Load("/nonexistent/font.ttf"), "a missing file");
    CHECK(!f.ok(), "still not loaded");
    CHECK(!f.Init({}), "no bytes");
    CHECK(!f.ok(), "still not loaded");
}

static void TestAgainstDirect(const std::string &path) {
    glyphs::Font font;
    if (!font.Load(path)) {
        std::printf("glyphs-test: no font at %s; layout checks skipped\n", path.c_str());
        return;
    }
    Direct direct;
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        std::fseek(f, 0, SEEK_END);
        direct.data.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        CHECK(std::fread(direct.data.data(), 1, direct.data.size(), f) == direct.data.size(), "read %s", path.c_str());
        std::fclose(f);
        CHECK(stbtt_InitFont(&direct.font, direct.data.data(), stbtt_GetFontOffsetForIndex(direct.data.data(), 0)), "init");
    }
    const char *texts[] = {"Screen 2", "Look straight ahead", "Résumé · 3 windows …", "Ünïcödé — ✓ 日本 😀",
                           "  spaces  ", "1", "Recentering"};
    for (int size : {14, 22, 36, 166})
        for (const char *t : texts)
            for (int pass = 0; pass < 2; ++pass) {  // the second from the cache
                const auto cps = glyphs::Codepoints(t);
                CHECK(font.Width(cps, size) == direct.Width(cps, size), "width of \"%s\" at %d", t, size);
                Pixels got, want;
                const int gotEnd = font.Draw(cps, size, 17, 300, [&](int x, int y, uint8_t cover) {
                    got[{x, y}].push_back(cover);
                });
                const int wantEnd = direct.Draw(cps, size, 17, 300, want);
                CHECK(gotEnd == wantEnd, "end of \"%s\" at %d: %d, want %d", t, size, gotEnd, wantEnd);
                CHECK(got == want, "pixels of \"%s\" at %d (pass %d): %zu places, want %zu", t, size, pass,
                      got.size(), want.size());
                CHECK(gotEnd == 17 + font.Width(cps, size), "Draw ends at x + Width");
            }
    // Load again (a second Init clears the cache): the same.
    CHECK(font.Load(path) && font.Width(glyphs::Codepoints("Screen 2"), 22) ==
                                 direct.Width(glyphs::Codepoints("Screen 2"), 22), "reloaded");
}

int main() {
    TestCodepoints();
    TestLoadFailures();
    TestAgainstDirect(glyphs::NotoSans());
    if (failures) {
        std::printf("glyphs-test: %d failed\n", failures);
        return 1;
    }
    std::printf("glyphs-test: ok\n");
    return 0;
}
