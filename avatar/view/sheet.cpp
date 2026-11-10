// A flat picture shown in the room (sheet.h).
#include "sheet.h"

#include <cstdio>

namespace sheet {

bool Sheet::Make(const vroutput::Maker &maker, const char *key, const char *name, int order, int w, int h) {
    if (vr::VROverlay()->CreateOverlay(key, name, &overlay_) != vr::VROverlayError_None)
        return std::fprintf(stderr, "%s: can't create the overlay %s (already running?)\n", maker.who, key), false;
    vr::VROverlay()->SetOverlayFlag(overlay_, vr::VROverlayFlags_SideBySide_Parallel, true);
    vr::VROverlay()->SetOverlaySortOrder(overlay_, uint32_t(order));
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, 2 * w, h);
    for (vroutput::Buffer &b : out_) {
        if (!vroutput::Make(maker, 2 * w, h, b)) return false;
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return std::fprintf(stderr, "%s: no depth for %s's buffers\n", maker.who, key), false;
    }
    return true;
}

void Sheet::Free(const vroutput::Maker &maker) {
    if (overlay_ != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(overlay_);
    for (vroutput::Buffer &b : out_) vroutput::Free(maker, b);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    *this = Sheet();
}

void Sheet::Stand(const mirror::Glass &at) {
    vr::HmdMatrix34_t pose;
    mirror::Pose(at, pose.m);
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_, float(at.width));
    vr::VROverlay()->SetOverlayTransformAbsolute(overlay_, vr::TrackingUniverseStanding, &pose);
}

vroutput::Buffer &Sheet::Next(const mirror::Glass *at) {
    int pick = 0;
    while (pick == shown_ || pick == before_) ++pick;
    drawing_ = pick;
    moves_[pick] = at != nullptr;
    if (at) at_[pick] = *at;
    glBindFramebuffer(GL_FRAMEBUFFER, out_[pick].fbo);
    return out_[pick];
}

bool Sheet::Show(const vroutput::Maker &maker) {
    if (drawing_ < 0 || !vroutput::Passed(maker, out_[drawing_])) return false;
    if (moves_[drawing_]) Stand(at_[drawing_]);   // the picture and the place it was drawn for, together
    vr::Texture_t tex = {&out_[drawing_].handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(overlay_, &tex);
    before_ = shown_, shown_ = drawing_, drawing_ = -1;
    if (!visible_) vr::VROverlay()->ShowOverlay(overlay_), visible_ = true;
    return true;
}

void Sheet::Hide() {
    if (visible_) vr::VROverlay()->HideOverlay(overlay_), visible_ = false;
}

}  // namespace sheet
