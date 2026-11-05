#include "framekeyboard/app.hpp"
#include "framekeyboard/vr_lifecycle.hpp"
#include <algorithm>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool condition, const char* requirement) {
    if (!condition) {
        throw std::runtime_error(requirement);
    }
}

// A private receiving application, not an OS input sink. Keys/text are attributed
// to the selected receiver so releasing through the wrong route is observable.
struct Receiver : KeySink {
    int target{};
    std::map<int, std::set<int>> held;
    std::vector<std::pair<int, std::string>> text;
    std::vector<std::pair<int, int>> downs;
    bool slow_text{}, pending{}, reply{};
    bool text_available() override { return true; }
    void send(int code, int value) override {
        if (value) {
            held[target].insert(code);
            downs.emplace_back(target, code);
        } else {
            held[target].erase(code);
        }
    }
    bool commit_text(const std::string& value) override {
        if (slow_text) {
            if (!pending) {
                pending = true;
                reply = false;
                return false;
            }
            if (!reply) {
                return false;
            }
            pending = false;
        }
        text.emplace_back(target, value);
        return true;
    }
    bool text_pending() const override { return pending; }
    void cancel_text() override { pending = false; }
    bool released() const {
        return std::all_of(held.begin(), held.end(),
                           [](const auto& item) { return item.second.empty(); });
    }
};

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string name = (fs::temp_directory_path() / "framekeyboard-lifecycle-XXXXXX").string();
        require(mkdtemp(name.data()), "create isolated test configuration");
        path = name;
    }
    ~TemporaryDirectory() { fs::remove_all(path); }
};

struct Headset {
    TemporaryDirectory config;
    Receiver receiver;
    std::unique_ptr<App> app;
    std::unique_ptr<VrLifecycle> lifecycle;
    bool dashboard{true}, compositor{}, remote{}, fail_show{};
    std::optional<std::uint32_t> drag_owner;
    std::map<unsigned, std::uint32_t> pointers;
    int next_target{}, presentations{}, dismissals{}, retries{};
    double now{1};

