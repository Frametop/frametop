#pragma once

#include "cjk.hpp"
#include "instance.hpp"
#include "japanese.hpp"
#include "output.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include "settings_ui.hpp"
#include "settings_writer.hpp"
#include <atomic>
#include <csignal>
#include <optional>
#include <utility>

namespace framekeyboard {
class NativeReplacement;
class Updates;
enum class Mode { Help, Preview, Vr, Render, RenderSettings, Check, Describe };
enum class InputBackend { None, Ei, Uinput };
struct Options {
    fs::path data_dir, config_dir, ei_socket, text_socket;
    Mode mode{Mode::Help};
    InputBackend input{InputBackend::None};
    std::string output, target_language;
    double duration{};
    bool start_enabled{false};
    bool autostart{};
    bool background_update{};
};

class App {
  public:
    App(const Options& options, KeySink& sink, SettingsWriter::Write write_preferences = {},
        Updates* updates = nullptr, SettingsWriter::Write reconcile_preferences = {});
    PanelView view() const;
    // True only for a newly accepted keyboard key, so VR can provide haptics.
    bool down(unsigned pointer, double x, double y, double now);
    // True when an unpressed pointer enters a different keyboard key.
    bool move(unsigned pointer, double x, double y);
    // True when this pointer releases a captured keyboard key, even outside it.
    bool up(unsigned pointer, double x, double y);
    bool pointer_pressed(unsigned pointer) const {
        return keyboard_.pointer_pressed(pointer) || pressed_controls_.contains(pointer) ||
               settings_ui_.pressed(pointer);
    }
    void cancel_pointer(unsigned pointer);
    void cancel(bool discard_composition = true);
    bool tick(double now);
    void paint(double now);
    void show_settings();
    void back();
    bool scroll(unsigned pointer, double dx, double dy);
    bool pinned() const { return settings_.pinned; }
    bool use_drag_to_move() const { return settings_.use_drag_to_move; }
    bool auto_leveling() const { return settings_.auto_leveling; }
    bool replace_native_keyboard() const { return settings_.replace_native_keyboard; }
    bool start_with_steamos() const {
        return settings_.replace_native_keyboard && settings_.start_with_steamos;
    }
    bool background_start() const {
        return options_.autostart || (options_.background_update && settings_.replace_native_keyboard);
    }
    bool replacement_ready() const;
    bool take_native_fallback();
    bool resident_replacement() const {
        return options_.mode == Mode::Vr && settings_.replace_native_keyboard;
    }
    void close_panel();
    bool take_dismissal();
    void set_replacement_available(bool available, std::string reason = {}) {
        if (available) {
            reason.clear();
        }
        dirty |= replacement_available_ != available || replacement_unavailable_reason_ != reason;
        replacement_available_ = available;
        replacement_unavailable_reason_ = std::move(reason);
    }
    void set_native_session(bool open) {
        dirty |= native_session_ != open;
        native_session_ = open;
    }
    bool allows_movement(MoveSource source) const {
        return !pinned() && use_drag_to_move() == (source == MoveSource::Grip);
    }
    void summon();
    void set_dragging(bool dragging);
    // Hiding the VR context releases keys before closing the input gate.
    void set_interaction_active(bool active);
    void report_status(const std::string& message);
    std::vector<PlacementAction> take_placement_actions();
    void apply(Selection selection);
    // Reload immediately when storage is idle, otherwise coalesce the request
    // and complete it from tick() after the newest pending snapshot is saved.
    void reload();
    void flush_settings();
    // Only explicit profile/storage work gets extra time. The deadline stays
    // fixed until a complete VR frame, even across nested or repeated work.
    double slow_work_deadline() const { return slow_work_deadline_.load(); }
    void frame_completed() { slow_work_deadline_.store(0); }
    const fs::path& config_dir() const { return options_.config_dir; }
    bool quitting() const { return quit_; }
    bool updating() const;
    bool take_recenter();
    bool dirty{true};
    PanelRenderer renderer;
    const Selection& selection() const { return settings_.active; }

  private:
    std::vector<Control> controls() const;
    void update_settings_ui();
    void activate(Selection selection);
    void save();
    void update_save_status();
    void allow_slow_work();
    void rebuild_cycle_entries();
    void action(const ControlAction& action);
    void refresh_typing();
    bool japanese_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_japanese();
    void convert_japanese();
    bool japanese() const;
    bool cjk_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_cjk();
    void choose_cjk(int index);
    bool unicode_mode() const;
    bool text_key(const Key& key, const std::set<int>& mods) const;
    int native_code(const Key& key, const std::set<int>& mods);
    void update_output_status();
    bool retry_output();
    Updates* updates_{};
    Options options_;
    Profiles profiles_;
    Settings settings_;
    SettingsWriter settings_writer_;
    std::string save_status_;
    bool save_status_is_startup_{};
    bool reload_requested_{};
    Selection pending_, default_;
    // Only explicit Apply changes the saved choice. Startup fallback and
    // favorite cycling affect the runtime selection/default independently.
    Selection persisted_active_;
    std::string startup_status_;
    std::vector<Selection> cycle_entries_;
    std::vector<Favorite> pending_favorites_;
    bool pending_hide_numpad_{};
    bool pending_use_drag_to_move_{};
    bool pending_auto_leveling_{true};
    bool pending_replace_native_keyboard_{};
    bool pending_start_with_steamos_{};
    bool native_fallback_{};
    bool replacement_available_{};
    std::string replacement_unavailable_reason_;
    bool native_session_{};
    bool dismiss_requested_{};
    SettingsUi settings_ui_;
    std::unique_ptr<LanguageMap> keymap_;
    // The launch declaration is frozen; Reload cannot redefine the target.
    std::optional<Language> target_language_;
    OutputController output_;
    KeyboardState keyboard_;
    JapaneseComposer composition_;
    std::unique_ptr<CjkComposer> cjk_;
    bool cjk_latin_{};
    bool japanese_latin_{};
    bool text_ready_{};
    bool typing_ready_{};
    struct HoverTarget {
        enum class Kind { None, Key, Control } kind{Kind::None};
        std::string id;
        bool operator==(const HoverTarget&) const = default;
    };
    std::map<unsigned, HoverTarget> hovered_;
    struct PressedControl {
        std::string id;
        ControlAction action;
    };
    std::map<unsigned, PressedControl> pressed_controls_;
    std::string status_;
    bool settings_open_{false}, quit_{false}, recenter_{false};
    std::vector<PlacementAction> placement_actions_;
    std::map<unsigned, std::pair<double, double>> pointer_positions_;
    bool dragging_{};
    bool interaction_active_{true};
    bool backend_ready_{true}, backend_can_resume_{};
    std::optional<bool> native_num_requested_;
    std::atomic<double> slow_work_deadline_{};
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, NativeReplacement& replacement, double duration);
} // namespace framekeyboard
