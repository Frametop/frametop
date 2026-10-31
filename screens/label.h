// Labels for ft-screens' own panels (the lazy susan's carousel, the reset question, the pill):
// text in Noto Sans (the container's, found with fc-match), rasterized with stb_truetype.
#pragma once

#include <cstdint>

namespace label {

// Text centred on (cx, cy) in an RGBA buffer w x h, blended toward `rgb` at `alpha`. Its width
// in pixels, or 0 with no font.
int Draw(uint8_t *px, int w, int h, const char *text, int size, int cx, int cy, uint8_t alpha,
         const uint8_t rgb[3]);
// How wide `text` would be at `size`.
int Width(const char *text, int size);

}  // namespace label
