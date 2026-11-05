#pragma once
#include "framekeyboard/layout.hpp"
#include "framekeyboard/placement.hpp"
#include <array>
#include <optional>

namespace framekeyboard {
// Touch typing (Frametop): a controller's tip presses the key it touches, as a finger does on
// Half-Life: Alyx's keypads. Pure geometry and state, no OpenVR calls (tests/touch_tests.cpp).

// A point in the room over the panel: x, y in its texture's pixels, top-left origin, the
// popup margin taken off (what laser events give after vr.cpp's flip), and depth in metres
// out of its front (negative behind it). `width`: the overlay's width in metres.
struct PanelPoint {
    double x{}, y{}, depth{};
};
PanelPoint locate_on_panel(const Transform& panel, double width, const std::array<double, 3>& point);
// The room point `along` metres past a component's origin on its -z (a controller's "tip").
std::array<double, 3> tip_point(const Transform& device, const Transform& component, double along);

struct TouchEvent {
    // Hover: the tip is over the case, in front (move the pointer there). Leave: it's gone
    // (clear its hover). Down, Move (pressed), Up: the key. Cancel: lost while pressed.
    enum class Kind { None, Hover, Leave, Down, Move, Up, Cancel } kind{Kind::None};
    double x{}, y{};
};

// One tip, sampled as often as it can be (vr.cpp reads it every few ms while it's near). It
// hovers within `hover` in front of the case. It presses when it reaches the surface from the
// front (having been `arm` out), moving in at `min_speed` or more: a tip drifting in slowly
// doesn't press, but a push from there does. The key comes up once the tip draws back
// `release_travel` from its deepest point (it needn't come out in front again), slides `drift`
// from where it pressed, leaves the case by more than `slide`, goes more than `deep` behind it,
// or is lost. After that, pushing in `retap_travel` again presses the next key, in front of the
// surface or behind it, at least `debounce` after the last press. A tip that comes through the
// surface from behind or from the side never presses: it has to come out in front first.
class TouchPoke {
  public:
    static constexpr double hover = .06, arm = .008, min_speed = .04, release_travel = .008,
                            retap_travel = .006, debounce = .06, deep = .12;
    static constexpr double slide = 24, drift = 40; // texture pixels
    // `point`: none while the tip isn't tracked or the panel isn't up. `surface`: the case.
    // `now`: seconds, for the tip's speed.
    TouchEvent update(const std::optional<PanelPoint>& point, const Rect& surface, double now);
    void reset() { *this = {}; }
    bool pressed() const { return state_ == State::Pressed; }
    bool near() const { return state_ != State::Idle; }

  private:
    enum class State { Idle, Hover, Pressed, Lifted };
    // Metres a second toward the panel over the last few samples (0 with too few).
    double speed(double now, double depth) const;
    State state_{State::Idle};
    bool armed_{};
    double deepest_{}, shallowest_{}, down_x_{}, down_y_{}, last_down_{-1e9};
    struct Sample {
        double time{}, depth{};
    };
    std::array<Sample, 16> history_{};
    std::size_t samples_{};
};
} // namespace framekeyboard
