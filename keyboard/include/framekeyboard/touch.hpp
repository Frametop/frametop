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

// One tip. It hovers within `hover` in front of the case, and presses when it crosses the
// surface from the front, having been at least `arm` out since its last press. The key comes
// up once the tip is `release` out again, leaves the case by more than `slide`, goes more
// than `deep` behind it, or is lost. A tip that comes through the surface from behind or
// from the side never presses: it has to come out in front first.
class TouchPoke {
  public:
    static constexpr double hover = .06, arm = .008, release = .012, deep = .12, slide = 24;
    // `point`: none while the tip isn't tracked or the panel isn't up. `surface`: the case.
    TouchEvent update(const std::optional<PanelPoint>& point, const Rect& surface);
    void reset() { *this = {}; }
    bool pressed() const { return state_ == State::Pressed; }
    bool near() const { return state_ != State::Idle; }

  private:
    enum class State { Idle, Hover, Pressed };
    State state_{State::Idle};
    bool armed_{};
};
} // namespace framekeyboard
