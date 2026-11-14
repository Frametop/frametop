// Your own body below you, drawn (below.h has the sums): both eyes' views through the square
// under you, side by side, the left eye's in the left half, clear where no body is.
#pragma once

#include "below.h"
#include "bodygpu.h"

namespace belowview {

// Into the framebuffer bound (2 x `side` by `side` pixels, with depth): the body as each eye
// sees it through the square `under`. `bones`: the pose's matrices, folded (below::Fold).
void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[bodyfile::kRoles][12], const mirror::Glass &under,
          const avatar::Vec eyes[2], int side);

}  // namespace belowview
