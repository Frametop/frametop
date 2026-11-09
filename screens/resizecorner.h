// Crisp resize feedback only: hit geometry and corner selection remain independent.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string_view>
#include "../pointer/common/overlayregion.h"
#include "hoverhaptics.h"

namespace resizecorner {
inline bool Color(std::string_view text, uint32_t *out) {
    if (text.size() != 7 || text[0] != '#') return false;
    uint32_t value = 0;
    for (char c : text.substr(1)) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = value * 16 + unsigned(digit);
    }
    *out = value;
    return true;
}
struct Style {
    bool sharp = true, native = true;
    uint32_t color = 0xf6f9ff, hoverColor = 0x66ddff;
    double opacity = 0.27, lengthCm = 4.7, widthMm = 5.1, gapCm = 3.1;
    double paddingCm = 2.4, exclusionCm = 0.5, reachCm = 20, roundness = 100;
    bool preview = false;
    bool haptics = true;
    bool always[4] = {false, false, false, true};  // top left, top right, bottom left, bottom right
};

using HoverBuzz = hoverhaptics::Gate;

// Original behavior is intentionally independent of all new appearance/target settings.
inline double LegacySize(double grip) { return grip * 1.8; }
inline double LegacyZone(double width, double height) { return std::clamp(0.2 * std::min(width, height), 0.05, 0.2); }
inline double LegacyCloseness(double distance, double grip, double width, double height) {
    const double size = LegacySize(grip), zone = LegacyZone(width, height);
    return 1 - std::clamp((distance - size) / std::max(1e-3, zone - size), 0.0, 1.0);
}

inline double Animate(double value, bool ready, double dt) {
    const double step = std::clamp(dt, 0.0, 0.1) / 0.18;
    return ready ? std::min(1.0, value + step) : std::max(0.0, value - step);
}
inline double Ease(double value) { return value * value * (3 - 2 * value); }
inline double Strength(bool visible, bool always, double proximity, bool ready) {
    return !visible ? 0 : ready ? 1 : std::max(std::clamp(proximity, 0.0, 1.0), always ? 0.65 : 0.0);
}

// A stable envelope around the entire hover animation. The primitive union is used
// unchanged for controller masks, pointer hits and preview pixels (32 is OpenVR's limit).
inline double TargetRadius(const Style &s) {
    const double line = s.widthMm / 1000 * (s.native ? 1.2 : 1.6);
    const double delta = (s.native ? 0.003 : 0) + (line - s.widthMm / 1000) / 2;
    const double base = s.paddingCm / 100 + line / 2 + std::sqrt(2.0) * delta;
    // Circle sampling of the bend adds a conservative half-step, so even its
    // outer arc has no gaps between primitives, including very thin targets.
    constexpr double k = 3.141592653589793 / 108;
    const double gap = std::max(s.gapCm / 100, s.exclusionCm / 100 + base + 0.001);
    return base + k * (gap + line / 2 + (s.native ? 0.003 : 0)) / (1 - k);
}
inline double Gap(const Style &s) {
    return std::max(s.gapCm / 100, s.exclusionCm / 100 + TargetRadius(s) + 0.001);
}
inline double Canvas(const Style &s) {
    const double line = s.widthMm / 1000 * (s.native ? 1.2 : 1.6);
    return 2 * (std::max(s.lengthCm / 100 * (s.native ? 1.12 : 1), Gap(s) + line / 2 + (s.native ? 0.003 : 0))
                + TargetRadius(s) + 0.002);
}
inline overlayregion::Region Target(const Style &s, int sx, int sy) {
    overlayregion::Region region;
    const double size = Canvas(s), r = TargetRadius(s);
    const double arm = s.lengthCm / 100 * (s.native ? 1.12 : 1);
    const double gap = Gap(s) + s.widthMm / 2000 * (s.native ? 1.2 : 1.6) + (s.native ? 0.003 : 0);
    const double bend = gap * std::clamp(s.roundness, 0.0, 100.0) / 100, c = gap - bend;
    auto circle = [&](double x, double y) { region.push_back({true, 0.5 + sx * x / size, 0.5 + sy * y / size, r / size, 0}); };
    auto rect = [&](double x, double y, double w, double h) {
        region.push_back({false, 0.5 + (sx > 0 ? x : -x - w) / size,
                         0.5 + (sy > 0 ? y : -y - h) / size, w / size, h / size});
    };
    rect(-arm, gap - r, arm + c, 2 * r);
    rect(gap - r, -arm, 2 * r, arm + c);
    circle(-arm, gap), circle(gap, -arm);
    for (int i = 0; i < 28; ++i) {
        const double a = i * (3.141592653589793 / 2) / 27;
        circle(c + bend * std::cos(a), c + bend * std::sin(a));
    }
    return region;
}

