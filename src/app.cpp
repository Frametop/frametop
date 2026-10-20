#include "framekeyboard/app.hpp"
#include "framekeyboard/updates.hpp"
#include <utility>

#include <algorithm>
#include <chrono>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <tuple>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
bool App::updating() const {
    return updates_ && updates_->prepared();
}
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
bool same_keymap(const Language& a, const Language& b) {
    return std::tie(a.rules, a.model, a.keymap, a.variant, a.options) ==
           std::tie(b.rules, b.model, b.keymap, b.variant, b.options);
}
std::unique_ptr<CjkComposer> prepare_cjk(const Language& language) {
    if (language.input_method == "korean-2set" || language.input_method.starts_with("chinese-")) {
        return std::make_unique<CjkComposer>(language.input_method);
    }
    return {};
}
std::string compatible_layout(const Profiles& profiles, const Selection& selection) {
    try {
        validate_selection(profiles, selection);
        return selection.layout;
    } catch (const std::exception&) {
        // Prefer the smallest complete geometry, not alphabetical catalog order.
        const Layout* best = nullptr;
        for (const auto& [id, layout] : profiles.layouts) {
            try {
                validate_selection(profiles, {id, selection.language, selection.theme});
                if (!best || layout.keys.size() < best->keys.size()) {
                    best = &layout;
                }
            } catch (const std::exception&) {
            }
        }
        if (best) {
            return best->id;
        }
    }
    return selection.layout; // Apply reports the validation error if none fit.
}
bool same_pair(const Selection& a, const Selection& b) {
    return a.layout == b.layout && a.language == b.language;
}
std::string language_short_name(const Language& language) {
    // Locale preserves distinctions such as US/UK English and Chinese script.
    auto name = language.locale.empty() ? language.id : language.locale;
    std::replace(name.begin(), name.end(), '_', '-');
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return g_ascii_toupper(c); });
    return name;
}
} // namespace
App::App(const Options& options, KeySink& sink, SettingsWriter::Write write_preferences,
         Updates* updates, SettingsWriter::Write reconcile_preferences)
    : updates_(updates), options_(options),
      profiles_(load_profiles(options.data_dir, options.config_dir)),
      settings_writer_(options.config_dir, std::move(write_preferences),
                       std::move(reconcile_preferences)),
      output_(sink), keyboard_(output_) {
    std::vector<std::string> settings_errors;
    settings_ = load_settings(options.config_dir, settings_errors);
    profiles_.errors.insert(profiles_.errors.end(), settings_errors.begin(), settings_errors.end());
    persisted_active_ = settings_.active;
    auto fallback_status = [&](const std::string& message) {
        profiles_.errors.push_back(message);
        if (!startup_status_.empty()) {
            startup_status_ += " ";
        }
        startup_status_ += message;
    };
    if (!profiles_.themes.contains(settings_.active.theme)) {
        fallback_status("Saved theme unavailable. Using Graphite.");
        settings_.active.theme = "graphite";
    }
    if (!profiles_.languages.contains(settings_.active.language)) {
        fallback_status("Saved language unavailable. Using English.");
        settings_.active.language = "en-us";
    }
    try {
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
        cjk_ = prepare_cjk(profiles_.languages.at(settings_.active.language));
    } catch (const std::exception& error) {
        fallback_status("Saved language could not be prepared. Using English. " +
                        std::string(error.what()));
        settings_.active.language = "en-us";
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at("en-us"));
    }
    const auto layout = compatible_layout(profiles_, settings_.active);
    if (layout != settings_.active.layout) {
        fallback_status("Saved layout unavailable or incompatible. Using " + layout + ".");
        settings_.active.layout = layout;
    }
    try {
        validate_selection_profiles(profiles_, settings_.active);
    } catch (const std::exception& error) {
        profiles_.errors.push_back(error.what());
        // Even the default IDs can have user overrides. Restore only their
        // embedded models as a last resort, keeping other profiles and the
        // persisted choice available for repair in Settings.
        settings_.active = {};
        const auto defaults = load_profiles({}, {});
        profiles_.layouts[settings_.active.layout] = defaults.layouts.at(settings_.active.layout);
        profiles_.languages[settings_.active.language] =
            defaults.languages.at(settings_.active.language);
        profiles_.themes[settings_.active.theme] = defaults.themes.at(settings_.active.theme);
        keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
        cjk_.reset();
        startup_status_ = "Saved selection has no compatible layout. Using English defaults.";
    }
    keyboard_.set_num(settings_.num_lock);
    pending_ = default_ = settings_.active;
    rebuild_cycle_entries();
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    update_settings_ui();
    status_ = !startup_status_.empty()   ? startup_status_
              : profiles_.errors.empty() ? ""
                                         : profiles_.errors.front();
    if (const auto target = profiles_.languages.find(options_.target_language);
        target != profiles_.languages.end()) {
        target_language_ = target->second;
    }
    // A saved language mismatch must leave Settings reachable, not abort launch.
    refresh_typing();
    // Refresh owned units after an update without requiring Apply or Reload.
    // Invalid settings must not remove an existing startup configuration.
    if (options.mode == Mode::Vr && settings_errors.empty()) {
        settings_writer_.reconcile(settings_);
    }
}
std::vector<Control> App::controls() const {
    std::vector<Control> result;
    auto add = [&](ControlAction action, Control control) {
        control.action = std::move(action);
        result.push_back(std::move(control));
    };
    PanelView geometry;
    geometry.layout = &profiles_.layouts.at(settings_.active.layout);
    geometry.theme = &profiles_.themes.at(settings_.active.theme);
    geometry.language = &profiles_.languages.at(settings_.active.language);
    geometry.hide_numpad = settings_.hide_numpad;
    geometry.settings = settings_open_;
    const auto body = panel_case_bounds(geometry);
    auto append_move_handle = [&] {
        if (!settings_.pinned && !settings_.use_drag_to_move) {
            add({ControlKind::MoveHandle}, {"move-handle",
                                            "Move keyboard",
                                            {body.x + body.width / 2 - 220, panel_height + 12, 440, 60},
                                            dragging_,
                                            Icon::None,
                                            ControlStyle::MoveHandle});
        }
    };
    if (settings_open_) {
        add({ControlKind::Settings}, {"settings", "Back (Esc)", {18, 12, 60, 42}, false, Icon::Back});
        add({ControlKind::Apply}, {"apply", "Apply and save", {88, 12, 60, 42}, false, Icon::Apply});
        add({ControlKind::Reload}, {"reload", "Reload config", {158, 12, 60, 42}, false, Icon::Reload});
        add({ControlKind::Updates},
            {"updates", updates_ ? updates_->label() : "Check for updates", {550, 12, 500, 42}});
        if (settings_.replace_native_keyboard && native_session_) {
            add({ControlKind::NativeFallback},
                {"native-fallback", "Use Steam keyboard", {250, 12, 245, 42}});
        }
        if (resident_replacement()) {
            add({ControlKind::Quit},
                {"quit", "Quit Full Keyboard", {body.x + body.width - 268, 12, 250, 42}});
        }
        append_move_handle();
        return result;
    }
    const double x = body.x + 18;
    add({ControlKind::Settings}, {"settings", "Settings", {x, 12, 60, 42}, false, Icon::Settings});
    add({ControlKind::Recenter}, {"recenter", "Recenter", {x + 70, 12, 60, 42}, false, Icon::Recenter});
    add({ControlKind::Smaller},
        {"size-smaller", "Smaller keyboard", {x + 140, 12, 60, 42}, false, Icon::ScaleDown});
    add({ControlKind::Larger},
        {"size-larger", "Larger keyboard", {x + 210, 12, 60, 42}, false, Icon::ScaleUp});
    add({ControlKind::Pin}, {"pin",
                             settings_.pinned ? "Unpin keyboard" : "Pin keyboard",
                             {x + 280, 12, 60, 42},
                             false,
                             settings_.pinned ? Icon::PinOn : Icon::PinOff});
    if (!settings_.favorites.empty()) {
        add({ControlKind::LanguageCycle},
            {"language-cycle",
             language_short_name(profiles_.languages.at(settings_.active.language)),
             {x + 350, 12, 100, 42}});
    }
    add({ControlKind::Close}, {"close",
                               resident_replacement() ? "Hide keyboard" : "Close",
                               {body.x + body.width - 78, 12, 60, 42},
                               false,
                               Icon::Close});
    const auto ime_start = result.size();
    if (japanese()) {
        add({ControlKind::ImeToggle},
            {"ime-toggle", japanese_latin_ ? "A / あ" : "あ / A", {610, 12, 115, 42}});
        if (!japanese_latin_) {
            add({ControlKind::ImeHiragana}, {"ime-hiragana", "ひらがな", {735, 12, 120, 42}});
            add({ControlKind::ImeKatakana}, {"ime-katakana", "カタカナ", {865, 12, 120, 42}});
            add({ControlKind::ImeCommit}, {"ime-commit", "確定 / Commit", {995, 12, 180, 42}});
            add({ControlKind::ImeCancel}, {"ime-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = composition_.candidates();
            if (!candidates.empty()) {
                add({ControlKind::ImePrev}, {"ime-prev", "↑", {20, 148, 55, 42}});
                const int page = composition_.selected() / 5 * 5;
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    add({ControlKind::ImeCandidate, i},
                        {"ime-candidate-" + std::to_string(i),
                         std::to_string(i + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == composition_.selected()});
                }
                add({ControlKind::ImeNext}, {"ime-next", "↓", {1290, 148, 55, 42}});
                add({ControlKind::ImeSegmentPrev}, {"ime-segment-prev", "←", {1355, 148, 55, 42}});
                add({ControlKind::ImeSegmentNext}, {"ime-segment-next", "→", {1420, 148, 55, 42}});
                add({ControlKind::None}, {"ime-segment-label",
                                          std::to_string(composition_.active_segment() + 1) + "/" +
                                              std::to_string(composition_.segment_count()),
                                          {1485, 148, 90, 42}});
            } else {
                add({ControlKind::ImeConvert}, {"ime-convert", "変換 / Convert", {20, 148, 230, 42}});
            }
        }
    }
    if (cjk_) {
        add({ControlKind::CjkToggle},
            {"cjk-toggle",
             cjk_latin_ ? "A / native" : (cjk_->chinese() ? "中文 / A" : "한 / A"),
             {610, 12, 150, 42}});
        if (!cjk_latin_) {
            add({ControlKind::CjkCommit}, {"cjk-commit", "Commit", {995, 12, 180, 42}});
            add({ControlKind::CjkCancel}, {"cjk-cancel", "Cancel", {1185, 12, 110, 42}});
            const auto candidates = cjk_->candidates();
            if (!candidates.empty()) {
                const int page = cjk_->selected() / 5 * 5;
                add({ControlKind::CjkPrev}, {"cjk-prev", "↑", {20, 148, 55, 42}});
                for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size())); ++i) {
                    add({ControlKind::CjkCandidate, i},
                        {"cjk-candidate-" + std::to_string(i),
                         std::to_string(i - page + 1) + " " + candidates[static_cast<std::size_t>(i)],
                         {85.0 + (i - page) * 240, 148, 230, 42},
                         i == cjk_->selected()});
                }
                add({ControlKind::CjkNext}, {"cjk-next", "↓", {1290, 148, 55, 42}});
            }
        }
    }
    // Composition controls use the available case width; the main toolbar keeps
    // its button sizes and order when the numpad is hidden.
    for (std::size_t i = ime_start; i < result.size(); ++i) {
        result[i].bounds.x = body.x + result[i].bounds.x * body.width / panel_width;
        result[i].bounds.width *= body.width / panel_width;
    }
    append_move_handle();
    return result;
}
void App::update_settings_ui() {
    auto field = [](ControlKind kind, SettingField value) {
        value.action.kind = kind;
        return value;
    };
    auto choices = [](const auto& profiles) {
        std::vector<SettingChoice> result;
        for (const auto& [id, profile] : profiles) {
            result.push_back({id, profile.name});
        }
        return result;
    };
    const bool favorite =
        std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                    [&](const Favorite& f) { return same_pair(f.selection, pending_); });
    settings_ui_.set_cards(
        {{"languages",
          "Languages and Layouts",
          {field(ControlKind::ChooseLayout, {"layout", "Layout:", pending_.layout,
                                             ControlStyle::Dropdown, false, choices(profiles_.layouts)}),
           field(ControlKind::ChooseLanguage,
                 {"language", "Language:", pending_.language, ControlStyle::Dropdown, false,
                  choices(profiles_.languages)}),
           field(ControlKind::Favorite,
                 {"favorite", "Favorite:", "", ControlStyle::Checkbox, favorite, {}})}},
         {"general",
          "General",
          {field(ControlKind::ChooseTheme, {"theme", "Theme:", pending_.theme, ControlStyle::Dropdown,
                                            false, choices(profiles_.themes)}),
           field(ControlKind::HideNumpad, {"hide-numpad",
                                           "Hide numpad",
                                           "",
                                           ControlStyle::Checkbox,
                                           pending_hide_numpad_,
                                           {},
                                           true}),
           field(ControlKind::UseDragToMove, {"use-drag-to-move",
                                              "Use Drag to Move",
                                              "",
                                              ControlStyle::Checkbox,
                                              pending_use_drag_to_move_,
                                              {},
                                              true}),
           field(ControlKind::AutoLeveling, {"auto-leveling",
                                             "Auto leveling",
                                             "",
                                             ControlStyle::Checkbox,
                                             pending_auto_leveling_,
                                             {},
                                             true})}},
         {"experimental",
          "Experimental",
          {field(ControlKind::ReplaceNativeKeyboard, {"replace-native-keyboard",
                                                      "Replace native keyboard:",
                                                      "",
                                                      ControlStyle::Checkbox,
                                                      pending_replace_native_keyboard_,
                                                      {},
                                                      false,
                                                      300}),
           field(ControlKind::StartWithSteamos,
                 {"start-with-steamos",
                  "Start with SteamOS",
                  "",
                  ControlStyle::Checkbox,
                  pending_start_with_steamos_ && pending_replace_native_keyboard_,
                  {},
                  false,
                  300,
                  pending_replace_native_keyboard_})}}});
}
PanelView App::view() const {
    PanelView v;
    v.layout = &profiles_.layouts.at(settings_.active.layout);
    v.theme = &profiles_.themes.at(settings_.active.theme);
    v.language = &profiles_.languages.at(settings_.active.language);
    v.keymap = keymap_.get();
    v.keyboard = &keyboard_;
    v.hide_numpad = settings_.hide_numpad;
    v.controls = controls();
    if (settings_open_) {
        settings_ui_.append(v);
    }
    v.status = dragging_ ? (settings_.use_drag_to_move ? "Release grab to place keyboard"
                                                       : "Release trigger to place keyboard")
                         : (status_.empty() ? startup_status_ : status_);
    if (settings_open_ && !dragging_ && updates_ && !updates_->failure_reason().empty()) {
        v.status = updates_->failure_reason();
    }
    // Image exports show the layout without the interactive preview's input notice.
    const bool image_export = options_.mode == Mode::Render || options_.mode == Mode::RenderSettings;
    if (v.status.empty() && !typing_ready_ && !image_export) {
        v.status = options_.input == InputBackend::None
                       ? "Preview only: this launch cannot type."
                       : "Typing disabled: choose a profile matching the target keymap used at launch.";
    }
    v.settings = settings_open_;
    if (settings_open_ && settings_.replace_native_keyboard && !replacement_available_ &&
        v.status.empty()) {
        v.status = replacement_unavailable_reason_.empty() ? "Native replacement unavailable. "
                                                           : replacement_unavailable_reason_ + " ";
        v.status += "Steam keyboard remains available.";
    }
    v.composing = japanese() && !japanese_latin_;
    if (v.composing) {
        v.preedit = composition_.preedit(true);
        if (v.language->input_method == "japanese-kana") {
            const auto mods = keyboard_.modifiers();
            const bool shifted =
                mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
            for (const auto& key : v.layout->keys) {
                const auto& map = shifted && v.language->kana_shift.contains(key.action)
                                      ? v.language->kana_shift
                                      : v.language->kana;
                if (auto it = map.find(key.action); it != map.end()) {
                    v.key_labels[key.id] = it->second;
                }
            }
        }
    }
    if (cjk_ && !cjk_latin_) {
        v.composing = true;
        v.preedit = cjk_->preedit();
        const auto mods = keyboard_.modifiers();
        const bool shifted =
            mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
        for (const auto& key : v.layout->keys) {
            const auto& map = shifted && v.language->composition_shift.contains(key.action)
                                  ? v.language->composition_shift
                                  : v.language->composition_keys;
            if (const auto it = map.find(key.action); it != map.end()) {
                v.key_labels[key.id] = it->second;
            }
        }
    }
    if (!backend_ready_) {
        v.status = backend_can_resume_ ? "Keyboard paused or disconnected. Waiting for the compositor."
                   : resident_replacement()
                       ? "Keyboard connection lost. Use Settings > Quit Full Keyboard, then relaunch."
                       : "Keyboard connection lost. Reopen the app to reconnect.";
    } else if ((unicode_mode() || japanese() || cjk_) && options_.input != InputBackend::None &&
               !text_ready_) {
        v.status = "Text delivery unavailable. Waiting for the compositor connection to recover.";
    }
    for (const auto& [pointer, target] : hovered_) {
        (void)pointer;
        if (target.kind == HoverTarget::Kind::Key) {
            v.hovered_keys.insert(target.id);
        } else if (target.kind == HoverTarget::Kind::Control) {
            v.hovered_controls.insert(target.id);
        }
    }
    return v;
}
bool App::scroll(unsigned pointer, double dx, double dy) {
    if (!settings_open_ || !interaction_active_ || dragging_ || !pointer_positions_.contains(pointer)) {
        return false;
    }
    const auto [x, y] = pointer_positions_.at(pointer);
    if (!settings_ui_.scroll(x, y, dx, dy)) {
        return false;
    }
    // A scroll must never put a different action under a held trigger.
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
    return true;
}
bool App::move(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    pointer_positions_[pointer] = {x, y};
    if (settings_open_ && settings_ui_.move(pointer, x, y)) {
        pressed_controls_.clear();
        hovered_.clear();
        dirty = true;
        return false;
    }
    const auto v = view();
    HoverTarget target;
    for (auto it = v.controls.rbegin(); it != v.controls.rend(); ++it) {
        if (v.popup && it->style != ControlStyle::MenuItem &&
            it->action.kind != ControlKind::ScrollMenu) {
            continue;
        }
        if (it->hit(x, y)) {
            target = {HoverTarget::Kind::Control, it->id};
            break;
        }
    }
    if (target.kind == HoverTarget::Kind::None) {
        if (const auto* key = renderer.hit_key(v, x, y)) {
            target = {HoverTarget::Kind::Key, key->id};
        }
    }
    if (hovered_[pointer] != target) {
        hovered_[pointer] = target;
        dirty = true;
        return target.kind == HoverTarget::Kind::Key && !dragging_ &&
               !keyboard_.pointer_pressed(pointer);
    }
    return false;
}

