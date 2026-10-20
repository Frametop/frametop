#include "framekeyboard/app.hpp"
#include "framekeyboard/startup.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-settings-tests-XXXXXX";
        require(mkdtemp(name.data()) != nullptr, "temporary directory");
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct CaptureSink : KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
Control control(const PanelView& view, const std::string& id) {
    for (const auto& c : view.controls) {
        if (c.id == id) {
            return c;
        }
    }
    throw std::runtime_error("missing control " + id);
}
bool has(const PanelView& view, const std::string& id) {
    return std::any_of(view.controls.begin(), view.controls.end(),
                       [&](const Control& c) { return c.id == id; });
}
void click(App& app, const std::string& id, unsigned pointer = 0) {
    const auto c = control(app.view(), id);
    const double x = c.bounds.x + c.bounds.width / 2, y = c.bounds.y + c.bounds.height / 2;
    require(c.hit(x, y), "test clicks a visible control");
    app.down(pointer, x, y, 1);
    app.up(pointer, x, y);
}
void select(App& app, const std::string& field, const std::string& choice) {
    click(app, field);
    const auto id = "choose:" + field + ":" + choice;
    for (int i = 0; i < 50 && !has(app.view(), id); ++i) {
        const auto popup = *app.view().popup;
        app.move(0, popup.x + 10, popup.y + 10);
        app.scroll(0, 0, -100); // Start at the beginning, regardless of the selection.
        for (int j = 0; j < i && !has(app.view(), id); ++j) {
            app.scroll(0, 0, 1);
        }
    }
    click(app, id);
}
Settings saved(const fs::path& dir) {
    std::vector<std::string> errors;
    auto value = load_settings(dir, errors);
    require(errors.empty(), "saved config parses");
    return value;
}
Settings saved(App& app) {
    app.flush_settings();
    return saved(app.config_dir());
}
std::pair<double, double> key_point(App& app, const std::string& action) {
    const auto view = app.view();
    for (double y = 96; y < panel_height; y += 4) {
        for (double x = 0; x < panel_width; x += 4) {
            if (const auto* key = app.renderer.hit_key(view, x, y); key && key->action == action) {
                return {x, y};
            }
        }
    }
    throw std::runtime_error("missing visible key " + action);
}
void native_num_restore() {
    struct NativeSink : CaptureSink {
        std::optional<bool> lock;
        bool toggle_pending{};
        std::optional<bool> num_lock_state() override { return lock; }
        void send(int code, int value) override {
            CaptureSink::send(code, value);
            if (code == KEY_NUMLOCK && value == 1) {
                toggle_pending = true; // Feedback can arrive after multiple UI frames.
            }
        }
    };
    for (auto input : {InputBackend::Uinput, InputBackend::Ei}) {
        for (bool saved_num : {false, true}) {
            for (bool target_num : {false, true}) {
                TemporaryDirectory directory;
                Settings settings;
                settings.num_lock = saved_num;
                if (input == InputBackend::Ei) {
                    settings.active = {"ja-jis-full", "ja-jis", "graphite"};
                }
                save_settings(directory.path, settings);
                Options options;
                options.config_dir = directory.path;
                options.mode = Mode::Vr;
                options.input = input;
                options.target_language = settings.active.language;
                options.start_enabled = true;
                NativeSink sink;
                sink.lock = target_num;
                App app(options, sink);
                require(sink.events.empty(), "restoring native Num Lock sends no startup input");
                const auto [x, y] = key_point(app, "Numpad1");
                if (target_num != saved_num) {
                    require(!app.down(0, x, y, 1), "mismatched keypad waits for lock feedback");
                    require(!app.down(0, x, y, 1), "repeated press cannot undo pending lock toggle");
                    require(sink.events ==
                                std::vector<std::pair<int, int>>{{KEY_NUMLOCK, 1}, {KEY_NUMLOCK, 0}},
                            "native lock is reconciled exactly once");
                    sink.lock = saved_num;
                }
                sink.events.clear();
                require(app.down(0, x, y, 2), "keypad accepted once local and target locks agree");
                app.up(0, x, y);
                require(sink.events == std::vector<std::pair<int, int>>{{KEY_KP1, 1}, {KEY_KP1, 0}},
                        "native keypad preserves its physical code with matching Num Lock");
                sink.events.clear();
                sink.lock.reset();
                require(!app.down(0, x, y, 3) && sink.events.empty(),
                        "unknown target lock never sends a guessed keypad or toggle");
                for (const auto* action :
                     {"NumpadEnter", "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide"}) {
                    const auto [kx, ky] = key_point(app, action);
                    require(app.down(0, kx, ky, 3), "lock-independent keypad key remains usable");
                    app.up(0, kx, ky);
                    require(sink.events == std::vector<std::pair<int, int>>{{key_code(action), 1},
                                                                            {key_code(action), 0}},
                            "keypad controls do not require target Num Lock feedback");
                    sink.events.clear();
                }
                const auto [nx, ny] = key_point(app, "NumLock");
                require(app.down(0, nx, ny, 3), "native Num Lock toggles the saved local preference");
                app.up(0, nx, ny);
                require(sink.events.empty() && saved(app).num_lock == !saved_num,
                        "native local toggle is persisted without guessing a target lock");
                app.set_interaction_active(false);
                sink.lock = !saved_num;
                require(!app.down(0, x, y, 4) && sink.events.empty(),
                        "hidden input cannot synchronize target locks");
            }
        }
    }
}
void compact_toolbar() {
    for (bool favorites : {false, true}) {
        TemporaryDirectory directory;
        fs::create_directories(directory.path / "layouts");
        std::ofstream(directory.path / "layouts/compact.json")
            << R"({"schema_version":1,"id":"compact","name":"Compact regression","width":4096,"height":100,"keys":[{"id":"a","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}},{"id":"num1","label":"1","x":196,"y":0,"width":3900,"height":80,"action":{"kind":"key","value":"Numpad1"}}]})";
        Settings settings;
        settings.active.layout = "compact";
        settings.hide_numpad = true;
        if (favorites) {
            settings.favorites = {{"en", "English", {}}};
        }
        save_settings(directory.path, settings);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        const auto view = app.view();
        const auto body = panel_case_bounds(view);
        for (std::size_t i = 0; i < view.controls.size(); ++i) {
            if (view.controls[i].style == ControlStyle::MoveHandle) {
                continue; // The movement handle deliberately sits below the case.
            }
            const auto& a = view.controls[i].bounds;
            require(body.contains(a.x, a.y) && body.contains(a.x + a.width - 1, a.y + a.height - 1),
                    "compact case contains every toolbar control");
            for (std::size_t j = i + 1; j < view.controls.size(); ++j) {
                const auto& b = view.controls[j].bounds;
                require(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y ||
                            b.y + b.height <= a.y,
                        "compact toolbar controls cannot overlap");
            }
        }
        click(app, favorites ? "language-cycle" : "settings");
        require(!app.quitting(), "compact Settings and language cycle cannot hit Close");
        require(favorites ? app.selection().layout == "en-us-full" : app.view().settings,
                "compact toolbar action reaches its intended destination");
    }
}
void startup_profile_fallback() {
    for (const auto& selection : {Selection{"missing-layout", "de-de", "graphite"},
                                  Selection{"international-full", "de-de", "missing-theme"}}) {
        TemporaryDirectory directory;
        Settings settings;
        settings.active = selection;
        save_settings(directory.path, settings);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        require(app.selection().language == "de-de" && app.selection().theme == "graphite",
                "startup fallback preserves the usable language and theme");
        require(selection.layout == "missing-layout" || app.selection().layout == selection.layout,
                "missing theme preserves the saved layout");
        const auto profiles = load_profiles({}, {});
        validate_selection(profiles, app.selection());
        click(app, "pin");
        const auto [x, y] = key_point(app, "NumLock");
        app.down(0, x, y, 1);
        app.up(0, x, y);
        const auto persisted = saved(app);
        require(persisted.active == selection && persisted.pinned && persisted.num_lock,
                "ordinary saves retain unavailable saved profiles and update preferences");
        require(!app.view().status.empty(), "fallback explanation survives ordinary saves");
        app.show_settings();
        click(app, "apply");
        require(saved(app).active == app.selection(), "explicit Apply commits the displayed fallback");
    }
}
void incompatible_saved_language() {
    for (const auto* id : {"xx-test", "en-us"}) {
        TemporaryDirectory directory;
        fs::create_directories(directory.path / "languages");
        const auto path = directory.path / "languages" / (std::string(id) + ".json");
        const auto json = std::string(R"({"schema_version":1,"id":")") + id +
                          R"(","name":"No compatible layout","locale":"en-US",
            "keymap":{"rules":"evdev","model":"pc105","layout":"us","variant":"","options":[]},
            "legends":{"source":"keymap","overrides":{}},"font_families":["sans-serif"],
            "required_keys":["Backquote","Convert"]})";
        std::ofstream(path) << json;
        const Selection selection{"en-us-full", id, "graphite"};
        save_selection(directory.path, selection);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        require(app.selection() == Selection{} && app.view().language->required_keys.empty(),
                "unmatchable language falls back to usable embedded English defaults");
        require(!app.view().status.empty(), "last-resort fallback explains the saved selection failure");
        click(app, "pin");
        require(saved(app).active == selection && saved(app).pinned,
                "last-resort fallback preserves saved profiles during ordinary saves");
        std::ifstream original(path);
        require(std::string(std::istreambuf_iterator<char>(original), {}) == json,
                "runtime recovery never rewrites the incompatible profile file");
        app.show_settings();
        require(app.view().settings, "Settings remains reachable to repair the saved profile");
    }
}
void missing_saved_language_status() {
    for (bool malformed : {false, true}) {
        TemporaryDirectory directory;
        if (malformed) {
            fs::create_directories(directory.path / "languages");
            std::ofstream(directory.path / "languages/broken.json") << "{broken";
        }
        const Selection selection{"en-us-full", "removed-language", "graphite"};
        save_selection(directory.path, selection);
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Render;
        CaptureSink sink;
        App app(options, sink);
        const auto message = "Saved language unavailable. Using English.";
        require(app.selection().language == "en-us" && app.view().status == message,
                "missing language reports its own readable fallback reason");
        click(app, "pin");
        require(saved(app).active == selection && app.view().status == message,
                "fallback reason and saved choice survive unrelated preference saves");
    }
}
void key_control_namespaces() {
    TemporaryDirectory directory;
    fs::create_directories(directory.path / "layouts");
    std::ofstream(directory.path / "layouts/collision.json")
        << R"({"schema_version":1,"id":"collision","name":"Colliding IDs","width":300,"height":100,"keys":[
        {"id":"close","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}},
        {"id":"settings","label":"B","x":100,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyB"}},
        {"id":"apply","label":"C","x":200,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyC"}}]})";
    struct TextSink : CaptureSink {
        std::vector<std::string> text;
        bool text_available() override { return true; }
        bool commit_text(const std::string& value) override {
            text.push_back(value);
            return true;
        }
    } sink;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    App app(options, sink);
    app.apply({"collision", "en-us", "graphite"});
    app.paint(1);
    const auto idle = app.renderer.rgba();
    for (const auto* action : {"KeyA", "KeyB", "KeyC"}) {
        const auto [x, y] = key_point(app, action);
        require(app.move(0, x, y), "colliding key hover is still a keyboard target");
        const auto view = app.view();
        const auto id = app.renderer.hit_key(view, x, y)->id;
        require(view.hovered_keys.contains(id) && view.hovered_controls.empty(),
                "key hover does not select a same-named control");
        app.paint(1);
        const auto hovered = app.renderer.rgba();
        for (const auto& control : view.controls) {
            if (control.id != id) {
                continue;
            }
            for (int py = static_cast<int>(control.bounds.y);
                 py < control.bounds.y + control.bounds.height; ++py) {
                for (int px = static_cast<int>(control.bounds.x);
                     px < control.bounds.x + control.bounds.width; ++px) {
                    const auto offset =
                        static_cast<std::size_t>((py + popup_margin) * panel_width + px) * 4;
                    require(std::equal(idle.begin() + offset, idle.begin() + offset + 4,
                                       hovered.begin() + offset),
                            "colliding toolbar pixels stay idle");
                }
            }
        }
        require(app.down(0, x, y, 2), "colliding key press accepted");
        app.up(0, x, y);
        require(!app.quitting() && !app.view().settings, "key cannot invoke toolbar actions");
    }
    require(sink.text == std::vector<std::string>{"a", "b", "c"}, "colliding IDs type normally");
    const auto close = control(app.view(), "close");
    const auto [x, y] = key_point(app, "KeyA");
    app.down(0, close.bounds.x + 2, close.bounds.y + 2, 3);
    require(app.view().hovered_keys.empty() && app.view().hovered_controls.contains("close"),
            "control hover cannot highlight the same-named key");
    app.up(0, x, y);
    require(!app.quitting(), "releasing a toolbar press on a same-named key cannot activate it");
    click(app, "settings");
    require(app.view().settings, "real controls remain usable");
}
void favorite_cache_reload() {
    TemporaryDirectory directory;
    Settings settings;
    settings.favorites = {{"later", "Added later", {"later", "en-us", "graphite"}},
                          {"de", "German", {"international-full", "de-de", "graphite"}}};
    save_settings(directory.path, settings);
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    click(app, "language-cycle");
    require(app.selection().language == "de-de", "cache skips absent favorite profile");
    click(app, "language-cycle");
    fs::create_directories(directory.path / "layouts");
    std::ofstream(directory.path / "layouts/later.json")
        << R"({"schema_version":1,"id":"later","name":"Added later","width":100,"height":100,"keys":[{"id":"a","label":"A","x":0,"y":0,"width":80,"height":80,"action":{"kind":"key","value":"KeyA"}}]})";
    app.reload();
    click(app, "language-cycle");
    require(app.selection().layout == "later", "reload adds newly valid cached favorite");
    app.apply({});
    app.flush_settings(); // This fixture exercises an immediate, idle Reload.
    fs::remove(directory.path / "layouts/later.json");
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de", "reload removes deleted cached favorite");
}
void background_preferences() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto unblock = release.get_future();
    std::vector<Settings> writes;
    auto app = std::make_unique<App>(options, sink, [&](const Settings& settings) {
        if (writes.empty()) {
            entered.set_value();
            unblock.wait(); // Model a stalled fsync without timing-dependent sleeps.
        }
        writes.push_back(settings);
        save_settings(directory.path, settings);
    });
    click(*app, "pin");
    if (started.wait_for(2s) != std::future_status::ready) {
        release.set_value();
        throw std::runtime_error("background save did not start");
    }
    auto input = std::async(std::launch::async, [&] {
        const auto [x, y] = key_point(*app, "NumLock");
        require(app->down(0, x, y, 1), "Num Lock accepts input during stalled save");
        app->up(0, x, y);
        click(*app, "pin");
        app->tick(1);
    });
    const bool responsive = input.wait_for(2s) == std::future_status::ready;
    release.set_value(); // Always unblock cleanup, even when the assertion fails.
    input.get();
    require(responsive, "input does not wait for blocked preference storage");
    app.reset(); // Destruction must drain the last pending snapshot.
    require(writes.size() == 2 && writes.front().pinned && !writes.back().pinned &&
                writes.back().num_lock,
            "ordered writes coalesce pending snapshots and shutdown saves the newest state");
    require(saved(directory.path).num_lock && !saved(directory.path).pinned,
            "shutdown makes the final preferences durable");

    // Failures reach App on its own thread; a later successful save clears them.
    bool fail = true;
    App errors(options, sink, [&](const Settings& settings) {
        if (fail) {
            throw std::runtime_error("test storage failure");
        }
        save_settings(directory.path, settings);
    });
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.find("Applied, but not saved: test storage failure") !=
                std::string::npos,
            "background failure reports unsaved runtime state");
    fail = false;
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.empty(), "a successful retry clears the prior save error");
    std::ofstream(directory.path / "config.json") << "{broken";
    click(errors, "pin");
    errors.flush_settings();
    require(errors.view().status.find("Applied, but not saved:") != std::string::npos,
            "atomic background save refuses malformed config");
    std::ifstream broken(directory.path / "config.json");
    require(std::string((std::istreambuf_iterator<char>(broken)), {}) == "{broken",
            "failed background save preserves malformed config bytes");

    std::promise<void> failing, retry;
    auto failure_started = failing.get_future();
    auto retry_ready = retry.get_future();
    int attempts = 0;
    SettingsWriter writer({}, [&](const Settings&) {
        if (++attempts == 1) {
            failing.set_value();
            retry_ready.wait();
            throw std::runtime_error("obsolete failure");
        }
    });
    writer.enqueue({});
    if (failure_started.wait_for(2s) != std::future_status::ready) {
        retry.set_value();
        throw std::runtime_error("failure test writer did not start");
    }
    writer.enqueue({});
    const bool suppressed = !writer.take_result();
    retry.set_value();
    writer.flush();
    require(suppressed, "pending retry suppresses obsolete completion errors");
    const auto result = writer.take_result();
    require(attempts == 2 && result && result->error.empty(),
            "newest successful snapshot supersedes an older failed write");
}
void deferred_reload() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Vr;
    CaptureSink sink;
    std::promise<void> first_started, first_release, last_started, last_release;
    auto first_writing = first_started.get_future();
    auto first_unblock = first_release.get_future();
    auto last_writing = last_started.get_future();
    auto last_unblock = last_release.get_future();
    std::atomic<bool> timed_out{};
    std::atomic<int> writes{}, reconciliations{};
    App app(
        options, sink,
        [&](const Settings& settings) {
            const auto count = ++writes;
            if (count == 1) {
                first_started.set_value();
                if (first_unblock.wait_for(2s) != std::future_status::ready) {
                    timed_out = true;
                }
            } else if (count == 2) {
                last_started.set_value();
                if (last_unblock.wait_for(2s) != std::future_status::ready) {
                    timed_out = true;
                }
            }
            save_settings(directory.path, settings);
        },
        nullptr, [&](const Settings&) { ++reconciliations; });
    app.flush_settings();
    require(reconciliations == 1, "startup reconciliation completed before the reload fixture");
    app.apply({});
    require(first_writing.wait_for(2s) == std::future_status::ready, "first save started");
    app.frame_completed();
    app.show_settings();
    click(app, "reload");
    require(!timed_out, "Reload returns while the preference writer is blocked");
    for (int i = 0; i < 3; ++i) {
        app.tick(monotonic_seconds());
        app.paint(monotonic_seconds());
        require(app.slow_work_deadline() == 0,
                "waiting Reload keeps frames running without a slow-work allowance");
        app.frame_completed();
    }
    app.reload();
    app.reload(); // Repeated clicks coalesce while waiting.
    require(reconciliations == 1, "waiting reload never reconciles stale settings");
    app.back();
    click(app, "pin");
    app.apply({"en-us-full", "en-us", "midnight"});
    first_release.set_value();
    require(last_writing.wait_for(2s) == std::future_status::ready, "newest save started");
    app.frame_completed();
    app.tick(monotonic_seconds());
    require(!timed_out && reconciliations == 1 && app.slow_work_deadline() == 0,
            "Reload also waits without blocking for a newer queued snapshot");
    last_release.set_value();
    app.flush_settings();
    app.frame_completed();
    app.tick(monotonic_seconds());
    app.flush_settings();
    require(writes == 2 && reconciliations == 2,
            "all queued reload requests complete once after the newest save");
    require(app.pinned() && saved(directory.path).pinned &&
                saved(directory.path).active.theme == "midnight" && app.selection().theme == "midnight",
            "deferred Reload reads the final snapshot and preserves the active profile");
    app.frame_completed();
    app.tick(monotonic_seconds());
    app.flush_settings();
    require(reconciliations == 2 && !timed_out && sink.events.empty(),
            "completed Reload is not retried and sends no input");
}
void deferred_reload_errors() {
    using namespace std::chrono_literals;
    for (const bool storage_failure : {true, false}) {
        TemporaryDirectory directory;
        Options options;
        options.config_dir = directory.path;
        options.mode = Mode::Vr;
        CaptureSink sink;
        std::promise<void> entered, release;
        auto started = entered.get_future();
        auto unblock = release.get_future();
        std::atomic<bool> timed_out{};
        App app(
            options, sink,
            [&](const Settings& settings) {
                entered.set_value();
                timed_out = unblock.wait_for(2s) != std::future_status::ready;
                if (storage_failure) {
                    throw std::runtime_error("test deferred storage failure");
                }
                save_settings(directory.path, settings);
            },
            nullptr, [](const Settings&) {});
        app.flush_settings();
        app.apply({});
        require(started.wait_for(2s) == std::future_status::ready, "error fixture save started");
        app.reload();
        require(!timed_out, "error fixture Reload remains nonblocking");
        if (!storage_failure) {
            fs::create_directories(directory.path / "languages");
            std::ofstream(directory.path / "languages/en-us.json")
                << R"({"schema_version":1,"id":"en-us","name":"Missing physical key",
                       "locale":"en-US","keymap":{"rules":"evdev","model":"pc105",
                       "layout":"us","variant":"","options":[]},
                       "legends":{"source":"keymap","overrides":{}},
                       "font_families":["sans-serif"],
                       "required_keys":["IntlYen"]})";
        }
        release.set_value();
        app.flush_settings();
        app.frame_completed();
        app.tick(monotonic_seconds());
        app.flush_settings(); // A successful startup reconciliation is not a successful save.
        const auto status = app.view().status;
        require(status.find(storage_failure
                                ? "test deferred storage failure"
                                : "Language needs physical key IntlYen") != std::string::npos,
                "deferred Reload preserves save errors and reports preparation failures");
        app.paint(monotonic_seconds());
        app.frame_completed();
        app.tick(monotonic_seconds());
        require(!app.dirty && app.slow_work_deadline() == 0 && app.view().status == status &&
                    app.selection().language == "en-us" &&
                    app.view().language->name != "Missing physical key" && sink.events.empty(),
                "failed Reload keeps the old model and is not retried every frame");
    }
}
void preferences_with_broken_startup() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto root = directory.path / "install";
    const auto binary = root / "releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
    fs::create_symlink("releases/test", root / "current");
    fs::create_directories(units);
    const std::string foreign = "# User-owned unit\n";
    std::ofstream(unit) << foreign;
    auto read = [](const fs::path& path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    Options options;
    options.mode = Mode::Render;
    options.config_dir = config;
    CaptureSink sink;
    App app(options, sink, [&](const Settings& settings) {
        save_preferences_with_startup(settings, binary, config, units, [] {});
    });
    click(app, "pin");
    require(saved(app).pinned && app.view().status.starts_with("Preferences saved."),
            "foreign startup unit does not block a pin save and reports a separate warning");
    const auto [x, y] = key_point(app, "NumLock");
    app.down(0, x, y, 1);
    app.up(0, x, y);
    require(saved(app).num_lock, "Num Lock also saves despite a startup error");
    app.show_settings();
    click(app, "favorite");
    click(app, "auto-leveling");
    click(app, "replace-native-keyboard");
    click(app, "start-with-steamos");
    click(app, "apply");
    const auto partial = saved(app);
    require(partial.pinned && partial.num_lock && !partial.auto_leveling &&
                partial.favorites.size() == 1 && !partial.replace_native_keyboard &&
                !partial.start_with_steamos,
            "unit failure saves unrelated edits but retains both prior startup flags");
    require(app.view().status.starts_with("Other preferences saved.") && read(unit) == foreign &&
                !fs::is_symlink(link),
            "mixed-save warning identifies rejected startup changes without changing foreign unit");
    fs::remove(unit);
    app.show_settings();
    click(app, "apply");
    const auto complete = saved(app);
    require(complete.replace_native_keyboard && complete.start_with_steamos && fs::is_symlink(link) &&
                app.view().status.empty(),
            "retry commits startup flags and unit together and clears the warning");

    const auto previous_unit = read(unit);
    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_exec);
    app.show_settings();
    click(app, "start-with-steamos");
    click(app, "apply");
    app.flush_settings();
    fs::permissions(config, fs::perms::owner_all);
    require(app.view().status.starts_with("Applied, but not saved:") && saved(app).start_with_steamos &&
                read(unit) == previous_unit && fs::is_symlink(link),
            "storage failure still reports unsaved preferences and rolls startup back atomically");
    require(sink.events.empty(), "startup/save fixtures never send desktop input");

    // Development binaries also save ordinary edits when startup cannot be enabled.
    const auto development = directory.path / "development/framekeyboard";
    fs::create_directories(development.parent_path());
    std::ofstream(development) << "fixture\n";
    const auto private_config = directory.path / "development-config";
    Settings requested;
    requested.num_lock = requested.replace_native_keyboard = requested.start_with_steamos = true;
    bool warned = false;
    try {
        save_preferences_with_startup(requested, development, private_config,
                                      directory.path / "development-units", [] {});
    } catch (const StartupPreferencesWarning&) {
        warned = true;
    }
    const auto development_saved = saved(private_config);
    require(warned && development_saved.num_lock && !development_saved.replace_native_keyboard &&
                !development_saved.start_with_steamos,
            "unsupported release layout cannot block ordinary saves or persist startup flags");
}
void reload_startup() {
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto binary = directory.path / "install/releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "fixture\n";
    fs::create_symlink("releases/test", directory.path / "install/current");
    auto bytes = [&] {
        std::ifstream stream(config / "config.json");
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    Settings settings;
    settings.replace_native_keyboard = true;
    save_settings(config, settings);
    Options options;
    options.config_dir = config;
    options.mode = Mode::Vr;
    CaptureSink sink;
    int writes = 0, reconciliations = 0;
    bool fail = false;
    App app(
        options, sink, [&](const Settings&) { ++writes; }, nullptr,
        [&](const Settings& updated) {
            ++reconciliations;
            if (fail) {
                throw std::runtime_error("test manager unavailable");
            }
            configure_startup(updated, binary, config, units, [] {});
        });
    app.flush_settings();
    require(reconciliations == 1 && writes == 0 && !fs::exists(unit),
            "VR startup reconciles saved settings without writing JSON");
    const auto original = app.selection();
    const auto normal_status = app.view().status;
    settings.start_with_steamos = true;
    settings.active.layout = settings.active.language = "en-gb";
    save_settings(config, settings);
    auto before = bytes();
    app.reload();
    app.flush_settings();
    require(app.start_with_steamos() && fs::exists(unit) && fs::is_symlink(link),
            "Reload enables the startup unit for externally edited saved preferences");
    require(writes == 0 && reconciliations == 2 && bytes() == before &&
                app.selection().layout == original.layout &&
                app.selection().language == original.language,
            "Reload reconciles without saving JSON or activating an edited profile");
    settings.replace_native_keyboard = false;
    save_settings(config, settings);
    before = bytes();
    app.reload();
    app.flush_settings();
    require(!app.start_with_steamos() && !fs::exists(unit) && !fs::is_symlink(link) &&
                bytes() == before && writes == 0 && reconciliations == 3,
            "Reload also removes startup when replacement is disabled without changing JSON");
    settings.replace_native_keyboard = true;
    save_settings(config, settings);
    fail = true;
    app.reload();
    app.flush_settings();
    require(app.view().status == "SteamOS startup not configured: test manager unavailable" &&
                !fs::exists(unit),
            "Reload reports a startup reconciliation error distinctly from an unsaved config");
    fail = false;
    app.reload();
    app.flush_settings();
    require(app.view().status == normal_status && fs::exists(unit) && writes == 0 &&
                reconciliations == 5,
            "Reload retries reconciliation and clears its error");
    std::ofstream(config / "config.json") << "{broken";
    app.reload();
    app.flush_settings();
    require(reconciliations == 5 && bytes() == "{broken" && fs::exists(unit),
            "malformed external settings cannot reconfigure startup or be overwritten");
    require(sink.events.empty(), "startup reconciliation sends no input");
}
void launch_startup() {
    using namespace std::chrono_literals;
    TemporaryDirectory directory;
    const auto config = directory.path / "config";
    const auto binary = directory.path / "install/releases/test/bin/framekeyboard";
    const auto units = directory.path / "units";
    const auto unit = units / "framekeyboard-autostart.service";
    const auto link = units / "steamvr.service.wants/framekeyboard-autostart.service";
    const auto legacy = units / "gamescope-session.target.wants/framekeyboard-autostart.service";
    fs::create_directories(binary.parent_path());
    std::ofstream(binary) << "fixture\n";
    fs::create_symlink("releases/test", directory.path / "install/current");
    fs::create_directories(legacy.parent_path());
    const std::string old_unit = "# Managed by Full Keyboard\n[Service]\nRestart=on-failure\n";
    std::ofstream(unit) << old_unit;
    fs::create_symlink("../framekeyboard-autostart.service", legacy);
    Settings settings;
    settings.replace_native_keyboard = settings.start_with_steamos = true;
    save_settings(config, settings);
    auto read = [](const fs::path& path) {
        std::ifstream stream(path);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    const auto before = read(config / "config.json");
    Options options;
    options.mode = Mode::Vr;
    options.config_dir = config;
    CaptureSink sink;
    int writes = 0, reloads = 0, reconciliations = 0;
    std::promise<void> entered, release;
    auto started = entered.get_future(), unblock = release.get_future();
    App app(
        options, sink, [&](const Settings&) { ++writes; }, nullptr,
        [&](const Settings& updated) {
            ++reconciliations;
            entered.set_value();
            unblock.wait();
            configure_startup(updated, binary, config, units, [&] { ++reloads; });
        });
    const bool queued = started.wait_for(2s) == std::future_status::ready;
    release.set_value(); // Always unblock destruction before asserting.
    app.flush_settings();
    require(queued && reconciliations == 1 && reloads == 1 && fs::is_symlink(link) &&
                !fs::is_symlink(legacy) && read(unit).find("Restart=always") != std::string::npos,
            "VR launch asynchronously migrates an owned older service without Apply or Reload");
    require(writes == 0 && read(config / "config.json") == before && sink.events.empty(),
            "startup migration preserves saved bytes and sends no input");
    auto reconcile = [&](const Settings& updated) {
        ++reconciliations;
        configure_startup(updated, binary, config, units, [&] { ++reloads; });
    };
    {
        App again(options, sink, {}, nullptr, reconcile);
        again.flush_settings();
        require(reconciliations == 2 && reloads == 1,
                "matching startup configuration skips manager reload on the next launch");
    }
    options.mode = Mode::Preview;
    {
        App preview(options, sink, {}, nullptr, reconcile);
        preview.flush_settings();
        require(reconciliations == 2, "preview launch never reconciles session startup");
    }
    options.mode = Mode::Vr;
    std::ofstream(config / "config.json") << "{broken";
    {
        App malformed(options, sink, {}, nullptr, reconcile);
        malformed.flush_settings();
        require(reconciliations == 2 && fs::is_symlink(link) &&
                    read(config / "config.json") == "{broken",
                "invalid saved settings cannot remove startup or be overwritten on launch");
    }
}
void numpad_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    const auto full_body = panel_case_bounds(app.view());
    require(full_body.width == panel_width && !app.view().keyboard->num(), "default numpad state");
    const auto [x, y] = key_point(app, "NumLock");
    require(app.down(0, x, y, 1), "Num Lock accepted");
    require(app.down(1, x, y, 1), "second Num Lock pointer accepted");
    require(app.view().keyboard->num() && saved(app).num_lock,
            "Num Lock saves once during simultaneous controller holds");
    app.up(0, x, y);
    app.up(1, x, y);
    {
        App reopened(options, sink);
        require(reopened.view().keyboard->num(), "Num Lock survives restart without target input");
    }
    app.show_settings();
    click(app, "hide-numpad");
    require(control(app.view(), "hide-numpad").selected && !saved(app).hide_numpad,
            "Hide numpad stays pending until Apply");
    app.back();
    app.show_settings();
    require(!control(app.view(), "hide-numpad").selected, "Back discards Hide numpad edits");
    click(app, "hide-numpad");
    click(app, "apply");
    require(saved(app).hide_numpad && app.view().hide_numpad, "Apply saves global numpad visibility");
    const auto narrow = panel_case_bounds(app.view());
    require(narrow.width < full_body.width && narrow.x > 0, "hidden numpad narrows centered case");
    app.paint(2);
    const auto pixels = app.renderer.rgba();
    require(pixels[static_cast<std::size_t>((300 + popup_margin) * panel_width + 20) * 4 + 3] == 0,
            "trimmed case pixels are transparent");
    const auto hidden_view = app.view();
    for (double yy = 96; yy < panel_height; yy += 4) {
        for (double xx = 0; xx < panel_width; xx += 4) {
            const auto* key = app.renderer.hit_key(hidden_view, xx, yy);
            require(!key || (key->action != "NumLock" && !key->action.starts_with("Numpad")),
                    "hidden numpad has no hit regions");
        }
    }
    const auto close = control(app.view(), "close");
    require(close.bounds.x + close.bounds.width < narrow.x + narrow.width,
            "close button follows narrower case");
    const auto profiles = load_profiles({}, {});
    for (const auto& [id, layout] : profiles.layouts) {
        app.apply({id,
                   id == "ja-jis-full"     ? "ja-jis"
                   : id == "br-abnt2-full" ? "pt-br"
                                           : "en-us",
                   "graphite"});
        require(app.view().hide_numpad && app.view().keyboard->num() &&
                    panel_case_bounds(app.view()).width < full_body.width,
                "global visibility and Num Lock survive every bundled layout change");
    }
    app.apply({"en-us-full", "en-us", "graphite"});
    auto settings = saved(app);
    settings.favorites = {{"de", "German", {"international-full", "de-de", "graphite"}}};
    save_settings(directory.path, settings);
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de" && app.view().hide_numpad && app.view().keyboard->num(),
            "favorites keep global numpad state");
    {
        App reopened(options, sink);
        require(reopened.view().hide_numpad && reopened.view().keyboard->num(),
                "both numpad settings survive restart");
    }
    app.show_settings();
    click(app, "hide-numpad");
    click(app, "apply");
    require(panel_case_bounds(app.view()).width == full_body.width && app.view().keyboard->num(),
            "showing numpad restores width and prior Num Lock state");
    const auto [nx, ny] = key_point(app, "NumLock");
    app.down(0, nx, ny, 3);
    app.up(0, nx, ny);
    require(!saved(app).num_lock, "Num Lock off is persisted too");
    require(sink.events.empty(), "numpad settings tests deliver no desktop input");
    app.paint(3);
    app.renderer.write_png("/tmp/framekeyboard-numpad-visible.png");
    app.show_settings();
    app.paint(4);
    app.renderer.write_png("/tmp/framekeyboard-numpad-settings.png");
    click(app, "hide-numpad");
    click(app, "apply");
    app.paint(5);
    app.renderer.write_png("/tmp/framekeyboard-numpad-hidden.png");
}
void app_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::RenderSettings;
    CaptureSink sink;
    App app(options, sink);
    require(!has(app.view(), "language-cycle"), "no favorites hides language cycle");
    require(control(app.view(), "settings").bounds.x < control(app.view(), "size-larger").bounds.x &&
                control(app.view(), "size-larger").bounds.x < control(app.view(), "pin").bounds.x,
            "pin follows zoom buttons");
    const auto unpinned_icon = control(app.view(), "pin").icon;
    click(app, "pin");
    require(app.pinned() && saved(app).pinned, "pin queues persistent state immediately");
    require(!control(app.view(), "pin").selected, "pin active state changes icon without background");
    require(control(app.view(), "pin").icon != unpinned_icon, "pin has distinct on/off icons");
    app.set_dragging(true);
    require(app.view().status.empty(), "pin refuses drag capture");
    {
        App reopened(options, sink);
        require(reopened.pinned(), "pin survives restart");
    }
    click(app, "pin");
    require(!app.pinned(), "pin can be disabled");

    app.show_settings();
    require(app.view().cards.size() == 3 && app.view().cards[1].title == "General" &&
                app.view().cards[2].title == "Experimental",
            "production settings contains Languages and Layouts, General and Experimental cards");
    require(has(app.view(), "apply") && has(app.view(), "reload"), "settings toolbar actions");
    const auto update_button = control(app.view(), "updates");
    require(update_button.label == "Check for updates" &&
                update_button.bounds.x + update_button.bounds.width / 2 == panel_width / 2 &&
                update_button.bounds.y == 12,
            "update button is centered in the settings header");
    require(!has(app.view(), "close") && !has(app.view(), "recenter"), "settings toolbar is focused");
    click(app, "language");
    require(app.view().popup.has_value(), "dropdown opens");
    auto menu_count = [](const PanelView& v) {
        return std::count_if(v.controls.begin(), v.controls.end(),
                             [](const Control& c) { return c.style == ControlStyle::MenuItem; });
    };
    require(menu_count(app.view()) == 5, "dropdown shows five choices");
    const auto popup = *app.view().popup;
    const auto before = app.view().controls;
    app.move(0, popup.x + 5, popup.y + 5);
    app.scroll(0, 0, 1);
    require(app.view().controls != before && menu_count(app.view()) == 5,
            "dropdown scroll changes visible rows");
    // Dismissing on Apply must consume the click, leaving pending edits unsaved.
    click(app, "apply");
    require(!app.view().popup && app.view().settings, "outside click dismisses without applying");
    click(app, "language");
    app.back();
    require(!app.view().popup && app.view().settings, "Escape dismisses menu first");
    app.back();
    require(!app.view().settings, "Escape then exits settings");

    app.show_settings();
    select(app, "language", "de-de");
    require(control(app.view(), "layout").label != "US full-size with left clipboard keys",
            "language selection chooses compatible geometry");
    select(app, "layout", "international-full");
    click(app, "favorite");
    require(control(app.view(), "favorite").selected, "favorite checkbox reflects pending pair");
    app.back();
    require(saved(app).favorites.empty(), "Back discards unsaved favorite edits");
    app.show_settings();
    select(app, "language", "de-de");
    select(app, "layout", "international-full");
    click(app, "favorite");
    select(app, "theme", "midnight");
    // Save English as default with German retained in the pending favorite set.
    select(app, "language", "en-us");
    select(app, "layout", "en-us-full");
    click(app, "apply");
    auto settings = saved(app);
    require(settings.favorites.size() == 1 && settings.active.language == "en-us" &&
                settings.active.theme == "midnight",
            "Apply persists default, theme and favorite changes together");
    require(has(app.view(), "language-cycle"), "favorite exposes main cycle button");
    require(control(app.view(), "pin").bounds.x < control(app.view(), "language-cycle").bounds.x,
            "language cycle follows pin and zoom buttons");
    click(app, "language-cycle");
    require(app.selection().language == "de-de" && app.selection().theme == "midnight",
            "cycle activates favorite pair while retaining current theme");
    require(saved(app).active.language == "en-us", "cycling does not overwrite saved default");
    click(app, "pin");
    require(saved(app).active.language == "en-us", "pin save preserves default during favorite cycle");
    click(app, "language-cycle");
    require(app.selection().language == "en-us", "cycle returns to saved default");
    settings = saved(app);
    settings.favorites.insert(settings.favorites.begin(),
                              {"absent", "Removed profile", {"missing", "missing", "graphite"}});
    settings.favorites.insert(settings.favorites.begin(),
                              {"korean", "Korean", {"en-us-full", "ko-kr", "graphite"}});
    save_settings(directory.path, settings);
    app.reload();
    click(app, "language-cycle");
    require(app.selection().language == "de-de" || app.selection().language == "ko-kr",
            "missing favorite profile or optional engine does not block cycling");
    // Reset to the original favorite for the following deduplication check.
    settings.favorites.erase(settings.favorites.begin(), settings.favorites.begin() + 2);
    save_settings(directory.path, settings);
    app.reload();
    // A favorite of the saved default is a single cycle entry, even if duplicated
    // in a legacy file with a different theme.
    app.show_settings();
    select(app, "language", "de-de");
    select(app, "layout", "international-full");
    click(app, "apply");
    settings = saved(app);
    settings.favorites.push_back(
        {"duplicate", "duplicate", {"international-full", "de-de", "graphite"}});
    save_settings(directory.path, settings);
    app.reload();
    const auto same = app.selection();
    click(app, "language-cycle");
    require(app.selection() == same, "one unique favorite equal to default does not cycle");
    app.show_settings();
    require(control(app.view(), "favorite").selected,
            "legacy favorites recognized by layout/language pair");
    click(app, "favorite");
    click(app, "apply");
    require(saved(app).favorites.empty() && !has(app.view(), "language-cycle"),
            "uncheck removes all duplicates and hides main cycle");

    // Reload closes a menu, cancels a pressed choice, and discards staged edits.
    app.show_settings();
    click(app, "favorite");
    click(app, "language");
    // Corrupt config reload keeps the prior valid pin/favorites state and reports
    // the error, while clearing all pending controls.
    const auto config_path = directory.path / "config.json";
    std::ifstream original_file(config_path);
    const std::string original_config((std::istreambuf_iterator<char>(original_file)), {});
    std::ofstream(config_path) << "{broken";
    const bool was_pinned = app.pinned();
    app.reload();
    require(app.pinned() == was_pinned && !app.view().status.empty(),
            "invalid reload retains working settings");
    std::ofstream(config_path) << original_config;
    click(app, "language");
    auto c = control(app.view(), "choose:language:de-de");
    app.down(1, c.bounds.x + 5, c.bounds.y + 5, 1);
    require(app.pointer_pressed(1), "settings controls participate in pointer cancellation");
    app.reload();
    app.up(1, c.bounds.x + 5, c.bounds.y + 5);
    require(!app.view().popup && !control(app.view(), "favorite").selected && !app.pointer_pressed(1),
            "reload clears popup, captures and staged favorites");
    click(app, "language");
    c = control(app.view(), "choose:language:de-de");
    app.down(1, c.bounds.x + 5, c.bounds.y + 5, 1);
    app.move(0, c.bounds.x + 5, c.bounds.y + 5);
    app.scroll(0, 0, 1);
    app.up(1, c.bounds.x + 5, c.bounds.y + 5);
    require(app.view().popup.has_value(), "scroll invalidates a held menu choice");
    app.set_interaction_active(false);
    require(!app.view().popup && !app.pointer_pressed(1), "hide cancels popup and all UI captures");
    app.set_interaction_active(true);
    require(sink.events.empty(), "settings interaction never emits input in previews");

    // Unknown top-level config survives writes; malformed config is preserved.
    std::ifstream input(directory.path / "config.json");
    std::string data((std::istreambuf_iterator<char>(input)), {});
    data.insert(data.find('{') + 1, "\"custom_setting\":42,");
    std::ofstream(directory.path / "config.json") << data;
    save_settings(directory.path, settings);
    click(app, "settings"); // Back to main.
    click(app, "pin");
    app.flush_settings();
    std::ifstream updated(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(updated), {});
    require(data.find("custom_setting") != std::string::npos, "settings save preserves unknown fields");
    std::ofstream(directory.path / "config.json") << "{broken";
    click(app, "pin");
    app.flush_settings();
    require(app.view().status.find("not saved") != std::string::npos, "pin reports failed persistence");
    std::ifstream malformed(directory.path / "config.json");
    data.assign(std::istreambuf_iterator<char>(malformed), {});
    require(data == "{broken", "malformed config is never overwritten");
}
void behavior_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!app.use_drag_to_move() && app.allows_movement(MoveSource::Laser) &&
                !app.allows_movement(MoveSource::Grip),
            "missing preference defaults to laser movement with grip disabled");
    const auto handle = panel_move_handle_bounds(app.view());
    require(handle && handle->y > panel_height && handle->x + handle->width / 2 == panel_width / 2,
            "default movement handle is centered below the keyboard");
    const int x = static_cast<int>(handle->x + handle->width / 2);
    const int y = static_cast<int>(handle->y + handle->height / 2);
    auto handle_alpha = [&] {
        const auto rgba = app.renderer.rgba();
        return rgba[static_cast<std::size_t>((y + popup_margin) * panel_width + x) * 4 + 3];
    };
    app.paint(1);
    require(handle_alpha() > 0 && !app.renderer.hit_key(app.view(), x, y),
            "movement line paints below the case and cannot type a key");
    app.renderer.write_png("/tmp/framekeyboard-behavior-laser.png");
    click(app, "move-handle");
    app.set_dragging(true);
    require(app.view().status == "Release trigger to place keyboard",
            "laser movement has the correct release instruction");
    app.set_dragging(false);
    app.show_settings();
    const auto settings_view = app.view();
    require(settings_view.cards.size() == 3 && settings_view.cards[1].title == "General" &&
                !control(settings_view, "use-drag-to-move").selected,
            "General card has an unchecked movement preference");
    const auto numpad_bounds = control(settings_view, "hide-numpad").bounds;
    const auto movement_bounds = control(settings_view, "use-drag-to-move").bounds;
    const auto leveling_bounds = control(settings_view, "auto-leveling").bounds;
    require(numpad_bounds.x == movement_bounds.x && movement_bounds.x == leveling_bounds.x &&
                movement_bounds.y - numpad_bounds.y == leveling_bounds.y - movement_bounds.y,
            "General checkboxes form one evenly spaced vertical column");
    for (const auto* text : {"Hide numpad", "Use Drag to Move", "Auto leveling"}) {
        require(std::any_of(settings_view.field_labels.begin(), settings_view.field_labels.end(),
                            [&](const FieldLabel& field) {
                                return field.text == text && field.left &&
                                       field.bounds.x > movement_bounds.x + movement_bounds.width;
                            }),
                "General checkbox labels are on the right");
    }
    app.paint(2);
    app.renderer.write_png("/tmp/framekeyboard-behavior-settings.png");
    click(app, "use-drag-to-move");
    require(control(app.view(), "use-drag-to-move").selected && !app.use_drag_to_move(),
            "movement preference remains staged until Apply");
    app.back();
    app.show_settings();
    require(!control(app.view(), "use-drag-to-move").selected, "Back discards a staged movement change");
    click(app, "use-drag-to-move");
    click(app, "apply");
    require(app.use_drag_to_move() && saved(app).use_drag_to_move &&
                app.allows_movement(MoveSource::Grip) && !app.allows_movement(MoveSource::Laser) &&
                !panel_move_handle_bounds(app.view()),
            "Apply enables grab, saves it and removes the line hit region");
    app.paint(3);
    require(handle_alpha() == 0, "grab mode clears the old line pixels");
    {
        App reopened(options, sink);
        require(reopened.use_drag_to_move() && !panel_move_handle_bounds(reopened.view()),
                "grab preference survives restart");
    }
    click(app, "pin");
    require(!app.allows_movement(MoveSource::Grip) && !app.allows_movement(MoveSource::Laser) &&
                !panel_move_handle_bounds(app.view()),
            "pin blocks both movement methods in grab mode");
    click(app, "pin");
    require(app.allows_movement(MoveSource::Grip) && !panel_move_handle_bounds(app.view()),
            "unpinning retains grab mode");
    app.show_settings();
    click(app, "use-drag-to-move");
    click(app, "hide-numpad");
    click(app, "apply");
    require(!saved(app).use_drag_to_move && panel_move_handle_bounds(app.view()) &&
                panel_move_handle_bounds(app.view())->x + handle->width / 2 == panel_width / 2,
            "laser handle stays centered on the compact case");
    click(app, "pin");
    require(!panel_move_handle_bounds(app.view()) && !app.allows_movement(MoveSource::Laser) &&
                !app.allows_movement(MoveSource::Grip),
            "pin hides the line and blocks both methods in laser mode");
    app.paint(4);
    require(handle_alpha() == 0, "pin leaves no line pixels");
    app.renderer.write_png("/tmp/framekeyboard-behavior-pinned.png");
    click(app, "pin");
    app.paint(5);
    require(handle_alpha() > 0 && app.allows_movement(MoveSource::Laser),
            "unpinning restores the line and laser movement");
    app.renderer.write_png("/tmp/framekeyboard-behavior-compact.png");
    auto settings = saved(app);
    settings.use_drag_to_move = true;
    save_settings(directory.path, settings);
    app.reload();
    require(app.use_drag_to_move() && !panel_move_handle_bounds(app.view()),
            "Reload applies the saved movement preference");
    require(sink.events.empty(), "behavior tests deliver no desktop input");

    MoveCapture capture;
    require(capture.begin(MoveSource::Laser, 1, 0) && capture.owns_pointer(1, 0),
            "laser capture records the controller and pointer");
    require(!capture.begin(MoveSource::Grip, 2) && !capture.begin(MoveSource::Laser, 2, 1) &&
                !capture.owns_pointer(2, 0) && !capture.owns_pointer(1, 1),
            "a second hand or pointer cannot steal or release movement");
    capture.reset();
    require(!capture.active() && !capture.owns_pointer(1, 0) && capture.begin(MoveSource::Grip, 2) &&
                !capture.owns_pointer(2, 0),
            "cancellation clears capture and laser release cannot end a grip");
    capture.reset();
    require(capture.begin(MoveSource::Laser, 0, 0) && capture.owns_pointer(0, 0) &&
                !capture.owns_pointer(1, 0) && !capture.begin(MoveSource::Laser, 1, 0),
            "AUX head capture owns HMD device zero even when a controller uses the same cursor");
    capture.reset();
    require(!capture.owns_pointer(0, 0), "AUX release or tracking loss clears head capture");
}
void favorite_schema() {
    TemporaryDirectory directory;
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"favorites":[{"id":"pair","name":"Pair only","layout":"international-full","language":"de-de"}]})";
    const auto config = saved(directory.path);
    require(!config.use_drag_to_move, "legacy configs default to laser movement");
    require(config.auto_leveling, "legacy configs default to Auto leveling enabled");
    require(config.favorites.size() == 1 && config.favorites.front().selection.language == "de-de",
            "favorite pair does not require a legacy theme field");
}
void auto_leveling_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(app.auto_leveling(), "Auto leveling is enabled on a new install");
    app.show_settings();
    require(control(app.view(), "auto-leveling").selected, "Auto leveling starts checked");
    click(app, "auto-leveling");
    require(app.auto_leveling() && !control(app.view(), "auto-leveling").selected,
            "Auto leveling stays pending until Apply");
    app.back();
    app.show_settings();
    require(control(app.view(), "auto-leveling").selected, "Back discards Auto leveling edits");
    click(app, "auto-leveling");
    click(app, "apply");
    require(!app.auto_leveling() && !saved(app).auto_leveling, "Apply disables and saves Auto leveling");
    {
        App reopened(options, sink);
        require(!reopened.auto_leveling(), "disabled Auto leveling survives restart");
    }
    click(app, "pin");
    require(!saved(app).auto_leveling, "immediate pin save preserves disabled Auto leveling");
    auto settings = saved(app);
    settings.auto_leveling = true;
    save_settings(directory.path, settings);
    app.show_settings();
    click(app, "reload");
    require(app.auto_leveling() && control(app.view(), "auto-leveling").selected,
            "Reload applies saved Auto leveling and refreshes its checkbox");
    require(sink.events.empty(), "Auto leveling settings send no desktop input");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"auto_leveling":"false"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "Auto leveling requires a JSON boolean");
}
void native_replacement_settings() {
    TemporaryDirectory directory;
    Options options;
    options.config_dir = directory.path;
    options.mode = Mode::Render;
    CaptureSink sink;
    App app(options, sink);
    require(!app.replace_native_keyboard(), "replacement defaults off");
    app.show_settings();
    const auto view = app.view();
    const auto experimental = view.cards.back().bounds;
    const auto startup = control(view, "start-with-steamos");
    require(!startup.enabled && !startup.selected &&
                !startup.hit(startup.bounds.x + 4, startup.bounds.y + 4),
            "SteamOS startup defaults off and cannot be pressed without replacement");
    app.down(0, startup.bounds.x + 4, startup.bounds.y + 4, 1);
    app.up(0, startup.bounds.x + 4, startup.bounds.y + 4);
    require(!control(app.view(), "start-with-steamos").selected,
            "pressing a disabled startup checkbox cannot opt in");
    const auto checkbox = control(view, "replace-native-keyboard").bounds;
    const auto label =
        std::find_if(view.field_labels.begin(), view.field_labels.end(),
                     [](const FieldLabel& f) { return f.text == "Replace native keyboard:"; });
    require(view.cards.back().title == "Experimental" && label != view.field_labels.end(),
            "replacement belongs to Experimental");
    require(label->left && label->bounds.x == experimental.x + 30 &&
                checkbox.x > label->bounds.x + label->bounds.width,
            "experimental label has a small inset and precedes checkbox");
    click(app, "replace-native-keyboard");
    require(!app.replace_native_keyboard(), "replacement edit stays pending");
    require(control(app.view(), "start-with-steamos").enabled,
            "pending replacement enables startup editing");
    click(app, "start-with-steamos");
    app.back();
    app.show_settings();
    require(!control(app.view(), "replace-native-keyboard").selected, "Back discards replacement edit");
    require(!control(app.view(), "start-with-steamos").selected, "Back discards startup edits too");
    click(app, "replace-native-keyboard");
    click(app, "start-with-steamos");
    click(app, "apply");
    require(app.replace_native_keyboard() && saved(app).replace_native_keyboard,
            "Apply persists replacement");
    require(app.start_with_steamos() && saved(app).start_with_steamos, "Apply persists startup opt-in");
    app.show_settings();
    click(app, "replace-native-keyboard");
    require(!control(app.view(), "start-with-steamos").enabled &&
                !control(app.view(), "start-with-steamos").selected,
            "unchecking replacement disables and unchecks effective startup");
    click(app, "apply");
    require(!app.start_with_steamos(), "saved replacement-off cannot autostart");
    app.show_settings();
    click(app, "replace-native-keyboard");
    require(control(app.view(), "start-with-steamos").enabled &&
                control(app.view(), "start-with-steamos").selected,
            "reenabling replacement restores the saved startup choice");
    click(app, "apply");
    app.set_native_session(true);
    app.show_settings();
    click(app, "native-fallback");
    require(app.take_native_fallback() && !app.take_native_fallback(), "escape is consumed once");
    require(saved(app).replace_native_keyboard, "escape does not disable future replacement requests");
    App reopened(options, sink);
    require(reopened.replace_native_keyboard(), "replacement survives restart");
    auto settings = saved(app);
    settings.replace_native_keyboard = false;
    save_settings(directory.path, settings);
    click(app, "reload");
    require(!app.replace_native_keyboard(), "Reload disables replacement");
    require(!has(app.view(), "native-fallback"), "escape is absent when replacement is off");
    std::ofstream(directory.path / "config.json")
        << R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"replace_native_keyboard":"true"})";
    std::vector<std::string> errors;
    load_settings(directory.path, errors);
    require(!errors.empty(), "replacement requires JSON boolean");
    require(sink.events.empty(), "replacement settings send no desktop input");
}
void fractional_dropdown_scroll() {
    SettingsUi ui;
    SettingField field{"field", "Field", "c0"};
    for (int i = 0; i < 20; ++i) {
        field.choices.push_back({"c" + std::to_string(i), "Choice " + std::to_string(i)});
    }
    const std::vector<SettingsCard> cards{{"card", "Card", {field}}};
    ui.set_cards(cards);
    auto snapshot = [&] {
        PanelView view;
        ui.append(view);
        return view;
    };
    ui.toggle("field");
    const auto popup = *snapshot().popup;
    auto scroll = [&](double dy) { return ui.scroll(popup.x + 5, popup.y + 5, 0, dy); };
    for (int i = 0; i < 100; ++i) {
        scroll(.1);
    }
    require(has(snapshot(), "choose:field:c10") && !has(snapshot(), "choose:field:c9"),
            "one hundred fractional events move ten rows");
    for (int i = 0; i < 100; ++i) {
        scroll(-.1);
    }
    require(has(snapshot(), "choose:field:c0"), "negative fractional input returns to first row");
    scroll(1e300);
    scroll(.9); // Overscroll must not delay reversing direction.
    require(scroll(-1) && has(snapshot(), "choose:field:c14"), "reverse responds at bottom limit");
    scroll(-1e300);
    scroll(-.9);
    require(scroll(1) && !has(snapshot(), "choose:field:c0"), "reverse responds at top limit");
    require(!scroll(std::numeric_limits<double>::quiet_NaN()) &&
                !scroll(std::numeric_limits<double>::infinity()),
            "nonfinite fractional input is ignored");
    ui.close_popup();
    ui.toggle("field");
    scroll(.6);
    ui.close_popup();
    ui.toggle("field");
    require(!scroll(.5) && has(snapshot(), "choose:field:c0"), "new menu discards old remainder");
    require(scroll(.5), "new menu accumulates its own fractional motion");
    scroll(.6);
    ui.set_cards(cards);
    require(!scroll(.5), "profile changes discard fractional remainder");
    ui.close_popup();
    ui.toggle("field");
    scroll(.6);
    const auto bar = control(snapshot(), "scroll-menu");
    require(ui.down(0, bar.bounds.x + 2, bar.bounds.y + 2), "menu scrollbar captures pointer");
    ui.up(0);
    require(!scroll(.5), "scrollbar capture discards wheel remainder");
}
void scroll_cards() {
    SettingsUi ui;
    std::vector<SettingsCard> cards;
    for (int i = 0; i < 5; ++i) {
        SettingsCard card{"test-" + std::to_string(i), "Test card " + std::to_string(i), {}};
        for (int j = 0; j < 10; ++j) {
            SettingField field{card.id + "-field-" + std::to_string(j), "Test field:", "choice-0"};
            for (int k = 0; k < 12; ++k) {
                field.choices.push_back({"choice-" + std::to_string(k), "Choice " + std::to_string(k)});
            }
            card.fields.push_back(std::move(field));
        }
        cards.push_back(std::move(card));
    }
    ui.set_cards(cards);
    auto snapshot = [&] {
        PanelView view;
        ui.append(view);
        return view;
    };
    auto view = snapshot();
    require(view.cards.size() == 3 && !has(view, "test-3-field-0"),
            "offscreen cards have no hit controls");
    require(!has(view, "test-0-field-9"), "vertically clipped fields have no controls");
    require(ui.scroll(200, 200, 0, 2), "card scrolls vertically");
    view = snapshot();
    require(!has(view, "test-0-field-0") && has(view, "test-1-field-0"),
            "each card owns its vertical offset");
    const auto partial = control(view, "test-0-field-1");
    require(!partial.hit(partial.bounds.x + 2, partial.bounds.y + 2), "clipped portion cannot be hit");
    require(ui.scroll(200, 200, 1000, 0), "cards scroll horizontally");
    view = snapshot();
    require(!has(view, "test-0-field-1") && has(view, "test-4-field-0"),
            "horizontal scroll clips old cards and exposes new cards");
    require(!ui.scroll(200, 200, 1000, 0), "horizontal offset is bounded");
    require(ui.scroll(1200, 200, 0, 1000), "last card scrolls to its final rows");
    view = snapshot();
    require(has(view, "test-4-field-9") && !has(view, "test-4-field-0"),
            "vertical offset is bounded at last field");
    ui.reset();
    view = snapshot();
    const auto bar = control(view, "scroll-cards");
    const double x = bar.bounds.x + bar.bounds.width / 2, y = bar.bounds.y + 5;
    require(ui.down(1, x, y), "trigger captures horizontal scrollbar");
    require(!ui.move(2, x + 200, y), "other controller cannot move owned scrollbar");
    require(ui.move(1, x + 200, y), "captured controller scrolls cards");
    require(ui.pressed(1), "scrollbar ownership exposed for tracking loss cleanup");
    ui.cancel_pointer(1);
    require(!ui.move(1, x, y), "cancel stops scrollbar movement");
    ui.reset();
    ui.toggle("test-0-field-4");
    view = snapshot();
    require(view.popup && view.popup->y + view.popup->height > panel_height,
            "dropdown can extend beyond keyboard border");
    require(view.popup->height == 5 * 52, "overflow popup still shows five choices");
    const auto outside_case = control(view, "choose:test-0-field-4:choice-4");
    require(outside_case.hit(outside_case.bounds.x + 5, outside_case.bounds.y + 5),
            "overflow option remains clickable");
    require(outside_case.action.value == "choice-4", "overflow option resolves to choice");
    require(!ui.scroll(900, 200, 1, 1), "open popup blocks underlying card scrolling");
    require(ui.scroll(view.popup->x + 5, view.popup->y + 5, 0, 100),
            "popup scroll reaches last choices");
    view = snapshot();
    require(has(view, "choose:test-0-field-4:choice-11"), "all dropdown choices are reachable");
    const auto menu_bar = control(view, "scroll-menu");
    require(ui.down(0, menu_bar.bounds.x + 2, menu_bar.bounds.y + 2),
            "popup scrollbar captures pointer");
    ui.cancel();
    require(!ui.popup_open() && !ui.pressed(0), "cancel closes popup and releases scrollbar");

    // Paint real extra cards/menu through the production renderer and check
    // alpha at the escaped menu and beyond the card viewport.
    const auto profiles = load_profiles({}, {});
    NullSink sink;
    KeyboardState keyboard(sink);
    LanguageMap keymap(profiles.languages.at("en-us"));
    ui.reset();
    ui.toggle("test-0-field-4");
    view = snapshot();
    view.settings = true;
    view.layout = &profiles.layouts.at("en-us-full");
    view.language = &profiles.languages.at("en-us");
    view.theme = &profiles.themes.at("graphite");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    PanelRenderer renderer;
    renderer.paint(view, 1);
    const auto pixels = renderer.rgba();
    auto alpha = [&](int px, int py) {
        return pixels[static_cast<std::size_t>((py + popup_margin) * panel_width + px) * 4 + 3];
    };
    require(alpha(300, 700) == 255, "escaped popup pixels render outside case");
    require(alpha(900, 700) == 0, "outside popup margin stays transparent");
    renderer.write_png("/tmp/framekeyboard-settings-scroll-popup.png");
    ui.close_popup();
    view = snapshot();
    view.settings = true;
    view.layout = &profiles.layouts.at("en-us-full");
    view.language = &profiles.languages.at("en-us");
    view.theme = &profiles.themes.at("graphite");
    view.keyboard = &keyboard;
    view.keymap = &keymap;
    renderer.paint(view, 2);
    const auto clean = renderer.rgba();
    require(clean[static_cast<std::size_t>((700 + popup_margin) * panel_width + 300) * 4 + 3] == 0,
            "closing popup clears overflow pixels");
    renderer.write_png("/tmp/framekeyboard-settings-scroll-cards.png");
}
} // namespace
int main() {
    try {
        native_num_restore();
        compact_toolbar();
        startup_profile_fallback();
        incompatible_saved_language();
        missing_saved_language_status();
        key_control_namespaces();
        favorite_cache_reload();
        background_preferences();
        deferred_reload();
        deferred_reload_errors();
        preferences_with_broken_startup();
        reload_startup();
        launch_startup();
        numpad_settings();
        app_settings();
        behavior_settings();
        auto_leveling_settings();
        native_replacement_settings();
        favorite_schema();
        fractional_dropdown_scroll();
        scroll_cards();
        std::cout << "Settings, global numpad and pin persistence, favorites, popup cancellation and "
                     "card scrolling passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
