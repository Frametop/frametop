#pragma once
#include "framekeyboard/feedback.hpp"
#include "framekeyboard/frametop.hpp"
#include "framekeyboard/layout.hpp"
#include "framekeyboard/placement.hpp"
#include "framekeyboard/touch.hpp"
#include <array>
#include <functional>
#include <optional>

namespace framekeyboard {
class App;

// Frametop's part of the VR loop (FRAMETOP.md), kept out of upstream's run_vr (src/vr.cpp),
// which calls these hooks at fixed points, so a subtree pull conflicts there only in the calls.
// No OpenVR here: vr.cpp hands in what it reads through its runtime-watched wrappers
// (FrametopVrAccess), so the watchdog sees every SteamVR call, and tests can fake them.
//   - Visibility: with Frametop, its desktop counts as the dashboard does. The panel shows over
//     the screens with the dashboard closed while Frametop asked for it (a desktop text field, a
//     mapped button, a launch) and it wasn't closed since, or while a Steam field has it.
//   - Placement: it opens where Frametop's old keyboard did.
//   - Touch typing (both modes): each Frame controller's tip presses the key it touches.
//   - Fingertips (Frametop): ft-screens' hand input says when a hand hovers and presses.

// What the hooks press keys with: App in the program, a fake in tests.
class PointerSink {
  public:
    virtual ~PointerSink() = default;
    virtual bool move(unsigned pointer, double x, double y) = 0;
    virtual bool down(unsigned pointer, double x, double y, double now) = 0;
    virtual bool up(unsigned pointer, double x, double y) = 0;
    virtual void cancel_pointer(unsigned pointer) = 0;
};

// App's pointers, for the program.
class AppPointers : public PointerSink {
  public:
    explicit AppPointers(App& app) : app_(app) {}
    bool move(unsigned pointer, double x, double y) override;
    bool down(unsigned pointer, double x, double y, double now) override;
    bool up(unsigned pointer, double x, double y) override;
    void cancel_pointer(unsigned pointer) override;

  private:
    App& app_;
};

// What vr.cpp reads from SteamVR for the hooks.
struct FrametopVrAccess {
    static constexpr unsigned no_device = 0xFFFFFFFF; // vr::k_unTrackedDeviceIndexInvalid
    // The controller in hand 0 (left) or 1 (right), or no_device.
    std::function<unsigned(std::size_t hand)> hand_device;
    // Its "tip" component (device -> tip), if it's a connected Frame controller.
    std::function<std::optional<Transform>(unsigned device, bool left)> tip_component;
    // Its pose in the standing universe, if it's tracked.
    std::function<std::optional<Transform>(unsigned device)> pose;
};

class FrametopVr {
  public:
    // Lasers' cursor indices are small; the tips' pointers sit past them, fingertips' past those.
    static constexpr unsigned touch_pointer = 0x100, hand_pointer = 0x110;
    // A fingertip press heard nothing of this long (ft-screens stopped) is let go.
    static constexpr double hand_silence = 3;
    // The tip's poke point, just past the nose's surface (metres).
    static constexpr double tip_reach = .005;

    // `link`: null without Frametop (plain Full Keyboard: the dashboard alone, as upstream).
    FrametopVr(FrametopLink* link, PointerSink& pointers, FrametopVrAccess access);

    // Controllers get lasers for the panel with the dashboard closed too, as for Frametop's screens.
    bool lasers() const { return link_ != nullptr; }
    void place(PanelPlacement& placement) const;

    // Each frame before the lifecycle: read the relay and take its show/hide/toggle. `relaunch`:
    // upstream's (a recenter); `visible`: the panel is up now. Returns the relaunch to use.
    bool begin_frame(bool relaunch, bool visible);
    // Whether the panel may show: `dashboard` is up, or (with Frametop) the desktop shows and
    // Frametop asked for it, or `steam_field` (a Steam text field has the replacement open).
    bool surface(bool dashboard, bool steam_field) const;
    // After the lifecycle: tell Frametop whether it's up for it.
    void shown(bool visible);
    // Its X (or Quit) closed it.
    void dismissed() { open_ = false; }

    // Touch typing at `now`. `touching`: it's on and the panel is up, placed and not moving.
    // `gripping(device)`: that controller holds the panel. False when there was nothing to do.
    bool touch(bool touching, const PanelPlacement& displayed, const std::function<Rect()>& case_bounds,
               const std::function<bool(unsigned)>& gripping, KeyHaptics& feedback, double now);
    // A tip is near the panel: vr.cpp reads it every few ms until the next frame.
    bool tips_near() const { return pokes_[0].near() || pokes_[1].near(); }
    // Fingertips from the relay at `now`. `takes`: the panel is up, placed and not moving. False
    // without Frametop.
    bool hand_pokes(bool takes, const PanelPlacement& displayed, double now);

  private:
    struct HandTouch {
        bool active{}, pressed{};
        double x{}, y{}, heard{};
    };
    std::optional<std::array<double, 3>> tip(std::size_t hand, double now);
    void end_hand(std::size_t hand);

    FrametopLink* link_;
    PointerSink& pointers_;
    FrametopVrAccess access_;
    bool open_{}; // Frametop asked for it, and it wasn't closed since
    std::array<TouchPoke, 2> pokes_{};
    std::array<HandTouch, 2> hands_{};
    // The tips: each hand's device, its tip component once read, and when that was last tried.
    std::array<unsigned, 2> devices_{{FrametopVrAccess::no_device, FrametopVrAccess::no_device}};
    std::array<std::optional<Transform>, 2> tips_{};
    std::array<double, 2> tried_{{-1e9, -1e9}};
};
} // namespace framekeyboard
