// What the mirror shows, drawn (mirror.h has the sums): both eyes' views of the glass side by
// side, the left eye's in the left half, as SteamVR shows a side-by-side overlay.
#pragma once

#include "bodygpu.h"
#include "mirror.h"

namespace mirrorview {

// Into the framebuffer bound (2 x `w` by `h` pixels, with depth): the glass and its frame, and
// in it the body as each eye sees it. No body (nullptr), or an eye that sees nothing in the
// glass: the glass alone.
void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar *avatar, const float bones[bodyfile::kRoles][12], const mirror::Glass &glass,
          const avatar::Vec eyes[2], int w, int h);

}  // namespace mirrorview
