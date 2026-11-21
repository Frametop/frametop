// How dictation's notice looks (vrvoice.cpp shows it): the Frametop SteamOS UI design's "Dictation is listening"
// (its Prompts board; user, 2026-11-21: "look for our artifact that we used for our UI redesign and implement
// the dictation listening notice"), at twice its CSS pixels. A black pill: a blue mic disc with a soft blue ring,
// five level bars, "Listening" and a grey hint ("release to type" for hold to talk); then, while the last words
// are decoded and typed, a grey disc with a turning ring and "Typing…". Pure pixels, no OpenVR.
#pragma once

#include "fillround.h"
#include "iconpath.h"
#include "label.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace voicenoticeart {

enum class Kind { Listening, Typing };

struct Look {
    Kind kind = Kind::Listening;
    std::string hint;   // grey words after "Listening" ("" none)
    double level = 0;   // the mic's level now, 0..1 (Listening)
    double turn = 0;    // the turning ring's angle, in turns (Typing)
};

struct Art {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
};

constexpr int kH = 76, kText = 28, kDisc = 52;
constexpr uint8_t kPill[4] = {0, 0, 0, 217}, kBlue[4] = {26, 159, 255, 255}, kHalo[4] = {26, 159, 255, 77},
                  kGreyDisc[4] = {61, 68, 80, 255}, kWhite4[4] = {255, 255, 255, 255};
constexpr uint8_t kWhite[3] = {255, 255, 255}, kGrey[3] = {139, 146, 154}, kRingGrey[3] = {103, 112, 123};
// The design's bar heights (of 18), loudest in the middle.
constexpr double kBars[5] = {6 / 18.0, 14 / 18.0, 1.0, 10 / 18.0, 5 / 18.0};
constexpr const char *kMic = "M12 2a3 3 0 0 1 3 3v6a3 3 0 0 1-6 0V5a3 3 0 0 1 3-3zm-7 9h2a5 5 0 0 0 10 0h2a7 7 0 0 1-6 6.9V21h-2v-3.1A7 7 0 0 1 5 11z";

// The bar heights for a level: each at least a dot, all of them at full voice.
inline int BarHeight(int i, double level) {
    const double l = std::clamp(level, 0.0, 1.0);
    return std::max(8, int(std::lround(36 * kBars[i] * (0.25 + 0.75 * l))));
}

// A ring (cx, cy, radii r0..r1), `rgb` where the angle from the top (clockwise, in turns) is within
// [from, from + span), soft at its edges.
inline void Ring(std::vector<uint8_t> &px, int W, int H, double cx, double cy, double r0, double r1, double from, double span,
                 const uint8_t rgb[3]) {
    for (int y = int(cy - r1 - 1); y <= int(cy + r1 + 1); ++y)
        for (int x = int(cx - r1 - 1); x <= int(cx + r1 + 1); ++x) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = std::sqrt(dx * dx + dy * dy);
            const double a = std::clamp(std::min(r - r0, r1 - r) + 0.5, 0.0, 1.0);
            if (a <= 0) continue;
            double t = std::atan2(dx, -dy) / (2 * M_PI);   // (0 at the top, clockwise)
            t -= std::floor(t);
            double f = t - (from - std::floor(from));
            f -= std::floor(f);
            if (f >= span) continue;
            uint8_t *p = &px[(size_t(y) * W + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(p[k] + (rgb[k] - p[k]) * a));
            p[3] = uint8_t(std::max<double>(p[3], 255 * a));
        }
}

inline Art Draw(const Look &look) {
    using ftvr::FillRound;
    const bool listening = look.kind == Kind::Listening;
    const std::string word = listening ? "Listening" : "Typing\xE2\x80\xA6";
    const std::string hint = listening && !look.hint.empty() ? "\xC2\xB7 " + look.hint : "";
    constexpr int padL = 12, gap = 20, padR = 28, barW = 6, barGap = 6;
    const int bars = listening ? 5 * barW + 4 * barGap + gap : 0;
    const int wordW = label::Width(word.c_str(), kText), hintW = hint.empty() ? 0 : label::Width(hint.c_str(), kText) + 12;
    Art a;
    a.w = padL + kDisc + gap + bars + wordW + hintW + padR, a.h = kH;
    a.px.assign(size_t(a.w) * a.h * 4, 0);
    std::vector<uint8_t> &px = a.px;
    const int W = a.w, H = a.h, cy = H / 2;
    FillRound(px, W, H, 0, 0, W, H, H / 2, kPill);
    const int d0 = padL, dc = d0 + kDisc / 2;
    if (listening) {
        FillRound(px, W, H, d0 - 8 + 4, cy - kDisc / 2 - 8 + 4, d0 + kDisc + 8 - 4, cy + kDisc / 2 + 8 - 4, kDisc / 2 + 4, kHalo);
        FillRound(px, W, H, d0, cy - kDisc / 2, d0 + kDisc, cy + kDisc / 2, kDisc / 2, kBlue);
        iconpath::Paint(px, W, H, {{iconpath::Parse(kMic)}}, dc - 16, cy - 16, 32, kWhite);
    } else {
        FillRound(px, W, H, d0, cy - kDisc / 2, d0 + kDisc, cy + kDisc / 2, kDisc / 2, kGreyDisc);
        Ring(px, W, H, dc, cy, 10, 14, 0, 1, kRingGrey);
        Ring(px, W, H, dc, cy, 10, 14, look.turn, 0.25, kWhite);
    }
    int x = d0 + kDisc + gap;
    if (listening) {
        for (int i = 0; i < 5; ++i, x += barW + barGap) {
            const int bh = BarHeight(i, look.level);
            FillRound(px, W, H, x, cy - bh / 2, x + barW, cy - bh / 2 + bh, barW / 2, kWhite4);
        }
        x += gap - barGap;
    }
    label::Draw(px.data(), W, H, word.c_str(), kText, x + wordW / 2, cy, 255, kWhite);
    if (!hint.empty()) label::Draw(px.data(), W, H, hint.c_str(), kText, x + wordW + 12 + (hintW - 12) / 2, cy, 255, kGrey);
    return a;
}

}  // namespace voicenoticeart
