// Tests execute vr.cpp's real overlay adapter against an in-memory SteamVR boundary.
// No SteamVR initialization, camera, production socket, or child process is permitted.
#include <cassert>
#include <deque>
#include <limits>
#include <spawn.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "volume-openvr-defaults.h"

std::vector<std::vector<std::string>> launches;
int TestSpawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
              char *const argv[], char *const []) {
    std::vector<std::string> args;
    for (int i = 0; argv[i]; ++i) args.emplace_back(argv[i]);
    launches.push_back(args);
    return 0;
}
#define posix_spawn TestSpawn
#include "../vr.cpp"
#undef posix_spawn

struct FakeOverlay : vr::DefaultIVROverlay {
    struct Part {
        std::string key;
        bool shown = false;
        vr::VROverlayInputMethod input = vr::VROverlayInputMethod_None;
        std::map<vr::VROverlayFlags, bool> flags;
        Mat pose{};
        double width = 0;
        float alpha = 0, color[3] = {};
        uint32_t textureWidth = 0, textureHeight = 0, uploads = 0, sort = 0, shows = 0, hides = 0;
        std::deque<vr::VREvent_t> events;
    };
    std::map<vr::VROverlayHandle_t, Part> parts;
    std::vector<std::string> lifetime;
    bool steamMenu = false, dashboard = false, failNextShow = false;
    vr::EVROverlayError FindOverlay(const char *key, vr::VROverlayHandle_t *out) override {
        if (std::string(key) == "valve.steam.gamepadui.frame.menu.55880005") {
            *out = 9000; return vr::VROverlayError_None;
        }
        if (std::string(key) == "system.systemui" || std::string(key) == "valve.steam.gamepadui.bar") {
            *out = 9001; return vr::VROverlayError_None;
        }
        return vr::VROverlayError_UnknownOverlay;
    }
    bool IsOverlayVisible(vr::VROverlayHandle_t o) override { return o==9001 || (o==9000 ? steamMenu : parts.at(o).shown); }
    bool IsDashboardVisible() override { return dashboard; }
    vr::EVROverlayError CreateOverlay(const char *key, const char *, vr::VROverlayHandle_t *out) override {
        *out = parts.size() + 1;
        parts[*out].key = key;
        lifetime.push_back("create:" + std::string(key));
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t o) override {
        lifetime.push_back("destroy:" + parts.at(o).key);
        assert(parts.erase(o) == 1); return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t o, vr::VROverlayInputMethod m) override {
        parts.at(o).input = m; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t o, vr::VROverlayFlags f, bool b) override {
        parts.at(o).flags[f] = b; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t o, uint32_t order) override {
        parts.at(o).sort = order; return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlaySortOrder(vr::VROverlayHandle_t o, uint32_t *order) override {
        *order = parts.at(o).sort; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t o, float w) override {
        parts.at(o).width = w; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t o, vr::ETrackingUniverseOrigin origin,
                                                   const Mat *p) override {
        assert(origin == vr::TrackingUniverseStanding);
        parts.at(o).pose = *p; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t o, void *, uint32_t w, uint32_t h, uint32_t bpp) override {
        assert(bpp == 4); parts.at(o).textureWidth = w; parts.at(o).textureHeight = h;
        ++parts.at(o).uploads; lifetime.push_back("upload:" + parts.at(o).key);
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t o, float a) override {
        parts.at(o).alpha = a; return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayColor(vr::VROverlayHandle_t o, float r, float g, float b) override {
        auto &p = parts.at(o); p.color[0] = r, p.color[1] = g, p.color[2] = b;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t o) override {
        if (failNextShow) { failNextShow = false; return vr::VROverlayError_RequestFailed; }
        assert(parts.at(o).key.find("passthrough") == std::string::npos || parts.at(o).uploads > 0);
        ++parts.at(o).shows; parts.at(o).shown = true; return vr::VROverlayError_None;
    }
    vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t o) override {
        ++parts.at(o).hides; parts.at(o).shown = false; return vr::VROverlayError_None;
    }
    bool PollNextOverlayEvent(vr::VROverlayHandle_t o, vr::VREvent_t *event, uint32_t) override {
        auto &queue = parts.at(o).events;
        if (queue.empty()) return false;
        *event = queue.front(); queue.pop_front(); return true;
    }
    vr::VROverlayHandle_t Find(const std::string &key) {
        for (auto &[o, part] : parts) if (part.key == key) return o;
        return vr::k_ulOverlayHandleInvalid;
    }
} overlay;
struct FakeSystem : vr::DefaultIVRSystem {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    bool tipModel = false, controllerStateAvailable = true;
    bool GetControllerState(vr::TrackedDeviceIndex_t, vr::VRControllerState_t *state, uint32_t size) override {
        assert(size == sizeof(*state)); *state = {}; return controllerStateAvailable;
    }
    void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t *out,
                                         uint32_t count) override {
        std::copy(poses, poses + count, out);
    }
    vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t i) override {
        return i == 3 || i == 4 ? vr::TrackedDeviceClass_Controller : vr::TrackedDeviceClass_Invalid;
    }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty prop,
                                            char *out, uint32_t n, vr::ETrackedPropertyError *) override {
        const char *name = prop == vr::Prop_ControllerType_String ? (i == 4 ? "ft_pointer" : "test_controller") : (prop == vr::Prop_RenderModelName_String && tipModel && i == 3 ? "upstream-tip" : "");
        std::snprintf(out, n, "%s", name); return uint32_t(std::strlen(name) + 1);
    }
} systemVR;
struct FakeApplications : vr::DefaultIVRApplications {
    uint32_t scene = 0;
    uint32_t GetCurrentSceneProcessId() override { return scene; }
} applications;
struct FakeModels : vr::DefaultIVRRenderModels {
    int tipCalls = 0;
    bool GetComponentState(const char *model, const char *component, const vr::VRControllerState_t *,
                          const vr::RenderModel_ControllerMode_State_t *, vr::RenderModel_ComponentState_t *state) override {
        assert(std::string(model) == "upstream-tip" && std::string(component) == "tip");
        ++tipCalls; state->mTrackingToComponentLocal = PanelPose(0,0,0,0,-40,0); return true;
    }
    bool GetComponentStateForDevicePath(const char *, const char *, vr::VRInputValueHandle_t,
                                      const vr::RenderModel_ControllerMode_State_t *, vr::RenderModel_ComponentState_t *) override {
        assert(false && "must preserve upstream GetComponentState tip lookup"); return false;
    }
} models;
namespace vr {
extern "C" uint32_t VR_GetInitToken() { return 0; }
extern "C" void VR_ShutdownInternal() {} // boundary stub, never reaches SteamVR
extern "C" void *VR_GetGenericInterface(const char *name, EVRInitError *error) {
    *error = VRInitError_None;
    if (!std::strcmp(name, IVROverlay_Version)) return &overlay;
    if (!std::strcmp(name, IVRSystem_Version)) return &systemVR;
    if (!std::strcmp(name, IVRApplications_Version)) return &applications;
    if (!std::strcmp(name, IVRRenderModels_Version)) return &models;
    assert(false && "test tried an unexpected SteamVR interface"); return nullptr;
}
}
void Tick() { ft_vr_poll([](const ft_event *, void *) {}, nullptr); }
void Send(vr::VROverlayHandle_t handle, uint32_t type, unsigned device = 4, uint32_t button = vr::VRMouseButton_Left) {
    vr::VREvent_t ev{};
    ev.eventType = type; ev.trackedDeviceIndex = device; ev.data.mouse.button = button;
    overlay.parts.at(handle).events.push_back(ev);
}
void Command(const char *text) {
    char reply[512]{};
    ft_vr_command(text, reply, sizeof reply);
    assert(!std::strncmp(reply, "ok", 2));
}
std::string Reply(const char *text) {
    char reply[512]{}; ft_vr_command(text, reply, sizeof reply); return reply;
}
void ControlsShown(bool shown) {
    for (const auto &[o, part] : overlay.parts)
        if (part.key.find("passthrough") != std::string::npos) assert(part.shown == shown);
}
#include "volume-release-regressions.h"
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--persistence-fixture") return PersistenceFixture();
#ifdef FT_CPU_KEYBOARD
    if (argc == 2 && std::string(argv[1]) == "--keyboard-release") return KeyboardReleaseRegression();
    if (argc == 2 && std::string(argv[1]) == "--catcher-ownership") return CatcherOwnershipRegression();
    if (argc == 2 && std::string(argv[1]) == "--desktop-buttons") return DesktopButtonOwnershipRegression();
