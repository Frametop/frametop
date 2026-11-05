#include "framekeyboard/touch.hpp"
#include "framekeyboard/panel.hpp"
#include <algorithm>
#include <cmath>

namespace framekeyboard {
PanelPoint locate_on_panel(const Transform& panel, double width, const std::array<double, 3>& point) {
    // The panel's frame: columns right, up, out of its front, centre. Rigid, so its
    // inverse rotation is the transpose.
    std::array<double, 3> local{};
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            local[c] += panel[r][c] * (point[r] - panel[r][3]);
        }
    }
    // The overlay is `width` wide over panel_width pixels, with square texels, so the same
    // scale holds up the texture's height. OpenVR's mouse y runs up from the bottom.
    const double scale = panel_width / width;
    const double mouse_y = local[1] * scale + texture_height / 2.0;
    return {local[0] * scale + panel_width / 2.0, texture_height - mouse_y - popup_margin, local[2]};
}

std::array<double, 3> tip_point(const Transform& device, const Transform& component, double along) {
    std::array<double, 3> local{};
    for (std::size_t r = 0; r < 3; ++r) {
        local[r] = component[r][3] - along * component[r][2];
    }
    std::array<double, 3> world{};
    for (std::size_t r = 0; r < 3; ++r) {
        world[r] = device[r][3];
        for (std::size_t c = 0; c < 3; ++c) {
            world[r] += device[r][c] * local[c];
        }
    }
    return world;
}

double TouchPoke::speed(double now, double depth) const {
    // The oldest sample within 30 ms (at least 8 ms back), against now: tracking jitter
    // averages out, and a tap's push in still shows.
    const Sample* from = nullptr;
    for (std::size_t i = 0; i < std::min(samples_, history_.size()); ++i) {
        const auto& sample = history_[(samples_ - 1 - i) % history_.size()];
        if (now - sample.time > .03) {
            break;
        }
        from = &sample;
    }
    if (!from || now - from->time < .008) {
        return 0;
    }
    return (from->depth - depth) / (now - from->time);
}

TouchEvent TouchPoke::update(const std::optional<PanelPoint>& point, const Rect& surface, double now) {
    using Kind = TouchEvent::Kind;
    if (!point) {
        const auto was = state_;
        reset();
        return {was == State::Pressed ? Kind::Cancel : was == State::Idle ? Kind::None : Kind::Leave};
    }
    const auto [x, y, depth] = *point;
    const double inward = speed(now, depth);
    history_[samples_++ % history_.size()] = {now, depth};
    const bool over = surface.contains(x, y);
    const bool can_press = inward >= min_speed && now - last_down_ >= debounce;
    auto press = [&] {
        state_ = State::Pressed;
        armed_ = false;
        deepest_ = depth;
        down_x_ = x, down_y_ = y, last_down_ = now;
        return TouchEvent{Kind::Down, x, y};
    };
    if (state_ == State::Pressed) {
        deepest_ = std::min(deepest_, depth);
        const Rect loose{surface.x - slide, surface.y - slide, surface.width + 2 * slide,
                         surface.height + 2 * slide};
        if (depth - deepest_ < release_travel && depth > -deep && loose.contains(x, y) &&
            std::hypot(x - down_x_, y - down_y_) <= drift) {
            return {Kind::Move, x, y};
        }
        // Up. Still near the case: lifted, and a push in again presses the next key.
        const bool near_case = over && depth < hover && depth > -deep;
        state_ = near_case ? (depth >= arm ? State::Hover : State::Lifted) : State::Idle;
        armed_ = state_ == State::Hover;
        shallowest_ = depth;
        return {Kind::Up, x, y};
    }
    if (state_ == State::Lifted) {
        if (!over || depth >= hover || depth <= -deep) {
            reset();
            return {Kind::Leave, x, y};
        }
        shallowest_ = std::max(shallowest_, depth);
        if (depth >= arm) {
            state_ = State::Hover;
            armed_ = true;
            return {Kind::Hover, x, y};
        }
        if (shallowest_ - depth >= retap_travel && depth <= 0 && can_press) {
            return press();
        }
        return {Kind::Hover, x, y};
    }
    // Through the surface without coming from `arm` out in front (from behind, from the
    // side, or creeping in): not a press, and no hover behind the panel.
    const bool arms = armed_ || depth >= arm;
    if (!over || depth >= hover || (depth <= 0 && (state_ == State::Idle || !arms))) {
        const bool was = state_ == State::Hover;
        reset();
        return {was ? Kind::Leave : Kind::None, x, y};
    }
    state_ = State::Hover;
    armed_ = arms;
    if (depth <= 0) {
        if (can_press) {
            return press();
        }
        // In too slowly (or too soon): held back, and a push from here presses.
        state_ = State::Lifted;
        armed_ = false;
        shallowest_ = depth;
    }
    return {Kind::Hover, x, y};
}
} // namespace framekeyboard