    explicit Headset(bool background = true, bool replacement = true) {
        Settings settings;
        settings.replace_native_keyboard = replacement;
        save_settings(config.path, settings);
        Options options;
        options.mode = Mode::Vr;
        options.input = InputBackend::Ei;
        options.start_enabled = true;
        options.config_dir = config.path;
        options.autostart = background;
        // No real startup configuration or disk writer is part of this receiver.
        auto no_write = [](const Settings&) {};
        app = std::make_unique<App>(options, receiver, no_write, nullptr, no_write);
        app->set_interaction_active(false);
        lifecycle = std::make_unique<VrLifecycle>(
            background,
            VrLifecycleEffects{
                .stop_drag =
                    [&] {
                        drag_owner.reset();
                        app->set_dragging(false);
                    },
                .cancel_input = [&] { app->cancel(); },
                .clear_pointers = [&] { pointers.clear(); },
                .hide_overlay =
                    [&] {
                        require(receiver.released(),
                                "hiding must release input before removing the panel");
                        require(!drag_owner, "hiding must release movement ownership");
                        compositor = false;
                    },
                .route_input =
                    [&](bool to_steam) {
                        require(receiver.released(),
                                "target changes must release keys to the old receiver first");
                        require(!receiver.pending,
                                "target changes must cancel outstanding text before rerouting");
                        require(!drag_owner, "a target change must end movement before rerouting");
                        remote = to_steam;
                        receiver.target = next_target;
                    },
                .set_interaction = [&](bool active) { app->set_interaction_active(active); },
                .show_overlay =
                    [&](double) {
                        if (fail_show) {
                            throw std::runtime_error("injected presentation failure");
                        }
                        compositor = true;
                        ++presentations;
                    },
                .retry_bridge = [&](double) { ++retries; },
                .summon = [&] { app->summon(); },
                .dismiss_request =
                    [&] {
                        require(receiver.released() && !compositor,
                                "dismissal must leave no held keys or intercepting panel");
                        ++dismissals;
                    },
                .cancel_device =
                    [&](std::uint32_t device) {
                        for (auto it = pointers.begin(); it != pointers.end();) {
                            if (it->second == device) {
                                app->cancel_pointer(it->first);
                                it = pointers.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    },
            });
    }
    void status(NativeReplacement::State native, bool stalled = false, bool relaunch = false) {
        next_target = static_cast<int>(native.request);
        app->set_native_session(native.open);
        app->set_replacement_available(native.available);
        lifecycle->begin_frame({.replacement_enabled = app->replace_native_keyboard(),
                                .native = native,
                                .routed_remote = remote,
                                .watchdog_stalled = stalled,
                                .relaunch = relaunch,
                                .now = now});
        lifecycle->dashboard_visibility(dashboard);
        now += .02;
    }
    void request(unsigned id = 1, bool local = true, bool active = true) {
        status({id, true, true, active, true, local});
    }
    void display(bool placement = true) {
        lifecycle->tracking_sampled(true, now);
        lifecycle->present(placement, dashboard, compositor, now);
    }
    VrLifecycle::FrameReport report(bool backend_ready = true) const {
        return lifecycle->frame_report(backend_ready, dashboard, compositor);
    }
    std::pair<double, double> point(const std::string& action) const {
        const auto view = app->view();
        for (int y = 96; y < panel_height; y += 4) {
            for (int x = 0; x < panel_width; x += 4) {
                const auto* key = app->renderer.hit_key(view, x, y);
                if (key && key->action == action) {
                    return {x, y};
                }
            }
        }
        throw std::runtime_error("receiver fixture key missing: " + action);
    }
    bool press(unsigned pointer, const std::string& action, std::uint32_t device = 1) {
        const auto [x, y] = point(action);
        pointers[pointer] = device;
        return app->down(pointer, x, y, now);
    }
    void click(unsigned pointer, const std::string& action) {
        require(press(pointer, action), "fresh input must be accepted in the active field");
        const auto [x, y] = point(action);
        app->up(pointer, x, y);
    }
    void tick() {
        now += 2;
        app->tick(now);
    }
    void grab(std::uint32_t device) {
        drag_owner = device;
        app->set_dragging(true);
    }
};

void targets_keep_their_own_keys() {
    for (const auto [old_local, new_local] :
         {std::pair{true, false}, {false, true}, {true, true}, {false, false}}) {
        Headset h;
        h.request(10, old_local);
        h.display();
        require(h.press(1, "ControlLeft"), "a held modifier must reach the original field");
        require(h.press(100, "KeyA"), "using a held modifier delivers the chord to the field");
        require(h.receiver.held[10].contains(KEY_LEFTCTRL), "original receiver owns the modifier");
        h.request(10, old_local);
        require(h.receiver.held[10].contains(KEY_LEFTCTRL), "ordinary frames must not break a hold");
        h.request(11, new_local);
        h.display();
        require(h.receiver.released(), "the successor field must inherit no held modifier");
        h.click(2, "KeyA");
        require(h.receiver.text == std::vector<std::pair<int, std::string>>{{11, "a"}},
                "only new typing reaches the successor, without the old modifier");
        // A transport change within one request is also a new input context.
        require(h.press(3, "ControlLeft"), "new target accepts a fresh hold");
        require(h.press(101, "ArrowLeft"), "fresh chord reaches the current transport");
        h.request(11, !new_local);
        require(h.receiver.released(), "changing transport must also release the previous route");
    }
}

void late_work_cannot_cross_targets() {
    Headset h;
    h.request(20);
    h.display();
    h.receiver.slow_text = true;
    h.click(1, "KeyA");
    h.click(2, "Backspace");
    h.click(3, "KeyB");
    require(h.receiver.pending && h.receiver.downs.empty(),
            "fixture has text with editing queued behind it");
    h.request(21);
    h.display();
    h.receiver.reply = true; // A reply arriving after cancellation belongs to the old field.
    h.tick();
    require(h.receiver.downs.empty() && h.receiver.text.empty(),
            "late completion must not replay the old edit or text into another field");
    h.receiver.slow_text = false;
    h.click(4, "KeyC");
    require(h.receiver.text == std::vector<std::pair<int, std::string>>{{21, "c"}},
            "the successor remains usable for fresh input");
}

void a_closed_dashboard_delivers_nothing() {
    Headset h;
    h.request();
    h.display();
    require(h.press(1, "Backspace"), "fixture starts with a repeatable held key");
    h.dashboard = false;
    h.lifecycle->dashboard_visibility(false);
    const auto delivered = h.receiver.downs;
    h.tick();
    require(!h.press(2, "KeyA") && h.receiver.released() && h.receiver.downs == delivered,
            "dashboard closing releases holds and blocks queued pointer input and repeats");
    require(h.report().acknowledgement == 0 && !h.compositor,
            "a hidden dashboard cannot claim successful presentation");
    h.dashboard = true;
    h.request();
    h.display();
    h.click(3, "KeyB");
    require(h.receiver.text == std::vector<std::pair<int, std::string>>{{1, "b"}},
            "dashboard reopening accepts fresh input without replay");
    h.receiver.slow_text = true;
    h.click(4, "KeyA");
    h.click(5, "Backspace");
    h.dashboard = false;
    h.lifecycle->dashboard_visibility(false);
    h.receiver.reply = true;
    h.tick();
    require(h.receiver.downs == delivered && h.receiver.text.size() == 1,
            "late text cannot deliver a queued edit after dashboard closure");
}

void closing_and_native_fallback_have_different_meanings() {
    for (bool use_native : {false, true}) {
        Headset h;
        h.request(30);
        h.display();
        h.receiver.slow_text = true;
        h.click(1, "KeyA");
        h.click(2, "Backspace");
        h.click(3, "KeyB");
        require(h.receiver.pending && h.receiver.downs.empty(),
                "dismissal and native handoff begin with text and edits still queued");
        h.lifecycle->request_recenter();
        if (use_native) {
            h.lifecycle->fallback();
        } else {
            h.app->close_panel();
            require(h.app->take_dismissal(), "replacement X is a resident dismissal");
            h.lifecycle->dismiss();
        }
        require(!h.app->quitting() && !h.compositor && !h.press(1, "KeyA"),
                "closing or fallback leaves the resident process alive without typing");
        require(h.dismissals == (use_native ? 0 : 1) && h.report().fallback == use_native,
                "native fallback preserves the native request; X dismisses it");
        h.receiver.reply = true;
        h.tick();
        require(h.receiver.text.empty() && h.receiver.downs.empty(),
                "late completion after dismissal or native handoff must discard queued input");
        h.receiver.slow_text = false;
        h.request(30);
        h.display();
        require(!h.compositor && h.report().acknowledgement == 0,
                "late status cannot reopen or acknowledge a finished request");
        h.request(31);
        h.display();
        h.click(2, "KeyA");
        require(h.report().acknowledgement == 31 &&
                    h.receiver.text == std::vector<std::pair<int, std::string>>{{31, "a"}},
                "a new request restores presentation and typing without relaunch");
    }
}

void success_requires_real_presentation_and_takeover() {
    Headset h;
    h.request(40, true, false);
    require(h.report().acknowledgement == 0, "a request alone is not a presented keyboard");
    h.display(false);
    require(h.report().acknowledgement == 0, "missing placement cannot acknowledge a request");
    h.display();
    require(h.report().acknowledgement == 40 && !h.press(1, "KeyA"),
            "presentation can acknowledge readiness but typing waits for takeover");
    require(h.report(false).acknowledgement == 0,
            "an unavailable backend cannot acknowledge input readiness");
    h.compositor = false;
    require(h.report().acknowledgement == 0,
            "a cached visible flag cannot replace compositor visibility");
    h.display();
    h.request(40);
    h.click(2, "KeyA");
    require(h.receiver.text.size() == 1, "typing works once takeover is acknowledged");
    h.lifecycle->tracking_sampled(false, h.now);
    require(!h.compositor && !h.report().ready && h.report().acknowledgement == 0,
            "lost tracking hides the panel and withdraws readiness");
}

void pending_shows_survive_runtime_obstacles() {
    Headset h(false, false);
    h.status({});
    h.display();
    const auto shown = h.presentations;
    h.lifecycle->request_recenter();
    h.dashboard = false;
    h.lifecycle->present(true, false, true, h.now);
    require(h.presentations == shown, "a pending show cannot open outside the dashboard");
    h.dashboard = true;
    h.lifecycle->tracking_sampled(false, h.now);
    h.lifecycle->present(true, true, false, h.now);
    require(h.presentations == shown, "missing tracking must defer presentation");
    h.lifecycle->tracking_sampled(true, h.now);
    h.fail_show = true;
    try {
        h.lifecycle->present(true, true, false, h.now);
        throw std::logic_error("fixture failed to reject presentation");
    } catch (const std::runtime_error&) {
    }
    require(!h.lifecycle->visible(), "failed presentation must not advertise a visible panel");
    h.fail_show = false;
    h.lifecycle->present(true, true, false, h.now);
    require(h.presentations == shown + 1, "a failed show remains retryable when the runtime recovers");
    h.lifecycle->request_recenter();
    h.lifecycle->present(true, true, true, h.now);
    require(h.presentations == shown + 2, "explicit recenter refreshes an already visible panel");
    h.lifecycle->present(true, true, true, h.now);
    require(h.presentations == shown + 2, "ordinary frames do not resubmit an unchanged panel");
    h.lifecycle->request_recenter();
    h.fail_show = true;
    try {
        h.lifecycle->present(true, true, true, h.now);
        throw std::logic_error("fixture failed to reject recenter presentation");
    } catch (const std::runtime_error&) {
    }
    h.fail_show = false;
    h.lifecycle->present(true, true, true, h.now);
    require(h.presentations == shown + 3,
            "failed recenter presentation retries without requiring another user action");
}

void sleep_recovery_does_not_resurrect_old_input() {
    Headset h;
    h.status({1, false, false, false, true});
    h.display();
    require(h.presentations == 0, "background startup stays hidden until requested");
    h.request(50);
    h.display();
    require(h.press(1, "Backspace"), "fixture holds input before tracking loss");
    h.lifecycle->tracking_sampled(false, h.now);
    h.status({51, false, false, false, true});
    require(h.lifecycle->should_sample_tracking(true, h.now + 1),
            "a hidden resident must still detect tracking recovery");
    h.lifecycle->tracking_sampled(true, h.now + 1);
    h.lifecycle->present(true, true, false, h.now + 1);
    h.tick();
    require(!h.compositor && h.receiver.released(),
            "tracking recovery must not reopen an expired request or replay its hold");
    h.request(52);
    h.display();
    h.click(2, "KeyA");
    require(h.receiver.text == std::vector<std::pair<int, std::string>>{{52, "a"}},
            "a fresh request works after tracking recovers");
    h.status({}, false, true);
    h.display();
    require(h.compositor && h.retries == 1,
            "explicit relaunch remains a recovery path when the bridge is unavailable");
}

void cancellation_respects_device_ownership() {
    Headset h;
    h.request();
    h.display();
    require(h.press(1, "ArrowLeft", 7) && h.press(2, "ArrowRight", 8),
            "two controllers can own independent held keys");
    h.lifecycle->pointer_left(99, false);
    require(h.receiver.held[1].size() == 2, "an unidentified laser leave must not release either hand");
    h.lifecycle->device_lost(7, {});
    require(!h.receiver.held[1].contains(KEY_LEFT) && h.receiver.held[1].contains(KEY_RIGHT),
            "losing one controller releases only its input");
    h.lifecycle->pointer_left(8, false);
    require(h.receiver.released(), "the remaining controller's laser leave releases its own hold");
    h.grab(7);
    h.lifecycle->pointer_left(7, true);
    h.lifecycle->device_lost(8, h.drag_owner);
    require(h.drag_owner == 7, "laser leave or unrelated device loss must not steal movement ownership");
    h.lifecycle->device_lost(7, h.drag_owner);
    require(!h.drag_owner, "losing the movement owner must end the capture");
    require(h.press(3, "Backspace"), "input remains usable after device cancellation");
    h.lifecycle->focus_lost();
    require(h.receiver.released(), "focus or tracking-space loss must release input");
    h.grab(7);
    h.lifecycle->focus_lost();
    require(!h.drag_owner, "focus or tracking-space loss must end movement");
    h.grab(7);
    h.request(2);
    require(!h.drag_owner, "changing target must end movement before routing the successor");
    h.grab(7);
    h.lifecycle->hide();
    require(!h.drag_owner && h.pointers.empty(), "hiding must forget movement and pointer captures");
}

void watchdog_fallback_is_request_scoped() {
    Headset h;
    h.request(60);
    h.display();
    require(h.press(1, "ControlLeft"), "fixture holds a modifier before watchdog recovery");
    require(h.press(2, "ArrowLeft"), "using the modifier delivers the watchdog fixture chord");
    h.status({60, true, true, true, true, true}, true);
    h.display();
    require(!h.compositor && h.receiver.released() && h.report().fallback,
            "a stalled frame must give native input back without retaining held keys");
    h.request(60);
    h.display();
    require(!h.compositor, "a late healthy frame must not reclaim the failed request");
    h.request(61);
    h.display();
    h.click(3, "KeyA");
    require(h.receiver.text == std::vector<std::pair<int, std::string>>{{61, "a"}},
            "watchdog recovery must allow fresh requests to type");
}
} // namespace

int main() {
    try {
        targets_keep_their_own_keys();
        late_work_cannot_cross_targets();
        a_closed_dashboard_delivers_nothing();
        closing_and_native_fallback_have_different_meanings();
        success_requires_real_presentation_and_takeover();
        pending_shows_survive_runtime_obstacles();
        sleep_recovery_does_not_resurrect_old_input();
        cancellation_respects_device_ownership();
        watchdog_fallback_is_request_scoped();
        std::cout << "VR lifecycle requirements passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
