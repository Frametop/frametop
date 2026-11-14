// Buffers SteamVR shows, drawn on the GPU (vroutput.h).
#include "vroutput.h"

#include <drm_fourcc.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>

namespace vroutput {

std::vector<uint64_t> Modifiers() {
    uint64_t mods[64];
    uint32_t n = 64;
    std::vector<uint64_t> out;
    if (!vr::VRIPCResourceManager() || !vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, DRM_FORMAT_ABGR8888, &n, mods)) return out;
    for (uint32_t i = 0; i < n && i < 64; ++i)
        if (mods[i] != DRM_FORMAT_MOD_INVALID) out.push_back(mods[i]);
    return out;
}

bool Make(const Maker &m, int w, int h, Buffer &o) {
    o.bo = m.modifiers.empty() ? gbm_bo_create(m.gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
                               : gbm_bo_create_with_modifiers2(m.gbm, w, h, GBM_FORMAT_ABGR8888, m.modifiers.data(), unsigned(m.modifiers.size()),
                                                               GBM_BO_USE_RENDERING);
    if (!o.bo) return std::fprintf(stderr, "%s: can't allocate a %dx%d buffer\n", m.who, w, h), false;
    const int planes = gbm_bo_get_plane_count(o.bo);
    const uint64_t mod = m.modifiers.empty() ? DRM_FORMAT_MOD_LINEAR : gbm_bo_get_modifier(o.bo);
    int fds[4] = {-1, -1, -1, -1};
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(w), a.unHeight = uint32_t(h);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = DRM_FORMAT_ABGR8888, a.ulModifier = mod, a.unPlaneCount = uint32_t(planes);
    EGLint at[64];
    int n = 0;
    at[n++] = EGL_WIDTH, at[n++] = w, at[n++] = EGL_HEIGHT, at[n++] = h;
    at[n++] = EGL_LINUX_DRM_FOURCC_EXT, at[n++] = DRM_FORMAT_ABGR8888;
    static const EGLint kFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                  EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint kOff[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                   EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint kPitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                     EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint kLo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint kHi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                  EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (int i = 0; i < planes && i < 4; ++i) {
        fds[i] = gbm_bo_get_fd_for_plane(o.bo, i);
        const uint32_t off = gbm_bo_get_offset(o.bo, i), stride = gbm_bo_get_stride_for_plane(o.bo, i);
        a.plane[i].unOffset = off, a.plane[i].unStride = stride, a.plane[i].nFd = fds[i];
        at[n++] = kFd[i], at[n++] = fds[i], at[n++] = kOff[i], at[n++] = EGLint(off), at[n++] = kPitch[i], at[n++] = EGLint(stride);
        at[n++] = kLo[i], at[n++] = EGLint(mod & 0xffffffff), at[n++] = kHi[i], at[n++] = EGLint(mod >> 32);
    }
    at[n++] = EGL_NONE;
    o.image = m.egl.createImage(m.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &o.handle)) o.handle = 0;
    for (int fd : fds)
        if (fd >= 0) close(fd);   // the EGL image and SteamVR hold their own references
    if (o.image == EGL_NO_IMAGE_KHR || !o.handle)
        return std::fprintf(stderr, "%s: the buffer can't be %s\n", m.who, o.handle ? "drawn to" : "imported by SteamVR"), Free(m, o), false;
    glGenRenderbuffers(1, &o.rb);
    glBindRenderbuffer(GL_RENDERBUFFER, o.rb);
    m.egl.imageTargetRenderbuffer(GL_RENDERBUFFER, o.image);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o.rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "%s: output framebuffer incomplete\n", m.who), Free(m, o), false;
    return true;
}

void Free(const Maker &m, Buffer &o) {
    if (o.handle) vr::VRIPCResourceManager()->UnrefResource(o.handle);
    if (o.fence != EGL_NO_SYNC_KHR) m.egl.destroySync(m.dpy, o.fence);
    if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
    if (o.rb) glDeleteRenderbuffers(1, &o.rb);
    if (o.image != EGL_NO_IMAGE_KHR) m.egl.destroyImage(m.dpy, o.image);
    if (o.bo) gbm_bo_destroy(o.bo);
    o = Buffer{};
}

void Fence(const Maker &m, Buffer &o) {
    if (o.fence != EGL_NO_SYNC_KHR) m.egl.destroySync(m.dpy, o.fence);
    o.fence = m.egl.createSync(m.dpy, EGL_SYNC_FENCE_KHR, nullptr);
    glFlush();
}

bool Passed(const Maker &m, Buffer &o) {
    if (o.fence == EGL_NO_SYNC_KHR) return true;
    if (m.egl.clientWaitSync(m.dpy, o.fence, 0, 0) == EGL_TIMEOUT_EXPIRED_KHR) return false;
    m.egl.destroySync(m.dpy, o.fence);
    o.fence = EGL_NO_SYNC_KHR;
    return true;
}

}  // namespace vroutput
