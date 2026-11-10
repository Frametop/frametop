// A buffer a program draws on the GPU and SteamVR shows as an overlay's texture, with no copy:
// a GBM buffer object in a format and layout SteamVR takes, drawn to through an EGL image and
// handed to SteamVR as a DMA-BUF. ft-handview's hands and ft-avatars' mirror are drawn into
// these (the hand cutouts' Renderer, handrender.cpp, still has its own).
//
// The context is the caller's (gpuctx.h) and current on every call. A buffer's framebuffer has
// colour only: what needs depth draws into a buffer of its own first, or attaches one.
#pragma once

#include "gpuctx.h"

#include <openvr.h>

#include <cstdint>
#include <vector>

struct gbm_bo;
struct gbm_device;

namespace vroutput {

struct Buffer {
    gbm_bo *bo = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint rb = 0, fbo = 0;
    vr::SharedTextureHandle_t handle = 0;   // what SetOverlayTexture takes (TextureType_SharedTextureHandle)
    EGLSyncKHR fence = EGL_NO_SYNC_KHR;     // the GPU is still drawing it
};

// What buffers are made on. `who` starts the lines on stderr.
struct Maker {
    const char *who = "";
    gbm_device *gbm = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    gpuctx::Procs egl;
    std::vector<uint64_t> modifiers;   // Modifiers(); none: a linear buffer
};

// The layouts SteamVR takes for an overlay's buffer in these buffers' format (SteamVR is up).
std::vector<uint64_t> Modifiers();

// A buffer w by h pixels, bound as the framebuffer. False (a line on stderr, nothing kept) when
// it can't be allocated, drawn to, or imported by SteamVR.
bool Make(const Maker &m, int w, int h, Buffer &out);
// Lets go of everything the buffer holds (SteamVR's import too). Safe on one never made.
void Free(const Maker &m, Buffer &b);

// After the last draw call into it: the GPU is told to get on with it, and Passed says when it
// has finished (never waiting). A buffer goes to SteamVR once it has passed.
void Fence(const Maker &m, Buffer &b);
bool Passed(const Maker &m, Buffer &b);

}  // namespace vroutput
