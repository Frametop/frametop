// A picture for tests that says which one it is: a grey desktop with a bar that moves a little
// with every picture, and along its top the picture's number as a row of squares, light for a
// one and dark for a nought, after one light and one dark square to tell them by. The squares
// are large and plain, so the number comes back after the encoder and a decoder have had it.
//
// ft-sharepanel --pattern draws it; ft-peerpanel --probe reads it. Header only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace media::testpattern {

constexpr int kBits = 16;                  // the number's, so it wraps at 65536
constexpr int kSquares = 2 + kBits;
constexpr uint32_t kGrey = 0xff303030u, kBar = 0xff40c0f0u, kLight = 0xfff0f0f0u, kDark = 0xff101010u;   // A, B, G, R from the top byte down
constexpr int kBarHeight = 48;

// A square's side in a picture `width` wide.
inline int Square(int width) { return std::max(4, width / (kSquares + 2)); }

inline int BarAt(int height, int width, int frame) {
    const int top = Square(width), travel = std::max(1, height - top - kBarHeight);
    return top + frame * 6 % travel;
}

// Draws picture `frame`, each rectangle of it with fill(x0, y0, x1, y1, colour). `was` is the
// picture that is there now (only what changed since is drawn), or -1 when it is anything.
template <class Fill>
void Paint(int width, int height, int frame, int was, Fill fill) {
    if (was < 0) fill(0, 0, width, height, kGrey);
    else fill(0, BarAt(height, width, was), width, BarAt(height, width, was) + kBarHeight, kGrey);
    fill(0, BarAt(height, width, frame), width, BarAt(height, width, frame) + kBarHeight, kBar);
    const int side = Square(width);
    for (int i = 0; i < kSquares; ++i) {
        const bool light = i == 0 || (i >= 2 && (frame >> (i - 2) & 1));
        fill(i * side, 0, (i + 1) * side, side, light ? kLight : kDark);
    }
}

// Into RGBA rows of `stride` bytes.
inline void Draw(uint8_t *px, int stride, int width, int height, int frame, int was) {
    Paint(width, height, frame, was, [&](int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = std::max(0, y0); y < std::min(height, y1); ++y) {
            uint32_t *row = reinterpret_cast<uint32_t *>(px + size_t(y) * size_t(stride));
            std::fill(row + std::max(0, x0), row + std::min(width, x1), c);
        }
    });
}

// Into an NV12 picture (BT.709, full range, as blit.h's): Y rows at `luma`, Cb,Cr rows at
// `chroma`, `stride` bytes each. A Cb,Cr pixel is the colour of its first pixel.
inline void DrawNv12(uint8_t *luma, uint8_t *chroma, int stride, int width, int height, int frame, int was) {
    const auto byte = [](double v) { return uint8_t(std::clamp(v + 0.5, 0.0, 255.0)); };
    Paint(width, height, frame, was, [&](int x0, int y0, int x1, int y1, uint32_t c) {
        const double r = c & 0xff, g = c >> 8 & 0xff, b = c >> 16 & 0xff;
        const double y709 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(width, x1), y1 = std::min(height, y1);
        for (int y = y0; y < y1; ++y) {
            uint8_t *row = luma + size_t(y) * size_t(stride);
            std::fill(row + x0, row + std::max(x0, x1), byte(y709));
        }
        const uint8_t cb = byte((b - y709) / 1.8556 + 128), cr = byte((r - y709) / 1.5748 + 128);
        for (int y = (y0 + 1) / 2; y < (y1 + 1) / 2; ++y) {
            uint8_t *row = chroma + size_t(y) * size_t(stride);
            for (int x = (x0 + 1) / 2; x < (x1 + 1) / 2; ++x) row[2 * x] = cb, row[2 * x + 1] = cr;
        }
    });
}

// The number of the picture in these RGBA rows (at least Square(width) of them), or -1 when its
// squares aren't there.
inline int Read(const uint8_t *px, int stride, int width) {
    const int side = Square(width);
    int number = 0;
    for (int i = 0; i < kSquares; ++i) {
        const uint8_t *p = px + size_t(side / 2) * size_t(stride) + size_t(i * side + side / 2) * 4;
        const int light = (p[0] + p[1] + p[2]) / 3;
        if (light > 96 && light < 160) return -1;
        const bool one = light >= 160;
        if ((i == 0 && !one) || (i == 1 && one)) return -1;
        if (i >= 2 && one) number |= 1 << (i - 2);
    }
    return number;
}

}  // namespace media::testpattern
