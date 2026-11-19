// How the grab bar's menu and the reset question look (vrbarmenu.cpp, vrresetask.cpp draw with these).
// Pure pixels, no OpenVR, so an offline render shows them as the headset will.
//
// The Option C design's look (user, 2026-11-19: "Use that as the design"; its OptionC, States, Moving
// and Prompts boards), at twice its CSS pixels: a #171d25 card; quick tiles on top (#23262e, blue when
// on, white under a laser); then rows with an icon each, lit #3d4450 under a laser, #67707b while held,
// Reset (and the Close tile) #de3618; a value at the right (light blue for a placement that isn't the
// room, grey for the bar's edge) with a chevron where a row opens a page; a blue check on the choice
// picked; grey capitals over a page's choices. The reset question is Steam's dialog: a #23262e card,
// a title, a line on what happens, and Cancel and Reset, focus on Cancel.
#pragma once

#include "barmenuicons.h"
#include "fillround.h"
#include "label.h"
#include "menulayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace barmenuart {

using barmenuicons::Icon;

struct RowLook {
    std::string text;
    Icon icon = Icon::None;
    std::string value;        // words at its right
    bool valueBlue = false;   // ...in light blue (else grey)
    bool chevron = false;     // it opens a page
    bool on = false;          // a tile that's on: blue
    bool chosen = false;      // the choice picked (or Stay Shown on): a blue check at its right
    bool red = false;         // red under a laser (Reset Layout, the Close tile)
    std::string caption;      // grey capitals above it
};

constexpr uint8_t kCard[4] = {23, 29, 37, 250}, kTile[4] = {35, 38, 46, 255}, kHot[4] = {61, 68, 80, 255},
                  kPressed[4] = {103, 112, 123, 255}, kRedHot[4] = {222, 54, 24, 255}, kBlueOn[4] = {26, 159, 255, 255},
                  kBlueHot[4] = {71, 178, 255, 255}, kWhiteBg[4] = {255, 255, 255, 255}, kRule[4] = {39, 45, 53, 255};
constexpr uint8_t kText[3] = {220, 222, 223}, kWhite[3] = {255, 255, 255}, kGrey[3] = {139, 146, 154},
                  kValueBlue[3] = {103, 193, 245}, kBlue[3] = {26, 159, 255}, kDark[3] = {35, 38, 46}, kBody[3] = {184, 188, 191};

// Text from x (its left edge), centred on cy; bold draws it twice, a pixel apart.
inline int Text(std::vector<uint8_t> &px, int W, int H, const std::string &s, int size, int x, int cy, const uint8_t rgb[3],
                bool bold = false) {
    const int w = label::Width(s.c_str(), size);
    label::Draw(px.data(), W, H, s.c_str(), size, x + w / 2, cy, 255, rgb);
    if (bold) label::Draw(px.data(), W, H, s.c_str(), size, x + w / 2 + 1, cy, 255, rgb);
    return w;
}
inline void IconAt(std::vector<uint8_t> &px, int W, int H, Icon i, int x, int cy, int size, const uint8_t rgb[3]) {
    if (i != Icon::None) iconpath::Paint(px, W, H, barmenuicons::Shapes(i), x, cy - size / 2, size, rgb);
}
// Everything outside a rounded card of radius r goes clear (the design's overflow: hidden).
inline void Round(std::vector<uint8_t> &px, int W, int H, int r) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double cx = std::clamp(x + 0.5, double(r), double(W - r)), cy = std::clamp(y + 0.5, double(r), double(H - r));
            const double a = std::clamp(r - std::hypot(x + 0.5 - cx, y + 0.5 - cy) + 0.5, 0.0, 1.0);
            if (a < 1) px[(size_t(y) * W + x) * 4 + 3] = uint8_t(std::lround(px[(size_t(y) * W + x) * 4 + 3] * a));
        }
}
inline std::string Upper(std::string s) {
    for (char &c : s) c = char(std::toupper((unsigned char)c));
    return s;
}

