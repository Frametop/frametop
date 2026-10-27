#include "openvr_driver.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
using namespace vr;
namespace {
std::mutex mu;
ITrackedDeviceServerDriver *hmd, *controller;
DriverPose_t latest{};
ITrackedDeviceServerDriver *synthetic[2]{};
DriverPose_t synthetic_latest[2]{};
unsigned synthetic_count[2]{};
std::map<PropertyContainerHandle_t, std::map<ETrackedDeviceProperty, std::string>> strings;
std::map<PropertyContainerHandle_t, int32_t> roles;
struct Component {
    PropertyContainerHandle_t container;
    std::string path;
    bool boolean;
    EVRScalarType type;
    EVRScalarUnits units;
    double value = 0;
    unsigned updates = 0;
    bool live = true;
};
std::map<VRInputComponentHandle_t, Component> components;
VRInputComponentHandle_t next_handle = 1000;
std::string fail_path;
unsigned failures_remaining = 0;
unsigned nonneutral_disconnects = 0;
void input_metrics() {
    std::lock_guard<std::mutex> lock(mu);
    std::cout << "{\"nonneutral_disconnects\":" << nonneutral_disconnects << ",\"components\":[";
    bool first = true;
    for (const auto &entry : components) {
        const auto &c = entry.second;
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"handle\":" << entry.first << ",\"container\":" << c.container
          << ",\"path\":\"" << c.path << "\",\"boolean\":" << c.boolean
          << ",\"absolute\":" << (c.type == VRScalarType_Absolute)
          << ",\"two_sided\":" << (c.units == VRScalarUnits_NormalizedTwoSided)
          << ",\"value\":" << c.value << ",\"updates\":" << c.updates << ",\"live\":" << c.live << '}';
    }
    std::cout << "]}" << std::endl;
}
EVRInputError create_input(PropertyContainerHandle_t container, const char *path, VRInputComponentHandle_t *handle,
                          bool boolean, EVRScalarType type, EVRScalarUnits units) {
    std::lock_guard<std::mutex> lock(mu);
    const char *fail = std::getenv("FAKE_CREATE_FAILURE");
    if (fail && !strcmp(fail, path)) return VRInputError_InvalidParam;
    *handle = ++next_handle;
    if (std::getenv("FAKE_INVALID_HANDLE")) { *handle = k_ulInvalidInputComponentHandle; return VRInputError_None; }
    components.emplace(*handle, Component{container, path, boolean, type, units});
    return VRInputError_None;
}
EVRInputError update_input(VRInputComponentHandle_t handle, double value, bool boolean, double offset) {
    std::lock_guard<std::mutex> lock(mu);
    auto &c = components.at(handle);
    assert(c.live && c.boolean == boolean && offset == 0);
    assert(std::isfinite(value));
    assert(boolean ? (value == 0 || value == 1) :
           (value <= 1 && value >= (c.units == VRScalarUnits_NormalizedTwoSided ? -1 : 0)));
    if (fail_path == c.path && failures_remaining) { --failures_remaining; return VRInputError_InvalidParam; }
    c.value = value; ++c.updates;
    return VRInputError_None;
}
void retire_inputs(int hand) {
    std::lock_guard<std::mutex> lock(mu);
    for (auto &entry : components) if (entry.second.container == 120u + hand) entry.second.live = false;
}
bool raw_pose(const DriverPose_t &p) {
    bool raw = p.qWorldFromDriverRotation.w == 1 && p.qWorldFromDriverRotation.x == 0 &&
        p.qWorldFromDriverRotation.y == 0 && p.qWorldFromDriverRotation.z == 0 &&
        p.qDriverFromHeadRotation.w == 1 && p.qDriverFromHeadRotation.x == 0 &&
        p.qDriverFromHeadRotation.y == 0 && p.qDriverFromHeadRotation.z == 0 && p.poseTimeOffset == 0;
    for (int k = 0; k < 3; ++k) raw = raw && p.vecWorldFromDriverTranslation[k] == 0 &&
        p.vecDriverFromHeadTranslation[k] == 0 && p.vecVelocity[k] == 0 && p.vecAngularVelocity[k] == 0 &&
        p.vecAcceleration[k] == 0 && p.vecAngularAcceleration[k] == 0;
    return raw;
}
void controller_metrics() {
    // GetPose is a runtime callback, not a cached copy of the publisher metrics.
    DriverPose_t sampled[2]{};
    for (int k = 0; k < 2; ++k) if (synthetic[k]) sampled[k] = synthetic[k]->GetPose();
    std::lock_guard<std::mutex> lock(mu);
    std::cout << "{\"controllers\":[";
    for (int k = 0; k < 2; ++k) {
        if (k) std::cout << ',';
        auto &p = synthetic_latest[k]; auto &s = sampled[k];
        std::cout << "{\"registered\":" << bool(synthetic[k]) << ",\"count\":" << synthetic_count[k]
          << ",\"position\":[" << p.vecPosition[0] << ',' << p.vecPosition[1] << ',' << p.vecPosition[2]
          << "],\"quaternion\":[" << p.qRotation.w << ',' << p.qRotation.x << ',' << p.qRotation.y << ',' << p.qRotation.z
          << "],\"connected\":" << p.deviceIsConnected << ",\"valid\":" << p.poseIsValid
          << ",\"raw\":" << raw_pose(p) << ",\"running\":" << (p.result == TrackingResult_Running_OK)
          << ",\"sample_connected\":" << s.deviceIsConnected << ",\"sample_valid\":" << s.poseIsValid
          << ",\"sample_x\":" << s.vecPosition[0] << ",\"sample_qw\":" << s.qRotation.w
          << ",\"role\":" << roles[120 + k] << ",\"serial\":\"" << strings[120 + k][Prop_SerialNumber_String]
          << "\",\"controller_type\":\"" << strings[120 + k][Prop_ControllerType_String]
          << "\",\"input_profile\":\"" << strings[120 + k][Prop_InputProfilePath_String]
          << "\",\"model\":\"" << strings[120 + k][Prop_ModelNumber_String]
          << "\",\"manufacturer\":\"" << strings[120 + k][Prop_ManufacturerName_String]
          << "\",\"render_model\":\"" << strings[120 + k][Prop_RenderModelName_String] << "\"}";
    }
    std::cout << "]}" << std::endl;
}
double other_x = 0;
bool worn = false, other_worn = true;
unsigned pose_count = 0, worn_count = 0, host_mask = 0, input_mask = 0;
void *other_interface;
class Host : public IVRServerDriverHost {
public:
    bool TrackedDeviceAdded(const char *s, ETrackedDeviceClass c, ITrackedDeviceServerDriver *d) override {
        if (c == TrackedDeviceClass_HMD) { assert(!strcmp(s, "cv-real-hmd")); hmd = d; assert(d->Activate(7) == VRInitError_None); }
        else if (!strcmp(s, "cv-controller")) { assert(c == TrackedDeviceClass_Controller); controller = d; }
        else {
            assert(c == TrackedDeviceClass_Controller);
            int k = !strcmp(s, "frame_testbench_left") ? 0 : 1;
            assert(k == 0 || !strcmp(s, "frame_testbench_right"));
            assert(!synthetic[k]);
            if (std::getenv("FAKE_REJECT_CONTROLLERS")) return false;
            synthetic[k] = d;
            // Probe GetPose before activation and reenter state during synchronous Activate.
            auto p = d->GetPose(); assert(raw_pose(p) && !p.deviceIsConnected && !p.poseIsValid && p.qRotation.w == 1);
            if (!std::getenv("FAKE_DEFER_CONTROLLERS")) assert(d->Activate(20 + k) == VRInitError_None);
        }
        return true;
    }
    void TrackedDevicePoseUpdated(uint32_t i, const DriverPose_t &p, uint32_t size) override {
        assert(size == sizeof(p));
        if (i == 20 || i == 21) {
            // Runtime callbacks can synchronously query the very device being published.
            auto sample = synthetic[i - 20]->GetPose();
            assert(sample.deviceIsConnected == p.deviceIsConnected && sample.vecPosition[0] == p.vecPosition[0]);
            std::lock_guard<std::mutex> lock(mu);
            if (!p.deviceIsConnected) for (const auto &entry : components)
                if (entry.second.container == 100u + i && entry.second.live && entry.second.value != 0)
                    ++nonneutral_disconnects;
            synthetic_latest[i - 20] = p; ++synthetic_count[i - 20]; return;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (i == 7) { latest = p; ++pose_count; } else { assert(i == 9); other_x = p.vecPosition[0]; }
    }
    void VsyncEvent(double t) override { assert(t == 0.125); host_mask |= 1; }
    void VendorSpecificEvent(uint32_t i, EVREventType t, const VREvent_Data_t &d, double time) override {
        assert(i == 9 && t == VREvent_VendorSpecific_Reserved_Start && d.reserved.reserved0 == 123 && time == 0.25); host_mask |= 2;
    }
    bool IsExiting() override { host_mask |= 4; return true; }
    bool PollNextEvent(VREvent_t *e, uint32_t s) override { assert(e && s == sizeof(*e)); e->eventType = 123; host_mask |= 8; return true; }
    void GetRawTrackedDevicePoses(float t, TrackedDevicePose_t *p, uint32_t n) override { assert(t == 0.5f && n == 1); p->bPoseIsValid = true; host_mask |= 16; }
    void RequestRestart(const char *r, const char *e, const char *a, const char *c) override { assert(!strcmp(r,"reason") && !strcmp(e,"exe") && !strcmp(a,"args") && !strcmp(c,"cwd")); host_mask |= 32; }
    uint32_t GetFrameTimings(Compositor_FrameTiming *t, uint32_t n) override { assert(t && n == 1); host_mask |= 64; return 123; }
    void SetDisplayEyeToHead(uint32_t i, const HmdMatrix34_t &l, const HmdMatrix34_t &r) override { assert(i == 7 && l.m[0][0] == 2 && r.m[0][0] == 3); host_mask |= 128; }
    void SetDisplayProjectionRaw(uint32_t i, const HmdRect2_t &l, const HmdRect2_t &r) override { assert(i == 7 && l.vTopLeft.v[0] == 2 && r.vTopLeft.v[0] == 3); host_mask |= 256; }
    void SetRecommendedRenderTargetSize(uint32_t i, uint32_t w, uint32_t h) override { assert(i == 7 && w == 123 && h == 456); host_mask |= 512; }
} host;
class Input : public IVRDriverInput {
    int prox_count = 0;
public:
    EVRInputError CreateBooleanComponent(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h) override {
        if (c == 120 || c == 121) return create_input(c, n, h, true, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        if (c == 107 && !strcmp(n,"/proximity")) *h = ++prox_count + 500;
        else if (c == 109 && !strcmp(n,"/proximity")) *h = 503;
        else { assert(c == 107 && !strcmp(n,"/input/button/click")); *h = 504; }
        return VRInputError_None;
    }
    EVRInputError UpdateBooleanComponent(VRInputComponentHandle_t h, bool v, double t) override {
        if (h > 1000) return update_input(h, v, true, t);
        assert(h >= 501 && h <= 504); assert(t == -0.125 || t == 0);
        std::lock_guard<std::mutex> lock(mu);
        if (h == 501 || h == 502) { worn = v; ++worn_count; } else { other_worn = v; }
        return VRInputError_None;
    }
    EVRInputError CreateScalarComponent(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h, EVRScalarType t, EVRScalarUnits u) override { if (c == 120 || c == 121) return create_input(c, n, h, false, t, u); assert(c == 109 && !strcmp(n,"scalar") && t == VRScalarType_Absolute && u == VRScalarUnits_NormalizedOneSided); *h = 601; input_mask |= 1; return VRInputError_None; }
    EVRInputError UpdateScalarComponent(VRInputComponentHandle_t h, float v, double t) override { if (h > 1000) return update_input(h, v, false, t); assert(h == 601 && v == 0.5f && t == 0.125); input_mask |= 2; return VRInputError_InvalidParam; }
    EVRInputError CreateHapticComponent(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h) override { assert(c == 109 && !strcmp(n,"haptic")); *h = 602; input_mask |= 4; return VRInputError_None; }
    EVRInputError CreateSkeletonComponent(PropertyContainerHandle_t c, const char *n, const char *s, const char *b, EVRSkeletalTrackingLevel l, const VRBoneTransform_t *g, uint32_t count, VRInputComponentHandle_t *h) override { assert(c == 109 && !strcmp(n,"skeleton") && !strcmp(s,"path") && !strcmp(b,"base") && l == VRSkeletalTracking_Full && g && count == 1); *h = 603; input_mask |= 8; return VRInputError_None; }
    EVRInputError UpdateSkeletonComponent(VRInputComponentHandle_t h, EVRSkeletalMotionRange r, const VRBoneTransform_t *p, uint32_t n) override { assert(h == 603 && r == VRSkeletalMotionRange_WithController && p && n == 1); input_mask |= 16; return VRInputError_InvalidParam; }
    EVRInputError CreatePoseComponent(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h) override { assert(c == 109 && !strcmp(n,"pose")); *h = 604; input_mask |= 32; return VRInputError_None; }
    EVRInputError UpdatePoseComponent(VRInputComponentHandle_t h, const HmdMatrix34_t *p, double t) override { assert(h == 604 && p->m[0][0] == 2 && t == 0.125); input_mask |= 64; return VRInputError_InvalidParam; }
    EVRInputError CreateEyeTrackingComponent(PropertyContainerHandle_t c, const char *n, VRInputComponentHandle_t *h) override { assert(c == 109 && !strcmp(n,"eye")); *h = 605; input_mask |= 128; return VRInputError_None; }
    EVRInputError UpdateEyeTrackingComponent(VRInputComponentHandle_t h, const VREyeTrackingData_t *p, double t) override { assert(h == 605 && p && t == 0.125); input_mask |= 256; return VRInputError_InvalidParam; }
} input;
class Properties : public IVRProperties {
public:
    ETrackedPropertyError ReadPropertyBatch(PropertyContainerHandle_t, PropertyRead_t *, uint32_t) override { return TrackedProp_Success; }
    ETrackedPropertyError WritePropertyBatch(PropertyContainerHandle_t c, PropertyWrite_t *writes, uint32_t count) override {
        assert(c == 120 || c == 121);
        for (uint32_t i = 0; i < count; ++i) {
            auto &w = writes[i];
            assert(w.writeType == PropertyWrite_Set);
            if (w.unTag == k_unStringPropertyTag) strings[c][w.prop] = static_cast<const char *>(w.pvBuffer);
            if (w.prop == Prop_ControllerRoleHint_Int32) {
                assert(w.unTag == k_unInt32PropertyTag && w.unBufferSize == sizeof(int32_t));
                roles[c] = *static_cast<int32_t *>(w.pvBuffer);
            }
            w.eError = TrackedProp_Success;
        }
        return TrackedProp_Success;
    }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError) override { return "success"; }
    PropertyContainerHandle_t TrackedDeviceToPropertyContainer(TrackedDeviceIndex_t i) override { return 100 + i; }
} properties;
class Context : public IVRDriverContext {
public:
    void *GetGenericInterface(const char *n, EVRInitError *e = nullptr) override {
        if (e) *e = VRInitError_None;
        if (!strcmp(n,IVRServerDriverHost_Version)) return &host;
        if (!strcmp(n,IVRDriverInput_Version)) return &input;
        if (!strcmp(n,IVRProperties_Version)) return &properties;
        if (e) *e = VRInitError_Init_InterfaceNotFound;
        return other_interface;
    }
    DriverHandle_t GetDriverHandle() override { return 4321; }
} context;
void metrics() {
    std::lock_guard<std::mutex> lock(mu);
    bool raw = latest.qWorldFromDriverRotation.w == 1 && latest.qDriverFromHeadRotation.w == 1 && latest.poseTimeOffset == 0;
    for (int k = 0; k < 3; ++k) raw = raw && latest.vecWorldFromDriverTranslation[k] == 0 && latest.vecDriverFromHeadTranslation[k] == 0 && latest.vecVelocity[k] == 0 && latest.vecAngularVelocity[k] == 0 && latest.vecAcceleration[k] == 0 && latest.vecAngularAcceleration[k] == 0;
    std::cout << "{\"hmd_pose\":[" << latest.vecPosition[0] << ',' << latest.vecPosition[1] << ',' << latest.vecPosition[2] << ',' << latest.qRotation.w << ',' << latest.qRotation.x << ',' << latest.qRotation.y << ',' << latest.qRotation.z << "],\"raw_transform\":" << raw << ",\"valid\":" << (latest.poseIsValid && latest.deviceIsConnected && latest.result == TrackingResult_Running_OK) << ",\"other_x\":" << other_x << ",\"pose_count\":" << pose_count << ",\"worn_count\":" << worn_count << ",\"worn\":" << worn << ",\"other_worn\":" << other_worn << "}" << std::endl;
}
}
int main(int argc, char **) {
    std::cout << std::boolalpha;
    auto path = std::getenv("FRAME_TESTBENCH_REAL_DRIVER"); assert(path);
    if (argc > 1) {
        // Relative paths and alternate absolute spellings must not redirect.
        std::string sibling(path); sibling.insert(sibling.find_last_of('/') + 1, "./");
        auto plain = dlopen(sibling.c_str(), RTLD_NOW | RTLD_LOCAL);
        assert(plain && dlsym(plain, "FrameFakePhysical")); dlclose(plain);
        auto self = dlopen(nullptr, RTLD_NOW); assert(self); dlclose(self);
        assert(!dlopen("/no-such-other-driver.so", RTLD_NOW));
        std::cout << "scope-ok" << std::endl; return 0;
    }
    auto module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::cerr << dlerror() << '\n'; return 2; }
    using Factory = void *(*)(const char *, int *);
    auto factory = reinterpret_cast<Factory>(dlsym(module,"HmdDriverFactory")); assert(factory);
    int code = -1;
    auto provider = static_cast<IServerTrackedDeviceProvider *>(factory(IServerTrackedDeviceProvider_Version, &code));
    assert(provider && code == 0);
    // NOLOAD via the interposer's explicit bypass must reach the real module.
    auto real_open = reinterpret_cast<void *(*)(const char *, int)>(dlsym(RTLD_DEFAULT,"frame_real_dlopen"));
    auto real = real_open ? real_open(path, RTLD_NOW | RTLD_NOLOAD) : dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    assert(real);
    auto physical = reinterpret_cast<void (*)()>(dlsym(real,"FrameFakePhysical")); assert(physical);
    auto lifecycle = reinterpret_cast<unsigned (*)()>(dlsym(real,"FrameFakeLifecycle")); assert(lifecycle);
    auto get_controller = reinterpret_cast<void *(*)()>(dlsym(real,"FrameFakeController"));
    auto get_component = reinterpret_cast<void *(*)()>(dlsym(real,"FrameFakeComponent"));
    auto get_other = reinterpret_cast<void *(*)()>(dlsym(real,"FrameFakeOther"));
    other_interface = get_other();
    if (provider->Init(&context) != VRInitError_None) return 3;
    physical();
    bool cleaned = false;
    std::cout << "ready" << std::endl;
    std::string cmd;
    while (std::getline(std::cin, cmd)) {
        if (cmd == "quit") break;
        if (cmd == "physical") { physical(); metrics(); }
        else if (cmd == "deactivate") { hmd->Deactivate(); std::cout << "{\"deactivated\":" << ((lifecycle() & 2) != 0) << "}" << std::endl; }
        else if (cmd == "metrics") metrics();
        else if (cmd == "controllers") controller_metrics();
        else if (cmd == "inputs") input_metrics();
        else if (cmd.rfind("fail-input ", 0) == 0) {
            std::lock_guard<std::mutex> lock(mu);
            fail_path = cmd.substr(11); failures_remaining = 1;
            std::cout << "{\"ok\":true}" << std::endl;
        }
        else if (cmd == "controller-deactivate-left") { assert(synthetic[0]); synthetic[0]->Deactivate(); retire_inputs(0); controller_metrics(); }
        else if (cmd == "controller-activate") {
            for (int k = 0; k < 2; ++k) if (synthetic[k]) {
                synthetic[k]->Deactivate(); retire_inputs(k);
                assert(synthetic[k]->Activate(20 + k) == VRInitError_None);
            }
            controller_metrics();
        }
        else if (cmd == "controller-late-activate") {
            assert(cleaned);
            bool rejected = true;
            for (int k = 0; k < 2; ++k) if (synthetic[k])
                rejected = (synthetic[k]->Activate(20 + k) != VRInitError_None) && rejected;
            std::cout << "{\"rejected\":" << rejected << "}" << std::endl;
        }
        else if (cmd == "controller-methods") {
            for (auto *d : synthetic) {
                assert(d); d->EnterStandby(); assert(!d->GetComponent("unknown"));
                char response[2] = {'x', 'x'}; d->DebugRequest("ignored", response, sizeof(response)); assert(response[0] == 0);
                d->DebugRequest("ignored", nullptr, 0);
            }
            std::cout << "{\"ok\":true}" << std::endl;
        }
        else if (cmd == "cleanup") { provider->Cleanup(); cleaned = true; std::cout << "{\"cleaned\":" << ((lifecycle() & 16) != 0) << "}" << std::endl; }
        else if (cmd == "forward") {
            provider->RunFrame(); provider->EnterStandby(); provider->LeaveStandby(); assert(provider->ShouldBlockStandbyMode());
            assert(!strcmp(provider->GetInterfaceVersions()[0], k_InterfaceVersions[0]));
            hmd->EnterStandby(); char out[20]; hmd->DebugRequest("debug-test",out,sizeof(out)); assert(!strcmp(out,"debug-ok")); assert(hmd->GetPose().vecPosition[0] == 17);
            bool component_ok = hmd->GetComponent("display-test") == get_component();
            bool factory_ok = factory("other-factory", &code) == get_other() && code == 123;
            std::cout << "{\"all_forwarded\":" << (host_mask == 1023 && input_mask == 511 && (lifecycle() & 493) == 493) << ",\"component_identity\":" << component_ok << ",\"controller_identity\":" << (controller == get_controller()) << ",\"factory_identity\":" << factory_ok << "}" << std::endl;
        }
    }
    if (!cleaned) { hmd->Deactivate(); provider->Cleanup(); }
    assert((lifecycle() & 16) != 0);
    dlclose(real); dlclose(module);
}
