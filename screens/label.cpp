// Labels for ft-screens' own panels (label.h), with glyphs.h. The font loads the first time a
// label is measured.
#include "label.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace label {
namespace {

glyphs::Font g_font;

}  // namespace

int Width(const char *text, int size) {
    static bool tried = false;
    if (!g_font.ok() && !tried) {
        tried = true;
        const std::string path = glyphs::NotoSans();
        if (!g_font.Load(path)) std::printf("labels: no font (%s); panels have no text\n", path.c_str());
    }
    if (!g_font.ok() || !*text) return 0;
    return g_font.Width(glyphs::Codepoints(text), size);
}

int Draw(uint8_t *px, int w, int h, const char *text, int size, int cx, int cy, uint8_t alpha, const uint8_t rgb[3]) {
    const int width = Width(text, size);
    if (!width) return 0;
    g_font.Draw(glyphs::Codepoints(text), size, cx - width / 2, cy, [&](int qx, int qy, uint8_t cover) {
        if (qx < 0 || qy < 0 || qx >= w || qy >= h) return;
        const double a = cover / 255.0 * alpha / 255.0;
        uint8_t *p = &px[(size_t(qy) * w + qx) * 4];
        for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::lround(p[c] + (rgb[c] - p[c]) * a));
        p[3] = uint8_t(std::max<double>(p[3], 255 * a));
    });
    return width;
}

}  // namespace label
