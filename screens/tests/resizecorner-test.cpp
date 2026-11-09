#include "../resizecorner.h"
#include <cassert>
#include <iostream>

int main() {
    constexpr int n = 256;
    resizecorner::Style style;
    style.native = false;  // custom-color assertions below exercise the sharp custom style
    for (double length : {1.0, 4.0, 10.0})
        for (double width : {0.5, 2.0, 6.0}) {
            style.lengthCm = length, style.widthMm = width, style.color = 0x12abef;
            const double size = resizecorner::Canvas(style);
            const auto base = resizecorner::Texture(n, size, 1, -1, style);
            for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
                const auto px = resizecorner::Texture(n, size, sx, sy, style);
                int solid = 0, clear = 0, aa = 0;
                for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
                    const auto i = (size_t(y) * n + x) * 4;
                    const int a = px[i + 3];
                    solid += a == 255, clear += a == 0, aa += a > 0 && a < 255;
                    assert(px[i] == 0x12 && px[i + 1] == 0xab && px[i + 2] == 0xef);
                    if (sx * (x + 0.5 - n / 2) < 0 && sy * (n / 2 - y - 0.5) < 0) assert(a == 0);
                    const int bx = sx == 1 ? x : n - 1 - x, by = sy == -1 ? y : n - 1 - y;
                    assert(a == base[(size_t(by) * n + bx) * 4 + 3]);
                    if (x == 0 || y == 0 || x == n - 1 || y == n - 1) assert(a == 0);
                }
                if (width / 1000 >= 2 * size / n) assert(solid > 0);
                assert(aa > 0 && clear > n * n * 0.8);
            }
        }
    for (double gap : {0.0, 2.0, 10.0}) {
        style.gapCm = gap;
        const double size = resizecorner::Canvas(style);
        const auto idle = resizecorner::Texture(n, size, 1, -1, style);
        const auto ready = resizecorner::Texture(n, size, 1, -1, style, true);
        int idleArea = 0, readyArea = 0;
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            const auto i = (size_t(y) * n + x) * 4;
            idleArea += idle[i + 3] > 0, readyArea += ready[i + 3] > 0;
            if (ready[i + 3]) {
                assert(ready[i] == uint8_t(style.hoverColor >> 16));
                if (x == 0 || y == 0 || x == n - 1 || y == n - 1) assert(false);
                const double X = (x + 0.5 - n / 2.0) * size / n;
                const double Y = (y + 0.5 - n / 2.0) * size / n;
                const double distance = std::hypot(std::max(X, 0.0), std::max(Y, 0.0));
                assert(distance >= gap / 100 - size / n);
            }
        }
        assert(readyArea > idleArea);
    }
    style = resizecorner::Style{};
    for (bool native : {false, true}) for (double roundness : {0.0, 50.0, 100.0}) {
        style.native = native, style.roundness = roundness;
        const double size = resizecorner::Canvas(style);
        const auto idle = resizecorner::Texture(n, size, 1, -1, style);
        const auto ready = resizecorner::Texture(n, size, 1, -1, style, true);
        int idleArea = 0, readyArea = 0, idleMaxX = 0, readyMaxX = 0;
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            const auto i = (size_t(y) * n + x) * 4;
            idleArea += idle[i + 3] > 0, readyArea += ready[i + 3] > 0;
            if (idle[i + 3]) idleMaxX = std::max(idleMaxX, x);
            if (ready[i + 3]) readyMaxX = std::max(readyMaxX, x);
            if (native) {
                assert(idle[i] == 0xaa && ready[i] == 0xfa);
                assert(idle[i] == idle[i + 1] && ready[i] == ready[i + 2]);
            }
            if (x == 0 || y == 0 || x == n - 1 || y == n - 1) assert(!idle[i + 3] && !ready[i + 3]);
            if (x < n / 2 && y < n / 2) assert(!idle[i + 3] && !ready[i + 3]);
        }
        assert(readyArea > idleArea);
        if (native) assert(readyMaxX > idleMaxX);
    }
    style.native = false;
    style.roundness = 0;
    const auto square = resizecorner::Texture(n, resizecorner::Canvas(style), 1, -1, style);
    style.roundness = 100;
    assert(square != resizecorner::Texture(n, resizecorner::Canvas(style), 1, -1, style));
    double animated = 0;
    for (int i = 0; i < 8; ++i) {
        const double next = resizecorner::Animate(animated, true, 0.01);
        assert(next > animated && next < 1);
        animated = next;
    }
    const double reversed = resizecorner::Animate(animated, false, 0.01);
    assert(reversed > 0 && reversed < animated);
    for (int i = 0; i < 100; ++i) animated = resizecorner::Animate(animated, false, 0.01);
    assert(animated == 0);
    for (int i = 0; i < 100; ++i) animated = resizecorner::Animate(animated, true, 0.01);
    assert(animated == 1);
    assert(resizecorner::Ease(0) == 0 && resizecorner::Ease(1) == 1);
    for (int mask = 0; mask < 16; ++mask) for (int c = 0; c < 4; ++c) {
        const bool always = mask & (1 << c);
        assert(resizecorner::Strength(false, always, 1, true) == 0);
        assert((resizecorner::Strength(true, always, 0, false) > 0) == always);
        assert(resizecorner::Strength(true, always, 0, true) == 1);
    }
    const auto region = resizecorner::Target(style, 1, -1);
    const auto target = resizecorner::TargetTexture(64, region);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        assert(bool(target[(y * 64 + x) * 4 + 3]) == overlayregion::Contains(region, (x + 0.5) / 64, 1 - (y + 0.5) / 64));
    assert(style.widthMm == 5.1);
    style.native = true;
    const double canvas = resizecorner::Canvas(style);
    const auto middle = resizecorner::Texture(n, canvas, 1, -1, style, 0.5);
    assert(middle != resizecorner::Texture(n, canvas, 1, -1, style, 0));
    assert(middle != resizecorner::Texture(n, canvas, 1, -1, style, 1));
    const resizecorner::Style defaults;
    assert(defaults.native && defaults.sharp && defaults.haptics && !defaults.preview);
    assert(defaults.always[3] && !defaults.always[0] && !defaults.always[1] && !defaults.always[2]);
    assert(defaults.widthMm == 5.1 && defaults.lengthCm == 4.7 && defaults.opacity == 0.27);
    assert(defaults.gapCm == 3.1 && defaults.paddingCm == 2.4);
    assert(std::fabs(resizecorner::LegacySize(0.04) - 0.072) < 1e-12);
    assert(resizecorner::LegacyZone(2, 1) == 0.2);
    assert(resizecorner::LegacyZone(0.01, 0.02) == 0.05);
    assert(resizecorner::LegacyCloseness(0.072, 0.04, 2, 1) == 1);
    assert(resizecorner::LegacyCloseness(0.2, 0.04, 2, 1) == 0);
    assert(std::fabs(resizecorner::LegacyCloseness(0.136, 0.04, 2, 1) - 0.5) < 1e-12);
    resizecorner::HoverBuzz buzz;
    assert(buzz.Enter(2, true, true));  // only controller 2 is the requested pulse recipient
    assert(buzz.device == 2 && !buzz.Enter(2, true, true));
    assert(buzz.Enter(1, true, true));  // switch to the left controller, not a broadcast
    buzz.Leave(2);                    // stale right leave must not retrigger the left
    assert(!buzz.Enter(1, true, true));
    buzz.Leave(1);
    assert(buzz.Enter(1, true, true));
    assert(!buzz.Enter(3, false, true));  // virtual pointer/gaze: never buzz
    assert(!buzz.Enter(2, true, false));  // disabled: no haptics
    assert(!buzz.Enter(2, true, true));   // enabling while hovered doesn't spam a pulse
    buzz.Leave();
    assert(buzz.Enter(2, true, true));
    // Every mask primitive is outside the screen plus its exclusion margin,
    // including extreme padding, tight requested gaps and all orientations.
    for (bool native : {false, true}) for (double gap : {0.0, 10.0})
        for (double padding : {0.1, 1.0, 10.0}) for (double exclusion : {0.0, 0.5, 5.0})
            for (double rounding : {0.0, 50.0, 100.0}) for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
                style.native = native, style.gapCm = gap, style.paddingCm = padding;
                style.exclusionCm = exclusion, style.roundness = rounding;
                const double size = resizecorner::Canvas(style);
                const auto mask = resizecorner::Target(style, sx, sy);
                assert(mask.size() == 32);
                overlayregion::Region controllerMask;
                for (const auto &primitive : mask) controllerMask.push_back(overlayregion::TopLeft(primitive));
                // The actual controller mask must match the preview after API-space conversion,
                // not just agree with the pointer helper in the original UV coordinate system.
                for (int py = 0; py < 17; ++py) for (int px = 0; px < 17; ++px) {
                    const double u = (px + 0.37) / 17, v = (py + 0.23) / 17;
                    assert(overlayregion::Contains(mask, u, v) == overlayregion::Contains(controllerMask, u, 1 - v));
                }
                std::string key;
                overlayregion::Region decoded;
                assert(overlayregion::Encode("frametop.screen.1.resize", mask).size() < 4095);
                assert(overlayregion::Decode(overlayregion::Encode("frametop.screen.1.resize", mask).c_str(), &key, &decoded));
                for (const auto &p : mask) {
                    double x = sx * (p.x - 0.5) * size, y = sy * (p.y - 0.5) * size;
                    double clear;
                    if (p.circle) clear = std::hypot(std::max(x, 0.0), std::max(y, 0.0)) - p.w * size;
                    else {
                        if (sx < 0) x -= p.w * size;
                        if (sy < 0) y -= p.h * size;
                        clear = std::hypot(std::max(x, 0.0), std::max(y, 0.0));
                    }
                    assert(clear >= exclusion / 100 - 1e-9);
                }
            }
    // The stable target contains the full animated stroke, not merely its idle bend.
    style = resizecorner::Style{};
    style.paddingCm = 0.1, style.gapCm = 10;
    for (bool native : {false, true}) for (double rounding : {0.0, 50.0, 100.0}) {
        style.native = native, style.roundness = rounding;
        const double size = resizecorner::Canvas(style);
        const auto mask = resizecorner::Target(style, 1, -1);
        for (double blend : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            const auto stroke = resizecorner::Texture(n, size, 1, -1, style, blend);
            for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
                if (stroke[(y * n + x) * 4 + 3] == 255)
                    assert(overlayregion::Contains(mask, (x + 0.5) / n, 1 - (y + 0.5) / n));
        }
    }
    std::cout << "ok crisp corners: gap, AA, hover, native gray/white growth, roundness and clear canvas\n";
}
