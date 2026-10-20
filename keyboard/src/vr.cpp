#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/native_replacement.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/updates.hpp"
#include "framekeyboard/vr_haptics.hpp"
#include "framekeyboard/vr_lifecycle.hpp"
#include "framekeyboard/vr_runtime.hpp"
#include "framekeyboard/vr_texture.hpp"
#include "openvr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace framekeyboard {
namespace {
// Only the VR/main thread installs this binding. The overlay watchdog never
// writes main-loop runtime state, even when its own OpenVR call blocks.
thread_local NativeReplacement* runtime_watch = nullptr;
class RuntimeBinding {
  public:
    explicit RuntimeBinding(NativeReplacement& watch) : previous_(runtime_watch) {
        runtime_watch = &watch;
    }
    ~RuntimeBinding() { runtime_watch = previous_; }

  private:
    NativeReplacement* previous_;
};
class RuntimeWait {
  public:
    RuntimeWait() : watch_(runtime_watch) {
        if (watch_) {
            watch_->begin_runtime_wait(monotonic_seconds());
        }
    }
    ~RuntimeWait() {
        if (watch_) {
            watch_->end_runtime_wait(monotonic_seconds());
        }
    }
    RuntimeWait(const RuntimeWait&) = delete;
    RuntimeWait& operator=(const RuntimeWait&) = delete;

