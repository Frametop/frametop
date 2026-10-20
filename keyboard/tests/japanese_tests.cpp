#include "framekeyboard/app.hpp"
#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace framekeyboard;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct Sink : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    std::vector<std::string> delivery;
    bool available = true, accept = true, blocked = false;
    void send(int code, int value) override {
        keys.emplace_back(code, value);
        delivery.push_back("key");
    }
    bool text_available() override { return available; }
    bool output_blocked() const override { return blocked; }
    bool commit_text(const std::string& value) override {
        if (!accept) {
            return false;
        }
        delivery.push_back("commit");
        text.push_back(value);
        return true;
    }
};
void click_key(App& app, const std::string& id, unsigned pointer = 0, bool release = true,
               bool expect_accepted = true) {
    const auto view = app.view();
    for (int y = 96; y < panel_height; y += 2) {
        for (int x = 0; x < panel_width; x += 2) {
            auto* key = app.renderer.hit_key(view, x, y);
            if (key && key->id == id) {
                if (app.down(pointer, x, y, 1) != expect_accepted) {
                    throw std::runtime_error("key acceptance: " + id);
                }
                if (release) {
                    app.up(pointer, x, y);
                }
                return;
            }
        }
    }
    throw std::runtime_error("Missing test key: " + id);
}
void control(App& app, const std::string& id) {
    for (const auto& c : app.view().controls) {
        if (c.id == id) {
            app.down(9, c.bounds.x + 2, c.bounds.y + 2, 1);
            app.up(9, c.bounds.x + 2, c.bounds.y + 2);
            return;
        }
    }
    throw std::runtime_error("Missing test control: " + id);
}
void romaji_legends() {
    char directory[] = "/tmp/framekeyboard-romaji-legends-XXXXXX";
    require(mkdtemp(directory), "temporary legend profile");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{directory};
    fs::create_directories(fs::path(directory) / "languages");
    for (bool overridden : {false, true}) {
        std::ofstream(fs::path(directory) / "languages/ja-test.json")
            << R"({"schema_version":1,"id":"ja-test","name":"Test Romaji","locale":"ja-JP",
                   "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
                   "font_families":["sans-serif"],"input_method":"japanese-romaji",
                   "legends":{"source":"keymap","overrides":)"
            << (overridden ? R"({"KeyK":"s"})" : "{}") << "}}";
        for (bool caps : {false, true}) {
            for (bool shift : {false, true}) {
                Options options;
                options.config_dir = directory;
                options.mode = Mode::Vr;
                options.input = InputBackend::Ei;
                options.start_enabled = true;
                Sink sink;
                App app(options, sink);
                app.apply({"en-us-full", "ja-test", "graphite"});
                if (caps) {
                    click_key(app, "CapsLock");
                }
                if (shift) {
                    click_key(app, "ShiftLeft");
                }
                click_key(app, "KeyK");
                if (shift) {
                    click_key(app, "ShiftLeft");
                }
                click_key(app, "KeyI");
                require(app.view().preedit == "き", "Romaji ignores cosmetic legends with Shift/Caps");
                click_key(app, "Enter");
                require(sink.text == std::vector<std::string>{"き"}, "Romaji commits mapped symbols");
                click_key(app, "Space");
                require(app.view().preedit.empty() && sink.text == std::vector<std::string>{"き", " "},
                        "idle Romaji Space types directly without starting composition");
            }
        }
    }
}
int main() {
    try {
        romaji_legends();
        JapaneseComposer composer;
        for (const auto& [roman, expected] :
             std::vector<std::pair<std::string, std::string>>{{"nihon", "にほん"},
                                                              {"konnichiha", "こんにちは"},
                                                              {"gakkou", "がっこう"},
                                                              {"shin'you", "しんよう"},
                                                              {"nn", "ん"},
                                                              {"kanna", "かんな"},
                                                              {"matcha", "まっちゃ"},
                                                              {"kyou", "きょう"},
                                                              {"shashin", "しゃしん"},
                                                              {"xtsu", "っ"},
                                                              {"ca", "か"},
                                                              {"ci", "し"},
                                                              {"cu", "く"},
                                                              {"ce", "せ"},
                                                              {"co", "こ"},
                                                              {"fya", "ふゃ"},
                                                              {"fyu", "ふゅ"},
                                                              {"fyo", "ふょ"},
                                                              {"tha", "てゃ"},
                                                              {"thi", "てぃ"},
                                                              {"thu", "てゅ"},
                                                              {"the", "てぇ"},
                                                              {"tho", "てょ"},
                                                              {"dha", "でゃ"},
                                                              {"dhi", "でぃ"},
                                                              {"dhu", "でゅ"},
                                                              {"dhe", "でぇ"},
                                                              {"dho", "でょ"},
                                                              {"wu", "う"},
                                                              {"cca", "っか"},
                                                              {"fyu-jon", "ふゅーじょん"},
                                                              {"CA", "か"}}) {
            composer.cancel();
            for (char c : roman) {
                composer.roman(c);
            }
            require(composer.commit_text() == expected, "romaji conversion");
        }
        composer.cancel();
        composer.kana("か");
        composer.kana("\u3099");
        require(composer.preedit() == "が", "dakuten composes");
        composer.script(true);
        require(composer.preedit() == "ガ", "katakana");
        composer.script(false);
        require(composer.preedit() == "が", "hiragana");
        composer.backspace();
        require(composer.empty(), "UTF-8 backspace");
        composer.roman('k');
        composer.backspace();
        require(composer.empty(), "pending romaji backspace");
        // Host developers can point this test at an extracted Anthy package;
        // production code uses only the system library/dictionary.
        void* anthy = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
        if (const auto* dictionary = std::getenv("FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY")) {
            require(anthy, "test Anthy library missing");
            auto override = reinterpret_cast<void (*)(const char*, const char*)>(
                dlsym(anthy, "anthy_conf_override"));
            require(override, "Anthy config API");
            override("DIC_FILE", dictionary);
        }
        if (anthy) {
            composer.cancel();
            for (char c : std::string("nihon")) {
                composer.roman(c);
            }
            composer.convert();
            const auto candidates = composer.candidates();
            const auto found = std::find(candidates.begin(), candidates.end(), "日本");
            require(found != candidates.end(), "Anthy offers 日本");
            composer.choose(static_cast<int>(found - candidates.begin()));
            require(composer.commit_text() == "日本", "candidate commit");
            composer.cycle(1);
            composer.cycle(-1);
            require(composer.commit_text() == "日本", "candidate navigation");
            composer.unconvert();
            require(composer.preedit() == "にほん", "unconvert");
            dlclose(anthy);
        } else {
            require(!std::getenv("FRAMEKEYBOARD_TEST_ANTHY_REQUIRED"), "Anthy required for this run");
            std::cout << "Anthy integration skipped: library absent.\n";
        }
        char directory[] = "/tmp/framekeyboard-japanese-XXXXXX";
        require(mkdtemp(directory), "temporary profile");
        struct Cleanup {
            fs::path path;
            ~Cleanup() { fs::remove_all(path); }
        } cleanup{directory};
        Options options;
        options.config_dir = directory;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.target_language = "en-us";
        Sink sink;
        App app(options, sink);
        app.apply({"en-us-full", "ja-romaji", "graphite"});
        sink.blocked = true;
        click_key(app, "KeyK");
        click_key(app, "KeyI");
        require(app.view().preedit == "き", "output barrier permits local Romaji preedit");
        click_key(app, "Backspace");
        require(app.view().preedit.empty() && sink.keys.empty() && sink.text.empty(),
                "output barrier permits preedit editing without transport output");
        sink.blocked = false;
        for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyO", "KeyN"}) {
            click_key(app, id);
        }
        require(app.view().preedit == "にほn", "local preedit");
        require(sink.keys.empty() && sink.text.empty(), "preedit sends no raw keys or text");
        if (anthy) {
            control(app, "ime-convert");
            if (const auto* directory = std::getenv("FRAMEKEYBOARD_TEST_RENDER_DIR")) {
                app.paint(2);
                app.renderer.write_png(fs::path(directory) / "framekeyboard-ja-candidates.png");
            }
            click_key(app, "Escape");
        }
        app.cancel(false);
        require(!app.view().preedit.empty(), "laser leave keeps preedit");
        click_key(app, "Enter");
        require(sink.text == std::vector<std::string>{"にほん"}, "Enter commits without submit");
        require(sink.keys.empty(), "commit does not submit form");
        click_key(app, "Enter");
        require(sink.keys == std::vector<std::pair<int, int>>{{28, 1}, {28, 0}},
                "empty Enter submits normally");
        click_key(app, "KeyA");
        sink.accept = false;
        control(app, "ime-commit");
        require(app.view().preedit == "あ", "failed delivery retains text");
        sink.accept = true;
        app.cancel();
        require(app.view().preedit.empty(), "target cancellation discards preedit");
        click_key(app, "KeyA");
        require(!app.view().preedit.empty(), "preedit exists before dashboard closes");
        const auto sent_before_hide = sink.text.size();
        app.set_interaction_active(false);
        require(app.view().preedit.empty(), "dashboard close discards Japanese preedit");
        control(app, "ime-commit");
        app.set_interaction_active(true);
        control(app, "ime-commit");
        require(sink.text.size() == sent_before_hide, "no hidden or stale Japanese commit");
        for (const auto* forwarding_key : {"Tab", "Delete", "Home", "End", "Paste"}) {
            click_key(app, "KeyA");
            sink.delivery.clear();
            const auto text_count = sink.text.size();
            click_key(app, forwarding_key);
            require(sink.text.size() == text_count + 1 && sink.text.back() == "あ",
                    "navigation and shortcuts preserve unfinished Japanese text");
            require(sink.delivery.size() >= 3 && sink.delivery.front() == "commit" &&
                        sink.delivery[1] == "key" && app.view().preedit.empty(),
                    "Japanese commit precedes every forwarded key-down");
        }
        click_key(app, "KeyA");
        click_key(app, "ControlLeft", 1, false);
        sink.delivery.clear();
        click_key(app, "KeyA", 2);
        app.up(1, 0, 0);
        require(sink.delivery.front() == "commit" && sink.delivery[1] == "key",
                "commit precedes the modifier as well as the shortcut key");
        for (const auto* forwarding_key : {"Tab", "Delete", "Paste"}) {
            click_key(app, "KeyA");
            sink.accept = false;
            sink.delivery.clear();
            click_key(app, forwarding_key, 0, true, false);
            require(app.view().preedit == "あ" && sink.delivery.empty(),
                    "failed commit retains preedit and blocks focus-changing key");
            sink.accept = true;
            click_key(app, forwarding_key);
            require(app.view().preedit.empty() && sink.delivery.front() == "commit",
                    "retry commits retained text before forwarding");
        }
        click_key(app, "KeyA");
        sink.delivery.clear();
        click_key(app, "Escape");
        require(app.view().preedit.empty() && sink.delivery.empty(),
                "Escape deliberately cancels without committing");
        app.show_settings();
        control(app, "language");
        app.move(0, 300, 320);
        app.scroll(0, 0, -2);
        control(app, "choose:language:ja-kana");
        control(app, "apply");
        require(app.selection().layout == "ja-jis-full" && app.selection().language == "ja-kana",
                "Kana preset applies required layout");
        click_key(app, "KeyT");
        click_key(app, "BracketLeft");
        require(app.view().preedit == "が", "direct kana voiced sound");
        app.cancel();
        click_key(app, "ShiftLeft");
        require(app.view().key_labels.at("KeyE") == "ぃ", "shift kana legend");
        click_key(app, "KeyE");
        require(app.view().preedit == "ぃ", "shift kana input");
        app.cancel();
        sink.keys.clear();
        click_key(app, "ControlLeft", 1, false);
        click_key(app, "KeyA", 2);
        app.up(1, 0, 0);
        require(sink.keys == std::vector<std::pair<int, int>>{{29, 1}, {30, 1}, {30, 0}, {29, 0}},
                "Japanese Ctrl+A remains a physical shortcut");
        app.apply({"ja-jis-full", "ja-jis", "graphite"});
        sink.keys.clear();
        click_key(app, "KeyA");
        require(sink.keys.empty(), "external JIS requires declared JP target");
        options.target_language = "ja-jis";
        App external(options, sink);
        click_key(external, "Convert");
        require(sink.keys == std::vector<std::pair<int, int>>{{92, 1}, {92, 0}},
                "JIS conversion key uses evdev");
        options.start_enabled = false;
        options.target_language = "en-us";
        App disabled(options, sink);
        disabled.apply({"en-us-full", "ja-romaji", "graphite"});
        const auto before = sink.text.size();
        click_key(disabled, "KeyA");
        click_key(disabled, "Enter");
        require(sink.text.size() == before, "IME respects opt-in");
        options.start_enabled = true;
        sink.available = false;
        App unavailable(options, sink);
        click_key(unavailable, "KeyA");
        click_key(unavailable, "Enter");
        require(sink.text.size() == before, "missing text transport cannot emit Japanese");
        unavailable.apply({"en-us-full", "en-us", "graphite"});
        sink.keys.clear();
        click_key(unavailable, "KeyA");
        require(sink.keys.empty(),
                "Unicode English never falls back to a possibly mismatched physical keymap");
        if (anthy) {
            sink.available = true;
            sink.text.clear();
            App converted(options, sink);
            converted.apply({"en-us-full", "ja-romaji", "graphite"});
            for (const auto& id : {"KeyN", "KeyI", "KeyH", "KeyO", "KeyN"}) {
                click_key(converted, id);
            }
            control(converted, "ime-convert");
            std::string choice;
            for (const auto& button : converted.view().controls) {
                if (button.id.starts_with("ime-candidate-") && button.label.ends_with(" 日本")) {
                    choice = button.id;
                }
            }
            require(!choice.empty(), "日本 candidate is clickable");
            control(converted, choice);
            control(converted, "ime-commit");
            require(sink.text == std::vector<std::string>{"日本"},
                    "candidate button commits through text sink");
        }
        std::cout
            << "Japanese composition, candidates, kana, shortcuts, gates and cancellation passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
