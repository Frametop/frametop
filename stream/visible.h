// Which part of a decoded buffer is the picture. The iris decoder's buffers are the picture
// rounded up, 128 pixels across and 32 down, and its crop says what of that is seen. Up to
// SteamOS 0.5.9 the crop is right once the capture format is set. From 0.5.10 on (qcom-iris,
// kernel 7.2) setting the format makes the crop the whole buffer, so it has to be read before
// (2026-11-25: a 640x360 stream was shown as 640x384). Header-only, no devices here.
#pragma once

#include <cstdint>

struct VisibleRect {
    int32_t left = 0, top = 0;
    uint32_t width = 0, height = 0;
};

// The crop read before the format was set and after it, and the buffer's size. The one from after,
// unless that is the whole buffer and the one from before is a picture that rounds up to it (and
// not what was left of an earlier stream).
inline VisibleRect Visible(const VisibleRect &before, const VisibleRect &after, uint32_t codedWidth, uint32_t codedHeight) {
    const auto inside = [&](const VisibleRect &r) {
        return r.left >= 0 && r.top >= 0 && r.width && r.height && uint64_t(r.left) + r.width <= codedWidth && uint64_t(r.top) + r.height <= codedHeight;
    };
    const VisibleRect whole{0, 0, codedWidth, codedHeight};
    if (!inside(after)) return inside(before) ? before : whole;
    if (after.width < codedWidth || after.height < codedHeight) return after;
    const bool rounds = (before.width + 127) / 128 == (codedWidth + 127) / 128 && (before.height + 31) / 32 == (codedHeight + 31) / 32;
    return inside(before) && rounds ? before : after;
}
