#include "framekeyboard/placement.hpp"
#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
constexpr double radians = 3.14159265358979323846 / 180.0;
using Rotation = std::array<std::array<double, 3>, 3>;
// Flatten the heading so looking down or tilting one's head does not summon a
// slanted panel. When looking straight up/down, derive heading from head-right.
Rotation level_heading(const Transform& head) {
    double zx = head[0][2], zz = head[2][2];
    if (std::hypot(zx, zz) < .001) {
        zx = -head[2][0];
        zz = head[0][0];
    }
    const double length = std::hypot(zx, zz);
    if (length < .001) {
        return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    }
    zx /= length;
    zz /= length;
    return {{{zz, 0, zx}, {0, 1, 0}, {-zx, 0, zz}}};
}
} // namespace
Transform HorizonAlignment::update(const Transform& raw, double now, bool grabbed, bool enabled) {
    if (grabbed || !enabled) {
        // Preserve the exact pose while moving or with assistance disabled.
        // Reset the easing clock so release or enabling starts smoothly.
        reset();
        return raw;
    }
    // A local Z rotation changes roll without changing the panel normal, hence
    // preserves its heading and tilt. World Y projected onto right/up gives roll.
    const double vertical = std::hypot(raw[1][0], raw[1][1]);
    if (!std::isfinite(now) || !std::isfinite(vertical) || vertical < .001) {
        // A panel facing straight up/down has no well-defined horizon roll.
        reset();
        return raw;
    }
    const double roll = std::atan2(raw[1][0], raw[1][1]);
    const bool within = std::abs(roll) <= 5 * radians + 1e-9;
    if (within != within_threshold_) {
        within_threshold_ = within;
        from_correction_ = correction_;
        started_ = now;
    }
    const double t = std::clamp((now - started_) / .5, 0.0, 1.0);
    animating_ = t < 1 && std::abs(from_correction_ - (within ? -roll : 0.0)) > 1e-6;
    const double ease = t * t * (3 - 2 * t);
    // Track the live roll while easing in. On leaving the capture range, ease
    // the existing correction away instead of jumping back to the raw pose.
    correction_ = std::lerp(from_correction_, within ? -roll : 0.0, ease);
    const double c = std::cos(correction_), s = std::sin(correction_);
    auto result = raw;
    for (std::size_t row = 0; row < 3; ++row) {
        result[row][0] = raw[row][0] * c + raw[row][1] * s;
        result[row][1] = -raw[row][0] * s + raw[row][1] * c;
    }
    return result;
}
Transform compose(const Transform& a, const Transform& b) {
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t k = 0; k < 3; ++k) {
                result[r][c] += a[r][k] * b[k][c];
            }
        }
        result[r][3] += a[r][3];
    }
    return result;
}
std::optional<Transform> dashboard_anchor(const Transform& scaled_bottom) {
    for (const auto& row : scaled_bottom) {
        for (double value : row) {
            if (!std::isfinite(value)) {
                return {};
            }
        }
        if (std::abs(row[3]) >= 1000) {
            return {};
        }
    }
    // Remove overlay scale without flattening pitch. The normal determines
    // facing direction; rebuild an orthogonal right/up basis around it.
    std::array<double, 3> normal{}, right{}, up{};
    const double normal_length =
        std::hypot(scaled_bottom[0][2], scaled_bottom[1][2], scaled_bottom[2][2]);
    if (normal_length < .001) {
        return {};
    }
    double projection = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        normal[r] = scaled_bottom[r][2] / normal_length;
        projection += scaled_bottom[r][0] * normal[r];
    }
    for (std::size_t r = 0; r < 3; ++r) {
        right[r] = scaled_bottom[r][0] - projection * normal[r];
    }
    const double right_length = std::hypot(right[0], right[1], right[2]);
    if (right_length < .001) {
        return {};
    }
    for (auto& value : right) {
        value /= right_length;
    }
    up = {normal[1] * right[2] - normal[2] * right[1], normal[2] * right[0] - normal[0] * right[2],
          normal[0] * right[1] - normal[1] * right[0]};
    double up_projection = 0;
    for (std::size_t r = 0; r < 3; ++r) {
        up_projection += up[r] * scaled_bottom[r][1];
    }
    if (up_projection < .001) {
        return {}; // Reject degenerate or mirrored bases.
    }
    Transform anchor = scaled_bottom;
    for (std::size_t r = 0; r < 3; ++r) {
        anchor[r][0] = right[r];
        anchor[r][1] = up[r];
        anchor[r][2] = normal[r];
    }
    return anchor;
}
void DashboardAnchor::restore(std::optional<Transform> anchor, std::optional<Transform> bar,
                              bool full_rotation) {
    anchor_ = anchor;
    bar_ = bar;
    full_rotation_ = full_rotation;
}
std::optional<Transform> DashboardAnchor::update(std::optional<Transform> main,
                                                 std::optional<Transform> bar) {
    if (!main && !bar) {
        return {};
    }
    if (anchor_ && bar_ && bar) {
        auto current = *bar;
        if (!full_rotation_) {
            // Old saves contain yaw-only bar poses. Apply their last translation
            // and yaw delta once, then adopt full rotation without a tilt jump.
            const auto heading = level_heading(current);
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 3; ++c) {
                    current[r][c] = heading[r][c];
                }
            }
        }
        if (current != *bar_) {
            anchor_ = move_with_dashboard(*anchor_, *bar_, current);
        }
    } else if (main) {
        anchor_ = main;
    } else {
        // A first launch directly into an app has no calibrated main-tab mount.
        // Use the live bar instead of an invisible tab's stale room position.
        anchor_ = bar;
    }
    // Keep one reference across tab switches: the bar has its own fixed tilt,
    // so replacing its carried anchor with a different tab's basis could snap.
    bar_ = bar;
    full_rotation_ = true;
    return anchor_;
}
Transform move_with_dashboard(const Transform& panel, const Transform& previous,
                              const Transform& current) {
    // Invert only the rigid anchor, never Steam's scaled overlay matrix.
    Transform inverse{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            inverse[r][c] = previous[c][r];
            inverse[r][3] -= previous[c][r] * previous[c][3];
        }
    }
    return compose(current, compose(inverse, panel));
}
void PanelDrag::begin(const Transform& controller, const Transform& panel,
                      std::array<double, 3> ray_direction) {
    Transform inverse{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            inverse[r][c] = controller[c][r];
            inverse[r][3] -= controller[c][r] * controller[c][3];
        }
    }
    relative_ = compose(inverse, panel);
    const double length = std::hypot(ray_direction[0], ray_direction[1], ray_direction[2]);
    ray_direction_ = {0, 0, -1};
    if (std::isfinite(length) && length > .001) {
        for (std::size_t i = 0; i < 3; ++i) {
            ray_direction_[i] = ray_direction[i] / length;
        }
    }
    depth_ = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        depth_ += relative_[i][3] * ray_direction_[i];
    }
    // Do not snap an existing out-of-range panel when grabbed. Allow the stick
    // to bring it back toward the normal 20 cm to 3 m range, but not farther out.
    minimum_depth_ = std::min(.2, depth_);
    maximum_depth_ = std::max(3.0, depth_);
}
void PanelDrag::move_depth(double stick_y, double elapsed_seconds) {
    constexpr double dead_zone = .2, speed = .65;
    if (!std::isfinite(stick_y) || !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0 ||
        std::abs(stick_y) <= dead_zone) {
        return;
    }
    const double magnitude = (std::min(std::abs(stick_y), 1.0) - dead_zone) / (1 - dead_zone);
    // Cap elapsed time so a suspended/stalled frame cannot teleport the panel.
    const double delta = std::copysign(magnitude, stick_y) * speed * std::min(elapsed_seconds, .05);
    const double next = std::clamp(depth_ + delta, minimum_depth_, maximum_depth_);
    for (std::size_t i = 0; i < 3; ++i) {
        relative_[i][3] += ray_direction_[i] * (next - depth_);
    }
    depth_ = next;
}
Transform PanelDrag::update(const Transform& controller) const {
    return compose(controller, relative_);
}
void PanelPlacement::set_transform(const Transform& world) {
    world_ = world;
    ready_ = true;
}
void PanelPlacement::restore(const Transform& world, double width) {
    set_transform(world);
    set_width(width);
}
void PanelPlacement::set_width(double width) {
    width_ = std::clamp(width, .45, 2.0);
}
void PanelPlacement::recenter(const Transform& head, const Transform* dashboard_bottom,
                              double height_over_width) {
    const auto rotation = level_heading(dashboard_bottom ? *dashboard_bottom : head);
    // Tilt the top edge away from the viewer: 50 degrees from upright,
    // leaving the typing surface 40 degrees above a horizontal desk.
    const double pitch = -50 * radians;
    const double c = std::cos(pitch), s = std::sin(pitch);
    for (std::size_t row = 0; row < 3; ++row) {
        world_[row][0] = rotation[row][0];
        world_[row][1] = rotation[row][1] * c + rotation[row][2] * s;
        world_[row][2] = -rotation[row][1] * s + rotation[row][2] * c;
        // Leave 6 cm between the dashboard bottom and the keyboard's top edge,
        // with the keyboard center 24 cm toward the viewer.
        world_[row][3] = dashboard_bottom
                             ? (*dashboard_bottom)[row][3] + rotation[row][2] * .24 -
                                   rotation[row][1] * (.06 + width_ * height_over_width * c / 2)
                             : head[row][3] - rotation[row][2] * .85 - rotation[row][1] * .65;
    }
    ready_ = true;
}
void PanelPlacement::adjust(PlacementAction action) {
    switch (action) {
    case PlacementAction::Smaller:
        width_ -= .05;
        break;
    case PlacementAction::Larger:
        width_ += .05;
        break;
    }
    width_ = std::clamp(width_, .45, 2.0);
}
} // namespace framekeyboard
