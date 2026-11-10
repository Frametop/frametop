// Your own body below you, drawn (belowview.h).
#include "belowview.h"

#include <algorithm>

namespace belowview {

void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[bodyfile::kRoles][12], const mirror::Glass &under,
          const avatar::Vec eyes[2], int side) {
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
    glClearColor(0, 0, 0, 0);
    glViewport(0, 0, 2 * side, side);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);   // a body part past one eye's half stays out of the other's
    for (int e = 0; e < 2; ++e) {
        bodygpu::View view;
        viewmath::Mat4 clip;
        if (!below::EyeView(under, eyes[e], clip)) continue;
        std::copy(clip.m, clip.m + 16, view.clip);
        const avatar::Vec light = below::Light();
        view.light[0] = float(light.x), view.light[1] = float(light.y), view.light[2] = float(light.z);
        view.mirrored = true;   // the top goes first
        glScissor(e * side, 0, side, side);
        glViewport(e * side, 0, side, side);
        avatar.Draw(painter, bones, view);
    }
    glDisable(GL_SCISSOR_TEST);
}

}  // namespace belowview