#endif
    assert(Reply("passthrough state3d") == "ok 0 0 0.75 -0.65 1 0.35 0.65 0 0 0 no-vr");
    assert(Reply("passthrough committed") == Reply("passthrough state3d"));
    const auto deskMask = Reply("passthrough mask");
    assert(deskMask.find("\"hands_wanted\":false") != std::string::npos);
    assert(deskMask.find("\"hands_valid\":false") != std::string::npos);
    assert(deskMask.find("\"capsules\":[]") != std::string::npos);
    assert(Reply("passthrough controls state") == "ok controls 0"); // safe without VR
    assert(Reply("passthrough controls 1") == "ok controls 1");
    assert(overlay.parts.empty());
    assert(Reply("passthrough controls 0") == "ok controls 0");
    g_vr = true; // do NOT initialize SteamVR
    systemVR.poses[0].bPoseIsValid = true;
    systemVR.poses[0].mDeviceToAbsoluteTracking = Translation(0, 1.6, 0);
    Command("passthrough set 1 0 0.75 -0.65 1 0.35 0.65 37");
    Tick();
    assert(overlay.parts.empty()); // controls default off, independent of legacy enabled
    Command("passthrough controls 1"); Tick();
    assert(overlay.parts.size() == 9); // four rectangle edges, four resize handles, one grab bar
    const auto edges = passthrough::RectangleEdges(g_passthrough);
    for (int i = 0; i < 4; ++i) {
        // Display-only edges use the helper's excluded guide namespace, so its
        // ComputeOverlayIntersection scan cannot stick the cursor to a ribbon.
        const auto o = overlay.Find("frametop.guide.passthrough.edge." + std::to_string(i));
        assert(o != vr::k_ulOverlayHandleInvalid);
        const auto &part = overlay.parts.at(o);
        assert(part.shown && part.input == vr::VROverlayInputMethod_None);
        assert(!part.flags.count(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
        assert(part.sort == uint32_t(10001 + i));
        assert(part.flags.at(vr::VROverlayFlags_VisibleInDashboard));
        double length = 0;
        for (int k = 0; k < 3; ++k) length += std::pow(edges[i].b[k] - edges[i].a[k], 2);
        length = std::sqrt(length);
        assert(std::fabs(part.width - length) < 1e-6);
        for (int k = 0; k < 3; ++k)
            assert(std::fabs(part.pose.m[k][3] - (edges[i].a[k] + edges[i].b[k]) / 2) < 1e-6);
    }
    for (int i : {0, 1, 4, 5}) {
        const auto o = overlay.Find("frametop.passthrough.face." + std::to_string(i));
        assert(o != vr::k_ulOverlayHandleInvalid);
        const auto &part = overlay.parts.at(o);
        assert(part.shown && part.input == vr::VROverlayInputMethod_Mouse);
        assert(part.sort == uint32_t(10013 + i));
        assert(part.flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
        const auto center = passthrough::RectangleControls(g_passthrough)[i];
        for (int k = 0; k < 3; ++k) assert(std::fabs(part.pose.m[k][3] - center[k]) < 1e-6);
    }
    const auto bar = overlay.Find("frametop.passthrough.grab");
    assert(bar != vr::k_ulOverlayHandleInvalid);
    auto &barPart = overlay.parts.at(bar);
    assert(barPart.shown && barPart.input == vr::VROverlayInputMethod_Mouse && barPart.sort == 10019);
    assert(barPart.textureWidth == 256 && barPart.textureHeight == 56 && barPart.uploads == 1);
    const auto barCenter = passthrough::GrabCenter(g_passthrough);
    for (int k = 0; k < 3; ++k) assert(std::fabs(barPart.pose.m[k][3] - barCenter[k]) < 1e-6);
    const auto legacyState = Reply("passthrough state");
    for (const auto *bad : {"passthrough controls", "passthrough controls 2", "passthrough controls on",
                            "passthrough controls 1 extra", "passthrough controls state extra"})
        assert(Reply(bad).find("error") == 0);
    assert(Reply("passthrough controls state") == "ok controls 1");
    assert(Reply("passthrough state") == legacyState);
    assert(legacyState == "ok 1 0 0.75 -0.65 1 0.35 0.65 37 idle");
    Command("hide"); Tick();
    for (auto &[o, part] : overlay.parts) assert(part.shown); // independent of screens
    const auto beforeRetry = overlay.lifetime;
    Command("passthrough controls 0");
    overlay.failNextShow = true;
    Command("passthrough controls 1");
    assert(!g_volumeEdges[0].shown); // a failed API call is not cached as success
    Tick(); ControlsShown(true);
    assert(overlay.lifetime == beforeRetry); // retry visibility, never recreate
    const auto savedLifetime = overlay.lifetime;
    const auto savedPlacement = Reply("passthrough state3d");
    overlay.steamMenu = true; usleep(60000); Tick();
    ControlsShown(true); // menu changes compositing order, never control visibility
    assert(Reply("passthrough state3d") == savedPlacement);
    Command("passthrough layer 1"); Tick(); ControlsShown(true);
    Command("passthrough layer 0"); Tick(); ControlsShown(true);
    overlay.steamMenu = false; usleep(60000); Tick(); ControlsShown(true);
    assert(overlay.lifetime == savedLifetime); // no uploads/create/destroy
    // Head yaw alone does not move billboards or trigger a 180-degree cutoff.
    const auto posesBeforeTurn = overlay.parts;
    for (double yaw : {90., 180., -90., 0.}) {
        systemVR.poses[0].mDeviceToAbsoluteTracking = PanelPose(0, 1.6, 0, yaw, 0, 0);
        Tick(); ControlsShown(true);
        for (const auto &[o, part] : overlay.parts) {
            assert(std::memcmp(part.pose.m, posesBeforeTurn.at(o).pose.m, sizeof part.pose.m) == 0);
            assert(RigidPose(part.pose));
        }
    }
    assert(overlay.lifetime == savedLifetime);
    const auto handle = overlay.Find("frametop.passthrough.face.1");
    const float idle = overlay.parts.at(handle).alpha;
    Send(handle, vr::VREvent_MouseMove); Tick();
    assert(overlay.parts.at(handle).alpha > idle);
    // Layer changes preserve handles, textures, hover and visibility. No camera
    // lifetime/visibility input participates in the owning native controls loop.
    const auto lifetime = overlay.lifetime;
    const auto counts = overlay.parts;
    const auto volumeBeforeLayer = Reply("passthrough state3d");
    assert(Reply("passthrough layer state") == "ok layer 0");
    for (bool above : {true, false, true, false}) {
        Command(above ? "passthrough layer 1" : "passthrough layer 0"); Tick();
        assert(Reply("passthrough layer state") == (above ? "ok layer 1" : "ok layer 0"));
        assert(Reply("passthrough state3d") == volumeBeforeLayer);
        for (auto &[o, part] : overlay.parts) {
            assert(part.sort == counts.at(o).sort && part.sort > 10000u);
            assert(part.shows == counts.at(o).shows && part.hides == counts.at(o).hides);
            assert(part.uploads == counts.at(o).uploads);
        }
        assert(overlay.parts.at(handle).alpha > idle);
        assert(overlay.lifetime == lifetime);
    }
    for (auto bad : {"passthrough layer", "passthrough layer 2", "passthrough layer true", "passthrough layer state extra"})
        assert(Reply(bad).find("error") == 0);
    // A one-sample HMD validity dropout must not hide every owned control.
    systemVR.poses[0].bPoseIsValid = false; Tick();
    ControlsShown(true);
    systemVR.poses[0].bPoseIsValid = true; Tick();
    assert(overlay.lifetime == lifetime);
    for (auto &[o, part] : overlay.parts)
        assert(part.shows == counts.at(o).shows && part.hides == counts.at(o).hides);
    Send(handle, vr::VREvent_FocusLeave); Tick();
    assert(overlay.parts.at(handle).alpha == idle);
    Command("passthrough set 1 0 0.75 -0.65 1 0.35 0.65 0"); Tick();
    const auto start = g_passthrough;
    const auto center = passthrough::RectangleControls(start)[1];
    systemVR.poses[4].bPoseIsValid = true;
    systemVR.poses[4].mDeviceToAbsoluteTracking = Translation(center[0], center[1], center[2] + 0.1);
    Send(handle, vr::VREvent_MouseButtonDown); Tick();
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += 0.2f;
    Tick();
    assert(std::fabs(g_passthrough.size[0] - 1.2) < 1e-6);
    assert(std::fabs(g_passthrough.center[0] - 0.1) < 1e-6);
    assert(overlay.parts.at(handle).alpha == 1.f);
    const auto other = overlay.Find("frametop.passthrough.face.4");
    Send(other, vr::VREvent_MouseButtonUp); Tick(); // release over another overlay
    assert(launches.size() == 1 && launches[0].size() == 2 && launches[0][1] == "passthrough-capture-release");
    char executable[PATH_MAX]; assert(realpath("/proc/self/exe", executable));
    std::string helper(executable);
    for (int up=0;up<3;++up) helper.resize(helper.rfind('/'));
    helper += "/layout/ft-layout";
    assert(launches[0][0] == helper); // exercises upstream executable-relative RunLayout
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += 0.2f; Tick();
    assert(std::fabs(g_passthrough.size[0] - 1.2) < 1e-6); // stopped
    launches.clear();
    // Volume clicks do not belong to KWin. Its own release catcher still catches a
    // controller release in empty space, and helper `up` works without g_press.
    assert(g_press.buttons == 0);
    vr::VROverlay()->CreateOverlay("frametop.catcher", "test catcher", &g_catcher);
    auto begin = [&] {
        Command("passthrough set 1 0 0.75 -0.65 1 0.35 0.65 0"); Tick();
        const auto p = passthrough::RectangleControls(g_passthrough)[1];
        systemVR.poses[4].bPoseIsValid = true;
        systemVR.poses[4].mDeviceToAbsoluteTracking = Translation(p[0], p[1], p[2] + 0.1);
        Send(handle, vr::VREvent_MouseButtonDown); Tick();
        systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += 0.2f; Tick();
        assert(g_volumeDrag.Active());
    };
    begin();
    Send(bar, vr::VREvent_MouseButtonDown); Tick();
    assert(g_volumeDrag.Active() && !g_volumeMove.Active());
    Send(other, vr::VREvent_MouseButtonUp, 4, vr::VRMouseButton_Right); Tick();
    assert(g_volumeDrag.Active() && launches.empty()); // only the dragging left button ends it
    assert(overlay.parts.at(g_catcher).shown);
    Send(g_catcher, vr::VREvent_MouseButtonUp); Tick();
    assert(!g_volumeDrag.Active() && launches.size() == 1);
    launches.clear();
    begin();
    Command("up");
    for (int i = 0; i < 10; ++i) Tick();
    assert(!g_volumeDrag.Active() && launches.size() == 1);
    launches.clear();
    begin();
    Command("passthrough set 1 1 2 3 2 3 4 45");
    assert(!g_volumeDrag.Active());
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += 0.2f; Tick();
    assert(g_passthrough.size == (passthrough::Point{2, 3, 4}) && launches.empty());
    begin();
    systemVR.poses[4].bPoseIsValid = false; Tick();
    assert(!g_volumeDrag.Active() && g_passthrough.size[0] == 1 && launches.empty());
    begin();
    systemVR.poses[0].bPoseIsValid = false; Tick();
    assert(!g_volumeDrag.Active() && g_passthrough.size[0] == 1 && launches.empty());
    ControlsShown(true); // cancelled input, but display-only grace
    usleep(120000); Tick();
    for (auto &[o, part] : overlay.parts) assert(!part.shown);
    systemVR.poses[0].bPoseIsValid = true;
    Command("passthrough set 0 0 0.75 -0.65 1 0.35 0.65 37"); Tick();
    ControlsShown(true); // legacy alpha disabled does not hide controls
    assert(!g_passthrough.enabled && launches.empty());
    const auto disabled = g_passthrough;
    const auto facePoint = passthrough::RectangleControls(g_passthrough)[1];
    systemVR.poses[4].mDeviceToAbsoluteTracking = Translation(facePoint[0], facePoint[1], facePoint[2] + .2);
    Send(handle, vr::VREvent_MouseButtonDown); Tick();
    assert(g_volumeDrag.Active());
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += .1; Tick();
    Command("passthrough controls 0"); // immediate hide and rollback, before next tick
    ControlsShown(false);
    assert(!g_volumeDrag.Active() && g_passthrough.center == disabled.center && g_passthrough.size == disabled.size);
    Send(handle, vr::VREvent_MouseButtonDown); Tick();
    assert(!g_volumeDrag.Active() && launches.empty());
    Command("passthrough controls 1"); Tick();
    // One-time bar upload survives pose/hover/toggle updates, no overlay churn.
    assert(barPart.uploads == 1 && overlay.parts.size() == 10); // nine parts + catcher
    const auto clickPoint = passthrough::GrabCenter(g_passthrough);
    systemVR.poses[4].mDeviceToAbsoluteTracking = Translation(clickPoint[0], clickPoint[1], clickPoint[2] + .2);
    Send(bar, vr::VREvent_MouseButtonDown); Tick();
    Send(bar, vr::VREvent_MouseButtonUp); Tick();
    assert(!g_volumeMove.Active() && launches.empty()); // click without movement does not save
    auto beginMove = [&](unsigned device = 4) {
        Command("passthrough set 0 0 0.75 -0.65 1 0.35 0.65 37"); Tick();
        const auto p = passthrough::GrabCenter(g_passthrough);
        systemVR.poses[device].bPoseIsValid = true;
        systemVR.poses[device].mDeviceToAbsoluteTracking = Translation(p[0] + .04, p[1], p[2] + .2);
        const auto initial = g_passthrough;
        Send(bar, vr::VREvent_MouseButtonDown, device); Tick();
        assert(g_volumeMove.Active());
        assert(g_passthrough.center == initial.center); // no jump on press
        systemVR.poses[device].mDeviceToAbsoluteTracking.m[0][3] += .2;
        systemVR.poses[device].mDeviceToAbsoluteTracking.m[1][3] += .1;
        systemVR.poses[device].mDeviceToAbsoluteTracking.m[2][3] -= .3;
        Tick();
        assert(std::fabs(g_passthrough.center[0] - initial.center[0] - .2) < 1e-6);
        assert(std::fabs(g_passthrough.center[1] - initial.center[1] - .1) < 1e-6);
        assert(std::fabs(g_passthrough.center[2] - initial.center[2] + .3) < 1e-6);
        assert(g_passthrough.size == initial.size && g_passthrough.yaw == 37 && !g_passthrough.enabled);
        assert(barPart.alpha == 1.f);
    };
    beginMove();
    Send(handle, vr::VREvent_MouseButtonDown, 3); Tick();
    assert(!g_volumeDrag.Active() && g_volumeMove.Device() == 4);
    Send(other, vr::VREvent_MouseButtonUp, 3); Tick();
    Send(other, vr::VREvent_MouseButtonUp, 4, vr::VRMouseButton_Right); Tick();
    assert(g_volumeMove.Active() && launches.empty());
    Screen screen;
    StartDrag(screen, Drag::Move, 4); assert(screen.drag == Drag::None);
    Send(other, vr::VREvent_MouseButtonUp); Tick();
    assert(!g_volumeMove.Active() && launches.size() == 1 && launches.back()[1] == "passthrough-capture-release");
    launches.clear();
    beginMove();
    Command("passthrough controls 0");
    ControlsShown(false); assert(!g_volumeMove.Active() && g_passthrough.center == disabled.center && launches.empty());
    Command("passthrough controls 1");
    beginMove();
    Command("passthrough set 1 1 2 3 2 3 4 45");
    assert(!g_volumeMove.Active()); Tick();
    Send(bar, vr::VREvent_MouseButtonUp); Tick();
    assert(g_passthrough.center == (passthrough::Point{1, 2, 3}) && launches.empty());
    beginMove();
    assert(Reply("passthrough controls state") == "ok controls 1");
    assert(Reply("passthrough set 0 0 0 0 0 1 1 0").find("error") == 0);
    assert(g_volumeMove.Active());
    systemVR.poses[4].bPoseIsValid = false; Tick();
    assert(!g_volumeMove.Active() && g_passthrough.center == disabled.center && launches.empty());
    beginMove();
    systemVR.poses[0].bPoseIsValid = false;
    Send(bar, vr::VREvent_MouseButtonUp); Tick();
    ControlsShown(true);
    usleep(120000); Tick();
    ControlsShown(false);
    assert(!g_volumeMove.Active() && g_passthrough.center == disabled.center && launches.empty());
    // Even a queued press on a previously shown overlay cannot start without a head.
    Send(bar, vr::VREvent_MouseButtonDown); Tick(); assert(!g_volumeMove.Active());
    systemVR.poses[0].bPoseIsValid = true;
    beginMove();
    Command("up"); for (int i = 0; i < 10; ++i) Tick();
    assert(!g_volumeMove.Active() && launches.size() == 1); launches.clear();
    beginMove(3); // real controller uses the same ray flow and release catcher
    Command("up"); for (int i = 0; i < 10; ++i) Tick();
    assert(g_volumeMove.Active() && launches.empty()); // mouse cannot release controller
    Send(g_catcher, vr::VREvent_MouseButtonUp, 3); Tick();
    assert(!g_volumeMove.Active() && launches.size() == 1); launches.clear();
    // A screen-owned controller cannot simultaneously acquire a desk handle.
    g_screens[0].drag = Drag::Move; g_screens[0].dragDevice = 3;
    Send(bar, vr::VREvent_MouseButtonDown, 3); PollVolumeControls([](const ft_event *, void *) {}, nullptr);
    assert(!g_volumeMove.Active()); g_screens.clear();
    applications.scene = 123; g_lasers = Lasers::Always; Tick();
    for (const auto &[o, part] : overlay.parts)
        if (part.input == vr::VROverlayInputMethod_Mouse && part.key != "frametop.catcher")
            assert(!part.flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
    applications.scene = 0; g_gameRunning = false; g_lasers = Lasers::Dashboard; Tick();
    assert(!barPart.flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
    g_lasers = Lasers::OutsideGames; Tick();
    assert(barPart.flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
    assert(barPart.uploads == 1);
    assert(std::none_of(overlay.lifetime.begin(), overlay.lifetime.end(), [](const auto &s) { return s.find("destroy:") == 0; }));
    // Native panel events must not leak a volume-owned gesture into KWin.
    beginMove();
    ft_vr_screen_create(0, 1, 1);
    auto &nativeScreen = g_screens.at(0);
    const auto originalScreenPose = nativeScreen.pose;
    std::vector<ft_event> delivered;
    auto panelTick = [&] {
        ft_vr_poll([](const ft_event *e, void *data) {
            static_cast<std::vector<ft_event> *>(data)->push_back(*e);
        }, &delivered);
    };
    Send(nativeScreen.overlay, vr::VREvent_MouseMove);
    Send(nativeScreen.overlay, vr::VREvent_MouseButtonDown);
    panelTick();
    assert(g_press.buttons == 0 && nativeScreen.drag == Drag::None && delivered.empty());
    Send(nativeScreen.overlay, vr::VREvent_MouseButtonUp);
    panelTick();
    assert(!g_volumeMove.Active() && launches.size() == 1 && delivered.empty());
    assert(std::memcmp(&nativeScreen.pose, &originalScreenPose, sizeof(Mat)) == 0);
    ft_vr_screen_destroy(0); launches.clear();
    // Final pose sample on the release tick must be saved, not the previous tick.
    beginMove();
    const double priorX = g_passthrough.center[0];
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] += .15;
    Send(bar, vr::VREvent_MouseButtonUp); Tick();
    assert(std::fabs(g_passthrough.center[0] - priorX - .15) < 1e-6);
    assert(launches.size() == 1); launches.clear();
    // Malformed tracking poses are treated as tracking loss, even with valid bit set.
    beginMove();
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][3] = std::numeric_limits<float>::quiet_NaN(); Tick();
    assert(!g_volumeMove.Active() && g_passthrough.center == disabled.center && launches.empty());
    beginMove();
    systemVR.poses[0].mDeviceToAbsoluteTracking.m[0][3] = std::numeric_limits<float>::infinity(); Tick();
    ControlsShown(false);
    assert(!g_volumeMove.Active() && g_passthrough.center == disabled.center && launches.empty());
    systemVR.poses[0].mDeviceToAbsoluteTracking = Translation(0, 1.6, 0);
    // Full rigid carry equals the desktop's actual press-relative transform.
    beginMove();
    const auto rotationStart = g_passthrough;
    const Mat deviceStart = systemVR.poses[4].mDeviceToAbsoluteTracking;
    const Mat volumeStart = PanelPose(rotationStart.center[0], rotationStart.center[1], rotationStart.center[2], rotationStart.yaw, 0, 0);
    const Mat deviceNext = Mul(deviceStart, PanelPose(.12, -.08, .05, 21, -28, 34));
    const Mat expected = Mul(deviceNext, Mul(Inverse(deviceStart), volumeStart));
    systemVR.poses[4].mDeviceToAbsoluteTracking = deviceNext; Tick();
    for(int axis=0;axis<3;++axis) {
        passthrough::Point local{};local[axis]=1;
        const auto point=passthrough::ToWorld(g_passthrough,local);
        for(int k=0;k<3;++k) assert(std::fabs(point[k]-expected.m[k][3]-expected.m[k][axis])<2e-6);
    }
    const auto held = Reply("passthrough state3d");
    if (argc == 2 && std::strcmp(argv[1], "--rotation-fixture") == 0) {
        // Export actual held IPC plus an independently composed desktop matrix.
        // Python consumes this CPU fixture, never a live compositor socket.
        std::printf("ROTATION_FIXTURE {\"state\":\"%s\",\"pose\":[", held.c_str());
        for(int i=0;i<3;++i) for(int j=0;j<4;++j)
            std::printf("%s%.9g", i || j ? "," : "", expected.m[i][j]);
        std::printf("],\"size\":[%.9g,%.9g,%.9g]}\n", g_passthrough.size[0],g_passthrough.size[1],g_passthrough.size[2]);
    }
    for(int i=0;i<20;++i) Tick();
    assert(Reply("passthrough state3d")==held); // no accumulating drift
    Send(g_catcher, vr::VREvent_MouseButtonUp); Tick();
    assert(!g_volumeMove.Active() && launches.size()==1);
    assert(Reply("passthrough state3d")==held); launches.clear();
    beginMove();
    systemVR.poses[4].mDeviceToAbsoluteTracking = deviceNext; Tick();
    Command("passthrough controls 0");
    assert(g_passthrough.center==disabled.center && g_passthrough.yaw==disabled.yaw);
    Command("passthrough controls 1"); Tick();
    // A rotated device grabbing an already rotated volume must not manufacture
    // movement or a release-save from float matrix multiply roundoff.
    for (unsigned device : {3u, 4u}) {
        for (int i=0;i<20;++i) {
            Command("passthrough set 0 1.123456 2.234567 -3.345678 1 .35 .65 31 24 -17"); Tick();
            const auto saved = g_passthrough;
            const auto p = passthrough::GrabCenter(saved);
            systemVR.poses[device].bPoseIsValid = true;
            systemVR.poses[device].mDeviceToAbsoluteTracking = PanelPose(p[0],p[1],p[2]+.4,i*3-30,i-20,i*2-17);
            Send(bar,vr::VREvent_MouseButtonDown,device); Tick();
            assert(g_volumeMove.Active());
            for(int repeat=0;repeat<5;++repeat) Tick();
            assert(g_passthrough.center==saved.center && g_passthrough.yaw==saved.yaw &&
                   g_passthrough.pitch==saved.pitch && g_passthrough.roll==saved.roll);
            Send(g_catcher,vr::VREvent_MouseButtonUp,device); Tick();
            assert(!g_volumeMove.Active() && launches.empty());
        }
    }
    // Re-grab a fully rotated box and compare the real poll loop with desktop carry.
    auto pressFull = [&](unsigned device) {
        Command("passthrough controls 1");
        Command("passthrough set 0 1.123456 2.234567 -3.345678 1 .35 .65 31 24 -17"); Tick();
        const auto p = passthrough::GrabCenter(g_passthrough);
        systemVR.poses[device].bPoseIsValid = true;
        systemVR.poses[device].mDeviceToAbsoluteTracking = PanelPose(p[0]+.04,p[1],p[2]+.4,12,-23,7);
        Send(bar,vr::VREvent_MouseButtonDown,device); Tick();
        assert(g_volumeMove.Active());
        return systemVR.poses[device].mDeviceToAbsoluteTracking;
    };
    auto assertPose = [&](const Mat &expected) {
        const auto actual=passthrough::VolumePose(g_passthrough);
        for(int i=0;i<3;++i) for(int j=0;j<4;++j)
            assert(std::fabs(actual[i][j]-expected.m[i][j])<2e-6);
    };
    auto assertControls = [&] {
        const auto edges=passthrough::RectangleEdges(g_passthrough);
        for(int i=0;i<4;++i) {
            const auto &part=overlay.parts.at(g_volumeEdges[i].overlay);
            for(int k=0;k<3;++k) {
                assert(std::fabs(part.pose.m[k][3]-part.pose.m[k][0]*part.width/2-edges[i].a[k])<2e-6);
                assert(std::fabs(part.pose.m[k][3]+part.pose.m[k][0]*part.width/2-edges[i].b[k])<2e-6);
            }
        }
        const auto faces=passthrough::RectangleControls(g_passthrough);
        for(int i:{0,1,4,5}) for(int k=0;k<3;++k)
            assert(std::fabs(overlay.parts.at(g_volumeFaces[i].overlay).pose.m[k][3]-faces[i][k])<2e-6);
        const auto grab=passthrough::GrabCenter(g_passthrough);
        for(int k=0;k<3;++k) assert(std::fabs(barPart.pose.m[k][3]-grab[k])<2e-6);
    };
    for(unsigned device:{3u,4u}) {
        const Mat press=pressFull(device);
        const auto original=Reply("passthrough state3d");
        const Mat initial=PanelPose(g_passthrough.center[0],g_passthrough.center[1],g_passthrough.center[2],31,24,-17);
        const Mat held=Mul(press,PanelPose(.15,-.1,.07,33,41,-27));
        systemVR.poses[device].mDeviceToAbsoluteTracking=held; Tick();
        assertPose(Mul(held,Mul(Inverse(press),initial))); assertControls();
        Send(other,vr::VREvent_MouseButtonUp,device==3?4:3); Tick();
        Send(other,vr::VREvent_MouseButtonUp,device,vr::VRMouseButton_Right); Tick();
        assert(g_volumeMove.Active() && launches.empty());
        // Returning to the press pose restores exact saved angles and needs no save.
        systemVR.poses[device].mDeviceToAbsoluteTracking=press; Tick();
        assert(Reply("passthrough state3d")==original);
        Send(g_catcher,vr::VREvent_MouseButtonUp,device); Tick();
        assert(!g_volumeMove.Active() && launches.empty());
        const Mat press2=pressFull(device);
        const Mat final=Mul(press2,PanelPose(-.12,.04,.06,-25,38,19));
        systemVR.poses[device].mDeviceToAbsoluteTracking=final;
        // Both translation and rotation are sampled on the release tick itself.
        Send(g_catcher,vr::VREvent_MouseButtonUp,device); Tick();
        assertPose(Mul(final,Mul(Inverse(press2),initial))); assertControls();
        assert(!g_volumeMove.Active() && launches.size()==1 && launches[0][1]=="passthrough-capture-release");
        launches.clear();
    }
    for(int reason=0;reason<5;++reason) {
        const Mat press=pressFull(4);
        const auto saved=Reply("passthrough state3d");
        systemVR.poses[4].mDeviceToAbsoluteTracking=Mul(press,PanelPose(.1,.2,.3,12,34,56)); Tick();
        assert(Reply("passthrough state3d")!=saved);
        if(reason==0) Command("passthrough controls 0");
        if(reason==1) systemVR.poses[4].bPoseIsValid=false;
        if(reason==2) systemVR.poses[0].bPoseIsValid=false;
        if(reason==3) systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][0]*=2;
        if(reason==4) Command("passthrough set 1 0 1 -2 2 1 2 -17 44 -88");
        Send(g_catcher,vr::VREvent_MouseButtonUp); Tick();
        assert(!g_volumeMove.Active() && launches.empty());
        if(reason==4) assert(g_passthrough.yaw==-17 && g_passthrough.pitch==44 && g_passthrough.roll==-88);
        else assert(Reply("passthrough state3d")==saved);
        systemVR.poses[0].bPoseIsValid=true;
    }
    const Mat helperPress=pressFull(4);
    systemVR.poses[4].mDeviceToAbsoluteTracking=Mul(helperPress,PanelPose(0,0,0,21,-16,32)); Tick();
    Command("up");
    for(int i=0;i<10;++i) Tick();
    assert(!g_volumeMove.Active() && launches.size()==1 && std::fabs(g_passthrough.roll+17)>1);
    launches.clear();
    // Malformed press tracking cannot acquire a rigid drag, despite valid bit.
    for(int bad=0;bad<2;++bad) {
        pressFull(4);Command("passthrough controls 0");Command("passthrough controls 1");Tick();
        const auto saved=Reply("passthrough state3d");
        for(int k=0;k<3;++k) systemVR.poses[4].mDeviceToAbsoluteTracking.m[k][0]*=bad==0?-1:2;
        Send(bar,vr::VREvent_MouseButtonDown);Tick();
        assert(!g_volumeMove.Active() && Reply("passthrough state3d")==saved && launches.empty());
    }
    // Pausing cancels unsaved edits; display grace must never override pause.
    const Mat pausePress = pressFull(4);
    const auto pauseSaved = Reply("passthrough state3d");
    systemVR.poses[4].mDeviceToAbsoluteTracking = Mul(pausePress, PanelPose(.1,0,0,12,0,0)); Tick();
    Command("pause on"); Tick();
    assert(!VolumeDragging() && Reply("passthrough state3d") == pauseSaved && launches.empty());
    ControlsShown(false);
    Send(bar, vr::VREvent_MouseButtonDown); Tick(); assert(!VolumeDragging());
    systemVR.poses[0].bPoseIsValid = false; Tick(); ControlsShown(false);
    systemVR.poses[0].bPoseIsValid = true;
    Command("pause off"); Tick(); ControlsShown(true);
    // Invalid rigid HMD basis hides controls, even if every value is finite.
    systemVR.poses[0].mDeviceToAbsoluteTracking.m[0][0] = 2; Tick(); ControlsShown(false);
    systemVR.poses[0].mDeviceToAbsoluteTracking = Translation(0,1.6,0); Tick();
    // Resize must roll back, rather than save, a malformed valid controller pose.
    begin();
    systemVR.poses[4].mDeviceToAbsoluteTracking.m[0][0] = 2;
    Send(g_catcher, vr::VREvent_MouseButtonUp); Tick();
    assert(!VolumeDragging() && g_passthrough.size[0] == 1 && launches.empty());
    // Volume dashboard policy must not leave the shared screen release catcher
    // inert after a gesture. Upstream screen catchers remain interactive.
    begin(); g_lasers = Lasers::Dashboard; Tick();
    assert(!overlay.parts.at(g_catcher).flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
    Send(g_catcher, vr::VREvent_MouseButtonUp); Tick(); launches.clear();
    g_lasers = Lasers::OutsideGames;
    PressDown(4, BTN_LEFT, -1, 0, 0); UpdateCatcher();
    assert(overlay.parts.at(g_catcher).flags.at(vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible));
    g_press.buttons = 0; UpdateCatcher();
    // Upstream adapter regressions: tip lookup, catcher release ownership, stuck
    // presses, reset control and attention logic compile/run from the real vr.cpp.
    systemVR.poses[3].bPoseIsValid = true;
    systemVR.poses[3].mDeviceToAbsoluteTracking = Translation(0,1,-.2);
    systemVR.tipModel = true;
    RefreshPoses(); Mat tip{};
    assert(LaserPose(3, &tip) && models.tipCalls == 1);
    const Mat expectedTip = Mul(systemVR.poses[3].mDeviceToAbsoluteTracking, PanelPose(0,0,0,0,-40,0));
    assert(std::memcmp(tip.m, expectedTip.m, sizeof tip.m) == 0);
    assert(LaserPose(3, &tip) && models.tipCalls == 1); // cached
    ft_vr_screen_create(0, 640, 480);
    auto &regressionScreen = g_screens.at(0);
    regressionScreen.visible = true;
    delivered.clear();
    auto record = [](const ft_event *event, void *data) {
        static_cast<std::vector<ft_event> *>(data)->push_back(*event);
    };
    PressDown(3, BTN_LEFT, 0, 100, 100);
    ReleaseAwayBy(4, vr::VRMouseButton_Left, record, &delivered);
    assert(g_press.buttons && delivered.empty());
    ReleaseAwayBy(3, vr::VRMouseButton_Left, record, &delivered);
    assert(!g_press.buttons && delivered.size() == 2 && delivered[0].type == FT_BUTTON && !delivered[0].pressed);
    delivered.clear(); PressDown(3, BTN_LEFT, 0, 100, 100);
    g_press.idleSince = NowMs() - kStuckMs - 1;
    ReleaseStuck(record, &delivered);
    assert(!g_press.buttons && delivered.size() == 2);
    Send(regressionScreen.resetButton, vr::VREvent_MouseButtonDown, 3); Tick();
    assert(launches.size() == 1 && launches[0][1] == "reset"); launches.clear();
    ft_vr_screen_destroy(0);
    // Hand renderer readiness must never be reported as native portal readiness.
    g_cutterState = 1;
    assert(Reply("passthrough state3d").find(" idle") != std::string::npos);
    g_cutterState = 0;
    ft_vr_shutdown();
    assert(overlay.parts.empty());
    assert(overlay.lifetime.back().find("destroy:") == 0);
    std::puts("volume OpenVR adapter: independent controls, grab/resize, sort/input/lifetime, cancellation, ownership and capture");
    return 0;
}
