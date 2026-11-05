// Print SteamVR's view of tracked devices: class, hand role, controller type,
// connection, pose validity, activity level, charging, and the dashboard's primary pointer device.
// Runs as a background OpenVR client (in the dev container).
#include <openvr.h>

#include "vrmath.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

// Measure a floating panel (see md::ScanPanel).
static int Scan(vr::IVRSystem *sys, const char *key, double step) {
    vr::VROverlayHandle_t h;
    if (vr::VROverlay()->FindOverlay(key, &h) != vr::VROverlayError_None) {
        std::printf("no overlay %s\n", key);
        return 1;
    }
    vr::TrackedDevicePose_t head;
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &head, 1);
    const md::Vec3 eye = md::Position(head.mDeviceToAbsoluteTracking);
    const md::Panel p = md::ScanPanel(h, eye, step);
    std::printf("scan %s: %d hits, head (%.3f %.3f %.3f)\n", key, p.hits, eye.x, eye.y, eye.z);
    if (!p.found) return 1;
    const md::Vec3 c = p.center, x = p.basis.x, y = p.basis.y, z = p.basis.z, to = c - eye;
    std::printf("center (%.3f %.3f %.3f) width %.3f height %.3f distance %.3f\n", c.x, c.y, c.z, p.width, p.height,
                md::Length(to));
    std::printf("x (%.3f %.3f %.3f) y (%.3f %.3f %.3f) front (%.3f %.3f %.3f)\n", x.x, x.y, x.z, y.x, y.y, y.z, z.x,
                z.y, z.z);
    return 0;
}