bool App::down(unsigned pointer, double x, double y, double now) {
    if (dragging_ || !interaction_active_) {
        return false;
    }
    move(pointer, x, y);
    const auto v = view();
    if (settings_open_ && settings_ui_.down(pointer, x, y)) {
        pressed_controls_.clear();
        dirty = true;
        return false;
    }
    if (v.popup && !v.popup->contains(x, y)) {
        settings_ui_.close_popup();
        pressed_controls_.clear();
        hovered_.clear();
        dirty = true;
        return false; // Dismissal consumes the click instead of hitting through.
    }
    if (hovered_[pointer].kind == HoverTarget::Kind::Control) {
        const auto control =
            std::find_if(v.controls.rbegin(), v.controls.rend(),
                         [&](const Control& item) { return item.id == hovered_[pointer].id; });
        if (control != v.controls.rend()) {
            pressed_controls_[pointer] = {control->id, control->action};
        }
        return false;
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        const auto modifiers = keyboard_.modifiers();
        const bool ime_local = japanese_key(*key, false, modifiers) || cjk_key(*key, false, modifiers);
        const bool text_local = !ime_local && text_key(*key, modifiers);
        const bool sends_text = text_local && key->action != "CapsLock" && key->action != "NumLock";
        const bool cancel_accent = unicode_mode() && keymap_->composing() &&
                                   (key->action == "Escape" || key->action == "Backspace");
        const bool retry_text = output_.retry_available() && key->action == "Enter";
        const bool local = ime_local || text_local || cancel_accent || retry_text;
        if (!backend_ready_ || keyboard_.pointer_pressed(pointer)) {
            return false;
        }
        if (!output_.admit((!local || sends_text) && !retry_text, now)) {
            update_output_status();
            dirty = true;
            return false;
        }
        const bool num_dependent = key->action == "NumpadDecimal" ||
                                   (key->action.starts_with("Numpad") && key->action.size() == 7 &&
                                    g_ascii_isdigit(key->action.back()));
        if (!unicode_mode() && num_dependent && output_.enabled()) {
            // Never guess a native target's lock state or blindly toggle it at
            // launch. Wait for feedback before forwarding a lock-dependent key.
            const auto target_num = output_.num_lock_state();
            if (target_num && native_num_requested_ == target_num) {
                native_num_requested_.reset();
            }
            if (native_num_requested_ || !target_num || *target_num != keyboard_.num()) {
                if (target_num && !native_num_requested_) {
                    output_.send(KEY_NUMLOCK, 1);
                    output_.send(KEY_NUMLOCK, 0);
                    native_num_requested_ = !*target_num;
                }
                status_ = "Waiting for target Num Lock. Press the keypad key again.";
                dirty = true;
                return false;
            }
        }
        const bool sends_action = key->action_kind == ActionKind::Shortcut ||
                                  !is_modifier(key_code(key->action)) || !key->sticky;
        if (!ime_local && (!composition_.empty() || (cjk_ && !cjk_->empty())) && sends_action) {
            // Commit before either native events or ordinary Unicode text. Spaces,
            // digits and punctuation must not overtake the unfinished IME word.
            try {
                commit_japanese();
                commit_cjk();
            } catch (const std::exception& error) {
                status_ = error.what();
            }
            dirty = true;
            if (!composition_.empty() || (cjk_ && !cjk_->empty())) {
                return false; // Failed commit must not lose text or change the target.
            }
            if (!output_.admit(!local || sends_text, now)) {
                update_output_status();
                return false; // The preedit commit may have filled the final queue slot.
            }
        }
        // Capture the symbol before a tapped Shift/AltGr is consumed by down().
        const auto symbol = text_local
                                ? keymap_->symbol(*key, modifiers, keyboard_.caps(), keyboard_.num())
                                : XKB_KEY_NoSymbol;
        const bool old_num = keyboard_.num();
        const bool accepted =
            keyboard_.down(pointer, *key, now, local, local ? 0 : native_code(*key, modifiers));
        if (accepted) {
            try {
                if (retry_text) {
                    retry_output();
                } else if (ime_local) {
                    japanese_key(*key, true, modifiers);
                    cjk_key(*key, true, modifiers);
                } else if (cancel_accent) {
                    keymap_->cancel_compose();
                    status_.clear();
                } else if (sends_text) {
                    const auto text = keymap_->compose(symbol);
                    if (options_.input == InputBackend::None) {
                        status_.clear();
                    } else {
                        output_.text_press(pointer, text, now);
                    }
                    if (keymap_->composing()) {
                        status_ = "Accent pending";
                    }
                } else if (!local) {
                    keymap_->cancel_compose();
                    status_.clear();
                }
            } catch (const std::exception& error) {
                status_ = error.what();
            }
            if (keyboard_.num() != old_num) {
                settings_.num_lock = keyboard_.num();
                save();
            }
        }
        update_output_status();
        dirty |= accepted;
        return accepted;
    }
    return false;
}