inline std::vector<uint8_t> TargetTexture(int n, const overlayregion::Region &region) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        auto *p = &px[(size_t(y) * n + x) * 4];
        const double u = (x + 0.5) / n, v = 1 - (y + 0.5) / n, d = 1.0 / n;
        if (!overlayregion::Contains(region, u, v)) continue;
        const bool edge = !overlayregion::Contains(region, u - d, v) || !overlayregion::Contains(region, u + d, v) ||
                          !overlayregion::Contains(region, u, v - d) || !overlayregion::Contains(region, u, v + d);
        p[0] = 102, p[1] = 221, p[2] = 255, p[3] = edge ? 180 : 24;
    }
    return px;
}

// A round-ended L, with a quarter-circle joining the arms outside the panel.
// Signed coordinates point out of the corner, so all four orientations share it.
inline std::vector<uint8_t> Texture(int n, double size, int sx, int sy, const Style &style, double hover = 0) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    hover = std::clamp(hover, 0.0, 1.0);
    const double arm = style.lengthCm / 100 * (1 + (style.native ? 0.12 : 0) * hover);
    const double line = style.widthMm / 1000 * (1 + (style.native ? 0.2 : 0.6) * hover);
    const double gap = Gap(style) + line / 2 + (style.native ? 0.003 * hover : 0), pixel = size / n;
    const double radius = gap * std::clamp(style.roundness, 0.0, 100.0) / 100, centre = gap - radius;
    const uint32_t from = style.native ? 0xaaaaaa : style.color, to = style.native ? 0xfafafa : style.hoverColor;
    auto channel = [&](int shift) { return uint8_t(std::lround(((from >> shift) & 255) * (1 - hover) + ((to >> shift) & 255) * hover)); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double X = sx * (x + 0.5 - n / 2.0) * pixel;
            const double Y = sy * (n / 2.0 - y - 0.5) * pixel;
            const double horizontal = std::hypot(X - std::clamp(X, -arm, centre), Y - gap);
            const double vertical = std::hypot(X - gap, Y - std::clamp(Y, -arm, centre));
            double d = std::min(horizontal, vertical);
            if (X >= centre && Y >= centre) d = std::min(d, std::fabs(std::hypot(X - centre, Y - centre) - radius));
            // One-pixel coverage AA, not a diffuse halo. Keep the panel completely clear.
            double a = X < 0 && Y < 0 ? 0 : std::clamp((line / 2 - d) / pixel + 0.5, 0.0, 1.0);
            if (!style.sharp && !(X < 0 && Y < 0)) {
                const double fade = std::clamp(1 - std::max(0.0, d - line / 2) / std::max(pixel, style.paddingCm / 200), 0.0, 1.0);
                a = std::max(a * 0.6, 0.5 * fade * fade);
            }
            auto *p = &px[(size_t(y) * n + x) * 4];
            p[0] = channel(16), p[1] = channel(8), p[2] = channel(0);
            p[3] = uint8_t(std::lround(a * 255));
        }
    return px;
}
}  // namespace resizecorner
