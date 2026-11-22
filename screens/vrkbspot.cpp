// Where Full Keyboard opens (keyboard/host/placement.py turns it into the panel's pose): the
// bottom centre of Steam's page while SteamVR's dashboard shows it (Frametop's tile of the page,
// frametop.steamdash, when Steam's window is ours: vrsteamwin.cpp), else the headset's pose. The
// relay passes it on in "kbpanel show" (compositor.c's keyboard_command). Also whether a laser is on
// the panel (LaserOnFullKeyboard).

#include "vr.h"
#include "vrpanel.h"

#include <openvr.h>

#include <cmath>
#include <cstdio>

namespace {
// key's bottom centre in the standing universe, if it's shown (an invisible tab reports a valid
// but frozen transform). Its mouse coordinates run up from the bottom and include its scale.
bool Bottom(const char *key, vr::HmdMatrix34_t *out) {
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    vr::HmdVector2_t scale{};
    if (ov->FindOverlay(key, &h) != vr::VROverlayError_None || !ov->IsOverlayVisible(h) ||
        ov->GetOverlayMouseScale(h, &scale) != vr::VROverlayError_None || !std::isfinite(scale.v[0]) ||
        !std::isfinite(scale.v[1]) || !(scale.v[0] > 0) || !(scale.v[1] > 0))
        return false;
    const vr::HmdVector2_t bottom{{scale.v[0] / 2, 0}};
    return ov->GetTransformForOverlayCoordinates(h, vr::TrackingUniverseStanding, bottom, out) ==
           vr::VROverlayError_None;
}
}  // namespace

extern "C" bool ft_vr_keyboard_spot(char *out, int size) {
    if (!vr::VROverlay() || !vr::VRSystem() || size < 1) return false;
    vr::HmdMatrix34_t m{};
    const char *kind = "dash";
    if (!vr::VROverlay()->IsDashboardVisible() ||
        !(Bottom("frametop.steamdash", &m) || Bottom("valve.steam.gamepadui.main", &m))) {
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bPoseIsValid) return false;
        m = pose.mDeviceToAbsoluteTracking;
        kind = "head";
    }
    int n = std::snprintf(out, size, "%s", kind);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            // The relay takes up to three digits before the point: anything else isn't a pose.
            if (!std::isfinite(m.m[r][c]) || std::fabs(m.m[r][c]) >= 1000 || n < 0 || n >= size) return false;
            n += std::snprintf(out + n, size - n, " %.4f", m.m[r][c]);
        }
    return n > 0 && n < size;
}

namespace ftvr {
bool LaserOnFullKeyboard(vr::TrackedDeviceIndex_t dev, vr::VROverlayHandle_t behind) {
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    Mat l;
    if (!ov || dev == kNone || ov->FindOverlay("org.framekeyboard.panel", &h) != vr::VROverlayError_None ||
        !ov->IsOverlayVisible(h) || !LaserPose(dev, &l))
        return false;
    vr::VROverlayIntersectionParams_t params{};
    params.vSource = {l.m[0][3], l.m[1][3], l.m[2][3]};
    params.vDirection = {-l.m[0][2], -l.m[1][2], -l.m[2][2]};
    params.eOrigin = vr::TrackingUniverseStanding;
    vr::VROverlayIntersectionResults_t r{}, b{};
    if (!ov->ComputeOverlayIntersection(h, &params, &r)) return false;
    return behind == vr::k_ulOverlayHandleInvalid || !ov->ComputeOverlayIntersection(behind, &params, &b) ||
           r.fDistance <= b.fDistance;
}
}  // namespace ftvr
