#pragma once
#include "framekeyboard/placement.hpp"
#include <optional>

namespace framekeyboard {
// Angular travel between a controller component's released and current poses.
// Translation is deliberately ignored: the render model defines the button pivot.
double component_rotation_degrees(const Transform& released, const Transform& current);

// Recover one signed stick axis from a render component's tilt. Calibration
// uses synthetic neutral/full-up poses, so model orientation and handedness do
// not need hardcoded Euler angles.
class ComponentAxis {
  public:
    bool calibrate(const Transform& neutral, const Transform& positive);
    double read(const Transform& current) const;

  private:
    Transform neutral_{};
    std::array<double, 3> axis_{};
    double full_angle_{};
};

struct GripTransition {
    bool held{}, pressed{};
};
class GripLatch {
  public:
    // Frame's grip component has 9.5 degrees of travel. A small dead zone and
    // separate release threshold keep a resting finger from chattering capture.
    // Missing samples disarm it; startup/reconnect requires a release first.
    GripTransition update(std::optional<double> angle_degrees);
    void reset();

  private:
    bool armed_{}, held_{};
};
// Recover a line capture when overlay mouse-up is lost. An initial rest pose
// can be stale, so allow a short grace period before trusting repeated rest
// samples without a preceding held sample. Missing samples break that streak.
class TriggerReleaseGuard {
  public:
    void begin(double now);
    bool update(std::optional<double> angle_degrees, double now);

  private:
    double started_{};
    bool seen_held_{};
    std::optional<double> released_since_, sampled_at_;
};
} // namespace framekeyboard
