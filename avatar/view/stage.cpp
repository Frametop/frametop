// SteamVR and the GPU for ft-avatars with a headset on (stage.h).
#include "stage.h"

#include "rendernode.h"

#include <cstdio>

namespace stage {

bool Stage::Open(const char *who) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) return std::fprintf(stderr, "%s: openvr: %s\n", who, vr::VR_GetVRInitErrorAsEnglishDescription(err)), false;
    vr_ = true;
    if (!vr::VRIPCResourceManager()) return std::fprintf(stderr, "%s: openvr: no IVRIPCResourceManagerClient\n", who), false;
    maker.who = who, maker.egl = gpuctx::Load(), maker.modifiers = vroutput::Modifiers();
    maker.gbm = rendernode::Acquire(who);
    return maker.gbm && gpuctx::Open(who, maker.gbm, maker.egl, gpuctx::kDestroyImage, &maker.dpy, &ctx) && painter.Init(who);
}

void Stage::Close() {
    painter.Done();
    gpuctx::Close(maker.dpy, ctx);
    if (maker.gbm) rendernode::Release();
    if (vr_) vr::VR_Shutdown();
    *this = Stage();
}

}  // namespace stage
