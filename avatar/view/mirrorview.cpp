// The mirror drawn (mirrorview.h).
#include "mirrorview.h"

#include <algorithm>

namespace mirrorview {

namespace {

constexpr float kFrame[3] = {0.55f, 0.58f, 0.62f};   // the glass's edge
constexpr float kGlass[3] = {0.09f, 0.10f, 0.12f};   // the glass where nothing is in it
constexpr double kFrameM = 0.012;                    // the edge's width, metres

}  // namespace

void Draw(const bodygpu::Painter &painter, const bodygpu::Avatar *avatar, const float bones[bodyfile::kRoles][12], const mirror::Glass &glass,
          const avatar::Vec eyes[2], int w, int h) {
    // the edge is on the glass's plane, so it is the same in both halves: it sits at the glass's depth
    const int edge = std::max(1, int(kFrameM / glass.width * w + 0.5));
    glEnable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
    for (int e = 0; e < 2; ++e) {
        glScissor(e * w, 0, w, h);
        glClearColor(kFrame[0], kFrame[1], kFrame[2], 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glScissor(e * w + edge, edge, w - 2 * edge, h - 2 * edge);
        glClearColor(kGlass[0], kGlass[1], kGlass[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        viewmath::Mat4 clip;
        if (!avatar || !mirror::EyeView(glass, eyes[e], clip)) continue;
        bodygpu::View view;
        std::copy(clip.m, clip.m + 16, view.clip);
        const avatar::Vec light = mirror::Light(glass);
        view.light[0] = float(light.x), view.light[1] = float(light.y), view.light[2] = float(light.z);
        glViewport(e * w, 0, w, h);   // the scissor keeps the body off the edge
        avatar->Draw(painter, bones, view);
    }
    glDisable(GL_SCISSOR_TEST);
}

}  // namespace mirrorview
