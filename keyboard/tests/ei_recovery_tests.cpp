#include "framekeyboard/app.hpp"
#include "framekeyboard/ei_input.hpp"
#include "framekeyboard/native_replacement.hpp"
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
#include "gamescope-input-method-server.h"
#include <wayland-server.h>
#endif
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <libeis.h>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace framekeyboard;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
// This compositor exists only inside the test. Its keys never reach the desktop.
struct Server {
    eis* context{eis_new(nullptr)};
    eis_client* client{};
    eis_seat* seat{};
    eis_device* device{};
    std::vector<std::pair<unsigned, bool>> keys;
    fs::path directory, socket;
    std::string keyboard_layout;
    explicit Server(const std::string& socket_name = "input", const std::string& layout = "us",
                    const fs::path& existing_socket = {})
        : keyboard_layout(layout) {
        if (existing_socket.empty()) {
            std::string temporary = "/tmp/full-keyboard-eis-XXXXXX";
            require(mkdtemp(temporary.data()), "temporary directory");
            directory = temporary;
            socket = directory / socket_name;
        } else {
            socket = existing_socket;
            directory = socket.parent_path();
            fs::create_directories(directory);
        }
        require(context && eis_set_flag(context, EIS_FLAG_DEVICE_READY) == 0, "device-ready protocol");
        require(eis_setup_backend_socket(context, socket.c_str()) == 0, "isolated EIS socket");
    }
    ~Server() {
        if (device) {
            eis_device_remove(device);
            eis_device_unref(device);
        }
        if (seat) {
            eis_seat_remove(seat);
            eis_seat_unref(seat);
        }
        if (client) {
            eis_client_unref(client);
        }
        eis_unref(context);
        fs::remove_all(directory);
    }
    void pump() {
        eis_dispatch(context);
        while (auto* event = eis_get_event(context)) {
            switch (eis_event_get_type(event)) {
            case EIS_EVENT_CLIENT_CONNECT:
                client = eis_client_ref(eis_event_get_client(event));
                eis_client_connect(client);
                seat = eis_client_new_seat(client, "test");
                eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
                eis_seat_add(seat);
                break;
            case EIS_EVENT_SEAT_BIND: {
                if (device || !eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
                    break;
                }
                device = eis_seat_new_device(seat);
                eis_device_configure_name(device, "isolated test keyboard");
                eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);
                auto* xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                xkb_rule_names names{};
                names.layout = keyboard_layout.c_str();
                auto* map = xkb_keymap_new_from_names(xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
                char* text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
                const auto size = std::strlen(text) + 1;
                const int fd = memfd_create("test-keymap", MFD_CLOEXEC);
                require(fd >= 0 && write(fd, text, size) == static_cast<ssize_t>(size), "keymap file");
                auto* keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd, size);
                require(keymap, "EIS keymap");
                eis_keymap_add(keymap);
                eis_keymap_unref(keymap);
                close(fd);
                free(text);
                xkb_keymap_unref(map);
                xkb_context_unref(xkb);
                eis_device_add(device);
                break;
            }
            case EIS_EVENT_DEVICE_READY:
                eis_device_resume(eis_event_get_device(event));
                break;
            case EIS_EVENT_KEYBOARD_KEY:
                keys.emplace_back(eis_event_keyboard_get_key(event),
                                  eis_event_keyboard_get_key_is_press(event));
                break;
            default:
                break;
            }
            eis_event_unref(event);
        }
    }
    template <class Condition> void until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (condition()) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("EIS test timed out");
    }
};
} // namespace
namespace {
struct TextListener {
    int fd{-1};
    explicit TextListener(const fs::path& path) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        require(fd >= 0, "text listener socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = path.string();
        require(name.size() < sizeof(address.sun_path), "text listener path length");
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        require(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                    listen(fd, 4) == 0,
                "text listener bind");
    }
    ~TextListener() {
        if (fd >= 0) {
            close(fd);
        }
    }
    bool contacted() const {
        pollfd pending{fd, POLLIN, 0};
        return poll(&pending, 1, 0) > 0 && (pending.revents & POLLIN);
    }
};
void socket_routing_tests() {
    require(gamescope_text_socket("/run/user/1000/gamescope-7-ei") == "/run/user/1000/gamescope-7",
            "numbered compositor mapping");
    require(gamescope_text_socket("gamescope-12-ei") == "gamescope-12",
            "relative socket mapping retains runtime-directory semantics");
    for (const auto* name :
         {"input", "", "gamescope-ei", "gamescope--ei", "gamescope-wrong-ei", "gamescope-1-ei.backup"}) {
        require(gamescope_text_socket(name).empty(), "unknown socket must not guess a target");
    }
    require(gamescope_text_socket("input", "custom-display") == "custom-display",
            "explicit text socket takes priority");
    for (int scenario = 0; scenario < 3; ++scenario) {
        Server server(scenario == 0 ? "gamescope-7-ei" : "input");
        TextListener default_display(server.directory / "gamescope-0");
        const auto text_path = server.directory / (scenario == 0 ? "gamescope-7" : "custom-display");
        TextListener selected_display(text_path);
        auto connecting = std::async(std::launch::async, [&] {
            return std::make_unique<EiSink>(server.socket, scenario == 1 ? text_path : fs::path{});
        });
        server.until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
        auto input = connecting.get();
        // Deliberately no Wayland protocol server: the bounded handshake fails,
        // but accept queues reveal exactly which compositor was contacted.
        require(!input->text_available(), "dummy text listener has no protocol support");
        require(selected_display.contacted() == (scenario != 2), "text socket routing");
        require(!default_display.contacted(), "must never contact unrelated gamescope-0");
    }
}
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
// An isolated protocol peer, pumped explicitly so sync replies can be delayed.
struct TextServer {
    wl_display* display{wl_display_create()};
    wl_resource* input{};
    std::string pending;
    std::vector<std::string> commits;
    unsigned connections{};
    explicit TextServer(const fs::path& socket) {
        require(display && wl_display_add_socket(display, socket.c_str()) == 0,
                "isolated Wayland socket");
        wl_global_create(display, &wl_seat_interface, 1, this,
                         [](wl_client* client, void*, std::uint32_t, std::uint32_t id) {
                             wl_resource_create(client, &wl_seat_interface, 1, id);
                         });
        wl_global_create(
            display, &gamescope_input_method_manager_interface, 1, this,
            [](wl_client* client, void* data, std::uint32_t, std::uint32_t id) {
                auto* resource =
                    wl_resource_create(client, &gamescope_input_method_manager_interface, 1, id);
                static const struct gamescope_input_method_manager_interface implementation{
                    [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
                    [](wl_client* client, wl_resource* manager, wl_resource*, std::uint32_t id) {
                        auto& self = *static_cast<TextServer*>(wl_resource_get_user_data(manager));
                        ++self.connections;
                        self.input =
                            wl_resource_create(client, &gamescope_input_method_interface, 1, id);
                        static const struct gamescope_input_method_interface implementation{
                            [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
                            [](wl_client*, wl_resource* resource, std::uint32_t) {
                                auto& self =
                                    *static_cast<TextServer*>(wl_resource_get_user_data(resource));
                                self.commits.push_back(self.pending);
                                self.pending.clear();
                            },
                            [](wl_client*, wl_resource* resource, const char* text) {
                                static_cast<TextServer*>(wl_resource_get_user_data(resource))->pending =
                                    text;
                            },
                            [](wl_client*, wl_resource*, std::uint32_t) {},
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr};
                        wl_resource_set_implementation(
                            self.input, &implementation, &self, [](wl_resource* resource) {
                                auto& self =
                                    *static_cast<TextServer*>(wl_resource_get_user_data(resource));
                                if (self.input == resource) {
                                    self.input = nullptr;
                                }
                            });
                        gamescope_input_method_send_done(self.input, 1);
                    }};
                wl_resource_set_implementation(resource, &implementation, data, nullptr);
            });
    }
    ~TextServer() {
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }
    void pump() {
        wl_event_loop_dispatch(wl_display_get_event_loop(display), 0);
        wl_display_flush_clients(display);
    }
};
void asynchronous_text() {
    Server server;
    TextServer text(server.directory / "text");
    double now = 10;
    auto connecting = std::async(std::launch::async, [&] {
        return std::make_unique<EiSink>(server.socket, server.directory / "text", [&] { return now; });
    });
    server.until([&] {
        text.pump();
        return connecting.wait_for(0ms) == std::future_status::ready;
    });
    auto sink = connecting.get();
    server.until([&] {
        text.pump();
        return sink->text_available();
    });
    sink->take_input_reset();
    NativeReplacement wrapper(*sink);
    const auto start = std::chrono::steady_clock::now();
    require(!wrapper.commit_text("slow") && wrapper.text_pending(), "commit starts asynchronously");
    require(std::chrono::steady_clock::now() - start < 16ms, "commit does not wait for compositor");
    sink->send(30, 1);
    now += 2;
    sink->pump();
    require(sink->text_available() && !wrapper.commit_text("slow") && !wrapper.text_pending(),
            "timeout reports failure without killing text connection");
    sink->send(31, 1);
    server.pump();
    require(server.keys.empty(), "native downs cannot overtake unresolved text after timeout");
    // Deliver the old sync, then retry explicitly on the same connection.
    for (int i = 0; i < 10; ++i) {
        text.pump();
        sink->pump();
    }
    require(!wrapper.commit_text("retry") && wrapper.text_pending(), "same connection accepts retry");
    server.until([&] {
        text.pump();
        sink->pump();
        return wrapper.commit_text("retry");
    });
    require(text.commits == std::vector<std::string>{"slow", "retry"}, "no automatic duplicate commit");
    sink->send(32, 1);
    sink->send(32, 0);
    server.until([&] { return server.keys.size() == 2; });
    require(!wrapper.commit_text("cancel") && wrapper.text_pending(), "local pending is forwarded");
    wrapper.cancel_text();
    require(!wrapper.text_pending(), "local cancellation is forwarded");
    sink->send(33, 1);
    server.pump();
    require(server.keys.size() == 2, "cancel retains outstanding text ordering barrier");
    for (int i = 0; i < 10; ++i) {
        text.pump();
        sink->pump();
    }
    const auto connections = text.connections;
    gamescope_input_method_send_unavailable(text.input);
    text.pump();
    require(!sink->text_available(), "unavailable retires connection");
    now += .9;
    text.pump();
    require(!sink->text_available() && text.connections == connections, "reconnect respects backoff");
    now += .2;
    server.until([&] {
        text.pump();
        return sink->text_available();
    });
    require(text.connections == connections + 1, "text reconnect restores readiness");
}
void app_ordering_barrier() {
    Server server;
    TextServer text(server.directory / "text");
    double now = 10;
    auto connecting = std::async(std::launch::async, [&] {
        return std::make_unique<EiSink>(server.socket, server.directory / "text", [&] { return now; });
    });
    server.until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
    auto sink = connecting.get();
    server.until([&] {
        text.pump();
        return sink->text_available();
    });
    NativeReplacement wrapper(*sink);
    Options options;
    options.config_dir = server.directory / "config";
    options.mode = Mode::Vr;
    options.input = InputBackend::Ei;
    options.start_enabled = true;
    App app(options, wrapper);
    auto point = [&](const std::string& action) -> std::pair<int, int> {
        const auto view = app.view();
        for (int y = 96; y < panel_height; y += 4) {
            for (int x = 0; x < panel_width; x += 4) {
                const auto* key = app.renderer.hit_key(view, x, y);
                if (key && key->action == action) {
                    return {x, y};
                }
            }
        }
        throw std::runtime_error("missing barrier fixture key");
    };
    const auto [ax, ay] = point("KeyA");
    require(app.down(0, ax, ay, now), "App starts an asynchronous character commit");
    app.up(0, ax, ay);
    now += 2;
    app.tick(now);
    app.cancel();
    require(!wrapper.text_pending() && wrapper.output_blocked(),
            "cancellation clears retry state but keeps the transport ordering barrier");
    const auto [bx, by] = point("KeyB");
    require(!app.down(0, bx, by, now) && !app.pointer_pressed(0),
            "blocked Unicode press has no accepted capture, pressed visual or haptic");
    app.up(0, bx, by);
    require(app.view().status.starts_with("Waiting for compositor text delivery."),
            "blocked Unicode press explains the outstanding text");
    const auto [cx, cy] = point("CapsLock");
    for (int i = 0; i < 2; ++i) {
        require(app.down(0, cx, cy, now), "local lock toggle is allowed during the barrier");
        app.up(0, cx, cy);
    }
    const auto [x, y] = point("ArrowLeft");
    require(!app.down(0, x, y, now) && !app.pointer_pressed(0),
            "blocked native press has no accepted capture, pressed visual or haptic");
    app.up(0, x, y);
    require(app.view().status.starts_with("Waiting for compositor text delivery."),
            "blocked native press explains the outstanding text");
    server.pump();
    require(server.keys.empty(), "blocked native press never reaches libei");
    for (int i = 0; i < 10; ++i) {
        text.pump();
        app.tick(now);
    }
    require(!wrapper.output_blocked() && app.view().status.empty(),
            "late sync clears the barrier and waiting status");
    require(app.down(0, x, y, now), "same native press is accepted after late sync");
    app.up(0, x, y);
    server.until([&] { return server.keys.size() == 2; });
    require(server.keys == std::vector<std::pair<unsigned, bool>>{{105, true}, {105, false}},
            "unblocked native press reaches libei with balanced release");
    const auto [ex, ey] = point("Enter");
    require(app.down(0, ex, ey, now), "Enter has no rejected Unicode text to retry");
    app.up(0, ex, ey);
    server.until([&] { return server.keys.size() == 4; });
    require(server.keys[2] == std::pair<unsigned, bool>{28, true} &&
                server.keys[3] == std::pair<unsigned, bool>{28, false} &&
                text.commits == std::vector<std::string>{"a"},
            "blocked Unicode press leaves no text queued for Enter or automatic delivery");
    require(app.down(0, bx, by, now), "same Unicode press is accepted after late sync");
    app.up(0, bx, by);
    server.until([&] {
        text.pump();
        app.tick(now);
        return !wrapper.text_pending();
    });
    require(text.commits == std::vector<std::string>{"a", "b"},
            "unblocked Unicode press is delivered exactly once");
    require(app.down(0, ax, ay, now), "start another character commit");
    app.up(0, ax, ay);
    for (int i = 0; i < 10; ++i) {
        text.pump();
        sink->pump();
    }
    require(wrapper.text_pending(), "completed text still awaits App result consumption");
    require(app.down(0, x, y, now),
            "native press can consume an already completed text result without extra rejection");
    app.up(0, x, y);
    server.until([&] { return server.keys.size() == 6; });
}
#endif
void reconnect() {
    auto server = std::make_unique<Server>();
    const auto socket = server->socket;
    const auto text_socket = server->directory / "text";
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
    auto text = std::make_unique<TextServer>(text_socket);
#endif
    double now = 10;
    auto connecting = std::async(std::launch::async, [&] {
        return std::make_unique<EiSink>(socket, text_socket, [&] { return now; });
    });
    server->until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
    auto sink = connecting.get();
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
    server->until([&] {
        text->pump();
        return sink->text_available();
    });
#endif
    sink->take_input_reset();
    sink->send(30, 1);
    server->until([&] { return server->keys.size() == 1; });
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
    text.reset();
#endif
    server.reset();
    for (int i = 0; i < 100 && sink->pump(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    require(!sink->pump() && sink->can_resume() && sink->take_input_reset() && !sink->take_input_reset(),
            "disconnect is recoverable and reports reset once");
    now += 1;
    const auto started = std::chrono::steady_clock::now();
    require(!sink->pump(), "absent socket leaves reconnect pending");
    require(std::chrono::steady_clock::now() - started < 16ms,
            "reconnect never waits for a handshake or missing compositor");
    sink->send(31, 1);
    require(!sink->take_input_reset(), "failed reconnect does not replay reset or holds");
    server = std::make_unique<Server>("input", "us", socket);
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
    text = std::make_unique<TextServer>(text_socket);
#endif
    now += 1.9;
    require(!sink->pump(), "second attempt waits for the doubled delay");
    now += .2;
    const auto negotiating = std::chrono::steady_clock::now();
    sink->pump();
    require(std::chrono::steady_clock::now() - negotiating < 16ms,
            "reconnect starts negotiation without waiting for the server");
    server->until([&] { return sink->pump(); });
    sink->send(30, 0);
    sink->send(32, 1);
    sink->send(32, 0);
    server->until([&] { return server->keys.size() == 2; });
    require(server->keys == std::vector<std::pair<unsigned, bool>>{{32, true}, {32, false}},
            "replacement server receives only fresh keys");
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
    server->until([&] {
        text->pump();
        return sink->text_available();
    });
    require(text->connections == 1, "native reconnect also recreates Unicode connection");
#endif
}
} // namespace
int main() {
    try {
        socket_routing_tests();
        reconnect();
#ifdef FRAMEKEYBOARD_TEST_WAYLAND_SERVER
        asynchronous_text();
        app_ordering_barrier();
#endif
        for (const auto* layout : {"us", "fr"}) {
            Server mapping("input", layout);
            auto connecting =
                std::async(std::launch::async, [&] { return std::make_unique<EiSink>(mapping.socket); });
            mapping.until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
            auto input = connecting.get();
            require(input->shortcut_code('a', 999) == (std::string(layout) == "fr" ? 16 : 30),
                    "Ctrl+A maps to actual target keymap");
            require(input->shortcut_code('c', 999) == 46, "Copy maps to target C");
            eis_device_keyboard_send_xkb_modifiers(mapping.device, 0, 0, 1u << 4, 0);
            mapping.until([&] {
                input->pump();
                return input->num_lock_state() == std::optional<bool>(true);
            });
            eis_device_keyboard_send_xkb_modifiers(mapping.device, 0, 0, 0, 0);
            mapping.until([&] {
                input->pump();
                return input->num_lock_state() == std::optional<bool>(false);
            });
        }
        Server server;
        auto connecting = std::async(
            std::launch::async, [&] { return std::make_unique<EiSink>(server.directory / "input"); });
        server.until([&] { return connecting.wait_for(0ms) == std::future_status::ready; });
        auto sink = connecting.get();
        sink->take_input_reset();
        sink->send(30, 1);
        server.until([&] { return server.keys.size() == 1; });
        eis_device_pause(server.device);
        server.until([&] { return !sink->pump(); });
        require(sink->can_resume() && sink->take_input_reset(), "pause is recoverable and clears holds");
        sink->send(31, 1);
        eis_device_resume(server.device);
        server.until([&] { return sink->pump(); });
        sink->send(30, 0); // Old key was released by EIS at pause, not replayed.
        sink->send(32, 1);
        sink->send(32, 0);
        server.until([&] { return server.keys.size() >= 3; });
        require(server.keys ==
                    std::vector<std::pair<unsigned, bool>>{{30, true}, {32, true}, {32, false}},
                "resume permits fresh keys but never paused or stale keys");
        eis_device_pause(server.device);
        eis_device_resume(server.device);
        server.pump();
        sink->send(33, 1); // Must not pass the reset barrier before the app clears its UI holds.
        require(sink->take_input_reset(), "same-batch pause/resume retains reset notification");
        require(sink->pump(), "same-batch pause/resume stays usable after reset acknowledgement");
        sink->send(34, 1);
        sink->send(34, 0);
        server.until([&] { return server.keys.size() >= 5; });
        require(server.keys[3] == std::pair<unsigned, bool>{34, true} &&
                    server.keys[4] == std::pair<unsigned, bool>{34, false},
                "output before interruption acknowledgement is suppressed");
        eis_client_disconnect(server.client);
        server.until([&] { return !sink->pump(); });
        require(sink->can_resume() && sink->take_input_reset(), "disconnect schedules reconnect");
        std::cout << "Compositor socket routing, real libei pause/resume, stale-key suppression and "
                     "disconnect passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
