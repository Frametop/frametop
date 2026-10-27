// Test-only API shim. This executable cannot connect to SteamVR.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
// Advance only the helper's explicit deadline clock after a selected API call.
// This makes expiry-during-setup tests deterministic even on a loaded machine;
// std::chrono's real countdown clock is untouched. This header is test-only.
inline bool ft_test_deadline_expired = false;
inline int ft_test_clock_gettime(clockid_t clock, timespec *now) {
    const int result = ::clock_gettime(clock, now);
    if (!result && clock == CLOCK_MONOTONIC && ft_test_deadline_expired) now->tv_sec += 120;
    return result;
}
#define clock_gettime ft_test_clock_gettime
namespace vr {
using VROverlayHandle_t = uint64_t;
constexpr VROverlayHandle_t k_ulOverlayHandleInvalid = 0;
constexpr uint32_t k_unTrackedDeviceIndex_Hmd = 0;
enum EVROverlayError { VROverlayError_None, VROverlayError_Failed };
enum EVRInitError { VRInitError_None, VRInitError_Failed };
enum EVRApplicationType { VRApplication_Background };
enum EVROverlayInputMethod { VROverlayInputMethod_None };
enum VROverlayFlags { VROverlayFlags_MakeOverlaysInteractiveIfVisible };
enum { VREvent_ImageFailed = 517, VREvent_Quit = 700 };
struct VREvent_t { uint32_t eventType; };
struct HmdMatrix34_t { float m[3][4]; };
inline void Trace(const char *operation) {
    if (FILE *f = std::fopen(std::getenv("COUNTDOWN_TRACE"), "a")) {
        std::fprintf(f, "%s\n", operation);
        std::fclose(f);
    }
}
inline bool Fails(const char *operation) {
    const char *fail = std::getenv("COUNTDOWN_FAIL");
    return fail && !std::strcmp(fail, operation);
}
inline void Delay(const char *operation) {
    const char *where = std::getenv("COUNTDOWN_EXPIRE_ON");
    if (where && !std::strcmp(where, operation)) ft_test_deadline_expired = true;
}
struct IVROverlay {
    EVROverlayError CreateOverlay(const char *key, const char *, VROverlayHandle_t *h) {
        Trace(key); *h = 17; Trace("create"); Delay("create"); return VROverlayError_None;
    }
    EVROverlayError SetOverlayInputMethod(VROverlayHandle_t, EVROverlayInputMethod) { Trace("no-input"); return VROverlayError_None; }
    EVROverlayError SetOverlayFlag(VROverlayHandle_t, VROverlayFlags, bool on) { Trace(on ? "interactive" : "non-interactive"); return VROverlayError_None; }
    EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t, float w) { Trace(w == .4f ? "width-0.4" : "wrong-width"); return VROverlayError_None; }
    EVROverlayError SetOverlaySortOrder(VROverlayHandle_t, uint32_t) { return VROverlayError_None; }
    EVROverlayError SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t, uint32_t index, const HmdMatrix34_t *p) {
        Trace(index == 0 && p->m[2][3] == -1.f && p->m[1][3] == .06f ? "head-relative" : "wrong-transform");
        return VROverlayError_None;
    }
    EVROverlayError SetOverlayRaw(VROverlayHandle_t, void *, uint32_t w, uint32_t h, uint32_t bpp) {
        Trace(w == 640 && h == 360 && bpp == 4 ? "upload" : "wrong-image");
        Delay("upload");
        return Fails("upload") ? VROverlayError_Failed : VROverlayError_None;
    }
    EVROverlayError ShowOverlay(VROverlayHandle_t) { Trace("show"); return VROverlayError_None; }
    EVROverlayError HideOverlay(VROverlayHandle_t) { Trace("hide"); return VROverlayError_None; }
    EVROverlayError DestroyOverlay(VROverlayHandle_t) { Trace("destroy"); return VROverlayError_None; }
    bool PollNextOverlayEvent(VROverlayHandle_t, VREvent_t *, uint32_t) { Delay("poll"); return false; }
    const char *GetOverlayErrorNameFromEnum(EVROverlayError) { return "test failure"; }
};
inline IVROverlay *VROverlay() { static IVROverlay o; return &o; }
inline void *VR_Init(EVRInitError *e, EVRApplicationType) { Trace("init"); Delay("init"); *e = Fails("init") ? VRInitError_Failed : VRInitError_None; return nullptr; }
inline void VR_Shutdown() { Trace("shutdown"); }
inline const char *VR_GetVRInitErrorAsEnglishDescription(EVRInitError) { return "test initialization failure"; }
}