  private:
    NativeReplacement* watch_;
};
// Start the scope before fetching the interface. It lasts through the complete
// API expression, and also covers methods using a retained local interface.
template <typename Interface> class RuntimeInterface {
  public:
    explicit RuntimeInterface(Interface* (*get)()) : value_(get()) {}
    Interface* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

  private:
    RuntimeWait wait_;
    Interface* value_;
};
auto vr_system() {
    return RuntimeInterface<vr::IVRSystem>(vr::VRSystem);
}
auto vr_overlay() {
    return RuntimeInterface<vr::IVROverlay>(vr::VROverlay);
}
auto vr_input() {
    return RuntimeInterface<vr::IVRInput>(vr::VRInput);
}
auto vr_models() {
    return RuntimeInterface<vr::IVRRenderModels>(vr::VRRenderModels);
}
auto vr_compositor() {
    return RuntimeInterface<vr::IVRCompositor>(vr::VRCompositor);
}
void check(vr::EVROverlayError error, const char* operation) {
    if (error != vr::VROverlayError_None) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 vr_overlay()->GetOverlayErrorNameFromEnum(error));
    }
}
Transform from_vr(const vr::HmdMatrix34_t& pose) {
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = pose.m[r][c];
        }
    }
    return result;
}
bool pointer_device(vr::TrackedDeviceIndex_t device) {
    if (device >= vr::k_unMaxTrackedDeviceCount) {
        return false;
    }
    const auto kind = vr_system()->GetTrackedDeviceClass(device);
    return kind == vr::TrackedDeviceClass_Controller || kind == vr::TrackedDeviceClass_HMD;
}
std::optional<Transform> read_pointer_pose(vr::TrackedDeviceIndex_t device) {
    if (!pointer_device(device)) {
        return {};
    }
    // Dashboard overlays receive laser events even when legacy controller input
    // is unavailable. Query tracking separately; a missing button state is not
    // evidence that the controller is untracked or its trigger was released.
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                 vr::k_unMaxTrackedDeviceCount);
    const auto& pose = poses[device];
    if (!pose.bPoseIsValid || !pose.bDeviceIsConnected) {
        return {};
    }
    return from_vr(pose.mDeviceToAbsoluteTracking);
}
struct GrabSample {
    bool active{}, held{}, pressed{};
    vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
    double stick_y{};
    std::array<double, 3> ray_direction{0, 0, -1};
};
class GrabInput {
  public:
    void reset() {
        for (auto& latch : latches_) {
            latch.reset();
        }
    }
    bool connect() {
        auto input = vr_input();
        if (!input || !vr_models()) {
            return false;
        }
        ready_ = input->GetInputSourceHandle("/user/hand/left", &hands_[0]) == vr::VRInputError_None &&
                 input->GetInputSourceHandle("/user/hand/right", &hands_[1]) == vr::VRInputError_None;
        return ready_;
    }
    std::array<GrabSample, 2> poll(std::optional<vr::TrackedDeviceIndex_t> laser_device = {}) {
        std::array<GrabSample, 2> result{};
        if (!ready_) {
            return result;
        }
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            const auto role =
                i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            const auto device = vr_system()->GetTrackedDeviceIndexForControllerRole(role);
            if (laser_device && device != *laser_device) {
                latches_[i].reset();
                continue;
            }
            if (device != devices_[i]) {
                latches_[i].reset();
                devices_[i] = device;
                sticks_[i] = {};
            }
            char model[1024]{};
            vr::ETrackedPropertyError error{};
            vr_system()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model,
                                                        sizeof(model), &error);
            const std::string expected = i == 0 ? "{frame_controller}frame_controller_left"
                                                : "{frame_controller}frame_controller_right";
            if (error || model != expected || !vr_system()->IsTrackedDeviceConnected(device)) {
                latches_[i].reset();
                sticks_[i] = {};
                continue;
            }
            vr::RenderModel_ControllerMode_State_t mode{};
            vr::RenderModel_ComponentState_t current{}, neutral{};
            vr::VRControllerState_t released{};
            auto models = vr_models();
            result[i] = {true, false, false, device};
            if (laser_device) {
                // A trigger-held line capture needs only this hand's stick.
                // Grip availability/state must not gate laser depth control.
                latches_[i].reset();
            } else {
                // Frame's dashboard masks grip actions and legacy controller
                // state. Compare the live component with a synthetic rest pose.
                const bool valid =
                    models->GetComponentStateForDevicePath(model, "button_grip", hands_[i], &mode,
                                                           &current) &&
                    models->GetComponentState(model, "button_grip", &released, &mode, &neutral);
                if (!valid) {
                    latches_[i].reset();
                    result[i] = {};
                    continue;
                }
                const double angle =
                    component_rotation_degrees(from_vr(neutral.mTrackingToComponentRenderModel),
                                               from_vr(current.mTrackingToComponentRenderModel));
                const auto grip = latches_[i].update(angle);
                result[i].held = grip.held;
                result[i].pressed = grip.pressed;
            }
            if (laser_device || result[i].held) {
                auto& stick = sticks_[i];
                if (!stick.attempted) {
                    stick.attempted = true;
                    vr::RenderModel_ComponentState_t rest{}, up{}, tip{};
                    vr::VRControllerState_t full_up{};
                    full_up.rAxis[0].y = 1;
                    stick.ready =
                        models->GetComponentState(model, "thumbstick", &released, &mode, &rest) &&
                        models->GetComponentState(model, "thumbstick", &full_up, &mode, &up) &&
                        stick.axis.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                             from_vr(up.mTrackingToComponentRenderModel));
                    if (models->GetComponentState(model, "tip", &released, &mode, &tip)) {
                        for (std::size_t r = 0; r < 3; ++r) {
                            stick.ray_direction[r] = -tip.mTrackingToComponentLocal.m[r][2];
                        }
                    }
                }
                vr::RenderModel_ComponentState_t current_stick{};
                if (stick.ready && models->GetComponentStateForDevicePath(model, "thumbstick", hands_[i],
                                                                          &mode, &current_stick)) {
                    result[i].stick_y =
                        stick.axis.read(from_vr(current_stick.mTrackingToComponentRenderModel));
                }
                result[i].ray_direction = stick.ray_direction;
            }
        }
        return result;
    }
    GrabSample laser_sample(vr::TrackedDeviceIndex_t device) {
        GrabSample fallback;
        fallback.device = device;
        // AUX captures the HMD, which has no thumbstick. Never borrow another
        // device's stick, and retain pose-only dragging if sampling is unavailable.
        if (device >= vr::k_unMaxTrackedDeviceCount ||
            vr_system()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
            return fallback;
        }
        for (const auto& sample : poll(device)) {
            if (sample.active && sample.device == device) {
                return sample;
            }
        }
        return fallback;
    }

  private:
    struct Stick {
        ComponentAxis axis;
        std::array<double, 3> ray_direction{0, 0, -1};
        bool attempted{}, ready{};
    };
    std::array<Stick, 2> sticks_{};
    std::array<vr::VRInputValueHandle_t, 2> hands_{};
    std::array<GripLatch, 2> latches_{};
    bool ready_{};
    std::array<vr::TrackedDeviceIndex_t, 2> devices_{
        {vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid}};
};
class LaserDrag {
  public:
    bool start(const GrabSample& input, const PanelPlacement& placement, double now, MoveSource source,
               unsigned pointer = 0) {
        const auto pose = read_pointer_pose(input.device);
        if (!pose || !capture_.begin(source, input.device, pointer)) {
            return false;
        }
        started_ = updated_ = now;
        transform_.begin(*pose, placement.transform(), input.ray_direction);
        trigger_release_.begin(now);
        trigger_neutral_.reset();
        if (source == MoveSource::Laser) {
            prepare_trigger();
        }
        return true;
    }
    bool update(PanelPlacement& placement, double now, double stick_y) {
        const auto pose = read_pointer_pose(device());
        if (!pose || now - started_ > 30) {
            stop();
            return false;
        }
        transform_.move_depth(stick_y, now - updated_);
        updated_ = now;
        placement.set_transform(transform_.update(*pose));
        return true;
    }
    bool active() const { return capture_.active(); }
    vr::TrackedDeviceIndex_t device() const { return capture_.device(); }
    std::optional<MoveSource> source() const { return capture_.source(); }
    bool owns_pointer(unsigned device, unsigned pointer) const {
        return capture_.owns_pointer(device, pointer);
    }
    bool trigger_released(double now) {
        if (source() != MoveSource::Laser || !trigger_neutral_) {
            return false;
        }
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t current{};
        if (!vr_models()->GetComponentStateForDevicePath(trigger_model_.c_str(), "trigger",
                                                         trigger_hand_, &mode, &current)) {
            return trigger_release_.update(std::nullopt, now);
        }
        const auto angle = component_rotation_degrees(*trigger_neutral_,
                                                      from_vr(current.mTrackingToComponentRenderModel));
        return trigger_release_.update(angle, now);
    }
    void stop() { capture_.reset(); }

