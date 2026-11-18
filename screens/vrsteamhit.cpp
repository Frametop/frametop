// Steam's own overlays out of the laser's way (vrsteamhit.h).
#include "vrsteamhit.h"
#include "vrpanel.h"

#include <cstdio>
#include <set>

namespace ftvr {
namespace steamhit {
namespace {

std::set<uint64_t> g_masked;   // Steam's overlays the laser goes through now
const char *const kFixed[] = {"valve.steam.gamepadui.main", "valve.steam.gamepadui.keyboard",
                              "valve.steam.gamepadui.notifications"};

void Hittable(uint64_t steams, bool hittable) {
    vr::VROverlayIntersectionMaskPrimitive_t none{};   // (a circle too small to hit, at a corner)
    none.m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Circle;
    none.m_Primitive.m_Circle = {0.f, 0.f, 1e-6f};
    const vr::EVROverlayError e = hittable ? vr::VROverlay()->SetOverlayIntersectionMask(steams, nullptr, 0)
                                           : vr::VROverlay()->SetOverlayIntersectionMask(steams, &none, 1);
    if (e != vr::VROverlayError_None && e != vr::VROverlayError_UnknownOverlay)
        std::printf("Steam's overlay 0x%llx: its laser mask not %s (%s)\n", (unsigned long long)steams,
                    hittable ? "cleared" : "set", vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
}

void Become(std::set<uint64_t> want, bool all) {
    for (const uint64_t h : want)
        if (all || !g_masked.count(h)) Hittable(h, false);
    for (const uint64_t h : g_masked)
        if (!want.count(h)) Hittable(h, true);
    g_masked = std::move(want);
}

}  // namespace

void Step(const steamwin::Fleet &fleet, bool all) {
    std::set<uint64_t> want;
    for (const auto &[id, t] : fleet.Windows()) {
        if (!t.Steams()) continue;
        if (t.Panel()) want.insert(t.Steams());
        else if (all) Hittable(t.Steams(), true);
    }
    Become(std::move(want), all);
}

void Clear() { Become({}, false); }

void Sweep() {
    for (const char *key : kFixed) {
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
        if (vr::VROverlay()->FindOverlay(key, &h) == vr::VROverlayError_None) Hittable(h, true);
    }
}

}  // namespace steamhit
}  // namespace ftvr
