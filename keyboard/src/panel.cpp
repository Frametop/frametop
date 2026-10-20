#include "framekeyboard/panel.hpp"

#include <algorithm>
#include <cmath>
#include <pango/pangocairo.h>
#include <stdexcept>

namespace framekeyboard {
std::optional<Rect> panel_move_handle_bounds(const PanelView& view) {
    for (const auto& control : view.controls) {
        if (control.style == ControlStyle::MoveHandle) {
            return control.bounds;
        }
    }
    return {};
}
bool hidden_key(const PanelView& view, const Key& key) {
    return view.hide_numpad && !view.settings &&
           (key.action == "NumLock" || key.action.starts_with("Numpad"));
}
namespace {
Color disabled_legend(const Theme& theme) {
    return {(theme.legend.r + theme.disabled.r) / 2, (theme.legend.g + theme.disabled.g) / 2,
            (theme.legend.b + theme.disabled.b) / 2};
}
// CSS ease-out is cubic-bezier(0, 0, .58, 1), as used by Steam's handle.
double handle_ease_out(double progress) {
    if (progress <= 0 || progress >= 1) {
        return progress;
    }
    double low = 0, high = 1;
    for (int i = 0; i < 16; ++i) {
        const double t = (low + high) / 2;
        const double x = 3 * .58 * (1 - t) * t * t + t * t * t;
        if (x < progress) {
            low = t;
        } else {
            high = t;
        }
    }
    const double t = (low + high) / 2;
    return t * t * (3 - 2 * t);
}
double visible_layout_width(const PanelView& view) {
    if (!view.hide_numpad || view.settings) {
        return view.layout->width;
    }
    double right = 0;
    bool removed = false;
    for (const auto& key : view.layout->keys) {
        if (hidden_key(view, key)) {
            removed = true;
        } else {
            right = std::max(right, key.bounds.x + key.bounds.width);
        }
    }
    // Do not crop intentional whitespace on a layout without a numpad, or an
    // all-numpad custom layout to zero width.
    return removed && right > 0 ? right : view.layout->width;
}

constexpr double normal_toolbar_height = 96;
struct Placement {
    double scale, x, y;
};
Placement placement(const PanelView& view, bool normal = false) {
    const double margin = view.theme->padding;
    const double toolbar_height = view.composing && !normal ? 208 : normal_toolbar_height;
    const double scale =
        std::min((panel_width - 2 * margin) / view.layout->width,
                 (panel_height - toolbar_height - 2 * margin - view.theme->depth) / view.layout->height);
    return {scale, (panel_width - visible_layout_width(view) * scale) / 2,
            toolbar_height + (panel_height - toolbar_height - view.layout->height * scale) / 2};
}
void source(cairo_t* cr, Color color) {
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
}
void rounded(cairo_t* cr, Rect r, double radius) {
    radius = std::min({radius, r.width / 2, r.height / 2});
    constexpr double pi = 3.141592653589793;
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -pi / 2, 0);
    cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, pi / 2);
    cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, pi / 2, pi);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, pi, 1.5 * pi);
    cairo_close_path(cr);
}
void gradient(cairo_t* cr, Rect r, Color top, Color middle, Color bottom) {
    auto* pattern = cairo_pattern_create_linear(0, r.y, 0, r.y + r.height);
    cairo_pattern_add_color_stop_rgb(pattern, 0, top.r, top.g, top.b);
    cairo_pattern_add_color_stop_rgb(pattern, .5, middle.r, middle.g, middle.b);
    cairo_pattern_add_color_stop_rgb(pattern, 1, bottom.r, bottom.g, bottom.b);
    cairo_set_source(cr, pattern);
    cairo_fill(cr);
    cairo_pattern_destroy(pattern);
}
void label(cairo_t* cr, const std::string& text, Rect r, double size, const std::string& family,
           Color color, bool fit = false, PangoAlignment alignment = PANGO_ALIGN_CENTER) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* font = pango_font_description_new();
    pango_font_description_set_family(font, family.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    if (fit) {
        int natural_width, natural_height;
        pango_layout_get_pixel_size(layout, &natural_width, &natural_height);
        if (natural_width > r.width - 8) {
            const double fitted = std::max(9.0, size * (r.width - 8) / natural_width);
            pango_font_description_set_absolute_size(font, fitted * PANGO_SCALE);
            pango_layout_set_font_description(layout, font);
        }
    }
    pango_layout_set_width(layout, static_cast<int>(std::max(1.0, r.width - 8) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, alignment);
    int width, height;
    pango_layout_get_pixel_size(layout, &width, &height);
    source(cr, color);
    cairo_move_to(cr, r.x + 4, r.y + (r.height - height) / 2);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}
// Shared vector icons keep controls independent of installed symbol fonts.
// Key icons are centered on the moving face, so they follow press animation.
void draw_icon(cairo_t* cr, Icon icon, Rect bounds, Color color, double size = 24) {
    constexpr double pi = 3.141592653589793;
    cairo_save(cr);
    cairo_translate(cr, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
    const double scale = std::min({size, bounds.width - 8, bounds.height - 8}) / 24;
    cairo_scale(cr, scale, scale);
    source(cr, color);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    auto line = [&](double x1, double y1, double x2, double y2) {
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
    };
    switch (icon) {
    case Icon::SteamFrame:
        // Trace the reference's 194px mark: 47.5px bars, a 128px
        // quarter-circle and small rounded exterior corners. Inner corners
        // stay square; stroking the outline would change the measured gaps.
        cairo_scale(cr, 24.0 / 194, 24.0 / 194);
        cairo_translate(cr, -225.5, -225.5);
        cairo_move_to(cr, 131.5, 128.5);
        cairo_line_to(cr, 319.5, 128.5);
        cairo_arc(cr, 319.5, 131.5, 3, -.5 * pi, 0);
        cairo_line_to(cr, 322.5, 176);
        cairo_line_to(cr, 176, 176);
        cairo_line_to(cr, 176, 322.5);
        cairo_line_to(cr, 131.5, 322.5);
        cairo_arc(cr, 131.5, 319.5, 3, .5 * pi, pi);
        cairo_line_to(cr, 128.5, 131.5);
        cairo_arc(cr, 131.5, 131.5, 3, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_move_to(cr, 322.5, 322.5);
        cairo_arc(cr, 322.5, 322.5, 128, pi, 1.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::SteamOS:
        // The reference uses concentric radii 48, 62.4 and 96. Its ink
        // bounds are 144x192, centered here without stretching to a square.
        cairo_arc(cr, -3, 0, 6, 0, 2 * pi);
        cairo_fill(cr);
        cairo_arc(cr, -3, 0, 12, -.5 * pi, .5 * pi);
        cairo_arc_negative(cr, -3, 0, 7.8, .5 * pi, -.5 * pi);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case Icon::Apply:
        line(-8, 0, -2, 6);
        line(-2, 6, 9, -7);
        break;
    case Icon::Reload:
        cairo_arc(cr, 0, 0, 8, -.5, 4.8);
        line(1, -8, -3, -10);
        line(1, -8, -2, -4);
        break;
    case Icon::PinOn:
    case Icon::PinOff:
        if (icon == Icon::PinOff) {
            cairo_rotate(cr, .65);
        }
        line(-5, -9, 5, -9);
        line(-4, -9, -4, -1);
        line(4, -9, 4, -1);
        cairo_move_to(cr, -4, -1);
        cairo_line_to(cr, -8, 3);
        cairo_line_to(cr, 8, 3);
        cairo_line_to(cr, 4, -1);
        line(0, 3, 0, 10);
        if (icon == Icon::PinOff) {
            cairo_stroke(cr);
            cairo_rotate(cr, -.65);
            line(-10, -10, 10, 10);
        }
        break;
    case Icon::Settings:
        // Each tooth has a flat tip and a recessed gap.
        for (int i = 0; i < 32; ++i) {
            const double angle = i * pi / 16;
            const double radius = i % 4 < 2 ? 11 : 8.5;
            const double x = radius * std::cos(angle), y = radius * std::sin(angle);
            if (i == 0) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_close_path(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 3.5, 0, 2 * pi);
        break;
    case Icon::Back:
        line(9, 0, -9, 0);
        line(-2, -7, -9, 0);
        cairo_line_to(cr, -2, 7);
        break;
    case Icon::Recenter:
        cairo_arc(cr, 0, 0, 7, 0, 2 * pi);
        line(-11, 0, -5, 0);
        line(5, 0, 11, 0);
        line(0, -11, 0, -5);
        line(0, 5, 0, 11);
        break;
    case Icon::Close:
        line(-7, -7, 7, 7);
        line(7, -7, -7, 7);
        break;
    case Icon::Copy:
        rounded(cr, {-3, -3, 13, 14}, 2);
        cairo_move_to(cr, 4, -6);
        cairo_line_to(cr, 4, -9);
        cairo_line_to(cr, -10, -9);
        cairo_line_to(cr, -10, 5);
        cairo_line_to(cr, -6, 5);
        break;
    case Icon::Paste:
        cairo_move_to(cr, -5, -8);
        cairo_line_to(cr, -9, -8);
        cairo_line_to(cr, -9, 11);
        cairo_line_to(cr, 9, 11);
        cairo_line_to(cr, 9, -8);
        cairo_line_to(cr, 5, -8);
        rounded(cr, {-5, -11, 10, 6}, 2);
        line(-4, 0, 4, 0);
        line(-4, 5, 2, 5);
        break;
    case Icon::ScaleDown:
    case Icon::ScaleUp:
        cairo_arc(cr, -3, -3, 8, 0, 2 * pi);
        line(3, 3, 10, 10);
        line(-7, -3, 1, -3);
        if (icon == Icon::ScaleUp) {
            line(-3, -7, -3, 1);
        }
        break;
    case Icon::None:
        break;
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}
void draw_key(cairo_t* cr, const PanelView& view, const Key& key, double amount) {
    const auto& t = *view.theme;
    const auto modifiers = view.keyboard->modifiers();
    std::set<int> legend_modifiers;
    for (const char* name : {"ShiftLeft", "ShiftRight", "AltRight"}) {
        const int code = key_code(name);
        if (modifiers.contains(code)) {
            legend_modifiers.insert(code);
        }
    }
    const auto r = key.bounds;
    // The side begins below the resting face. A fixed-size face moves down
    // over it; changing face height would make a pressed key look stretched.
    Rect side{r.x, r.y + t.travel, r.width, r.height + t.depth - t.travel};
    rounded(cr, side, t.radius);
    gradient(cr, side, t.side_top, t.side_bottom, t.side_bottom);
    Rect face{r.x, r.y + amount * t.travel, r.width, r.height};
    const int code = key.action_kind == ActionKind::Key ? key_code(key.action) : 0;
    const bool latched = modifiers.contains(code) ||
                         (key.action == "CapsLock" && view.keyboard->caps()) ||
                         (key.action == "NumLock" && view.keyboard->num());
    rounded(cr, face, t.radius);
    if (latched) {
        source(cr, t.latched);
        cairo_fill(cr);
    } else if (view.hovered_keys.contains(key.id)) {
        gradient(cr, face, t.hover, t.middle, t.bottom);
    } else {
        gradient(cr, face, t.top, t.middle, t.bottom);
    }
    if (key.icon != Icon::None) {
        const bool super_icon = key.icon == Icon::SteamFrame || key.icon == Icon::SteamOS;
        draw_icon(cr, key.icon, face, t.legend, super_icon ? 22 : 32);
        return;
    }
    auto text = view.keymap->legend(key, legend_modifiers, view.keyboard->caps(), view.keyboard->num());
    if (const auto custom = view.key_labels.find(key.id); custom != view.key_labels.end()) {
        text = custom->second;
    }
    std::string secondary;
    if (view.key_labels.contains(key.id)) {
        const auto small = view.language->kana_shift.find(key.action);
        if (small != view.language->kana_shift.end() && small->second != text) {
            secondary = small->second;
        }
    } else if (key.action_kind == ActionKind::Key && key.action.starts_with("Numpad")) {
        secondary = key.secondary_label;
    } else if (key.action_kind == ActionKind::Key && !text.empty() && text != key.label &&
               text.size() < 5) {
        auto shifted = legend_modifiers;
        shifted.insert(key_code("ShiftLeft"));
        secondary = view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
        if (secondary == text) {
            secondary.clear();
        }
    } else if (!key.secondary_label.empty()) {
        auto shifted = legend_modifiers;
        shifted.insert(key_code("ShiftLeft"));
        secondary = view.keymap->legend(key, shifted, view.keyboard->caps(), view.keyboard->num());
        if (secondary == text) {
            secondary.clear();
        }
    }
    if (!view.key_labels.contains(key.id) && g_utf8_strlen(text.c_str(), -1) == 1 &&
        g_utf8_strlen(secondary.c_str(), -1) == 1 && g_unichar_isalpha(g_utf8_get_char(text.c_str())) &&
        g_unichar_tolower(g_utf8_get_char(text.c_str())) ==
            g_unichar_tolower(g_utf8_get_char(secondary.c_str()))) {
        // Case belongs in the main legend, including letters on punctuation
        // positions in Cyrillic/AZERTY layouts. Distinct shifted symbols stay visible.
        secondary.clear();
    }
    // F10-F12 must not shrink simply because their names have three characters.
    const bool function_key = key.action_kind == ActionKind::Key && key.action.starts_with("F") &&
                              key.action.size() > 1 &&
                              key.action.find_first_not_of("0123456789", 1) == std::string::npos;
    const bool utility =
        g_utf8_strlen(text.c_str(), -1) > 2 && !key.action.starts_with("Key") && !function_key;
    if (!secondary.empty()) {
        label(cr, secondary, {face.x, face.y + 3, face.width, face.height * .40}, t.small_font_size,
              view.language->font, t.legend, true);
        label(cr, text, {face.x, face.y + face.height * .4, face.width, face.height * .55},
              t.font_size * .85, view.language->font, t.legend, true);
    } else {
        label(cr, text, face, utility ? t.small_font_size : t.font_size, view.language->font, t.legend,
              true);
    }
}
} // namespace
Rect panel_case_bounds(const PanelView& view) {
    const auto p = placement(view, true);
    // Custom geometry can leave only a few pixels after hiding its numpad.
    // Reserve the fixed main toolbar, and room for the scaled IME toolbar when
    // present. This same case is used by painting, controls and the VR mask.
    const bool ime = view.language && view.language->input_method != "xkb";
    const double minimum = ime ? 1300 : 560;
    const double width =
        std::max(minimum, panel_width - (view.layout->width - visible_layout_width(view)) * p.scale);
    return {(panel_width - width) / 2, 0, width, panel_height};
}
PanelRenderer::PanelRenderer() {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, panel_width, texture_height);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
        throw std::runtime_error("cannot allocate keyboard image");
    }
}
PanelRenderer::~PanelRenderer() {
    cairo_surface_destroy(surface_);
}
const Key* PanelRenderer::hit_key(const PanelView& view, double x, double y) const {
    if (view.settings) {
        return nullptr;
    }
    const auto p = placement(view);
    x = (x - p.x) / p.scale;
    y = (y - p.y) / p.scale;
    for (const auto& key : view.layout->keys) {
        if (!hidden_key(view, key) && key.bounds.contains(x, y)) {
            return &key;
        }
    }
    return nullptr;
}
void PanelRenderer::paint(const PanelView& view, double now, bool force_full) {
    const auto& t = *view.theme;
    const auto modifiers = view.keyboard->modifiers();
    const bool scene_changed =
        !previous_ || previous_->layout != *view.layout || previous_->theme != t ||
        previous_->language != *view.language || previous_->modifiers != modifiers ||
        previous_->caps != view.keyboard->caps() || previous_->num != view.keyboard->num() ||
        previous_->view.controls != view.controls || previous_->view.status != view.status ||
        previous_->view.settings != view.settings || previous_->view.composing != view.composing ||
        previous_->view.hide_numpad != view.hide_numpad || previous_->view.preedit != view.preedit ||
        previous_->view.key_labels != view.key_labels;
    bool full = force_full || scene_changed || view.settings;
    std::vector<Rect> damage;
    animating_ = false;
    if (const auto handle = panel_move_handle_bounds(view)) {
        const bool held = std::any_of(view.controls.begin(), view.controls.end(), [](const Control& c) {
            return c.style == ControlStyle::MoveHandle && c.selected;
        });
        const double target = held || view.hovered_controls.contains("move-handle") ? 1 : 0;
        const double old_amount = move_handle_.amount;
        const bool changed = target != move_handle_.target;
        if (changed) {
            move_handle_.from = move_handle_.amount;
            move_handle_.target = target;
            move_handle_.started = now;
        }
        const double progress = std::clamp((now - move_handle_.started) / .08, 0.0, 1.0);
        move_handle_.amount =
            move_handle_.from + (target - move_handle_.from) * handle_ease_out(progress);
        animating_ |= progress < 1;
        full |= changed || move_handle_.amount != old_amount;
    } else {
        move_handle_ = {};
    }
    for (const auto& key : view.layout->keys) {
        if (hidden_key(view, key)) {
            continue;
        }
        auto& animation = animations_[key.id];
        const double old_amount = animation.amount;
        const double target = view.keyboard->pressed(key.id) ? 1 : 0;
        const bool changed_target = target != animation.target;
        if (changed_target) {
            animation.from = animation.amount;
            animation.target = target;
            animation.started = now;
        }
        const double progress =
            t.duration_ms == 0 ? 1
                               : std::clamp((now - animation.started) * 1000 / t.duration_ms, 0.0, 1.0);
        animation.amount = animation.from + (animation.target - animation.from) * progress;
        animating_ |= !view.settings && progress < 1;
        if (!full &&
            (changed_target || animation.amount != old_amount ||
             previous_->view.hovered_keys.contains(key.id) != view.hovered_keys.contains(key.id))) {
            damage.push_back(key_bounds_.at(key.id));
        }
    }
    // Toolbar changes are infrequent and may overlap custom key geometry.
    if (!full) {
        for (const auto& control : view.controls) {
            if (previous_->view.hovered_controls.contains(control.id) !=
                view.hovered_controls.contains(control.id)) {
                full = true;
            }
        }
    }
    if (scene_changed) {
        key_bounds_.clear();
        // Capture the exact ink extents, including oversized/custom-font labels.
        // These surfaces are only used to measure; pixels are drawn directly onto
        // the image below, preserving Cairo's original blending and font rendering.
        if (!view.settings) {
            const auto p = placement(view);
            for (const auto& key : view.layout->keys) {
                if (hidden_key(view, key)) {
                    continue;
                }
                auto* recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
                auto* bounds_cr = cairo_create(recording);
                cairo_translate(bounds_cr, p.x, p.y);
                cairo_scale(bounds_cr, p.scale, p.scale);
                draw_key(bounds_cr, view, key, 0);
                double x, y, width, height;
                cairo_recording_surface_ink_extents(recording, &x, &y, &width, &height);
                // Union the resting and fully depressed positions, then round
                // outwards to pixel boundaries to avoid clipped antialiasing.
                const double left = std::floor(x) - 2, top = std::floor(y) - 2;
                key_bounds_[key.id] = {left, top, std::ceil(x + width) + 2 - left,
                                       std::ceil(y + height + t.travel * p.scale) + 2 - top};
                cairo_destroy(bounds_cr);
                cairo_surface_destroy(recording);
            }
        }
        previous_ =
            Snapshot{*view.layout,        t, *view.language, view, modifiers, view.keyboard->caps(),
                     view.keyboard->num()};
        // No animation state from a removed key should survive a profile reload.
        std::erase_if(animations_, [&](const auto& item) {
            return std::none_of(view.layout->keys.begin(), view.layout->keys.end(), [&](const Key& key) {
                return key.id == item.first && !hidden_key(view, key);
            });
        });
    } else {
        previous_->view = view;
    }
    if (!full && damage.empty()) {
        return;
    }
    auto* cr = cairo_create(surface_);
    cairo_translate(cr, 0, popup_margin);
    if (!full) {
        for (const auto& r : damage) {
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        }
        cairo_clip(cr);
    }
    // Clear and restore the case only inside the damaged pixels. Any neighboring
    // key intersecting that region is repainted in the original stacking order.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const auto body = panel_case_bounds(view);
    rounded(cr, body, t.surface_radius);
    source(cr, t.surface);
    cairo_fill(cr);
    if (!view.settings) {
        const auto p = placement(view);
        cairo_save(cr);
        cairo_translate(cr, p.x, p.y);
        cairo_scale(cr, p.scale, p.scale);
        for (const auto& key : view.layout->keys) {
            if (hidden_key(view, key)) {
                continue;
            }
            const auto& bounds = key_bounds_.at(key.id);
            const bool intersects =
                full || std::any_of(damage.begin(), damage.end(), [&](const Rect& r) {
                    return bounds.x < r.x + r.width && bounds.x + bounds.width > r.x &&
                           bounds.y < r.y + r.height && bounds.y + bounds.height > r.y;
                });
            if (intersects) {
                draw_key(cr, view, key, animations_.at(key.id).amount);
            }
        }
        cairo_restore(cr);
    }
    if (view.composing && !view.settings) {
        const auto& method = view.language->input_method;
        const char* hint = method == "korean-2set" ? "Type → Space/Enter: commit · Esc: cancel"
                           : method.starts_with("chinese-")
                               ? "Pinyin → Space/1–5: select · Enter: commit · Esc: cancel"
                               : "Type → Space: convert · Enter: commit · Esc: cancel";
        label(cr, view.preedit.empty() ? hint : view.preedit, {body.x + 20, 94, body.width - 40, 40}, 27,
              view.language->font, t.legend);
    }
    if (view.settings) {
        cairo_save(cr);
        cairo_rectangle(cr, 18, 78, 1564, 476);
        cairo_clip(cr);
        for (const auto& card : view.cards) {
            rounded(cr, card.bounds, 7);
            source(cr, t.legend);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
            // Titles stay fixed while the card's fields scroll beneath them.
            label(cr, card.title, {card.bounds.x + 14, card.bounds.y + 8, card.bounds.width - 28, 32},
                  22, view.language->font, t.legend, false, PANGO_ALIGN_LEFT);
        }
        cairo_restore(cr);
        for (const auto& field : view.field_labels) {
            cairo_save(cr);
            cairo_rectangle(cr, field.clip.x, field.clip.y, field.clip.width, field.clip.height);
            cairo_clip(cr);
            label(cr, field.text, field.bounds, 22, view.language->font,
                  field.enabled ? t.legend : disabled_legend(t), field.left,
                  field.left ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
            cairo_restore(cr);
        }
    }
    auto draw_control = [&](const Control& control) {
        cairo_save(cr);
        if (control.style == ControlStyle::MoveHandle) {
            const auto r = control.bounds;
            const double width = (r.width - 80) * (1 + .1 * move_handle_.amount);
            rounded(cr, {r.x + (r.width - width) / 2, r.y + r.height / 2 - 4, width, 8}, 4);
            auto channel = [&](double idle) {
                return idle / 255 + (1 - idle / 255) * move_handle_.amount;
            };
            cairo_set_source_rgb(cr, channel(61), channel(68), channel(80));
            cairo_fill(cr);
            cairo_restore(cr);
            return;
        }
        if (control.clip) {
            const auto& r = *control.clip;
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
            cairo_clip(cr);
        }
        const auto r = control.bounds;
        const auto legend = control.enabled ? t.legend : disabled_legend(t);
        const auto color =
            control.enabled && control.selected
                ? t.latched
                : (control.enabled && view.hovered_controls.contains(control.id) ? t.hover : t.bottom);
        rounded(cr, r, control.style == ControlStyle::Checkbox ? 3 : t.radius);
        source(cr, color);
        cairo_fill_preserve(cr);
        if (view.settings && control.style != ControlStyle::Scrollbar &&
            control.style != ControlStyle::MenuItem) {
            source(cr, legend);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }
        if (control.style == ControlStyle::Scrollbar) {
            source(cr, t.hover);
            rounded(cr, r, 3);
            cairo_fill(cr);
        } else if (control.style == ControlStyle::Dropdown) {
            label(cr, control.label, {r.x + 6, r.y, r.width - 58, r.height}, 22, view.language->font,
                  legend);
            source(cr, legend);
            cairo_move_to(cr, r.x + r.width - 50, r.y);
            cairo_line_to(cr, r.x + r.width - 50, r.y + r.height);
            const double x = r.x + r.width - 25, y = r.y + r.height / 2;
            cairo_move_to(cr, x - 7, y - 4);
            cairo_line_to(cr, x, y + 4);
            cairo_line_to(cr, x + 7, y - 4);
            cairo_stroke(cr);
        } else if (control.style == ControlStyle::Checkbox) {
            if (control.selected) {
                draw_icon(cr, Icon::Apply, r, legend, 28);
            }
        } else if (control.icon != Icon::None) {
            draw_icon(cr, control.icon, r, legend);
        } else {
            label(cr, control.label, r, view.settings ? 22 : 19, view.language->font, legend);
        }
        cairo_restore(cr);
    };
    for (const auto& control : view.controls) {
        if (control.style != ControlStyle::MenuItem && control.id != "scroll-menu") {
            draw_control(control);
        }
    }
    if (!view.status.empty()) {
        label(cr, view.status,
              view.settings ? Rect{30, 574, 1540, 24} : Rect{body.x + 18, 60, body.width - 36, 28}, 17,
              view.language->font, t.legend);
    }
    // Popup is the last layer and deliberately bypasses both card and case clips.
    if (view.popup) {
        const auto r = *view.popup;
        rounded(cr, {r.x - 2, r.y - 2, r.width + 4, r.height + 4}, 5);
        source(cr, t.legend);
        cairo_fill(cr);
        rounded(cr, r, 3);
        source(cr, t.bottom);
        cairo_fill(cr);
        for (const auto& control : view.controls) {
            if (control.style == ControlStyle::MenuItem || control.id == "scroll-menu") {
                draw_control(control);
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface_);
}
std::vector<unsigned char> PanelRenderer::rgba() const {
    const auto* bytes = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    std::vector<unsigned char> result(static_cast<std::size_t>(panel_width) * texture_height * 4);
    for (int y = 0; y < texture_height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(bytes + y * stride);
        for (int x = 0; x < panel_width; ++x) {
            const auto pixel = row[x];
            const unsigned alpha = pixel >> 24;
            const auto at = static_cast<std::size_t>(y * panel_width + x) * 4;
            // Compatibility uploads use straight RGBA; the native path skips this conversion.
            result[at] = static_cast<unsigned char>(
                alpha ? std::min(255u, ((pixel >> 16) & 255u) * 255u / alpha) : 0);
            result[at + 1] = static_cast<unsigned char>(
                alpha ? std::min(255u, ((pixel >> 8) & 255u) * 255u / alpha) : 0);
            result[at + 2] =
                static_cast<unsigned char>(alpha ? std::min(255u, (pixel & 255u) * 255u / alpha) : 0);
            result[at + 3] = static_cast<unsigned char>(alpha);
        }
    }
    return result;
}
void PanelRenderer::write_png(const fs::path& path) const {
    // Include overflow controls in previews instead of cropping the movement line.
    const bool popup = previous_ && previous_->view.popup.has_value();
    const auto handle = previous_ ? panel_move_handle_bounds(previous_->view) : std::nullopt;
    const int height = handle ? static_cast<int>(handle->y + handle->height + 12) : panel_height;
    auto* output = cairo_surface_create_for_rectangle(surface_, 0, popup ? 0 : popup_margin, panel_width,
                                                      popup ? texture_height : height);
    const auto status = cairo_surface_write_to_png(output, path.c_str());
    cairo_surface_destroy(output);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("cannot write preview PNG");
    }
}
} // namespace framekeyboard