// The menu on its layout (menulayout::Lay of the same rows); `lit` the row under a laser or a fingertip
// (-1 none), `pressed` while it's held (Rotate, Move All Screens).
inline std::vector<uint8_t> Menu(const std::vector<RowLook> &rows, const menulayout::Layout &l, int lit, bool pressed) {
    using ftvr::FillRound;
    const int W = l.w, H = l.h;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    ftvr::FillRect(px, W, H, 0, 0, W, H, kCard);
    for (int y : l.rules) ftvr::FillRect(px, W, H, 0, y - 1, W, y + 1, kRule);
    for (size_t i = 0; i < rows.size() && i < l.rows.size(); ++i) {
        const RowLook &r = rows[i];
        const menulayout::Box &b = l.rows[i];
        const bool hot = int(i) == lit, held = hot && pressed;
        const int cy = (b.y0 + b.y1) / 2;
        if (l.tiles[i]) {
            const uint8_t *bg = held ? kPressed : hot ? (r.red ? kRedHot : r.on ? kBlueHot : kWhiteBg) : r.on ? kBlueOn : kTile;
            const uint8_t *ink = hot && !held && !r.red && !r.on ? kDark : r.on || hot ? kWhite : kText;
            FillRound(px, W, H, b.x0, b.y0, b.x1, b.y1, 12, bg);
            const int mid = (b.x0 + b.x1) / 2;
            IconAt(px, W, H, r.icon, mid - 22, b.y0 + 44, 44, ink);
            const int tw = label::Width(r.text.c_str(), 24);
            Text(px, W, H, r.text, 24, mid - tw / 2, b.y0 + 92, ink, true);
            continue;
        }
        if (l.captions[i] >= 0) Text(px, W, H, Upper(r.caption), 22, 40, l.captions[i] + 36, kGrey, true);
        if (hot) ftvr::FillRect(px, W, H, b.x0, b.y0, b.x1, b.y1, held ? kPressed : r.red ? kRedHot : kHot);
        const uint8_t *ink = hot ? kWhite : kText;
        IconAt(px, W, H, r.icon, 40, cy, 40, ink);
        Text(px, W, H, r.text, 32, r.icon == Icon::None ? 40 : 112, cy, ink);   // (an edge has no icon: the design's text at the padding)
        int right = W - 40;
        if (r.chevron) IconAt(px, W, H, Icon::Chevron, right - 32, cy, 32, kGrey), right -= 32 + 8;
        if (r.chosen) IconAt(px, W, H, Icon::Check, right - 40, cy, 40, kBlue), right -= 40 + 12;
        if (!r.value.empty())
            Text(px, W, H, r.value, 28, right - label::Width(r.value.c_str(), 28), cy, r.valueBlue ? kValueBlue : kGrey);
    }
    Round(px, W, H, 20);
    return px;
}

// The reset question: its pixels, size and its two buttons (pixels, y down).
struct Ask {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    menulayout::Box cancel, reset;
};
constexpr const char *kAskTitle = "Reset the layout?";

// `body` wrapped to the card; `lit`: 0 Cancel, 1 Reset under a laser or a fingertip, -1 neither
// (then Cancel has the focus, white, as the design starts it).
inline Ask ResetAsk(const std::string &body, int lit) {
    using ftvr::FillRound;
    constexpr int W = 640, pad = 36, line = 40, bh = 80, gap = 16, bodySize = 28;
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string tryLine = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && label::Width(tryLine.c_str(), bodySize) > W - 2 * pad) lines.push_back(cur), cur = word;
        else cur = tryLine;
        word.clear();
    };
    for (char c : body) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    if (!cur.empty()) lines.push_back(cur);
    const int bodyTop = pad + 44 + 16, buttons = bodyTop + int(lines.size()) * line + 24, H = buttons + bh + 32;
    Ask a;
    a.w = W, a.h = H;
    a.px.assign(size_t(W) * H * 4, 0);
    FillRound(a.px, W, H, 0, 0, W, H, 16, kTile);
    Text(a.px, W, H, kAskTitle, 38, pad, pad + 22, kWhite, true);
    for (size_t i = 0; i < lines.size(); ++i) Text(a.px, W, H, lines[i], bodySize, pad, bodyTop + int(i) * line + line / 2, kBody);
    const int bw = (W - 2 * pad - gap) / 2;
    a.cancel = {pad, buttons, pad + bw, buttons + bh};
    a.reset = {W - pad - bw, buttons, W - pad, buttons + bh};
    const bool cancelLit = lit != 1;
    auto button = [&](const menulayout::Box &b, const char *text, bool white) {
        FillRound(a.px, W, H, b.x0, b.y0, b.x1, b.y1, 8, white ? kWhiteBg : kHot);
        const int tw = label::Width(text, 30);
        Text(a.px, W, H, text, 30, (b.x0 + b.x1) / 2 - tw / 2, (b.y0 + b.y1) / 2, white ? kDark : kWhite, true);
    };
    button(a.cancel, "Cancel", cancelLit);
    button(a.reset, "Reset", !cancelLit);
    return a;
}

}  // namespace barmenuart
