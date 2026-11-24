// The battery's picture on the palm menu's header: an outline with its terminal, filled as far as the
// battery is full: green while it gets charge, red nearly empty, else the text's own. Pure pixels.
#pragma once

#include "battery.h"
#include "fillround.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace batteryart {

constexpr int kW = 44, kH = 20;       // with its terminal
constexpr int kNub = 4, kEdge = 2, kGap = 2;

// How wide the fill is, in pixels (a few per cent still shows).
inline int FillWidth(int percent) {
    const int room = kW - kNub - 2 * (kEdge + kGap);
    return percent <= 0 ? 0 : std::max(2, room * std::min(percent, 100) / 100);
}

// Onto px (w x h), its left edge at x0 and its middle at cy; `bg` is what's behind it.
inline void Draw(std::vector<uint8_t> &px, int w, int h, int x0, int cy, const battery::Reading &r, const uint8_t bg[4]) {
    const uint8_t ink[4] = {225, 225, 232, 255}, green[4] = {89, 191, 64, 255}, red[4] = {222, 54, 24, 255};
    const uint8_t back[4] = {bg[0], bg[1], bg[2], 255};
    const int y0 = cy - kH / 2, x1 = x0 + kW - kNub;
    ftvr::FillRound(px, w, h, x0, y0, x1, y0 + kH, 5, ink);
    ftvr::FillRound(px, w, h, x0 + kEdge, y0 + kEdge, x1 - kEdge, y0 + kH - kEdge, 3, back);
    ftvr::FillRect(px, w, h, x1, cy - 4, x1 + kNub - 1, cy + 4, ink);
    const int in = kEdge + kGap, fill = FillWidth(r.percent);
    if (fill > 0)
        ftvr::FillRect(px, w, h, x0 + in, y0 + in, x0 + in + fill, y0 + kH - in,
                       battery::Low(r) ? red : battery::Charging(r.state) ? green : ink);
}

}  // namespace batteryart
