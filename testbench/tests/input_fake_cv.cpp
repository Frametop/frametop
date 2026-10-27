#include "openvr_driver.h"
#include <cassert>
#include <cstring>
using namespace vr;
namespace {
IVRDriverContext *ctx;
IVRServerDriverHost *host;
IVRDriverInput *input;
VRInputComponentHandle_t prox, prox2, other, button;
unsigned lifecycle = 0;
int component, factory_other;
class Device : public ITrackedDeviceServerDriver {
    bool hmd;
public:
    explicit Device(bool h) : hmd(h) {}
    EVRInitError Activate(uint32_t index) override {
        assert(index == 7);
        lifecycle |= 1;
        auto *props = static_cast<IVRProperties *>(ctx->GetGenericInterface(IVRProperties_Version));
        auto container = props->TrackedDeviceToPropertyContainer(index);
        assert(container == 107);
        assert(input->CreateBooleanComponent(container, "/proximity", &prox) == VRInputError_None);
        assert(input->CreateBooleanComponent(container, "/proximity", &prox2) == VRInputError_None);
        assert(input->CreateBooleanComponent(container, "/input/button/click", &button) == VRInputError_None);
        return VRInitError_None;
    }
    void Deactivate() override { lifecycle |= 2; }
    void EnterStandby() override { lifecycle |= 4; }
    void *GetComponent(const char *name) override {
        assert(!strcmp(name, "display-test"));
        return &component;
    }
    void DebugRequest(const char *req, char *out, uint32_t size) override {
        assert(!strcmp(req, "debug-test") && size == 20);
        strcpy(out, "debug-ok"); lifecycle |= 8;
    }
    DriverPose_t GetPose() override { DriverPose_t p{}; p.vecPosition[0] = hmd ? 17 : 18; return p; }
} hmd(true), controller(false);

void exercise() {
    assert(ctx->GetDriverHandle() == 4321);
    EVRInitError e = VRInitError_None;
    assert(ctx->GetGenericInterface("unknown-interface", &e) == &factory_other);
    assert(e == VRInitError_Init_InterfaceNotFound);
    host->VsyncEvent(0.125);
    VREvent_Data_t event{};
    event.reserved.reserved0 = 123;
    host->VendorSpecificEvent(9, VREvent_VendorSpecific_Reserved_Start, event, 0.25);
    assert(host->IsExiting());
    VREvent_t ev{}; assert(host->PollNextEvent(&ev, sizeof(ev))); assert(ev.eventType == 123);
    TrackedDevicePose_t pose{}; host->GetRawTrackedDevicePoses(0.5f, &pose, 1); assert(pose.bPoseIsValid);
    host->RequestRestart("reason", "exe", "args", "cwd"); // fake host only, never runs a process
    Compositor_FrameTiming timing{}; assert(host->GetFrameTimings(&timing, 1) == 123);
    HmdMatrix34_t left{}, right{}; left.m[0][0] = 2; right.m[0][0] = 3;
    host->SetDisplayEyeToHead(7, left, right);
    HmdRect2_t l{}, r{}; l.vTopLeft.v[0] = 2; r.vTopLeft.v[0] = 3;
    host->SetDisplayProjectionRaw(7, l, r);
    host->SetRecommendedRenderTargetSize(7, 123, 456);
    VRInputComponentHandle_t handle = 0;
    assert(input->CreateScalarComponent(109, "scalar", &handle, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided) == VRInputError_None); assert(handle == 601);
    assert(input->UpdateScalarComponent(handle, 0.5f, 0.125) == VRInputError_InvalidParam);
    assert(input->CreateHapticComponent(109, "haptic", &handle) == VRInputError_None); assert(handle == 602);
    VRBoneTransform_t bone{};
    assert(input->CreateSkeletonComponent(109, "skeleton", "path", "base", VRSkeletalTracking_Full, &bone, 1, &handle) == VRInputError_None); assert(handle == 603);
    assert(input->UpdateSkeletonComponent(handle, VRSkeletalMotionRange_WithController, &bone, 1) == VRInputError_InvalidParam);
    assert(input->CreatePoseComponent(109, "pose", &handle) == VRInputError_None); assert(handle == 604);
    assert(input->UpdatePoseComponent(handle, &left, 0.125) == VRInputError_InvalidParam);
    assert(input->CreateEyeTrackingComponent(109, "eye", &handle) == VRInputError_None); assert(handle == 605);
    VREyeTrackingData_t eye{};
    assert(input->UpdateEyeTrackingComponent(handle, &eye, 0.125) == VRInputError_InvalidParam);
}
class Provider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext *context) override {
        ctx = context;
        host = static_cast<IVRServerDriverHost *>(ctx->GetGenericInterface(IVRServerDriverHost_Version));
        input = static_cast<IVRDriverInput *>(ctx->GetGenericInterface(IVRDriverInput_Version));
        assert(host && input);
        assert(host->TrackedDeviceAdded("cv-real-hmd", TrackedDeviceClass_HMD, &hmd));
        assert(host->TrackedDeviceAdded("cv-controller", TrackedDeviceClass_Controller, &controller));
        assert(input->CreateBooleanComponent(109, "/proximity", &other) == VRInputError_None);
        return VRInitError_None;
    }
    void Cleanup() override { lifecycle |= 16; }
    const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override { lifecycle |= 32; exercise(); }
    bool ShouldBlockStandbyMode() override { lifecycle |= 64; return true; }
    void EnterStandby() override { lifecycle |= 128; }
    void LeaveStandby() override { lifecycle |= 256; }
} provider;
}
extern "C" void *HmdDriverFactory(const char *name, int *error) {
    if (!strcmp(name, IServerTrackedDeviceProvider_Version)) { if (error) *error = 0; return &provider; }
    if (error) *error = 123;
    return &factory_other;
}
extern "C" void FrameFakePhysical() {
    DriverPose_t p{};
    p.vecPosition[0] = 42;
    p.qRotation.w = 1;
    p.qWorldFromDriverRotation.w = 1; p.vecWorldFromDriverTranslation[0] = 5;
    p.qDriverFromHeadRotation.w = 1; p.vecDriverFromHeadTranslation[1] = 6;
    p.vecVelocity[0] = 3; p.poseTimeOffset = -0.25;
    p.result = TrackingResult_Uninitialized;
    host->TrackedDevicePoseUpdated(7, p, sizeof(p));
    p.vecPosition[0] = 99;
    host->TrackedDevicePoseUpdated(9, p, sizeof(p));
    assert(input->UpdateBooleanComponent(prox, false, -0.125) == VRInputError_None);
    assert(input->UpdateBooleanComponent(prox2, false, -0.125) == VRInputError_None);
    assert(input->UpdateBooleanComponent(other, false, -0.125) == VRInputError_None);
    assert(input->UpdateBooleanComponent(button, false, -0.125) == VRInputError_None);
}
extern "C" void *FrameFakeController() { return &controller; }
extern "C" void *FrameFakeComponent() { return &component; }
extern "C" void *FrameFakeOther() { return &factory_other; }
extern "C" unsigned FrameFakeLifecycle() { return lifecycle; }
