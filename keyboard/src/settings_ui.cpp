#include "framekeyboard/settings_ui.hpp"

#include <algorithm>
#include <cmath>

namespace framekeyboard {
namespace {
// Three production cards fit without a scrollbar for a two-pixel overflow.
constexpr double card_width = 509, card_gap = 18, row_height = 76, menu_row_height = 52;
constexpr int visible_choices = 5;
bool intersects(Rect a, Rect b) {
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}
} // namespace
void SettingsUi::set_cards(std::vector<SettingsCard> cards) {
    choice_remainder_ = 0;
    capture_.reset();
    cards_ = std::move(cards);
    horizontal_ = std::clamp(horizontal_, 0.0, horizontal_limit());
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        auto& offset = vertical_[cards_[i].id];
        offset = std::clamp(offset, 0.0, vertical_limit(i));
    }
    if (const auto* field = open_field(); field && !field->choices.empty()) {
        first_choice_ = std::clamp(
            first_choice_, 0, std::max(0, static_cast<int>(field->choices.size()) - visible_choices));
    } else {
        close_popup();
    }
}
Rect SettingsUi::card_bounds(std::size_t i) const {
    return {viewport.x + static_cast<double>(i) * (card_width + card_gap) - horizontal_, viewport.y,
            card_width, viewport.height};
}
Rect SettingsUi::body(std::size_t i) const {
    const auto r = card_bounds(i);
    return {std::max(r.x + 12, viewport.x), r.y + 52,
            std::max(0.0, std::min(r.x + r.width - 12, viewport.x + viewport.width) -
                              std::max(r.x + 12, viewport.x)),
            r.height - 68};
}
Rect SettingsUi::field_bounds(std::size_t i, std::size_t field) const {
    const auto r = card_bounds(i);
    if (cards_[i].fields[field].inline_label_width > 0) {
        return {r.x + 30 + cards_[i].fields[field].inline_label_width + 14,
                r.y + 68 + static_cast<double>(field) * row_height - vertical_.at(cards_[i].id), 56, 56};
    }
    return {r.x + 164, r.y + 68 + static_cast<double>(field) * row_height - vertical_.at(cards_[i].id),
            318, 56};
}
double SettingsUi::vertical_limit(std::size_t i) const {
    return std::max(0.0, 32 + static_cast<double>(cards_[i].fields.size()) * row_height -
                             (viewport.height - 68));
}
double SettingsUi::horizontal_limit() const {
    return std::max(0.0, static_cast<double>(cards_.size()) * (card_width + card_gap) - card_gap -
                             viewport.width);
}
const SettingField* SettingsUi::open_field() const {
    for (const auto& card : cards_) {
        for (const auto& field : card.fields) {
            if (field.id == open_field_) {
                return &field;
            }
        }
    }
    return nullptr;
}
std::optional<Rect> SettingsUi::popup_bounds() const {
    const auto* field = open_field();
    if (!field || field->choices.empty()) {
        return {};
    }
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        for (std::size_t j = 0; j < cards_[i].fields.size(); ++j) {
            if (cards_[i].fields[j].id == open_field_) {
                const auto r = field_bounds(i, j);
                const double height =
                    menu_row_height * std::min(visible_choices, static_cast<int>(field->choices.size()));
                double y = r.y + r.height + 4;
                if (y + height > panel_height + popup_margin - 4) {
                    y = r.y - height - 4;
                }
                return Rect{std::clamp(r.x, 8.0, panel_width - r.width - 8), y, r.width, height};
            }
        }
    }
    return {};
}
std::vector<SettingsUi::Scrollbar> SettingsUi::scrollbars() const {
    std::vector<Scrollbar> result;
    if (horizontal_limit() > 0) {
        const Rect track{viewport.x, viewport.y + viewport.height + 6, viewport.width, 10};
        const double length = track.width * track.width / (track.width + horizontal_limit());
        result.push_back({"scroll-cards",
                          track,
                          {track.x + horizontal_ / horizontal_limit() * (track.width - length), track.y,
                           length, track.height},
                          horizontal_limit(),
                          horizontal_,
                          0,
                          true});
    }
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        if (vertical_limit(i) == 0) {
            continue;
        }
        const auto r = card_bounds(i);
        const Rect track{r.x + r.width - 10, r.y + 52, 6, r.height - 68};
        if (!viewport.contains(track.x, track.y) ||
            !viewport.contains(track.x + track.width - 1, track.y)) {
            continue;
        }
        const auto offset = vertical_.at(cards_[i].id);
        const double length = track.height * track.height / (track.height + vertical_limit(i));
        result.push_back({"scroll-" + cards_[i].id,
                          track,
                          {track.x, track.y + offset / vertical_limit(i) * (track.height - length),
                           track.width, length},
                          vertical_limit(i),
                          offset,
                          i,
                          false});
    }
    const auto* field = open_field();
    const auto popup = popup_bounds();
    if (field && popup && field->choices.size() > visible_choices) {
        const Rect track{popup->x + popup->width - 8, popup->y, 6, popup->height};
        const double limit = static_cast<double>(field->choices.size() - visible_choices);
        const double length =
            track.height * visible_choices / static_cast<double>(field->choices.size());
        result.push_back(
            {"scroll-menu",
             track,
             {track.x, track.y + first_choice_ / limit * (track.height - length), track.width, length},
             limit,
             static_cast<double>(first_choice_),
             0,
             false});
    }
    return result;
}
void SettingsUi::append(PanelView& view) const {
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const auto r = card_bounds(i);
        if (!intersects(r, viewport)) {
            continue;
        }
        view.cards.push_back({cards_[i].title, r});
        const auto clip = body(i);
        for (std::size_t j = 0; j < cards_[i].fields.size(); ++j) {
            const auto& field = cards_[i].fields[j];
            auto bounds = field_bounds(i, j);
            if (field.style == ControlStyle::Checkbox) {
                bounds.width = bounds.height;
            }
            const Rect text =
                field.inline_label_width > 0
                    ? Rect{r.x + 30, bounds.y, field.inline_label_width, bounds.height}
                : field.label_after
                    ? Rect{bounds.x + bounds.width + 14, bounds.y,
                           r.x + r.width - 12 - (bounds.x + bounds.width + 14), bounds.height}
                    : Rect{r.x + 12, bounds.y, 142, bounds.height};
            if (intersects(text, clip)) {
                view.field_labels.push_back({field.label, text, clip,
                                             field.label_after || field.inline_label_width > 0,
                                             field.enabled});
            }
            if (intersects(bounds, clip)) {
                const auto selected =
                    std::find_if(field.choices.begin(), field.choices.end(),
                                 [&](const SettingChoice& choice) { return choice.id == field.value; });
                const auto name = selected == field.choices.end() ? field.value : selected->name;
                view.controls.push_back({field.id, name, bounds, field.checked, Icon::None, field.style,
                                         clip, field.enabled});
                view.controls.back().action =
                    field.style == ControlStyle::Dropdown
                        ? ControlAction{ControlKind::ToggleDropdown, 0, field.id}
                        : field.action;
            }
        }
    }
    view.popup = popup_bounds();
    if (const auto* field = open_field(); view.popup && field) {
        const int count = static_cast<int>(field->choices.size());
        for (int i = first_choice_; i < std::min(first_choice_ + visible_choices, count); ++i) {
            const auto& choice = field->choices[static_cast<std::size_t>(i)];
            view.controls.push_back(
                {"choose:" + field->id + ":" + choice.id,
                 choice.name,
                 {view.popup->x, view.popup->y + (i - first_choice_) * menu_row_height,
                  view.popup->width - 10, menu_row_height},
                 choice.id == field->value,
                 Icon::None,
                 ControlStyle::MenuItem});
            view.controls.back().action = field->action;
            view.controls.back().action.value = choice.id;
        }
    }
    for (const auto& bar : scrollbars()) {
        view.controls.push_back({bar.id, "", bar.thumb, false, Icon::None, ControlStyle::Scrollbar});
        if (bar.id == "scroll-menu") {
            view.controls.back().action.kind = ControlKind::ScrollMenu;
        }
    }
}
void SettingsUi::close_popup() {
    if (capture_ && capture_->bar.id == "scroll-menu") {
        capture_.reset();
    }
    open_field_.clear();
    first_choice_ = 0;
    choice_remainder_ = 0;
}
void SettingsUi::reset() {
    cancel();
    horizontal_ = 0;
    for (auto& [id, offset] : vertical_) {
        offset = 0;
    }
}
void SettingsUi::toggle(const std::string& id) {
    if (open_field_ == id) {
        close_popup();
        return;
    }
    close_popup();
    open_field_ = id;
    if (const auto* field = open_field(); field && !field->choices.empty()) {
        const auto it =
            std::find_if(field->choices.begin(), field->choices.end(),
                         [&](const SettingChoice& choice) { return choice.id == field->value; });
        const int selected = static_cast<int>(it - field->choices.begin());
        first_choice_ = std::clamp(
            selected, 0, std::max(0, static_cast<int>(field->choices.size()) - visible_choices));
    } else {
        close_popup();
    }
}
bool SettingsUi::scroll(double x, double y, double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return false;
    }
    if (popup_open()) {
        if (const auto popup = popup_bounds(); popup && popup->contains(x, y)) {
            const auto* field = open_field();
            const int limit = std::max(0, static_cast<int>(field->choices.size()) - visible_choices);
            const int old = first_choice_;
            choice_remainder_ += std::clamp(dy, -static_cast<double>(limit), static_cast<double>(limit));
            // Keep sub-row motion instead of rounding every event separately.
            // The tolerance handles sums such as ten binary floating-point .1s.
            const int steps =
                static_cast<int>(std::trunc(choice_remainder_ + std::copysign(1e-9, choice_remainder_)));
            first_choice_ = std::clamp(first_choice_ + steps, 0, limit);
            choice_remainder_ -= steps;
            if (std::abs(choice_remainder_) < 1e-9 || (first_choice_ == 0 && choice_remainder_ < 0) ||
                (first_choice_ == limit && choice_remainder_ > 0)) {
                choice_remainder_ = 0; // Discard overscroll so reversing responds immediately.
            }
            return first_choice_ != old;
        }
        return false; // A popup owns scrolling until selection or dismissal.
    }
    if (!viewport.contains(x, y) &&
        !Rect{viewport.x, viewport.y + viewport.height, viewport.width, 18}.contains(x, y)) {
        return false;
    }
    if (dx == 0) {
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            if (card_bounds(i).contains(x, y) && vertical_limit(i) > 0) {
                auto& offset = vertical_[cards_[i].id];
                const double old = offset;
                offset = std::clamp(offset + dy * 52, 0.0, vertical_limit(i));
                return offset != old;
            }
        }
    }
    const double old = horizontal_;
    horizontal_ = std::clamp(horizontal_ + (dx != 0 ? dx : dy) * 100, 0.0, horizontal_limit());
    return horizontal_ != old;
}
bool SettingsUi::down(unsigned pointer, double x, double y) {
    for (const auto& bar : scrollbars()) {
        if ((!popup_open() || bar.id == "scroll-menu") && bar.track.contains(x, y)) {
            if (!capture_) {
                if (bar.id == "scroll-menu") {
                    choice_remainder_ = 0;
                }
                capture_ = Capture{pointer, bar, bar.horizontal ? x : y, bar.offset};
                if (!bar.thumb.contains(x, y)) {
                    // Clicking the track centers the thumb at the laser/mouse.
                    capture_->start = (bar.horizontal ? bar.thumb.x + bar.thumb.width / 2
                                                      : bar.thumb.y + bar.thumb.height / 2);
                    move(pointer, x, y);
                }
            }
            return true;
        }
    }
    return false;
}
bool SettingsUi::move(unsigned pointer, double x, double y) {
    if (!capture_ || capture_->pointer != pointer) {
        return false;
    }
    const auto& bar = capture_->bar;
    const double travel =
        bar.horizontal ? bar.track.width - bar.thumb.width : bar.track.height - bar.thumb.height;
    const double offset =
        std::clamp(capture_->offset + ((bar.horizontal ? x : y) - capture_->start) * bar.limit / travel,
                   0.0, bar.limit);
    if (bar.id == "scroll-menu") {
        choice_remainder_ = 0;
        first_choice_ = static_cast<int>(std::round(offset));
    } else if (bar.horizontal) {
        horizontal_ = offset;
    } else {
        vertical_[cards_[bar.card].id] = offset;
    }
    return true;
}
bool SettingsUi::up(unsigned pointer) {
    if (capture_ && capture_->pointer == pointer) {
        capture_.reset();
        return true;
    }
    return false;
}
void SettingsUi::cancel_pointer(unsigned pointer) {
    up(pointer);
}
void SettingsUi::cancel() {
    capture_.reset();
    close_popup();
}
} // namespace framekeyboard
