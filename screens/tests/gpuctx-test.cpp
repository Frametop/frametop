// Which EGL and GLES calls the hand renderers can't do without (screens/gpuctx.h): the shared
// ones always, eglDestroyImageKHR and glEGLImageTargetTexture2DOES only for those that need them.
#include "../gpuctx.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// Stand-ins: only whether a pointer is set matters.
template <typename F>
static F Set() {
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(&std::puts));
}

int main() {
    using namespace gpuctx;
    Procs all;
    all.getPlatformDisplay = Set<PFNEGLGETPLATFORMDISPLAYEXTPROC>();
    all.createImage = Set<PFNEGLCREATEIMAGEKHRPROC>();
    all.destroyImage = Set<PFNEGLDESTROYIMAGEKHRPROC>();
    all.imageTargetTexture = Set<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>();
    all.imageTargetRenderbuffer = Set<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>();
    all.createSync = Set<PFNEGLCREATESYNCKHRPROC>();
    all.destroySync = Set<PFNEGLDESTROYSYNCKHRPROC>();
    all.clientWaitSync = Set<PFNEGLCLIENTWAITSYNCKHRPROC>();
    const unsigned both = kDestroyImage | kImageTargetTexture;
    check("every call: complete for any need", Complete(all, 0) && Complete(all, both));
    check("nothing looked up: not complete", !Complete(Procs{}, 0));

    Procs p = all;
    p.destroyImage = nullptr;
    check("no eglDestroyImageKHR: enough for the Renderer, not for ft-handview",
          Complete(p, kImageTargetTexture) && !Complete(p, kDestroyImage));
    p = all;
    p.imageTargetTexture = nullptr;
    check("no glEGLImageTargetTexture2DOES: enough for ft-handview, not for the Renderer",
          Complete(p, kDestroyImage) && !Complete(p, kImageTargetTexture));
    bool each = true;
    for (int i = 0; i < 6; ++i) {
        p = all;
        if (i == 0) p.getPlatformDisplay = nullptr;
        if (i == 1) p.createImage = nullptr;
        if (i == 2) p.imageTargetRenderbuffer = nullptr;
        if (i == 3) p.createSync = nullptr;
        if (i == 4) p.destroySync = nullptr;
        if (i == 5) p.clientWaitSync = nullptr;
        each = each && !Complete(p, 0);
    }
    check("any shared call missing: not complete", each);

    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
