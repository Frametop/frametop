#pragma once

#include "panel.hpp"

namespace framekeyboard {
struct SettingChoice {
    std::string id, name;
};
struct SettingField {
    std::string id, label, value;
    ControlStyle style{ControlStyle::Dropdown};
    bool checked{};
    std::vector<SettingChoice> choices{};
    bool label_after{};
    // An inline label has a small row inset and precedes its checkbox.
    double inline_label_width{};
    bool enabled{true};
    // Dropdowns use this kind for choosing a value; their row opens generically.
    ControlAction action{};
};
struct SettingsCard {
    std::string id, title;
    std::vector<SettingField> fields;
};
// Generic card/scroll state. App supplies the real fields; tests can supply extra
// cards and rows without adding fake preferences to production configuration.
class SettingsUi {
  public:
    static constexpr Rect viewport{18, 78, 1564, 476};
    void set_cards(std::vector<SettingsCard> cards);
    void append(PanelView& view) const;
    void reset();
    void close_popup();
    bool popup_open() const { return !open_field_.empty(); }
    void toggle(const std::string& field);
    bool scroll(double x, double y, double dx, double dy);
    bool down(unsigned pointer, double x, double y);
    bool move(unsigned pointer, double x, double y);
    bool up(unsigned pointer);
    bool pressed(unsigned pointer) const { return capture_ && capture_->pointer == pointer; }
    void cancel_pointer(unsigned pointer);
    void cancel();

  private:
    Rect card_bounds(std::size_t index) const;
    Rect body(std::size_t index) const;
    Rect field_bounds(std::size_t card, std::size_t field) const;
    double vertical_limit(std::size_t card) const;
    double horizontal_limit() const;
    const SettingField* open_field() const;
    std::optional<Rect> popup_bounds() const;
    struct Scrollbar {
        std::string id;
        Rect track, thumb;
        double limit{}, offset{};
        std::size_t card{};
        bool horizontal{};
    };
    std::vector<Scrollbar> scrollbars() const;
    std::vector<SettingsCard> cards_;
    std::map<std::string, double> vertical_;
    double horizontal_{};
    std::string open_field_;
    int first_choice_{};
    double choice_remainder_{};
    struct Capture {
        unsigned pointer;
        Scrollbar bar;
        double start{}, offset{};
    };
    std::optional<Capture> capture_;
};
} // namespace framekeyboard
