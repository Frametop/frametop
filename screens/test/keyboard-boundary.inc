// Rendering/pose stubs only. The runner appends the production keyboard::Poll verbatim.
namespace keyboard {
namespace {
constexpr int kMods = 3, kH = 400, kGrip = 20;
constexpr uint32_t kHide = 0xffff;
const uint32_t kModCode[kMods] = {1,2,3};
struct Key { struct { uint32_t code; } def; };
std::vector<Key> g_keys;
vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
int g_down=-1, g_hover=-1;
bool g_latched[kMods]{}, g_dirty=false, g_shown=false, g_hoverGrip=false;
uint32_t g_dragDevice=vr::k_unTrackedDeviceIndexInvalid;
vr::HmdMatrix34_t g_dragRel{}, g_pose{};
int ModOf(uint32_t) { return -1; }
int KeyAt(double,double) { return -1; }
bool DevicePose(uint32_t, vr::HmdMatrix34_t *) { return false; }
vr::HmdMatrix34_t Mul(const vr::HmdMatrix34_t &a,const vr::HmdMatrix34_t &) { return a; }
vr::HmdMatrix34_t Inverse(const vr::HmdMatrix34_t &a) { return a; }
void Place(const vr::HmdMatrix34_t &) {}
void Draw() {}
void StopCarrying() { g_dragDevice=vr::k_unTrackedDeviceIndexInvalid; g_dirty=true; }
}
void TestOverlay(vr::VROverlayHandle_t overlay) { g_overlay=overlay; }
void TestHeldKey() { g_keys = {{{30}}}; g_down = 0; g_latched[0] = true; }
bool Show(const vr::HmdMatrix34_t &) { std::abort(); }
void Hide() {}
bool Shown() { return false; }
bool Aimed(const vr::HmdMatrix34_t &) { return false; }
const vr::HmdMatrix34_t &Pose() { std::abort(); }
void EndDragBy(uint32_t device) { if(device==g_dragDevice) StopCarrying(); }
void SetLasers(bool) {}
void Destroy() {}
#include "keyboard-poll.inc"
}
