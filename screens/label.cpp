// Labels for ft-screens' own panels (label.h). The font loads the first time a label is
// measured; glyphs are cached by codepoint and pixel size.
#include "label.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace label {
namespace {

stbtt_fontinfo g_font;
std::vector<unsigned char> g_fontData;
bool g_fontOk = false;

struct Glyph {
    std::vector<unsigned char> bitmap;
    int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};
std::map<std::pair<uint32_t, int>, Glyph> g_glyphs;  // (codepoint, pixel size)

void LoadFont() {
    std::string path;
    if (FILE *p = popen("fc-match -f '%{file}' 'Noto Sans' 2>/dev/null", "r")) {
        char buf[512];
        if (std::fgets(buf, sizeof buf, p)) path = buf;
        pclose(p);
    }
    if (path.empty()) path = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf";
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        g_fontData.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        g_fontOk = std::fread(g_fontData.data(), 1, g_fontData.size(), f) == g_fontData.size() &&
                   stbtt_InitFont(&g_font, g_fontData.data(), stbtt_GetFontOffsetForIndex(g_fontData.data(), 0));
        std::fclose(f);
    }
    if (!g_fontOk) std::printf("labels: no font (%s); panels have no text\n", path.c_str());
}

const Glyph &GetGlyph(uint32_t cp, int size) {
    auto [it, fresh] = g_glyphs.try_emplace({cp, size});
    Glyph &g = it->second;
    if (fresh) {
        const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
        unsigned char *b = stbtt_GetCodepointBitmap(&g_font, 0, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
        if (b) g.bitmap.assign(b, b + size_t(g.w) * g.h), stbtt_FreeBitmap(b, nullptr);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &adv, &lsb);
        g.advance = int(std::lround(adv * scale));
    }
    return g;
}

std::vector<uint32_t> Codepoints(const char *s) {
    std::vector<uint32_t> out;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*p & 0xC0) == 0x80; --more) c = c << 6 | (*p++ & 0x3F);
        out.push_back(c);
    }
    return out;
}

}  // namespace

int Width(const char *text, int size) {
    static bool tried = false;
    if (!g_fontOk && !tried) tried = true, LoadFont();
    if (!g_fontOk || !*text) return 0;
    int width = 0;
    for (uint32_t cp : Codepoints(text)) width += GetGlyph(cp, size).advance;
    return width;
}

int Draw(uint8_t *px, int w, int h, const char *text, int size, int cx, int cy, uint8_t alpha, const uint8_t rgb[3]) {
    const int width = Width(text, size);
    if (!width) return 0;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    int x = cx - width / 2;
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : Codepoints(text)) {
        const Glyph &g = GetGlyph(cp, size);
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const int qx = x + g.xoff + gx, qy = baseline + g.yoff + gy;
                if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                const double a = g.bitmap[size_t(gy) * g.w + gx] / 255.0 * alpha / 255.0;
                uint8_t *p = &px[(size_t(qy) * w + qx) * 4];
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] + (rgb[c] - p[c]) * a));
                p[3] = uint8_t(std::max<double>(p[3], 255 * a));
            }
        x += g.advance;
    }
    return width;
}

}  // namespace label
