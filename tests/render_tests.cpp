#include "framekeyboard/panel.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace framekeyboard;
namespace {
const Key& find_key(const Layout& layout, const std::string& action) {
    for (const auto& key : layout.keys) {
        if (key.action == action || key.id == action) {
            return key;
        }
    }
    throw std::runtime_error("missing key " + action);
}
class Comparison {
  public:
    PanelRenderer incremental, full;
    double now = 100;
    unsigned frames{};
    void frame(const PanelView& view, const char* phase) {
        now += 1.0 / 60;
        incremental.paint(view, now);
        full.paint(view, now, true);
        auto* a = incremental.surface();
        auto* b = full.surface();
        for (int y = 0; y < texture_height; ++y) {
            const auto* actual = cairo_image_surface_get_data(a) + y * cairo_image_surface_get_stride(a);
            const auto* expected =
                cairo_image_surface_get_data(b) + y * cairo_image_surface_get_stride(b);
            if (std::memcmp(actual, expected, panel_width * 4) != 0) {
                incremental.write_png("/tmp/framekeyboard-render-actual.png");
                full.write_png("/tmp/framekeyboard-render-expected.png");
                throw std::runtime_error(std::string(phase) + ": pixels differ on row " +
                                         std::to_string(y) + " frame " + std::to_string(frames));
            }
        }
        if (incremental.animating() != full.animating()) {
            throw std::runtime_error("animation scheduling differs");
        }
        ++frames;
    }
    void settle(const PanelView& view, const char* phase) {
        for (int i = 0; i < 7; ++i) {
            frame(view, phase);
        }
    }
};
void handle_states() {
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    PanelView view;
    view.layout = &profiles.layouts.at("en-us-full");
    view.theme = &profiles.themes.at("graphite");
    view.language = &profiles.languages.at("en-us");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    view.controls = {{"move-handle",
                      "Move keyboard",
                      {580, 612, 440, 60},
                      false,
                      Icon::None,
                      ControlStyle::MoveHandle}};
    Comparison compare;
    auto require = [](bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    auto row = [&] {
        const auto pixels = compare.incremental.rgba();
        const auto offset = static_cast<std::size_t>((642 + popup_margin) * panel_width) * 4;
        return std::vector<unsigned char>(pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                                          pixels.begin() +
                                              static_cast<std::ptrdiff_t>(offset + panel_width * 4));
    };
    auto width = [](const auto& pixels) {
        int count = 0;
        for (int x = 0; x < panel_width; ++x) {
            count += pixels[static_cast<std::size_t>(x) * 4 + 3] != 0;
        }
        return count;
    };
    compare.frame(view, "idle movement line");
    const auto idle = row();
    const auto center = static_cast<std::size_t>(panel_width / 2) * 4;
    require(idle[center] == 61 && idle[center + 1] == 68 && idle[center + 2] == 80 &&
                idle[center + 3] == 255,
            "idle handle uses native opaque #3d4450");
    compare.incremental.write_png("/tmp/framekeyboard-handle-idle.png");
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "start movement hover");
    require(compare.incremental.animating(), "hover schedules its 80 ms transition");
    compare.frame(view, "movement hover transition");
    const auto between = row();
    require(between[center] > idle[center] && between[center] < 255 && width(between) > width(idle),
            "hover interpolates both size and color");
    compare.settle(view, "complete movement hover");
    const auto hovered = row();
    require(hovered[center] == 255 && hovered[center + 1] == 255 && hovered[center + 2] == 255 &&
                std::abs(width(hovered) - width(idle) * 1.1) <= 2 && !compare.incremental.animating(),
            "hover ends white and 10 percent wider, then stops repainting");
    compare.incremental.write_png("/tmp/framekeyboard-handle-hover.png");
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    view.controls.front().selected = true;
    compare.frame(view, "captured movement stays active");
    require(row() == hovered, "capture retains the native active appearance after hover is cleared");
    view.controls.front().selected = false;
    compare.frame(view, "release movement line");
    compare.settle(view, "return to idle movement line");
    require(row() == idle && !compare.incremental.animating(),
            "leaving the handle clears grown pixels and restores idle color");
    view.settings = true;
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "settings movement hover");
    require(compare.incremental.animating(), "hover animates while settings is open too");
    view.controls.clear();
    compare.frame(view, "pin during movement animation");
    require(width(row()) == 0 && !compare.incremental.animating(),
            "pin removes the complete line and stops its animation");
}
void exercise(Layout layout, Theme theme, Language language) {
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(language);
    PanelView view;
    view.layout = &layout;
    view.theme = &theme;
    view.language = &language;
    view.keymap = &keymap;
    view.keyboard = &keyboard;
    view.controls = {{"settings", "Settings", {18, 12, 60, 42}, false, Icon::Settings}};
    if (!language.composition_keys.empty()) {
        view.composing = true;
        for (const auto& key : layout.keys) {
            if (auto it = language.composition_keys.find(key.action);
                it != language.composition_keys.end()) {
                view.key_labels[key.id] = it->second;
            }
        }
    }
    Comparison compare;
    compare.frame(view, "initial");
    view.controls.push_back({"move-handle",
                             "Move keyboard",
                             {580, 612, 440, 60},
                             false,
                             Icon::None,
                             ControlStyle::MoveHandle});
    compare.frame(view, "show movement line");
    view.hovered_controls = {"move-handle"};
    compare.frame(view, "hover movement line");
    view.controls.pop_back();
    compare.frame(view, "hide movement line on pin or grab mode");
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    const auto a = find_key(layout, "KeyA"), b = find_key(layout, "KeyB");
    view.hovered_keys = {a.id};
    compare.frame(view, "hover");
    keyboard.down(0, a, compare.now);
    compare.frame(view, "press");
    keyboard.down(1, b, compare.now);
    compare.frame(view, "second controller");
    // Reverse partway through a press; the retained image must erase its old edge.
    keyboard.up(0);
    view.hovered_keys = {b.id};
    compare.settle(view, "release while second hand holds");
    keyboard.cancel_all();
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    compare.settle(view, "cancel");
    for (const auto* action :
         {"ShiftLeft", "CapsLock", "NumLock", "ControlLeft", "MetaLeft", "MetaRight"}) {
        const auto key = find_key(layout, action);
        keyboard.down(0, key, compare.now);
        compare.settle(view, "modifier/lock press");
        keyboard.up(0);
        compare.settle(view, "modifier/lock release");
        keyboard.down(1, a, compare.now);
        compare.settle(view, "modified key");
        keyboard.up(1);
        compare.settle(view, "modified release");
    }
    for (const auto* action : {"Enter", "NumpadEnter", "Space", "Copy", "ArrowUp"}) {
        const auto key = find_key(layout, action);
        view.hovered_keys = {key.id};
        keyboard.down(0, key, compare.now);
        compare.settle(view, "large/icon key press");
        keyboard.up(0);
        view.hovered_keys.clear();
        view.hovered_controls.clear();
        compare.settle(view, "large/icon key release");
    }
    view.hide_numpad = true;
    compare.frame(view, "hide numpad");
    view.hovered_keys = {a.id};
    keyboard.down(0, a, compare.now);
    compare.settle(view, "narrow keyboard press");
    keyboard.up(0);
    view.hovered_keys.clear();
    view.hovered_controls.clear();
    compare.settle(view, "narrow keyboard release");
    view.settings = true;
    compare.frame(view, "narrow keyboard settings");
    view.settings = false;
    compare.frame(view, "narrow keyboard return");
    view.hide_numpad = false;
    compare.frame(view, "restore numpad");
    view.hovered_controls = {"settings"};
    compare.frame(view, "toolbar hover");
    view.status = "Connection status changed";
    compare.frame(view, "status");
    view.settings = true;
    compare.frame(view, "settings");
    view.settings = false;
    view.status.clear();
    compare.frame(view, "return to keys");
    view.composing = true;
    view.preedit = "にほんご";
    view.key_labels[a.id] = "ち";
    compare.frame(view, "composition geometry");
    keyboard.down(0, a, compare.now);
    compare.settle(view, "kana press");
    keyboard.up(0);
    compare.settle(view, "kana release");
    view.preedit = "日本語";
    view.controls.push_back({"candidate", "日本語", {85, 148, 230, 42}});
    compare.frame(view, "candidate");
    view.composing = false;
    view.preedit.clear();
    view.key_labels.clear();
    // Mutate profiles in place, as Reload may reuse existing storage. Pointer
    // equality is not sufficient for invalidating a retained image.
    theme.top = {.6, .2, .1};
    theme.font_size = 40;
    theme.small_font_size = 30;
    theme.travel = 1;
    theme.depth = 2;
    theme.padding = 35;
    layout.keys.front().bounds.width += 3;
    compare.frame(view, "custom theme/layout reload");
    keyboard.down(0, a, compare.now);
    compare.settle(view, "large font press");
    keyboard.up(0);
    compare.settle(view, "large font release");
    view.hovered_keys = {b.id};
    compare.frame(view, "large font hover");
    theme.duration_ms = 0;
    compare.frame(view, "instant animation theme");
    keyboard.down(0, b, compare.now);
    compare.frame(view, "instant press");
    keyboard.up(0);
    compare.frame(view, "instant release");
    std::cout << language.id << ": " << compare.frames << " frames pixel-identical\n";
}
} // namespace
int main() {
    try {
        handle_states();
        const auto profiles = load_profiles({}, {});
        for (const auto& [id, language] : profiles.languages) {
            const auto layout = (language.keymap == "jp" || id == "ja-kana") ? "ja-jis-full"
                                : language.keymap == "br"                    ? "br-abnt2-full"
                                : !language.required_keys.empty()            ? "international-full"
                                                                             : "en-us-full";
            validate_selection(profiles, {layout, id, "graphite"});
            exercise(profiles.layouts.at(layout), profiles.themes.at("graphite"), language);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
