// Your own body below you, drawn (belowview.h).
#include "belowview.h"

#include "windowview.h"

namespace belowview {

void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[bodyfile::kRoles][12], const mirror::Glass &under,
          const avatar::Vec eyes[2], int side) {
    viewmath::Mat4 clip[2];
    const bool sees[2] = {below::EyeView(under, eyes[0], clip[0]), below::EyeView(under, eyes[1], clip[1])};
    windowview::Draw(painter, avatar, bones, clip, sees, below::Light(), side, side);
}

}  // namespace belowview
