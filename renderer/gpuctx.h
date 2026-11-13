// A surfaceless GLES 2 context on a GBM device (rendernode's), with the EGL
// and GLES extension calls used by the hand overlay.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdio>

struct gbm_device;

namespace gpuctx {

struct Procs {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC imageTargetRenderbuffer = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC clientWaitSync = nullptr;
};

// Needed on top of the ones every program needs (the display, creating images, the renderbuffer
// target and the fences).
enum Need : unsigned {
    kDestroyImage = 1u << 0,
    kImageTargetTexture = 1u << 1,  // the Renderer samples imported client buffers
};

inline Procs Load() {
    Procs p;
    p.getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    p.createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    p.destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    p.imageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    p.imageTargetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    p.createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    p.destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    p.clientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    return p;
}

// Whether p has every call the program needs.
inline bool Complete(const Procs &p, unsigned need) {
    return p.getPlatformDisplay && p.createImage && p.imageTargetRenderbuffer && p.createSync && p.destroySync &&
           p.clientWaitSync && (!(need & kDestroyImage) || p.destroyImage) &&
           (!(need & kImageTargetTexture) || p.imageTargetTexture);
}

// The context on gbm, made current. *dpy is set once the display is up and *ctx once the context
// is current; on false (and a line on stderr) a display already set is the caller's to Close.
inline bool Open(const char *who, gbm_device *gbm, const Procs &p, unsigned need, EGLDisplay *dpy, EGLContext *ctx) {
    if (!gbm || !Complete(p, need)) return std::fprintf(stderr, "%s: GBM or EGL extensions missing\n", who), false;
    EGLDisplay d = p.getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr))
        return std::fprintf(stderr, "%s: no EGL display\n", who), false;
    *dpy = d;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        if (c != EGL_NO_CONTEXT) eglDestroyContext(d, c);
        return std::fprintf(stderr, "%s: no surfaceless GLES context\n", who), false;
    }
    *ctx = c;
    return true;
}

// What Open made (either may be EGL_NO_...).
inline void Close(EGLDisplay dpy, EGLContext ctx) {
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (dpy != EGL_NO_DISPLAY) eglTerminate(dpy);
}

}  // namespace gpuctx
