#include "framekeyboard/app.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
struct Capture : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
struct TemporaryDirectory {
    fk::fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-tests-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fk::fs::remove_all(path, ignored);
    }
};
void write(const fk::fs::path& path, const std::string& text) {
    fk::fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
const fk::Key& key(const fk::Layout& layout, const std::string& id) {
    for (const auto& key : layout.keys) {
        if (key.id == id) {
            return key;
        }
    }
    throw std::runtime_error("missing test key " + id);
}
void state_tests(const fk::Layout& layout) {
    Capture sink;
    fk::KeyboardState state(sink);
    const auto left_super = key(layout, "MetaLeft");
    const auto right_super = key(layout, "MetaRight");
    require(left_super.sticky && !right_super.sticky, "Super keys have distinct tap behavior");
    state.down(0, left_super, 0);
    state.up(0);
    require(sink.events.empty() && state.modifiers().contains(KEY_LEFTMETA),
            "left Super tap latches without opening a menu");
    state.down(0, key(layout, "KeyA"), .1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTMETA, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTMETA, 0}},
            "latched Super sends a native chord");
    sink.events.clear();
    state.down(0, right_super, 0);
    state.tick(20);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}},
            "right Super sends immediate down without repeating");
    state.up(0);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                state.modifiers().empty(),
            "right Super sends up without latching");
    for (bool cancel_all : {false, true}) {
        sink.events.clear();
        state.down(0, right_super, 0);
        state.down(1, right_super, 0);
        state.up(0);
        require(sink.events.size() == 1, "overlapping Super holds share native ownership");
        if (cancel_all) {
            state.cancel_all();
        } else {
            state.cancel_pointer(1);
        }
        require(sink.events ==
                        std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                    state.modifiers().empty(),
                "canceling Super releases its native hold without a latch");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 0);
    state.up(0);
    require(sink.events.empty(), "a latched modifier must not reach the OS until used");
    state.down(0, key(layout, "KeyA"), 1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTCTRL, 0}},
            "Ctrl+A order");
    require(state.modifiers().empty(), "one-shot modifier consumed");
    sink.events.clear();
    state.down(0, key(layout, "Copy"), 2);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
            "Copy order");
    sink.events.clear();
    state.down(0, key(layout, "KeyA"), 3);
    state.down(1, key(layout, "KeyA"), 3);
    state.up(0);
    require(sink.events.size() == 1, "two pointers share one down");
    state.up(1);
    require(sink.events.size() == 2 && sink.events.back().second == 0, "last pointer releases key");
    for (const auto* id : {"CapsLock", "NumLock"}) {
        const auto lock = key(layout, id);
        const int code = fk::key_code(lock.action);
        auto locked = [&] { return code == KEY_CAPSLOCK ? state.caps() : state.num(); };
        // Exercise both release orders, and a new click while the other hand
        // still holds the key. Only the final release permits another toggle.
        for (unsigned first : {0u, 1u}) {
            sink.events.clear();
            require(!locked(), "lock starts off");
            state.down(0, lock, 3);
            state.down(1, lock, 3.1);
            require(locked(), "overlapping lock presses toggle only once");
            state.up(first);
            state.down(first, lock, 3.2);
            require(locked() && sink.events.size() == 1, "remaining hold prevents a second toggle");
            state.up(first);
            state.up(1 - first);
            require(sink.events == std::vector<std::pair<int, int>>{{code, 1}, {code, 0}},
                    "overlapping lock holds share one backend press and release");
            state.down(first, lock, 3.3);
            state.up(first);
            require(!locked() && sink.events.size() == 4, "next separate click toggles lock off");
        }
    }
    sink.events.clear();
    state.down(0, key(layout, "ShiftLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4);
    state.up(0);
    state.up(1);
    require(state.modifiers().empty(), "a modifier used while held must not latch on release");
    for (const auto* modifier : {"ControlLeft", "ShiftLeft", "AltLeft", "MetaLeft", "MetaRight"}) {
        sink.events.clear();
        const int code = fk::key_code(modifier);
        state.down(0, key(layout, modifier), 4);
        state.down(1, key(layout, "KeyA"), 4.1);
        state.up(0);
        require(sink.events.back() == std::pair<int, int>{code, 0},
                "releasing held modifier reaches backend before ordinary key release");
        require(!state.modifiers().contains(code), "released modifier does not affect next key");
        state.down(0, key(layout, "KeyB"), 4.2);
        state.up(0);
        state.up(1);
        require(std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 1}) == 1 &&
                    std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 0}) ==
                        1,
                "next key neither reacquires nor double-releases the old modifier");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4.1);
    state.down(2, key(layout, "ControlLeft"), 4.2);
    state.up(0);
    require(state.modifiers().contains(KEY_LEFTCTRL), "another hold retains the same modifier");
    state.up(2);
    require(!state.modifiers().contains(KEY_LEFTCTRL), "last modifier hold releases without latching");
    state.up(1);
    sink.events.clear();
    state.down(0, key(layout, "Backspace"), 5);
    state.tick(5.49);
    require(sink.events.size() == 1, "repeat delay");
    state.tick(5.51);
    require(sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2}, "repeat event");
    state.tick(40);
    require(state.active() && sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2},
            "long hold keeps repeating without replaying a backlog");
    state.up(0);
    state.down(0, key(layout, "ShiftLeft"), 41);
    state.down(1, key(layout, "ArrowLeft"), 41);
    state.tick(80);
    require(state.modifiers().contains(KEY_LEFTSHIFT), "modifier survives a long two-hand chord");
    state.cancel_pointer(1);
    require(state.pointer_pressed(0) && state.modifiers().contains(KEY_LEFTSHIFT),
            "leaving with the other pointer does not release the held modifier");
    state.down(1, key(layout, "ArrowRight"), 81);
    state.cancel_pointer(0);
    require(!state.modifiers().contains(KEY_LEFTSHIFT) && state.pointer_pressed(1),
            "canceling a modifier releases borrowed references without canceling the other key");
    state.up(1);
    state.down(0, key(layout, "ControlLeft"), 82);
    state.cancel_pointer(0);
    require(state.modifiers().empty(), "canceled modifier tap must not latch");
    sink.events.clear();
    state.down(0, key(layout, "Enter"), 41);
    state.cancel_all();
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_ENTER, 1}, {KEY_ENTER, 0}},
            "Enter and cancellation");
}
void profile_tests(const fk::Profiles& defaults) {
    TemporaryDirectory directory;
    write(
        directory.path / "layouts/custom.json",
        R"({"schema_version":1,"id":"compact","name":"Compact","width":200,"height":100,"keys":[{"id":"Enter","label":"Enter","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"Enter"}}]})");
    auto loaded = fk::load_profiles({}, directory.path);
    require(loaded.layouts.contains("compact"), "discover user layout");
    write(directory.path / "layouts/custom.json", "{broken");
    auto reloaded = fk::load_profiles({}, directory.path, &loaded);
    require(reloaded.layouts.contains("compact") && !reloaded.errors.empty(),
            "retain valid profile after malformed edit");
    fk::fs::remove(directory.path / "layouts/custom.json");
    reloaded = fk::load_profiles({}, directory.path, &reloaded);
    require(!reloaded.layouts.contains("compact"), "deleted user profile disappears");
    write(
        directory.path / "config.json",
        R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"custom":42,"favorites":[{"id":"de","name":"Deutsch","layout":"en-us-full","language":"de-de","theme":"midnight"}]})");
    fk::save_selection(directory.path, {"en-us-full", "de-de", "midnight"});
    std::vector<std::string> errors;
    auto saved = fk::load_settings(directory.path, errors);
    require(saved.active.language == "de-de" && saved.favorites.size() == 1 && errors.empty(),
            "settings preserve selection and favorites");
    std::ifstream file(directory.path / "config.json");
    const std::string content((std::istreambuf_iterator<char>(file)), {});
    require(content.find("custom") != std::string::npos, "preserve unrelated settings");
    write(directory.path / "config.json", "{broken");
    rejects([&] { fk::save_selection(directory.path, {}); }, "do not overwrite malformed settings");
    rejects([] { fk::parse_layout(R"({"schema_version":2})"); }, "reject future schema");
    rejects(
        [] {
            fk::parse_layout(
                R"({"schema_version":1,"id":"bad","name":"Bad","width":200,"height":100,"keys":[{"id":"A","label":"A","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyA"}},{"id":"B","label":"B","x":50,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyB"}}]})");
        },
        "reject overlaps");
    fk::LanguageMap english(defaults.languages.at("en-us")), german(defaults.languages.at("de-de"));
    const auto& layout = defaults.layouts.at("en-us-full");
    require(english.legend(key(layout, "KeyY"), {}, false, false) == "y", "US Y legend");
    require(english.legend(key(layout, "KeyY"), {KEY_LEFTSHIFT}, false, false) == "Y",
            "Shift uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {}, true, false) == "Y",
            "Caps Lock uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {KEY_RIGHTSHIFT}, true, false) == "y",
            "Shift with Caps Lock matches lowercase key output");
    require(german.legend(key(layout, "KeyY"), {}, false, false) == "z", "German Z legend");
    require(german.legend(key(layout, "BracketLeft"), {}, false, false) == "ü", "German umlaut");
    require(german.legend(key(layout, "Backquote"), {}, false, false) == "^", "German dead-key legend");
    rejects([&] { fk::validate_selection(defaults, {"en-us-full", "de-de", "graphite"}); },
            "reject missing language key");
    require(german.legend(key(layout, "KeyQ"), {KEY_RIGHTALT}, false, false) == "@",
            "German AltGr legend");
}
void app_tests() {
    TemporaryDirectory directory;
    fk::Options options;
    options.config_dir = directory.path;
    Capture sink;
    fk::App app(options, sink);
    app.paint(1);
    const auto original = app.renderer.rgba();
    require(original.size() == static_cast<std::size_t>(fk::panel_width * fk::texture_height * 4),
            "RGBA size");
    app.apply({"international-full", "de-de", "midnight"});
    app.paint(2);
    require(app.renderer.rgba() != original, "theme and language affect rendered image");
    app.show_settings();
    app.paint(3);
    require(app.renderer.hit_key(app.view(), 100, 100) == nullptr, "settings prevent hidden key hits");
    auto click_control = [&](const std::string& id) {
        for (const auto& control : app.view().controls) {
            if (control.id == id) {
                const double x = control.bounds.x + control.bounds.width / 2;
                const double y = control.bounds.y + control.bounds.height / 2;
                app.down(0, x, y, 4);
                app.up(0, x, y);
                return;
            }
        }
        throw std::runtime_error("missing settings control " + id);
    };
    click_control("language");
    app.move(0, 300, 320);
    app.scroll(0, 0, 6);
    click_control("choose:language:ja-romaji");
    click_control("apply");
    require(app.selection().language == "ja-romaji", "dropdown activates Japanese language");
    require(sink.events.empty(), "preview settings do not emit input");
    rejects([&] { app.apply({"missing", "en-us", "graphite"}); }, "invalid selection rejected");
    require(app.selection().theme == "midnight", "failed apply retains prior selection");
}
} // namespace
int main() {
    try {
        const auto defaults = fk::load_profiles({}, {});
        require(defaults.errors.empty(), "bundled profiles valid");
        require(defaults.layouts.at("en-us-full").keys.size() == 106, "106 baseline keys");
        state_tests(defaults.layouts.at("en-us-full"));
        profile_tests(defaults);
        app_tests();
        std::cout << "Key sequences, cancellation, repeat, profiles, persistence, language legends and "
                     "rendering passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