// Usage: vrprobe [overlay-key...]  (extra overlays to check besides the dashboard's)
//        vrprobe --scan <overlay-key> [step-degrees]
int main(int argc, char **argv) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem *sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--scan") == 0) {
        const int r = Scan(sys, argv[2], argc >= 4 ? std::atof(argv[3]) : 1.0);
        vr::VR_Shutdown();
        return r;
    }
    static const char *classes[] = {"invalid", "HMD", "controller", "tracker", "reference", "display"};
    static const char *roles[] = {"none", "left", "right", "optout", "treadmill", "stylus"};
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        const auto cls = sys->GetTrackedDeviceClass(i);
        if (cls == vr::TrackedDeviceClass_Invalid) continue;
        char type[64] = "", serial[64] = "";
        sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
        sys->GetStringTrackedDeviceProperty(i, vr::Prop_SerialNumber_String, serial, sizeof serial);
        const auto role = sys->GetControllerRoleForTrackedDeviceIndex(i);
        const int hint = sys->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerRoleHint_Int32);
        // Activity level and charging: whether a controller can be told in a hand from on a desk
        // or a charger (the handoff arbiter, pointer/helper/handoff.h; probe P4).
        static const char *levels[] = {"unknown", "idle", "interaction", "interaction-timeout", "standby", "idle-timeout"};
        const int level = int(sys->GetTrackedDeviceActivityLevel(i)) + 1;
        vr::ETrackedPropertyError pe;
        const bool charging = sys->GetBoolTrackedDeviceProperty(i, vr::Prop_DeviceIsCharging_Bool, &pe);
        std::printf("%u %-10s role=%-6s hint=%d type=%-16s connected=%d pose=%d activity=%s charging=%s %s\n", i,
                    cls < 6 ? classes[cls] : "?", role < 6 ? roles[role] : "?", hint, type,
                    sys->IsTrackedDeviceConnected(i), poses[i].bPoseIsValid,
                    level >= 0 && level < 6 ? levels[level] : "?",
                    pe == vr::TrackedProp_Success ? (charging ? "yes" : "no") : "n/a", serial);
    }
    std::printf("left hand = %u, right hand = %u\n",
                sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand),
                sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand));
    if (poses[0].bPoseIsValid) {
        // Head forward is -Z of the HMD pose. yaw 0 = -Z, positive yaw turns left (about +Y).
        const auto &m = poses[0].mDeviceToAbsoluteTracking.m;
        const float fx = -m[0][2], fy = -m[1][2], fz = -m[2][2];
        std::printf("head yaw = %.1f pitch = %.1f\n", std::atan2(-fx, -fz) * 180.0 / M_PI,
                    std::asin(fy) * 180.0 / M_PI);
    }
    // Pointer calibration: our device's forward ray vs where SteamVR's hit dot actually is.
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        char type[64] = "";
        sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
        if (std::strcmp(type, "ft_pointer") != 0 || !poses[i].bPoseIsValid) continue;
        const auto &m = poses[i].mDeviceToAbsoluteTracking.m;
        const double o[3] = {m[0][3], m[1][3], m[2][3]}, f[3] = {-m[0][2], -m[1][2], -m[2][2]};
        std::printf("ft_pointer origin (%.3f %.3f %.3f) forward (%.3f %.3f %.3f) yaw %.1f pitch %.1f\n", o[0], o[1], o[2],
                    f[0], f[1], f[2], std::atan2(-f[0], -f[2]) * 180 / M_PI, std::asin(f[1]) * 180 / M_PI);
        for (const char *key : {"system.pointer", "system.pointer.secondary", "frametop.pointer.cursor"}) {
            vr::VROverlayHandle_t h;
            if (vr::VROverlay()->FindOverlay(key, &h) != vr::VROverlayError_None) continue;
            vr::ETrackingUniverseOrigin origin;
            vr::HmdMatrix34_t t{};
            const bool visible = vr::VROverlay()->IsOverlayVisible(h);
            vr::VROverlayTransformType type2;
            vr::VROverlay()->GetOverlayTransformType(h, &type2);
            if (vr::VROverlay()->GetOverlayTransformAbsolute(h, &origin, &t) != vr::VROverlayError_None) {
                std::printf("  %-28s visible=%d transform type %d (not absolute)\n", key, visible, int(type2));
                continue;
            }
            const double p2[3] = {t.m[0][3] - o[0], t.m[1][3] - o[1], t.m[2][3] - o[2]};
            const double d = std::sqrt(p2[0] * p2[0] + p2[1] * p2[1] + p2[2] * p2[2]);
            const double dir[3] = {p2[0] / d, p2[1] / d, p2[2] / d};
            float w = 0;
            vr::VROverlay()->GetOverlayWidthInMeters(h, &w);
            std::printf("  %-28s visible=%d at (%.3f %.3f %.3f) dist %.2f yaw %.1f pitch %.1f width %.3f\n", key, visible,
                        t.m[0][3], t.m[1][3], t.m[2][3], d, std::atan2(-dir[0], -dir[2]) * 180 / M_PI,
                        std::asin(dir[1]) * 180 / M_PI, w);
        }
    }
    // Overlay check: transform type, size, and a ray test along ft_pointer's laser.
    std::vector<std::string> keys = {"valve.steam.gamepadui.floatingfooter", "valve.steam.gamepadui.bar", "system.systemui"};
    keys.insert(keys.end(), argv + 1, argv + argc);
    for (const auto &k : keys) {
        const char *key = k.c_str();
        vr::VROverlayHandle_t h;
        if (vr::VROverlay()->FindOverlay(key, &h) != vr::VROverlayError_None) continue;
        vr::VROverlayTransformType tt;
        vr::VROverlay()->GetOverlayTransformType(h, &tt);
        float w = 0;
        vr::VROverlay()->GetOverlayWidthInMeters(h, &w);
        uint32_t tw = 0, th = 0;
        vr::VROverlay()->GetOverlayTextureSize(h, &tw, &th);
        std::printf("overlay %-44s visible=%d type=%d width=%.3fm tex=%ux%u", key, vr::VROverlay()->IsOverlayVisible(h),
                    int(tt), w, tw, th);
        vr::ETrackingUniverseOrigin uo;
        vr::HmdMatrix34_t t{};
        if (tt == vr::VROverlayTransform_Absolute && vr::VROverlay()->GetOverlayTransformAbsolute(h, &uo, &t) == vr::VROverlayError_None)
            std::printf(" at (%.2f %.2f %.2f) normal (%.2f %.2f %.2f)", t.m[0][3], t.m[1][3], t.m[2][3], t.m[0][2],
                        t.m[1][2], t.m[2][2]);
        else
            std::printf(" transform type %d", int(tt));
        vr::HmdVector2_t mouse{};
        vr::VROverlay()->GetOverlayMouseScale(h, &mouse);
        vr::VROverlayInputMethod im = vr::VROverlayInputMethod_None;
        vr::VROverlay()->GetOverlayInputMethod(h, &im);
        uint32_t flags = 0;
        vr::VROverlay()->GetOverlayFlags(h, &flags);
        std::printf(" mouse=%.0fx%.0f input=%d flags=0x%x", mouse.v[0], mouse.v[1], int(im), flags);
        if (poses[0].bPoseIsValid) {  // gaze ray: from the head, straight ahead
            const auto &m = poses[0].mDeviceToAbsoluteTracking.m;
            vr::VROverlayIntersectionParams_t params{};
            params.vSource = {m[0][3], m[1][3], m[2][3]};
            params.vDirection = {-m[0][2], -m[1][2], -m[2][2]};
            params.eOrigin = vr::TrackingUniverseStanding;
            vr::VROverlayIntersectionResults_t hit{};
            const bool ok = vr::VROverlay()->ComputeOverlayIntersection(h, &params, &hit);
            std::printf(" | gaze hit=%d dist=%.2f uv=(%.2f %.2f) at (%.2f %.2f %.2f)", ok, hit.fDistance, hit.vUVs.v[0],
                        hit.vUVs.v[1], hit.vPoint.v[0], hit.vPoint.v[1], hit.vPoint.v[2]);
        }
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            char type[64] = "";
            sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
            if (std::strcmp(type, "ft_pointer") != 0 || !poses[i].bPoseIsValid) continue;
            const auto &m = poses[i].mDeviceToAbsoluteTracking.m;
            vr::VROverlayIntersectionParams_t params{};
            params.vSource = {m[0][3], m[1][3], m[2][3]};
            params.vDirection = {-m[0][2], -m[1][2], -m[2][2]};
            params.eOrigin = vr::TrackingUniverseStanding;
            vr::VROverlayIntersectionResults_t hit{};
            const bool ok = vr::VROverlay()->ComputeOverlayIntersection(h, &params, &hit);
            std::printf(" | laser hit=%d dist=%.2f uv=(%.2f %.2f)", ok, hit.fDistance, hit.vUVs.v[0], hit.vUVs.v[1]);
        }
        std::printf("\n");
    }
    std::printf("primary dashboard device = %u, dashboard visible = %d\n",
                vr::VROverlay()->GetPrimaryDashboardDevice(), vr::VROverlay()->IsDashboardVisible());
    vr::VR_Shutdown();
    return 0;
}