bool App::up(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    if (settings_open_ && settings_ui_.up(pointer)) {
        pressed_controls_.erase(pointer);
        dirty = true;
        return false;
    }
    const bool key_released = keyboard_.up(pointer);
    move(pointer, x, y);
    const auto it = pressed_controls_.find(pointer);
    if (it != pressed_controls_.end()) {
        const auto pressed = it->second;
        pressed_controls_.erase(it);
        if (hovered_[pointer] == HoverTarget{HoverTarget::Kind::Control, pressed.id}) {
            try {
                action(pressed.action);
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
    }
    dirty = true;
    return key_released;
}

void App::cancel_pointer(unsigned pointer) {
    settings_ui_.cancel_pointer(pointer);
    pointer_positions_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    settings_ui_.cancel();
    pointer_positions_.clear();
    if (discard_composition) {
        if (keymap_) {
            keymap_->cancel_compose();
        }
        composition_.cancel();
        if (cjk_) {
            cjk_->cancel();
        }
    }
    keyboard_.cancel_all();
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
}
bool App::tick(double now) {
    if (updates_) {
        if (updates_->tick(now)) {
            // A new async label must not turn an already-held press into installation.
            std::erase_if(pressed_controls_, [](const auto& entry) {
                return entry.second.action.kind == ControlKind::Updates;
            });
            dirty = true;
        }
        if (updates_->prepared()) {
            set_interaction_active(false);
            quit_ = true;
        }
    }
    update_save_status();
    if (reload_requested_ && !quit_ && settings_writer_.idle()) {
        try {
            reload();
        } catch (const std::exception& error) {
            status_ = error.what();
            dirty = true;
        }
    }
    refresh_typing();
    dirty |= output_.tick(now);
    update_output_status();
    return dirty || renderer.animating();
}
void App::paint(double now) {
    renderer.paint(view(), now);
    dirty = false;
}
void App::set_interaction_active(bool active) {
    if (active == interaction_active_) {
        return;
    }
    // Release through the still-open gate; disabling first would strand held
    // modifiers in the compositor. Also discard preedit and queued UI clicks.
    cancel();
    interaction_active_ = active;
    if (active) {
        refresh_typing();
    } else {
        output_.set_enabled(false);
    }
}
void App::set_dragging(bool dragging) {
    if (dragging && settings_.pinned) {
        return;
    }
    if (dragging_ == dragging) {
        return;
    }
    cancel();
    dragging_ = dragging;
    status_.clear();
}
void App::report_status(const std::string& message) {
    status_ = message;
    dirty = true;
}
void App::show_settings() {
    cancel();
    settings_open_ = true;
    pending_ = settings_.active;
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    update_settings_ui();
    settings_ui_.reset();
    dirty = true;
}
void App::back() {
    if (settings_ui_.popup_open()) {
        settings_ui_.close_popup();
        pressed_controls_.clear();
        hovered_.clear();
    } else if (settings_open_) {
        cancel();
        settings_open_ = false;
    }
    dirty = true;
}
void App::summon() {
    dismiss_requested_ = false;
    // Release held keys before moving; keep the input connection unchanged.
    cancel();
    settings_open_ = false;
    placement_actions_.clear();
    recenter_ = true;
    status_.clear();
}
std::vector<PlacementAction> App::take_placement_actions() {
    std::vector<PlacementAction> result;
    result.swap(placement_actions_);
    return result;
}
void App::activate(Selection selection) {
    allow_slow_work();
    validate_selection_profiles(profiles_, selection);
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    auto cjk = prepare_cjk(profiles_.languages.at(selection.language));
    cancel();
    japanese_latin_ = false;
    cjk_latin_ = false;
    cjk_ = std::move(cjk);
    settings_.active = std::move(selection);
    keymap_ = std::move(keymap);
    pending_ = settings_.active;
    refresh_typing();
    status_.clear();
    update_settings_ui();
    dirty = true;
}
void App::save() {
    try {
        auto saved = settings_;
        saved.active = persisted_active_;
        settings_writer_.enqueue(std::move(saved));
    } catch (const std::exception& error) {
        status_ = "Applied, but not saved: " + std::string(error.what());
    }
}
void App::update_save_status() {
    if (const auto result = settings_writer_.take_result()) {
        if (!result->startup_warning.empty()) {
            save_status_ = status_ = result->startup_warning;
            save_status_is_startup_ = true;
        } else if (result->error.empty()) {
            // Reconciliation does not save JSON, so it cannot resolve a prior
            // storage failure merely because Reload configured startup.
            if (!result->reconcile || save_status_is_startup_) {
                if (status_ == save_status_) {
                    status_.clear();
                }
                save_status_.clear();
                save_status_is_startup_ = false;
            }
        } else {
            save_status_is_startup_ = result->reconcile;
            save_status_ =
                (result->reconcile ? "SteamOS startup not configured: " : "Applied, but not saved: ") +
                result->error;
            status_ = save_status_;
        }
        dirty = true;
    }
}
void App::flush_settings() {
    allow_slow_work();
    settings_writer_.flush();
    update_save_status();
}
void App::allow_slow_work() {
    double unset = 0;
    slow_work_deadline_.compare_exchange_strong(unset, monotonic_seconds() + 30);
}
void App::apply(Selection selection) {
    activate(std::move(selection));
    default_ = settings_.active;
    persisted_active_ = settings_.active;
    startup_status_.clear();
    rebuild_cycle_entries();
    save();
}
void App::rebuild_cycle_entries() {
    std::vector<Selection> entries;
    std::map<std::string, bool> languages;
    auto append = [&](Selection candidate) {
        candidate.theme = settings_.active.theme;
        if (std::any_of(entries.begin(), entries.end(),
                        [&](const Selection& entry) { return same_pair(entry, candidate); })) {
            return;
        }
        try {
            validate_selection_profiles(profiles_, candidate);
            if (!languages.contains(candidate.language)) {
                // Compile each language once during cache rebuilding, never
                // compile the entire favorites list in a cycle click handler.
                languages[candidate.language] = false;
                LanguageMap check(profiles_.languages.at(candidate.language));
                languages[candidate.language] = true;
            }
            if (!languages.at(candidate.language)) {
                return;
            }
            entries.push_back(std::move(candidate));
        } catch (const std::exception&) {
            // A removed/incompatible favorite must not prevent cycling the rest.
        }
    };
    append(default_);
    for (const auto& favorite : settings_.favorites) {
        append(favorite.selection);
    }
    cycle_entries_ = std::move(entries);
}
void App::reload() {
    if (!settings_writer_.idle()) {
        reload_requested_ = true;
        status_ = "Waiting for settings to finish saving.";
        dirty = true;
        return;
    }
    // Only App's thread queues snapshots, so none can appear between the idle
    // check and this read. A newer save while waiting also stays ahead of Reload.
    // Clear the request before preparation so a failed profile is not retried
    // every frame, and consume completion errors before reconciliation queues.
    reload_requested_ = false;
    update_save_status();
    allow_slow_work();
    auto candidate = load_profiles(options_.data_dir, options_.config_dir, &profiles_);
    validate_selection_profiles(candidate, settings_.active);
    // Preparing a keymap can fail. Finish that work before releasing the old model.
    auto keymap = std::make_unique<LanguageMap>(candidate.languages.at(settings_.active.language));
    auto cjk = prepare_cjk(candidate.languages.at(settings_.active.language));
    cancel();
    cjk_ = std::move(cjk);
    profiles_ = std::move(candidate);
    keymap_ = std::move(keymap);
    std::vector<std::string> config_errors;
    const auto updated_settings = load_settings(options_.config_dir, config_errors);
    if (config_errors.empty()) {
        settings_.favorites = updated_settings.favorites;
        settings_.pinned = updated_settings.pinned;
        settings_.num_lock = updated_settings.num_lock;
        settings_.hide_numpad = updated_settings.hide_numpad;
        settings_.use_drag_to_move = updated_settings.use_drag_to_move;
        settings_.auto_leveling = updated_settings.auto_leveling;
        settings_.replace_native_keyboard = updated_settings.replace_native_keyboard;
        settings_.start_with_steamos = updated_settings.start_with_steamos;
        settings_writer_.reconcile(settings_);
        keyboard_.set_num(settings_.num_lock);
    }
    profiles_.errors.insert(profiles_.errors.end(), config_errors.begin(), config_errors.end());
    pending_ = settings_.active;
    pending_favorites_ = settings_.favorites;
    pending_hide_numpad_ = settings_.hide_numpad;
    pending_use_drag_to_move_ = settings_.use_drag_to_move;
    pending_auto_leveling_ = settings_.auto_leveling;
    pending_replace_native_keyboard_ = settings_.replace_native_keyboard;
    pending_start_with_steamos_ = settings_.start_with_steamos;
    rebuild_cycle_entries();
    update_settings_ui();
    settings_ui_.reset();
    refresh_typing();
    status_ = profiles_.errors.empty() ? save_status_ : profiles_.errors.front();
}
bool App::japanese() const {
    return profiles_.languages.at(settings_.active.language).input_method.starts_with("japanese-");
}
void App::refresh_typing() {
    const bool ready = output_.pump();
    const bool reset = output_.take_input_reset();
    const bool can_resume = output_.can_resume();
    if (reset || (backend_ready_ && !ready)) {
        native_num_requested_.reset();
        // Clear UI holds even if input was already disabled by a hidden dashboard.
        // The backend has released its keys; never replay those holds on resume.
        cancel();
    }
    dirty |= ready != backend_ready_ || can_resume != backend_can_resume_;
    backend_ready_ = ready;
    backend_can_resume_ = can_resume;
    const auto& language = profiles_.languages.at(settings_.active.language);
    text_ready_ = (unicode_mode() || japanese() || cjk_) && output_.text_available();
    // Unicode mode owns character mapping. Only physical-only backends and
    // external JIS still rely on a declared receiving-session keymap.
    const bool matching = target_language_ && same_keymap(*target_language_, language) &&
                          (japanese() || cjk_ || options_.target_language == settings_.active.language);
    // Hidden panels and native handoffs deliberately pause interaction. That is
    // separate from an unavailable typing backend and must not show a warning.
    const bool typing_ready =
        options_.start_enabled && options_.mode == Mode::Vr &&
        (options_.input == InputBackend::Ei || options_.input == InputBackend::Uinput) &&
        (unicode_mode() ? text_ready_ : matching && (!(japanese() || cjk_) || text_ready_)) &&
        backend_ready_;
    dirty |= typing_ready_ != typing_ready;
    typing_ready_ = typing_ready;
    if (output_.enabled() && !(interaction_active_ && typing_ready_)) {
        // Transport loss ends key ownership, but unsent composition stays for
        // an explicit retry once text delivery is available again.
        cancel(false);
    }
    output_.set_enabled(interaction_active_ && typing_ready_);
}
void App::convert_japanese() {
    // Anthy loads its library and dictionary lazily on first conversion.
    allow_slow_work();
    composition_.convert();
}
void App::commit_japanese() {
    const auto text = composition_.commit_text();
    if (text.empty()) {
        return;
    }
    if (options_.input == InputBackend::None) {
        composition_.cancel();
        status_ = "Preview: composition committed locally.";
    } else if (output_.commit_text(text)) {
        composition_.cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::japanese_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!japanese() || key.action_kind != ActionKind::Key) {
        return false;
    }
    const auto& code = key.action;
    if (code == "ZenkakuHankaku" || code == "KanaMode") {
        if (execute) {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        }
        return true;
    }
    if (japanese_latin_) {
        return false;
    }
    for (const char* modifier :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(modifier))) {
            return false;
        }
    }
    const bool shift = mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
    const bool composing = !composition_.empty();
    if (code == "Convert" || (code == "Space" && composing)) {
        if (execute) {
            if (shift && composition_.converting()) {
                composition_.cycle(-1);
            } else {
                convert_japanese();
            }
        }
        return true;
    }
    if (code == "NonConvert" || (composing && (code == "F6" || code == "F7"))) {
        if (execute) {
            composition_.script(code == "F7");
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Backspace" || code == "Escape" ||
         code == "ArrowLeft" || code == "ArrowRight" || code == "ArrowUp" || code == "ArrowDown")) {
        if (execute) {
            if (code == "Enter" || code == "NumpadEnter") {
                commit_japanese();
            } else if (code == "Backspace") {
                composition_.backspace();
            } else if (code == "Escape") {
                if (composition_.converting()) {
                    composition_.unconvert();
                } else {
                    composition_.cancel();
                }
            } else if (code == "ArrowLeft" || code == "ArrowRight") {
                composition_.segment(code == "ArrowLeft" ? -1 : 1);
            } else {
                composition_.cycle(code == "ArrowUp" ? -1 : 1);
            }
        }
        return true;
    }
    const auto& language = profiles_.languages.at(settings_.active.language);
    if (language.input_method == "japanese-kana") {
        const auto& map =
            shift && language.kana_shift.contains(code) ? language.kana_shift : language.kana;
        if (auto it = map.find(code); it != map.end()) {
            if (execute) {
                if (composition_.converting()) {
                    commit_japanese();
                    if (!composition_.empty()) {
                        return true;
                    }
                }
                composition_.kana(it->second == "゛"   ? "\u3099"
                                  : it->second == "゜" ? "\u309a"
                                                       : it->second);
            }
            return true;
        }
        return false;
    }
    if (code == "Space") {
        return false; // With no preedit, Space remains ordinary Unicode output.
    }
    // Legends may be cosmetic overrides. Composition follows the actual XKB
    // symbol, including Shift/Caps, independently of the displayed label.
    const auto character =
        xkb_keysym_to_utf32(keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num()));
    if (character >= 0x20 && character <= 0x7e && !is_modifier(key_code(code))) {
        if (execute) {
            if (composition_.converting()) {
                commit_japanese();
                if (!composition_.empty()) {
                    return true;
                }
            }
            composition_.roman(static_cast<char>(character));
        }
        return true;
    }
    return false;
}
bool App::unicode_mode() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    // External JIS explicitly delegates conversion to a system IME.
    const bool external_jis = language.input_method == "xkb" && language.keymap == "jp";
    return options_.input != InputBackend::Uinput && !external_jis;
}
bool App::text_key(const Key& key, const std::set<int>& mods) const {
    if (key.action_kind == ActionKind::Key && key.action == "NumLock") {
        return true; // Remember one local preference in every input mode.
    }
    if (!unicode_mode() || key.action_kind != ActionKind::Key) {
        return false;
    }
    if (key.action == "CapsLock" || key.action == "NumLock") {
        return true;
    }
    for (const auto* name : {"ControlLeft", "ControlRight", "AltLeft", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    if (mods.contains(key_code("AltRight"))) {
        Key alt;
        alt.action_kind = ActionKind::Key;
        alt.action = "AltRight";
        if (keymap_->symbol(alt, {}, false, false) != XKB_KEY_ISO_Level3_Shift) {
            return false;
        }
    }
    const auto symbol = keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num());
    return symbol == XKB_KEY_KP_Begin || LanguageMap::printable(symbol);
}
void App::update_output_status() {
    if (auto status = output_.take_status()) {
        status_ = std::move(*status);
        dirty = true;
    }
}
bool App::retry_output() {
    const bool accepted = output_.retry();
    update_output_status();
    return accepted;
}
int App::native_code(const Key& key, const std::set<int>& mods) {
    if (!unicode_mode()) {
        return 0;
    }
    if (key.action_kind == ActionKind::Shortcut) {
        return output_.shortcut_code(key.action == "copy" ? XKB_KEY_c : XKB_KEY_v,
                                     key_code(key.action == "copy" ? "KeyC" : "KeyV"));
    }
    if (key.action.starts_with("Numpad")) {
        // Num Lock is local in Unicode mode. Send explicit navigation codes,
        // so the receiving system's Num Lock cannot turn Home into '7'.
        switch (keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num())) {
        case XKB_KEY_KP_Home:
            return KEY_HOME;
        case XKB_KEY_KP_End:
            return KEY_END;
        case XKB_KEY_KP_Left:
            return KEY_LEFT;
        case XKB_KEY_KP_Right:
            return KEY_RIGHT;
        case XKB_KEY_KP_Up:
            return KEY_UP;
        case XKB_KEY_KP_Down:
            return KEY_DOWN;
        case XKB_KEY_KP_Prior:
            return KEY_PAGEUP;
        case XKB_KEY_KP_Next:
            return KEY_PAGEDOWN;
        case XKB_KEY_KP_Insert:
            return KEY_INSERT;
        case XKB_KEY_KP_Delete:
            return KEY_DELETE;
        default:
            break;
        }
    }
    auto shortcut_symbol = xkb_keysym_to_lower(keymap_->symbol(key, {}, false, false));
    if (!mods.empty() && (key.action.starts_with("Key") ||
                          (shortcut_symbol >= XKB_KEY_a && shortcut_symbol <= XKB_KEY_z))) {
        auto symbol = shortcut_symbol;
        // Cyrillic and IME shortcuts conventionally retain their Latin physical
        // counterparts; Latin layouts use their displayed letter (AZERTY A, etc.).
        if (symbol < XKB_KEY_a || symbol > XKB_KEY_z) {
            symbol = static_cast<xkb_keysym_t>(g_ascii_tolower(key.action.back()));
        }
        const int fallback = key_code(
            "Key" + std::string(1, static_cast<char>(g_ascii_toupper(static_cast<char>(symbol)))));
        return output_.shortcut_code(symbol, fallback);
    }
    return 0;
}
void App::choose_cjk(int index) {
    if (!cjk_) {
        return;
    }
    cjk_->choose(index);
    if (!cjk_->has_reading()) {
        commit_cjk();
    }
}
void App::commit_cjk() {
    if (!cjk_ || cjk_->empty()) {
        return;
    }
    const auto text = cjk_->text();
    if (!text.empty() && (options_.input == InputBackend::None || output_.commit_text(text))) {
        cjk_->cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::cjk_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!cjk_ || cjk_latin_ || key.action_kind != ActionKind::Key) {
        return false;
    }
    for (const auto* name :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    const auto& code = key.action;
    const bool composing = !cjk_->empty();
    if (cjk_->chinese() && composing && code.size() == 6 && code.starts_with("Digit") &&
        code.back() >= '1' && code.back() <= '5' && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight")) && !cjk_->candidates().empty()) {
        const int index = cjk_->selected() / 5 * 5 + code.back() - '1';
        if (execute && index < static_cast<int>(cjk_->candidates().size())) {
            choose_cjk(index);
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Escape" || code == "Backspace" ||
         (cjk_->chinese() && (code == "Space" || code == "ArrowUp" || code == "ArrowDown")))) {
        if (execute) {
            if (code == "Backspace") {
                cjk_->backspace();
            } else if (code == "Escape") {
                cjk_->cancel();
            } else if (code == "ArrowUp" || code == "ArrowDown") {
                cjk_->cycle(code == "ArrowUp" ? -1 : 1);
            } else if (code == "Space" && !cjk_->candidates().empty()) {
                choose_cjk(cjk_->selected());
            } else {
                commit_cjk();
            }
        }
        return true;
    }
    // Use physical US letter positions for Pinyin and two-set Hangul. Caps Lock
    // does not choose doubled Korean jamo; only Shift does.
    if (code.starts_with("Key") && code.size() == 4) {
        char ch = g_ascii_tolower(code.back());
        if (!cjk_->chinese() &&
            (mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight")))) {
            ch = g_ascii_toupper(ch);
        }
        if (execute) {
            cjk_->type(ch);
        }
        return true;
    }
    if (cjk_->chinese() && code == "Quote" && composing && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight"))) {
        if (execute) {
            cjk_->type('\'');
        }
        return true;
    }
    return false;
}
void App::action(const ControlAction& command) {
    const auto kind = command.kind;
    if (kind == ControlKind::Smaller || kind == ControlKind::Larger) {
        cancel();
        placement_actions_.push_back(kind == ControlKind::Smaller ? PlacementAction::Smaller
                                                                  : PlacementAction::Larger);
        return;
    }
    if (output_.text_pending() &&
        ((kind >= ControlKind::CjkToggle && kind <= ControlKind::CjkCandidate) ||
         (kind >= ControlKind::ImeToggle && kind <= ControlKind::ImeCandidate))) {
        if (kind != ControlKind::CjkCancel && kind != ControlKind::ImeCancel &&
            kind != ControlKind::ImeToggle) {
            return;
        }
        output_.cancel_text();
    }
    if ((kind >= ControlKind::CjkToggle && kind <= ControlKind::CjkCandidate) && cjk_) {
        if (kind == ControlKind::CjkToggle) {
            if (!cjk_->empty()) {
                commit_cjk();
                if (!cjk_->empty()) {
                    return;
                }
            }
            cjk_latin_ = !cjk_latin_;
        } else if (kind == ControlKind::CjkCommit) {
            if (!output_.retry_available()) {
                commit_cjk();
            } else {
                retry_output();
            }
        } else if (kind == ControlKind::CjkCancel) {
            cjk_->cancel();
        } else if (kind == ControlKind::CjkNext || kind == ControlKind::CjkPrev) {
            cjk_->cycle(kind == ControlKind::CjkNext ? 1 : -1);
        } else if (kind == ControlKind::CjkCandidate) {
            choose_cjk(command.index);
        }
        return;
    }
    if ((kind >= ControlKind::ImeToggle && kind <= ControlKind::ImeCandidate)) {
        if (kind == ControlKind::ImeToggle) {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        } else if (kind == ControlKind::ImeConvert) {
            convert_japanese();
        } else if (kind == ControlKind::ImeCommit) {
            if (!output_.retry_available()) {
                commit_japanese();
            } else {
                retry_output();
            }
        } else if (kind == ControlKind::ImeCancel) {
            composition_.cancel();
        } else if (kind == ControlKind::ImeHiragana || kind == ControlKind::ImeKatakana) {
            composition_.script(kind == ControlKind::ImeKatakana);
        } else if (kind == ControlKind::ImeNext || kind == ControlKind::ImePrev) {
            composition_.cycle(kind == ControlKind::ImeNext ? 1 : -1);
        } else if (kind == ControlKind::ImeSegmentNext || kind == ControlKind::ImeSegmentPrev) {
            composition_.segment(kind == ControlKind::ImeSegmentNext ? 1 : -1);
        } else if (kind == ControlKind::ImeCandidate) {
            composition_.choose(command.index);
        }
        return;
    }
    if (kind == ControlKind::Updates && settings_open_) {
        if (updates_) {
            updates_->click(monotonic_seconds());
        } else {
            report_status("Updates are available from the installed keyboard.");
        }
    } else if (kind == ControlKind::Close) {
        close_panel();
    } else if (kind == ControlKind::Quit && resident_replacement()) {
        set_interaction_active(false);
        quit_ = true;
    } else if (kind == ControlKind::Pin) {
        cancel();
        settings_.pinned = !settings_.pinned;
        status_.clear();
        save();
    } else if (kind == ControlKind::LanguageCycle) {
        const auto& entries = cycle_entries_;
        if (entries.size() > 1) {
            const auto current =
                std::find_if(entries.begin(), entries.end(),
                             [&](const Selection& entry) { return same_pair(entry, settings_.active); });
            const auto index =
                current == entries.end()
                    ? 0
                    : (static_cast<std::size_t>(current - entries.begin()) + 1) % entries.size();
            std::string failure;
            for (std::size_t offset = 0; offset < entries.size(); ++offset) {
                auto candidate = entries[(index + offset) % entries.size()];
                candidate.theme = settings_.active.theme;
                try {
                    activate(std::move(candidate));
                    return;
                } catch (const std::exception& error) {
                    // Missing optional engines must not strand the cycle at an
                    // unusable favorite. Keep the current keyboard until one works.
                    failure = error.what();
                }
            }
            status_ = failure;
        }
    } else if (kind == ControlKind::Settings) {
        if (settings_open_) {
            back();
        } else {
            show_settings();
        }
    } else if (kind == ControlKind::Recenter) {
        cancel();
        placement_actions_.clear();
        recenter_ = true;
    } else if (kind == ControlKind::Apply) {
        // Prepare/activate first. A failed candidate must leave all saved state intact.
        activate(pending_);
        settings_.favorites = pending_favorites_;
        settings_.hide_numpad = pending_hide_numpad_;
        settings_.use_drag_to_move = pending_use_drag_to_move_;
        settings_.auto_leveling = pending_auto_leveling_;
        settings_.replace_native_keyboard = pending_replace_native_keyboard_;
        settings_.start_with_steamos = pending_start_with_steamos_;
        default_ = settings_.active;
        persisted_active_ = settings_.active;
        startup_status_.clear();
        rebuild_cycle_entries();
        save();
        settings_open_ = false;
    } else if (kind == ControlKind::Reload) {
        reload();
    } else if (kind == ControlKind::ToggleDropdown) {
        pressed_controls_.clear();
        hovered_.clear();
        settings_ui_.toggle(command.value);
    } else if (kind == ControlKind::ChooseLayout || kind == ControlKind::ChooseLanguage ||
               kind == ControlKind::ChooseTheme) {
        pressed_controls_.clear();
        hovered_.clear();
        if (kind == ControlKind::ChooseLayout) {
            pending_.layout = command.value;
        } else if (kind == ControlKind::ChooseLanguage) {
            pending_.language = command.value;
            pending_.layout = compatible_layout(profiles_, pending_);
        } else if (kind == ControlKind::ChooseTheme) {
            pending_.theme = command.value;
        }
        settings_ui_.close_popup();
        update_settings_ui();
    } else if (kind == ControlKind::HideNumpad) {
        pending_hide_numpad_ = !pending_hide_numpad_;
        update_settings_ui();
    } else if (kind == ControlKind::UseDragToMove) {
        pending_use_drag_to_move_ = !pending_use_drag_to_move_;
        update_settings_ui();
    } else if (kind == ControlKind::ReplaceNativeKeyboard) {
        pending_replace_native_keyboard_ = !pending_replace_native_keyboard_;
        std::erase_if(pressed_controls_, [](const auto& entry) {
            return entry.second.action.kind == ControlKind::StartWithSteamos;
        });
        update_settings_ui();
    } else if (kind == ControlKind::StartWithSteamos && pending_replace_native_keyboard_) {
        pending_start_with_steamos_ = !pending_start_with_steamos_;
        update_settings_ui();
    } else if (kind == ControlKind::NativeFallback) {
        cancel();
        native_fallback_ = true;
    } else if (kind == ControlKind::AutoLeveling) {
        pending_auto_leveling_ = !pending_auto_leveling_;
        update_settings_ui();
    } else if (kind == ControlKind::Favorite) {
        const bool exists =
            std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                        [&](const Favorite& f) { return same_pair(f.selection, pending_); });
        if (exists) {
            std::erase_if(pending_favorites_,
                          [&](const Favorite& f) { return same_pair(f.selection, pending_); });
        } else {
            validate_selection(profiles_, pending_);
            if (pending_favorites_.size() >= 32) {
                throw std::runtime_error("At most 32 favorites can be saved.");
            }
            // IDs are metadata only; choose one that cannot collide with legacy favorites.
            int index = 1;
            std::string favorite_id;
            do {
                favorite_id = "favorite-" + std::to_string(index++);
            } while (std::any_of(pending_favorites_.begin(), pending_favorites_.end(),
                                 [&](const Favorite& f) { return f.id == favorite_id; }));
            pending_favorites_.push_back(
                {favorite_id, profiles_.languages.at(pending_.language).name, pending_});
        }
        update_settings_ui();
    }
}
bool App::replacement_ready() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    return options_.mode == Mode::Vr && options_.input == InputBackend::Ei && options_.start_enabled &&
           backend_ready_ && text_ready_ && !(language.input_method == "xkb" && language.keymap == "jp");
}
bool App::take_native_fallback() {
    return std::exchange(native_fallback_, false);
}
void App::close_panel() {
    set_interaction_active(false);
    if (resident_replacement()) {
        settings_open_ = false;
        dismiss_requested_ = true;
    } else {
        quit_ = true;
    }
}
bool App::take_dismissal() {
    return std::exchange(dismiss_requested_, false);
}
bool App::take_recenter() {
    const bool result = recenter_;
    recenter_ = false;
    return result;
}
} // namespace framekeyboard
