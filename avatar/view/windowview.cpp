// A body drawn as seen through a window in the room (windowview.h).
#include "windowview.h"

#include <algorithm>

namespace windowview {

void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar &avatar, const float bones[bodyfile::kRoles][12], const viewmath::Mat4 clip[2],
          const bool sees[2], const avatar::Vec &light, int w, int h) {
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
    glClearColor(0, 0, 0, 0);
    glViewport(0, 0, 2 * w, h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);   // a body part past one eye's half stays out of the other's
    for (int e = 0; e < 2; ++e) {
        if (!sees[e]) continue;
        bodygpu::View view;
        std::copy(clip[e].m, clip[e].m + 16, view.clip);
        view.light[0] = float(light.x), view.light[1] = float(light.y), view.light[2] = float(light.z);
        view.mirrored = true;   // the top goes first
        glScissor(e * w, 0, w, h);
        glViewport(e * w, 0, w, h);
        avatar.Draw(painter, bones, view);
    }
    glDisable(GL_SCISSOR_TEST);
}

}  // namespace windowview
