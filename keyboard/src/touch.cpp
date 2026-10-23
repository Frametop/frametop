#include "framekeyboard/touch.hpp"
#include "framekeyboard/panel.hpp"

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

TouchEvent TouchPoke::update(const std::optional<PanelPoint>& point, const Rect& surface) {
    using Kind = TouchEvent::Kind;
    if (!point) {
        const auto was = state_;
        reset();
        return {was == State::Pressed ? Kind::Cancel : was == State::Hover ? Kind::Leave : Kind::None};
    }
    const auto [x, y, depth] = *point;
    const bool over = surface.contains(x, y);
    if (state_ == State::Pressed) {
        const Rect loose{surface.x - slide, surface.y - slide, surface.width + 2 * slide,
                         surface.height + 2 * slide};
        if (depth < release && depth > -deep && loose.contains(x, y)) {
            return {Kind::Move, x, y};
        }
        // Back out in front over the case: hovering again, armed by the release's travel.
        state_ = over && depth > 0 && depth < hover ? State::Hover : State::Idle;
        armed_ = state_ == State::Hover && depth >= arm;
        return {Kind::Up, x, y};
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
        state_ = State::Pressed;
        armed_ = false;
        return {Kind::Down, x, y};
    }
    return {Kind::Hover, x, y};
}
} // namespace framekeyboard
