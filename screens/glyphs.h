// Text for Frametop's own CPU-drawn panels (ft-screens' labels, ft-spotpanel, ft-gazepanel,
// ft-handpanel, ft-reset-timer) with stb_truetype: the font, its glyphs cached by codepoint and
// pixel size, UTF-8 to codepoints, widths, and where each glyph's pixels go. Each panel blends
// them into its own picture its own way: Draw hands over every glyph pixel with its coverage
// (0-255, stb's own byte) and the panel does the rest, so its pixels stay what they were.
//
// Header only. Include stb_truetype.h first; the one file of a program that defines
// STB_TRUETYPE_IMPLEMENTATION does so before it. ft-handpanel is also built by the standalone
// Hand Recorder, which has all of Frametop as a submodule.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace glyphs {

// Where panels look for their font when nobody says: fontconfig's "Noto Sans", else the
// variable Noto Sans SteamOS ships.
inline std::string NotoSans() {
    std::string path;
    if (FILE *p = popen("fc-match -f '%{file}' 'Noto Sans' 2>/dev/null", "r")) {
        char buf[512];
        if (std::fgets(buf, sizeof buf, p)) path = buf;
        pclose(p);
    }
    if (path.empty()) path = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf";
    return path;
}

// s as codepoints: UTF-8, read leniently (a sequence cut short ends where it stops).
inline std::vector<uint32_t> Codepoints(const std::string &s) {
    std::vector<uint32_t> out;
    for (const unsigned char *p = (const unsigned char *)s.c_str(); *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*p & 0xC0) == 0x80; --more) c = c << 6 | (*p++ & 0x3F);
        out.push_back(c);
    }
    return out;
}

struct Glyph {
    std::vector<unsigned char> bitmap;  // w x h coverage, 0-255
    int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};

class Font {
public:
    Font() = default;
    // Not copied: the font info points into the font's own bytes.
    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;

    // Reads the font file at path; false if it can't be read or isn't a font.
    bool Load(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<unsigned char> bytes;
        std::fseek(f, 0, SEEK_END);
        bytes.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        return read && Init(std::move(bytes));
    }

    // Takes a font file's bytes; false if they aren't a font.
    bool Init(std::vector<unsigned char> bytes) {
        data_ = std::move(bytes);
        glyphs_.clear();
        ok_ = !data_.empty() && stbtt_InitFont(&info_, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0));
        return ok_;
    }

    bool ok() const { return ok_; }

    const Glyph &Get(uint32_t cp, int size) {
        auto [it, fresh] = glyphs_.try_emplace({cp, size});
        Glyph &g = it->second;
        if (fresh) {
            const float scale = Scale(size);
            unsigned char *b = stbtt_GetCodepointBitmap(&info_, 0, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
            if (b) g.bitmap.assign(b, b + size_t(g.w) * g.h), stbtt_FreeBitmap(b, nullptr);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&info_, int(cp), &adv, &lsb);
            g.advance = int(std::lround(adv * scale));
        }
        return g;
    }

    // The width of cps at size: their advances.
    int Width(const std::vector<uint32_t> &cps, int size) {
        int w = 0;
        for (uint32_t cp : cps) w += Get(cp, size).advance;
        return w;
    }

    // The baseline that centres a line of text at size on cy.
    int Baseline(int cy, int size) {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info_, &ascent, &descent, &gap);
        return cy + int(std::lround((ascent + descent) * Scale(size) / 2));
    }

    // Lays out cps at size from x (left edge), centred on cy: plot(px, py, coverage) for every
    // pixel of every glyph, in order, coverage 0-255, unclipped. Returns x after the last one.
    template <class Plot>
    int Draw(const std::vector<uint32_t> &cps, int size, int x, int cy, Plot &&plot) {
        const int baseline = Baseline(cy, size);
        for (uint32_t cp : cps) {
            const Glyph &g = Get(cp, size);
            for (int gy = 0; gy < g.h; ++gy)
                for (int gx = 0; gx < g.w; ++gx) plot(x + g.xoff + gx, baseline + g.yoff + gy, g.bitmap[size_t(gy) * g.w + gx]);
            x += g.advance;
        }
        return x;
    }

private:
    float Scale(int size) { return stbtt_ScaleForPixelHeight(&info_, float(size)); }

    stbtt_fontinfo info_{};
    std::vector<unsigned char> data_;
    std::map<std::pair<uint32_t, int>, Glyph> glyphs_;  // (codepoint, pixel size)
    bool ok_ = false;
};

}  // namespace glyphs
