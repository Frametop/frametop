// Stereo wallpapers (DEE-317): the desktop's wallpaper is one solid key colour (Display Settings
// sets it in Plasma), and the hand cutter (handrender.cpp) puts a picture of its own for each
// eye where it sees that colour. KWin draws the wallpaper and the windows into one buffer, so
// the key colour is the only way to tell them apart.
//
// Display Settings decodes the pictures and writes them as binary PPMs (P6, 8 bits), one per
// eye, to ~/.local/share/frametop/wallpaper/left.ppm and right.ppm. This header reads them and
// holds the key test, the same as the shader's (handrender.cpp kKeyed), so a test can check it
// without a GPU. Header only, no GL. Test: screens/tests/wallimage-test.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wallimage {

// Magenta: Display Settings' KEY, the colour Plasma's wallpaper is set to.
constexpr uint8_t kKey[3] = {255, 0, 255};
constexpr int kMaxSide = 4096;   // Display Settings scales pictures down to this and less

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // w * h * 3, top row first
};

// A binary PPM's bytes -> image. False for anything else: another magic, a maxval other than
// 255, a side of 0 or over kMaxSide, or fewer pixel bytes than the header says.
inline bool ParsePpm(const uint8_t *data, size_t size, Image *out) {
    size_t i = 0;
    auto skip = [&] {   // whitespace and # comments
        while (i < size) {
            if (data[i] == '#') {
                while (i < size && data[i] != '\n') ++i;
            } else if (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r') {
                ++i;
            } else {
                break;
            }
        }
    };
    auto number = [&](long *v) {
        skip();
        if (i >= size || data[i] < '0' || data[i] > '9') return false;
        long n = 0;
        while (i < size && data[i] >= '0' && data[i] <= '9' && n < 1'000'000) n = n * 10 + (data[i++] - '0');
        *v = n;
        return true;
    };
    if (size < 2 || data[0] != 'P' || data[1] != '6') return false;
    i = 2;
    long w = 0, h = 0, maxval = 0;
    if (!number(&w) || !number(&h) || !number(&maxval)) return false;
    if (w < 1 || h < 1 || w > kMaxSide || h > kMaxSide || maxval != 255) return false;
    // one whitespace byte, then the pixels
    if (i >= size || !(data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) return false;
    ++i;
    const size_t need = size_t(w) * size_t(h) * 3;
    if (size - i < need) return false;
    out->w = int(w), out->h = int(h);
    out->rgb.assign(data + i, data + i + need);
    return true;
}

inline bool LoadPpm(const std::string &path, Image *out, std::string *why) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return *why = "can't open " + path, false;
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    const size_t cap = size_t(kMaxSide) * kMaxSide * 3 + 4096;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() <= cap) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (!ParsePpm(bytes.data(), bytes.size(), out)) return *why = path + " isn't an 8-bit binary PPM (P6) up to 4096 px a side", false;
    return true;
}

// The folder the pictures are in: $HOME/.local/share/frametop/wallpaper.
inline std::string Dir() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/frametop/wallpaper";
}

inline float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// How much of a pixel (0..1 each) is wallpaper, and how bright the wallpaper is there: the key
// colour is all wallpaper at full brightness, and the key colour darkened (a window's black
// shadow over it: equal red and blue, no green) is wallpaper in that shadow, so shadows fall on
// the picture. Anything else, tinted edges included (a window's anti-aliased corner over the
// key), stays as KWin drew it. The same sums as kKeyed in handrender.cpp.
struct Keyed {
    float amount;       // 0: KWin's pixel, 1: the picture's
    float brightness;   // the picture times this (the shadow)
};

inline Keyed Key(float r, float g, float b) {
    const float s = std::max(r, b);
    const float off = std::max(std::abs(r - b), g);
    return {(1 - Smooth(0.03f, 0.08f, off)) * Smooth(0.30f, 0.45f, s), s};
}

}  // namespace wallimage
