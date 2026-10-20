#pragma once
#include "native_replacement.hpp"
#include "vr_runtime.hpp"
#include <functional>
#include <optional>

namespace framekeyboard {
// Runtime effects stay in the OpenVR shell. The coordinator owns their ordering;
// host receivers can exercise the same transitions without a headset or OS input.
struct VrLifecycleEffects {
    std::function<void()> stop_drag, cancel_input, clear_pointers, hide_overlay;
    std::function<void(bool)> route_input, set_interaction;
    std::function<void(double)> show_overlay, retry_bridge;
    std::function<void()> summon, dismiss_request;
    std::function<void(std::uint32_t)> cancel_device;
};

class VrLifecycle {
  public:
    VrLifecycle(bool background_start, VrLifecycleEffects effects);
    struct Frame {
        bool replacement_enabled{};
        NativeReplacement::State native;
        bool routed_remote{}, watchdog_stalled{}, relaunch{};
        double now{};
    };
    void begin_frame(const Frame& frame);
    void dashboard_visibility(bool visible);
    void hide();
    void dismiss();
    void fallback();
    void focus_lost();
    void device_lost(std::uint32_t device, std::optional<std::uint32_t> drag_owner);
    void pointer_left(std::uint32_t device, bool dragging);
    void request_recenter();
    void request_tracking_sample() { tracking_.request_sample(); }
    bool should_sample_tracking(bool dashboard_visible, double now) const;
    void tracking_sampled(bool valid, double now);
    bool waiting_for_tracking() const { return tracking_.waiting(); }
    bool panel_requested() const { return presentation_.show(); }
    bool visible() const { return visible_; }
    // Call after placement work; only successful runtime presentation consumes a pending show.
    void present(bool placement_ready, bool dashboard_visible, bool compositor_visible, double now);
    bool ready(bool backend_ready, bool dashboard_visible) const;
    struct FrameReport {
        bool ready{}, fallback{};
        std::uint64_t acknowledgement{};
    };
    FrameReport frame_report(bool backend_ready, bool dashboard_visible, bool compositor_visible) const;

  private:
    VrLifecycleEffects effects_;
    ReplacementPresentation presentation_;
    TrackingReadiness tracking_;
    NativeReplacement::State native_;
    std::uint64_t last_request_{};
    bool visible_{}, show_pending_{};
};
} // namespace framekeyboard
