#include "framekeyboard/grip.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace framekeyboard {
double component_rotation_degrees(const Transform& released, const Transform& current) {
    // trace(R_rest^T R_current) = 1 + 2*cos(angle). Clamp float rounding at rest.
    double trace = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            trace += released[r][c] * current[r][c];
        }
    }
    return std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0)) * 180 / std::numbers::pi;
}
namespace {
// Component travel stays close to neutral, where this quaternion extraction is
// well-conditioned. Reject large/invalid rotations rather than causing motion.
std::optional<std::array<double, 4>> relative_rotation(const Transform& neutral,
                                                       const Transform& current) {
    double rotation[3][3]{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                rotation[r][c] += neutral[k][r] * current[k][c];
            }
        }
    }
    const double w = std::sqrt(std::max(0.0, 1 + rotation[0][0] + rotation[1][1] + rotation[2][2])) / 2;
    if (!std::isfinite(w) || w < .8) {
        return {};
    }
    std::array<double, 4> q{w, (rotation[2][1] - rotation[1][2]) / (4 * w),
                            (rotation[0][2] - rotation[2][0]) / (4 * w),
                            (rotation[1][0] - rotation[0][1]) / (4 * w)};
    for (double value : q) {
        if (!std::isfinite(value)) {
            return {};
        }
    }
    return q;
}
} // namespace
bool ComponentAxis::calibrate(const Transform& neutral, const Transform& positive) {
    full_angle_ = 0;
    const auto q = relative_rotation(neutral, positive);
    if (!q) {
        return false;
    }
    const double length = std::hypot((*q)[1], (*q)[2], (*q)[3]);
    const double angle = 2 * std::atan2(length, (*q)[0]);
    if (angle < .01 || angle > 1) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        axis_[i] = (*q)[i + 1] / length;
    }
    neutral_ = neutral;
    full_angle_ = angle;
    return true;
}
double ComponentAxis::read(const Transform& current) const {
    if (full_angle_ <= 0) {
        return 0;
    }
    const auto q = relative_rotation(neutral_, current);
    if (!q) {
        return 0;
    }
    double projection = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        projection += (*q)[i + 1] * axis_[i];
    }
    // Extract twist about the up/down axis. Sideways stick tilt is the other
    // rotation axis and cancels from this ratio, including diagonal movement.
    return std::clamp(2 * std::atan2(projection, (*q)[0]) / full_angle_, -1.0, 1.0);
}
GripTransition GripLatch::update(std::optional<double> angle_degrees) {
    if (!angle_degrees || !std::isfinite(*angle_degrees)) {
        reset();
        return {};
    }
    if (!armed_) {
        armed_ = *angle_degrees < .5;
        return {};
    }
    const bool held = *angle_degrees >= (held_ ? .5 : 1.0);
    const GripTransition state{held, held && !held_};
    held_ = held;
    return state;
}
void GripLatch::reset() {
    held_ = false;
    armed_ = false;
}
void TriggerReleaseGuard::begin(double now) {
    *this = {};
    started_ = now;
}
bool TriggerReleaseGuard::update(std::optional<double> angle_degrees, double now) {
    if (!angle_degrees || !std::isfinite(*angle_degrees) || !std::isfinite(now) || now < started_) {
        released_since_.reset();
        sampled_at_.reset();
        return false;
    }
    // A delayed frame is not evidence of continuously released input.
    if (sampled_at_ && (now < *sampled_at_ || now - *sampled_at_ > .1)) {
        released_since_.reset();
    }
    sampled_at_ = now;
    if (*angle_degrees >= 1) {
        seen_held_ = true;
    }
    if (*angle_degrees >= .5) {
        released_since_.reset();
        return false;
    }
    // Preserve immediate normal-release recovery after seeing a hold,
    // including a genuine click shorter than the initial grace period.
    if (seen_held_) {
        return true;
    }
    if (now - started_ < .15) {
        released_since_.reset();
        return false;
    }
    if (!released_since_) {
        released_since_ = now;
    }
    return now - *released_since_ >= .05;
}
} // namespace framekeyboard