  private:
    void prepare_trigger() {
        auto models = vr_models();
        auto input = vr_input();
        if (!models || !input) {
            return;
        }
        const auto role = vr_system()->GetControllerRoleForTrackedDeviceIndex(device());
        if (role != vr::TrackedControllerRole_LeftHand && role != vr::TrackedControllerRole_RightHand) {
            return;
        }
        const bool left = role == vr::TrackedControllerRole_LeftHand;
        char model[1024]{};
        vr::ETrackedPropertyError error{};
        vr_system()->GetStringTrackedDeviceProperty(device(), vr::Prop_RenderModelName_String, model,
                                                    sizeof(model), &error);
        const std::string expected = left ? "{frame_controller}frame_controller_left"
                                          : "{frame_controller}frame_controller_right";
        if (error || model != expected ||
            input->GetInputSourceHandle(left ? "/user/hand/left" : "/user/hand/right", &trigger_hand_) !=
                vr::VRInputError_None) {
            return;
        }
        vr::VRControllerState_t released{};
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t neutral{};
        if (models->GetComponentState(model, "trigger", &released, &mode, &neutral)) {
            trigger_model_ = model;
            trigger_neutral_ = from_vr(neutral.mTrackingToComponentRenderModel);
        }
    }
    PanelDrag transform_;
    MoveCapture capture_;
    double started_{}, updated_{};
    std::string trigger_model_;
    vr::VRInputValueHandle_t trigger_hand_{};
    std::optional<Transform> trigger_neutral_;
    TriggerReleaseGuard trigger_release_;
};
class VrPanel {
  public:
    ~VrPanel() {
        if (!connected_) {
            return;
        }
        if (handle_) {
            vr_overlay()->HideOverlay(handle_);
            vr_overlay()->ClearOverlayTexture(handle_);
            vr_overlay()->DestroyOverlay(handle_);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        // The compositor can retain submitted images. End the VR connection before
        // destroying Vulkan resources, including on partially completed startup.
        vr::VR_Shutdown();
        texture_.destroy();
        vulkan_.destroy();
    }
    void connect() {
        vr::EVRInitError error = vr::VRInitError_None;
        // Connect only once: a background connect/disconnect followed immediately
        // by an overlay connection can leave SteamVR using the old legacy input
        // context. On this Linux-only app, refuse launch if no VR server is running.
        require_steamvr();
        vr::VR_Init(&error, vr::VRApplication_Overlay);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        connected_ = true;
        if (!vr_system() || !vr_compositor() || !vr_overlay()) {
            throw std::runtime_error("required OpenVR interfaces are unavailable");
        }
        auto overlay = vr_overlay();
        check(overlay->CreateOverlay("org.framekeyboard.panel", "Full Keyboard", &handle_),
              "CreateOverlay");
        check(overlay->SetOverlayWidthInMeters(handle_, static_cast<float>(PanelPlacement{}.width())),
              "SetOverlayWidthInMeters");
        check(overlay->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_Mouse),
              "SetOverlayInputMethod");
        vr::HmdVector2_t scale{{panel_width, texture_height}};
        check(overlay->SetOverlayMouseScale(handle_, &scale), "SetOverlayMouseScale");
        for (const auto flag : {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MultiCursor,
                                vr::VROverlayFlags_SendVRDiscreteScrollEvents}) {
            check(overlay->SetOverlayFlag(handle_, flag, true), "SetOverlayFlag");
        }
        std::string message;
        if (!vulkan_.init(message) || !texture_.create(vulkan_, handle_, message)) {
            throw std::runtime_error(message);
        }
    }
    bool read_head(Transform& head) {
        vr::TrackedDevicePose_t pose{};
        vr_system()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bPoseIsValid) {
            return false;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                head[row][col] = pose.mDeviceToAbsoluteTracking.m[row][col];
            }
        }
        return true;
    }
    std::optional<Transform> read_dashboard(const char* key, bool bottom_center) {
        auto overlay = vr_overlay();
        vr::VROverlayHandle_t dashboard{};
        if (!overlay->IsDashboardVisible() ||
            overlay->FindOverlay(key, &dashboard) != vr::VROverlayError_None ||
            !overlay->IsOverlayVisible(dashboard)) {
            return {};
        }
        vr::HmdVector2_t scale{};
        if (overlay->GetOverlayMouseScale(dashboard, &scale) != vr::VROverlayError_None ||
            !std::isfinite(scale.v[0]) || !std::isfinite(scale.v[1]) || scale.v[0] <= 0 ||
            scale.v[1] <= 0) {
            return {};
        }
        // Invisible tabs return a valid but frozen transform, so reject them
        // above. Coordinates use mouse pixels and include overlay scale.
        vr::HmdVector2_t bottom{{scale.v[0] / 2, bottom_center ? 0 : scale.v[1] / 2}};
        vr::HmdMatrix34_t world{};
        if (overlay->GetTransformForOverlayCoordinates(dashboard, vr::TrackingUniverseStanding, bottom,
                                                       &world) != vr::VROverlayError_None) {
            return {};
        }
        return dashboard_anchor(from_vr(world));
    }
    void place(const PanelPlacement& placement) {
        const auto world = placement.transform();
        vr::HmdMatrix34_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                transform.m[row][col] = static_cast<float>(world[row][col]);
            }
        }
        check(
            vr_overlay()->SetOverlayTransformAbsolute(handle_, vr::TrackingUniverseStanding, &transform),
            "Place keyboard");
        check(vr_overlay()->SetOverlayWidthInMeters(handle_, static_cast<float>(placement.width())),
              "Resize keyboard");
    }
    std::uint64_t universe() const {
        vr::ETrackedPropertyError error{};
        const auto id = vr_system()->GetUint64TrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64, &error);
        return error == vr::TrackedProp_Success ? id : 0;
    }
    void submit(PanelRenderer& renderer, const PanelView& view) {
        RuntimeWait runtime_wait;
        // The enlarged texture has transparent popup margins. Restrict laser
        // intersection to the case, movement handle and open menu so unused pixels cannot
        // steal focus from the dashboard.
        std::array<vr::VROverlayIntersectionMaskPrimitive_t, 3> mask{};
        auto rectangle = [&](std::size_t index, Rect bounds) {
            mask[index].m_nPrimitiveType = vr::OverlayIntersectionPrimitiveType_Rectangle;
            mask[index].m_Primitive.m_Rectangle = {
                static_cast<float>(bounds.x), static_cast<float>(bounds.y + popup_margin),
                static_cast<float>(bounds.width), static_cast<float>(bounds.height)};
        };
        rectangle(0, panel_case_bounds(view));
        std::size_t count = 1;
        if (view.popup) {
            rectangle(count++, *view.popup);
        }
        if (const auto handle = panel_move_handle_bounds(view)) {
            rectangle(count++, *handle);
        }
        check(vr_overlay()->SetOverlayIntersectionMask(handle_, mask.data(),
                                                       static_cast<std::uint32_t>(count)),
              "Set keyboard intersection mask");
        std::string error;
        if (!texture_.update(renderer, error)) {
            throw std::runtime_error(error);
        }
        const bool native = texture_.native_pixels();
        if (!reported_native_ || *reported_native_ != native) {
            std::cout << (native ? "Using native BGRA overlay texture.\n"
                                 : "Using RGBA compatibility overlay texture.\n")
                      << std::flush;
            reported_native_ = native;
        }
    }
    vr::VROverlayHandle_t handle() const { return handle_; }

  private:
    bool connected_{};
    vr::VROverlayHandle_t handle_{};
    VulkanContext vulkan_;
    PanelTexture texture_;
    std::optional<bool> reported_native_;
};
} // namespace
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration) {
    RuntimeBinding runtime_binding(replacement);
    app.set_interaction_active(false);
    VrPanel panel;
    panel.connect();
    std::atomic<double> replacement_progress{monotonic_seconds()};
    std::atomic<bool> replacement_watched{false}, replacement_stalled{false};
    replacement.prepare_watchdog();
    // Steam restores stock independently. This thread removes a frozen panel that could
    // otherwise cover or intercept the recovered native keyboard. Apart from the
    // atomic slow-work deadline, it never touches App/input state.
    std::jthread replacement_watchdog([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            replacement.watchdog_tick(monotonic_seconds(), app.slow_work_deadline());
            if (replacement_watched.load() && monotonic_seconds() - replacement_progress.load() > .7) {
                vr_overlay()->HideOverlay(panel.handle());
                replacement_stalled.store(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    VrHaptics haptics;
    auto manifest = fs::canonical("/proc/self/exe").parent_path().parent_path() /
                    "share/framekeyboard/vr/actions.json";
    if (!fs::is_regular_file(manifest)) {
        manifest = fs::canonical("/proc/self/exe").parent_path() / "vr/actions.json";
    }
    haptics.connect(manifest);
    KeyHaptics feedback;
    GrabInput grip_input;
    if (!grip_input.connect() && app.use_drag_to_move()) {
        app.report_status("Native grip unavailable; use Recenter to recover the keyboard.");
    }
    std::array<bool, vr::k_unMaxTrackedDeviceCount> hovered_devices{};
    std::map<unsigned, vr::TrackedDeviceIndex_t> pointer_devices;
    auto cancel_device = [&](vr::TrackedDeviceIndex_t device) {
        for (auto it = pointer_devices.begin(); it != pointer_devices.end();) {
            if (it->second == device) {
                app.cancel_pointer(it->first);
                feedback.cancel_pointer(it->first);
                it = pointer_devices.erase(it);
            } else {
                ++it;
            }
        }
    };
    struct ReleaseBeforeVrShutdown {
        App& app;
        ~ReleaseBeforeVrShutdown() {
            try {
                app.cancel();
            } catch (...) {
            }
        }
    } release{app};
    const double start = monotonic_seconds();
    bool done = false;
    const auto placement_path = app.config_dir() / "placement.json";
    std::optional<SavedPlacement> saved;
    try {
        saved = load_placement(placement_path);
    } catch (const std::exception& error) {
        std::cerr << "Saved placement ignored: " << error.what() << '\n';
    }
    bool recenter_pending = !saved, transform_dirty = false;
    std::uint64_t placement_universe = 0;
    PanelPlacement placement, displayed;
    if (saved) {
        // Size survives tracking-space changes, including a reset event before
        // the first valid head pose. Restore the world pose only after checking its universe.
        placement.set_width(saved->width);
    }
    HorizonAlignment horizon;
    bool auto_leveling = app.auto_leveling();
    std::optional<Transform> previous_dashboard;
    DashboardAnchor dashboard_anchor;
    if (saved && (!saved->universe || !panel.universe() || saved->universe == panel.universe())) {
        dashboard_anchor.restore(saved->dashboard, saved->dashboard_bar, saved->dashboard_full_rotation);
    }
    LaserDrag drag;
    auto stop_drag = [&](bool align_after_release = false) {
        feedback.cancel();
        // Tracking loss or timeout may already have stopped LaserDrag. Always
        // release the app's input gate as well; repeated cleanup is harmless.
        drag.stop();
        app.set_dragging(false);
        if (align_after_release) {
            // Release may arrive without another movement sample. Schedule the
            // first alignment frame even when the hand was completely still.
            horizon.reset();
            transform_dirty = true;
        } else {
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
        }
    };
    VrLifecycle lifecycle(
        app.background_start(),
        {
            .stop_drag = [&] { stop_drag(); },
            .cancel_input = [&] { app.cancel(); },
            .clear_pointers =
                [&] {
                    hovered_devices.fill(false);
                    pointer_devices.clear();
                    grip_input.reset();
                },
            .hide_overlay = [&] { check(vr_overlay()->HideOverlay(panel.handle()), "Hide keyboard"); },
            .route_input = [&](bool remote) { replacement.route(remote); },
            .set_interaction = [&](bool active) { app.set_interaction_active(active); },
            .show_overlay =
                [&](double now) {
                    app.paint(now);
                    panel.submit(app.renderer, app.view());
                    check(vr_overlay()->ShowOverlay(panel.handle()), "Show keyboard");
                    std::cout << "Full Keyboard overlay visible.\n" << std::flush;
                },
            .retry_bridge = [&](double now) { replacement.retry_now(now); },
            .summon =
                [&] {
                    std::cout << "Launch request received.\n" << std::flush;
                    app.summon();
                },
            .dismiss_request = [&] { replacement.dismiss_request(); },
            .cancel_device = [&](std::uint32_t device) { cancel_device(device); },
        });
    auto hide_panel = [&] { lifecycle.hide(); };
    auto place_panel = [&](double now) {
        const auto aligned = horizon.update(placement.transform(), now, drag.active(), auto_leveling);
        if (!displayed.ready() || aligned != displayed.transform() ||
            placement.width() != displayed.width() || transform_dirty) {
            displayed.restore(aligned, placement.width());
            panel.place(displayed);
            transform_dirty = false;
        }
    };
    std::vector<PlacementAction> adjustments;
    if (app.background_start()) {
        // The first texture upload can consume much of the summon deadline.
        // Prepare it while hidden, before the helper can advertise readiness.
        app.paint(monotonic_seconds());
        panel.submit(app.renderer, app.view());
        std::cout << "Full Keyboard started in background.\n" << std::flush;
    }
    auto dismiss_panel = [&] {
        if (app.take_dismissal()) {
            lifecycle.dismiss();
        }
    };
    replacement.frame_completed();
    while (!done && !app.quitting() && !interrupted) {
        const auto frame_started = std::chrono::steady_clock::now();
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        replacement.poll(now);
        if (replacement.take_unsupported_control()) {
            app.report_status("Control unavailable in Steam fields. Use Steam keyboard for shortcuts.");
        }
        const auto native = replacement.state();
        app.set_replacement_available(native.available, replacement.unavailable_reason());
        app.set_native_session(native.open);
        const bool replace = app.replace_native_keyboard();
        lifecycle.begin_frame({.replacement_enabled = replace,
                               .native = native,
                               .routed_remote = replacement.remote(),
                               .watchdog_stalled = replacement_stalled.exchange(false),
                               .relaunch = instance.poll_recenter(),
                               .now = now});
        const bool dashboard_visible = vr_overlay()->IsDashboardVisible();
        lifecycle.dashboard_visibility(dashboard_visible);
        // Hidden panels cannot be grabbed. Do not query both render models in
        // standby; require a released grip sample again when the panel returns.
        if (!lifecycle.visible()) {
            grip_input.reset();
        }
        const auto grips = lifecycle.visible() && app.use_drag_to_move() ? grip_input.poll()
                                                                         : std::array<GrabSample, 2>{};
        if (!app.use_drag_to_move()) {
            grip_input.reset();
        }
        if (drag.active() && !app.allows_movement(*drag.source())) {
            stop_drag();
        }
        vr::VREvent_t event{};
        while (vr_system()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr_system()->AcknowledgeQuit_Exiting();
                done = true;
            }
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                lifecycle.focus_lost();
                recenter_pending = true;
                saved.reset();
                previous_dashboard.reset();
                dashboard_anchor = {};
            }
            if (event.eventType == vr::VREvent_InputFocusChanged) {
                lifecycle.focus_lost();
            }
            if (event.eventType == vr::VREvent_TrackedDeviceDeactivated) {
                lifecycle.device_lost(event.trackedDeviceIndex,
                                      drag.active() ? std::optional{drag.device()} : std::nullopt);
            }
        }
        while (vr_overlay()->PollNextOverlayEvent(panel.handle(), &event, sizeof(event))) {
            // Recheck before every pointer event, not just once per batch. A
            // closing dashboard must not deliver queued clicks to a local app.
            if (!vr_overlay()->IsDashboardVisible()) {
                hide_panel();
            }
            if (event.eventType == vr::VREvent_OverlayClosed) {
                app.close_panel();
                dismiss_panel();
                if (app.quitting()) {
                    done = true;
                    break;
                }
            }
            if (!lifecycle.visible()) {
                continue;
            }
            // OpenVR reports bottom-left coordinates; Cairo uses top-left.
            const double x = event.data.mouse.x, y = texture_height - event.data.mouse.y - popup_margin;
            const auto pointer = event.data.mouse.cursorIndex;
            const auto device = event.trackedDeviceIndex;
            const bool controller =
                device < vr::k_unMaxTrackedDeviceCount &&
                vr_system()->GetTrackedDeviceClass(device) == vr::TrackedDeviceClass_Controller;
            const bool tracked_pointer = pointer_device(device);
            if (tracked_pointer && (event.eventType == vr::VREvent_MouseMove ||
                                    event.eventType == vr::VREvent_MouseButtonDown ||
                                    event.eventType == vr::VREvent_MouseButtonUp)) {
                pointer_devices[pointer] = device;
            }
            switch (event.eventType) {
            case vr::VREvent_MouseMove: {
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] =
                        panel_case_bounds(app.view()).contains(x, y);
                }
                if (!drag.active()) {
                    if (app.move(pointer, x, y) && controller) {
                        feedback.hover(device);
                    }
                }
                break;
            }
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left && !drag.active()) {
                    const auto view = app.view();
                    const auto handle = panel_move_handle_bounds(view);
                    if (app.allows_movement(MoveSource::Laser) && !view.popup && handle &&
                        handle->contains(x, y)) {
                        const auto input = grip_input.laser_sample(device);
                        if (tracked_pointer && placement.ready() &&
                            drag.start(input, displayed, now, MoveSource::Laser, pointer)) {
                            placement.restore(displayed.transform(), displayed.width());
                            horizon.reset();
                            feedback.cancel();
                            app.set_dragging(true);
                            std::cout << "Keyboard movement started: "
                                      << (controller ? "controller" : "head cursor") << ".\n"
                                      << std::flush;
                        } else if (!tracked_pointer) {
                            // Source metadata for a handle click only. Never log
                            // coordinates or pointer/key events over typed keys.
                            std::cout << "Keyboard movement ignored: untracked pointer device " << device
                                      << ", cursor " << pointer << ".\n"
                                      << std::flush;
                        }
                        break; // A handle press must never also reach a key.
                    }
                    // Grabbing cannot also type if grip and trigger arrive together.
                    const bool grabbing = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                        return grip.active && grip.held && grip.device == event.trackedDeviceIndex;
                    });
                    if (app.pinned() || !grabbing) {
                        const bool key_pressed = app.down(pointer, x, y, now);
                        if (key_pressed && controller) {
                            feedback.press(pointer, device);
                        }
                    }
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left) {
                    if (drag.owns_pointer(device, pointer)) {
                        stop_drag(true);
                        app.move(pointer, x, y);
                    } else if (!drag.active() && app.up(pointer, x, y)) {
                        feedback.release(pointer);
                    }
                }
                break;
            case vr::VREvent_ScrollDiscrete:
                if (!drag.active()) {
                    app.scroll(event.data.scroll.cursorIndex, -event.data.scroll.xdelta,
                               -event.data.scroll.ydelta);
                }
                break;
            case vr::VREvent_FocusLeave:
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] = false;
                } else {
                    hovered_devices.fill(false);
                }
                // Focus events do not contain cursorIndex. The runtime adapter
                // resolves the device through actual pointer events only.
                lifecycle.pointer_left(device, drag.active());
                break;
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
                hovered_devices.fill(false);
                lifecycle.focus_lost();
                break;
            default:
                break;
            }
            dismiss_panel();
        }
        if (done || app.quitting()) {
            break;
        }
        if (auto_leveling != app.auto_leveling()) {
            // Adopt the visible pose before disabling assistance so a partial
            // correction never jumps back to the original roll. Enabling starts
            // a fresh ease from that same pose, including changes via Reload.
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
            auto_leveling = app.auto_leveling();
            transform_dirty = true;
        }
        // A tracked head cursor or controller may hold a key for minutes. Lost
        // tracking releases only the keys belonging to that pointer device.
        std::set<vr::TrackedDeviceIndex_t> lost_devices;
        for (const auto& [pointer, device] : pointer_devices) {
            if (app.pointer_pressed(pointer) && !read_pointer_pose(device)) {
                lost_devices.insert(device);
            }
        }
        for (const auto device : lost_devices) {
            lifecycle.device_lost(device, std::nullopt);
        }
        if (drag.active() && !app.allows_movement(*drag.source())) {
            stop_drag();
        }
        if (drag.active() && drag.trigger_released(now)) {
            stop_drag(true);
        }
        if (drag.active() && drag.source() == MoveSource::Grip) {
            const bool held = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                return grip.active && grip.held && grip.device == drag.device();
            });
            if (!held) {
                stop_drag(true);
            }
        } else if (!drag.active() && lifecycle.visible() && placement.ready() &&
                   app.allows_movement(MoveSource::Grip)) {
            for (const auto& grip : grips) {
                if (grip.active && grip.pressed && grip.device < hovered_devices.size() &&
                    hovered_devices[grip.device] && drag.start(grip, displayed, now, MoveSource::Grip)) {
                    // Capture the visible pose, not the uncorrected pose behind
                    // horizon assistance, so regrabbing never snaps the keyboard.
                    placement.restore(displayed.transform(), displayed.width());
                    horizon.reset();
                    feedback.cancel();
                    app.set_dragging(true);
                    break;
                }
            }
        }
        for (const auto& [device, kind] : feedback.take(now)) {
            if (!drag.active() && lifecycle.visible()) {
                RuntimeWait runtime_wait;
                haptics.send(device, kind);
            }
        }
        if (app.take_recenter()) {
            replacement.retry_now(now);
            stop_drag();
            saved.reset();
            recenter_pending = true;
            adjustments.clear();
            lifecycle.request_recenter();
        }
        const auto actions = app.take_placement_actions();
        adjustments.insert(adjustments.end(), actions.begin(), actions.end());
        if (!actions.empty()) {
            lifecycle.request_tracking_sample();
        }
        if (drag.active()) {
            double stick_y = 0;
            if (drag.source() == MoveSource::Laser) {
                stick_y = grip_input.laser_sample(drag.device()).stick_y;
            } else {
                for (const auto& input : grips) {
                    if (input.active && input.held && input.device == drag.device()) {
                        stick_y = input.stick_y;
                    }
                }
            }
            if (drag.update(placement, now, stick_y)) {
                transform_dirty = true;
            } else {
                stop_drag();
            }
        }
        const bool panel_requested = lifecycle.panel_requested();
        if (lifecycle.should_sample_tracking(dashboard_visible, now)) {
            Transform head{};
            const bool was_waiting = lifecycle.waiting_for_tracking();
            const bool valid_head = panel.read_head(head);
            lifecycle.tracking_sampled(valid_head, now);
            if (!valid_head) {
                if (recenter_pending && !was_waiting) {
                    std::cout << "Waiting for valid headset pose before recentering.\n" << std::flush;
                }
                // Lifecycle hides input; retain placement through brief tracking loss.
            } else if (panel_requested) {
                // A hidden recovery probe only restores readiness. Placement and
                // presentation still require a fresh summon or manual launch.
                if (recenter_pending) {
                    dashboard_anchor = {}; // Recalibrate the mount only on an explicit reset.
                }
                const auto dashboard =
                    dashboard_anchor.update(panel.read_dashboard("valve.steam.gamepadui.main", true),
                                            panel.read_dashboard("valve.steam.gamepadui.bar", false));
                if (!adjustments.empty()) {
                    stop_drag();
                }
                if (saved) {
                    const auto universe = panel.universe();
                    if (saved->universe && universe && saved->universe != universe) {
                        recenter_pending = true;
                        std::cout << "Tracking space changed; recentering keyboard.\n";
                    } else {
                        placement.restore(saved->transform, saved->width);
                        placement_universe = universe;
                        previous_dashboard = saved->dashboard;
                        transform_dirty = true;
                        std::cout << "Restored saved keyboard placement.\n" << std::flush;
                    }
                    saved.reset();
                }
                if (recenter_pending) {
                    horizon.reset();
                    placement.recenter(head, dashboard ? &*dashboard : nullptr,
                                       static_cast<double>(panel_height) / panel_width);
                    previous_dashboard = dashboard;
                    placement_universe = panel.universe();
                    recenter_pending = false;
                    transform_dirty = true;
                    std::cout << (dashboard ? "Keyboard recentered below current dashboard.\n"
                                            : "Keyboard recentered below current headset heading.\n")
                              << std::flush;
                }
                if (dashboard) {
                    if (previous_dashboard && *previous_dashboard != *dashboard && placement.ready() &&
                        !drag.active()) {
                        placement.set_transform(
                            move_with_dashboard(placement.transform(), *previous_dashboard, *dashboard));
                        transform_dirty = true;
                    }
                    // While grabbed the hand owns placement; keep the anchor
                    // current so release preserves the new custom offset.
                    previous_dashboard = dashboard;
                }
                for (const auto action : adjustments) {
                    placement.adjust(action);
                    transform_dirty = true;
                }
                adjustments.clear();
                if (transform_dirty) {
                    place_panel(now);
                }
                // Read actual compositor visibility rather than trusting our cached flag.
                // Another UI can hide an overlay without destroying its owner process.
                const bool compositor_visible = vr_overlay()->IsOverlayVisible(panel.handle());
                lifecycle.present(placement.ready(), vr_overlay()->IsDashboardVisible(),
                                  compositor_visible, now);
            }
        }
        if (lifecycle.visible() && placement.ready() && (transform_dirty || horizon.animating())) {
            place_panel(now);
        }
        // Repeat and text commits share the same gate as pointer presses.
        if (!vr_overlay()->IsDashboardVisible()) {
            hide_panel();
        }
        const bool repaint = app.tick(now);
        if (lifecycle.visible() && repaint) {
            app.paint(now);
            panel.submit(app.renderer, app.view());
        }
        bool fallback = app.take_native_fallback();
        if (replacement_stalled.exchange(false)) {
            fallback = true;
        }
        if (fallback) {
            lifecycle.fallback();
        }
        // A hidden resident must remain ready for the next summon, including one that
        // opens the dashboard. Presentation is acknowledged separately for each request.
        const bool backend_ready = app.replacement_ready();
        const bool compositor_visible = lifecycle.ready(backend_ready, dashboard_visible) &&
                                        dashboard_visible && lifecycle.visible() &&
                                        vr_overlay()->IsOverlayVisible(panel.handle());
        const auto report = lifecycle.frame_report(backend_ready, dashboard_visible, compositor_visible);
        const auto completed = monotonic_seconds();
        replacement.update(replace, report.ready, report.acknowledgement, fallback || report.fallback,
                           completed, true);
        replacement_progress.store(completed);
        app.frame_completed();
        if (!done && !app.quitting() && !interrupted) {
            update_startup_ready(completed);
        }
        replacement.frame_completed();
        replacement_watched.store(replace && lifecycle.visible());
        // Follow the dashboard every visible frame, even without a laser over
        // the keys. Budget work inside the 60 Hz period instead of adding a full
        // sleep after it. Hidden panels still idle at 4 Hz; paints are event-driven.
        const auto period = std::chrono::microseconds(lifecycle.visible() ? 16667 : 250000);
        if (lifecycle.visible()) {
            std::this_thread::sleep_until(frame_started + period);
        } else {
            replacement.wait_until(frame_started + period);
        }
    }
    hide_panel();
    replacement_watched.store(false);
    if (app.updating() && !interrupted) {
        replacement.shutdown_for_update();
    } else {
        replacement.update(false, false, 0, false, monotonic_seconds());
    }
    app.cancel();
    if (displayed.ready()) {
        try {
            save_placement(placement_path, {displayed.transform(), displayed.width(), placement_universe,
                                            previous_dashboard, dashboard_anchor.bar(), true});
        } catch (const std::exception& error) {
            std::cerr << "Could not save keyboard placement: " << error.what() << '\n';
        }
    }
    if (!placement.ready()) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
