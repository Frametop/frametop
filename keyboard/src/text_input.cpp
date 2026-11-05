#include "framekeyboard/text_input.hpp"
#include "framekeyboard/local_socket.hpp"
#include "gamescope-input-method-client.h"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <glib.h>
#include <poll.h>
#include <stdexcept>
#include <wayland-client.h>

namespace framekeyboard {
struct GamescopeText::State {
    enum class Phase { Registry, Input, Ready, Dead } phase{Phase::Registry};
    enum class Result { None, Waiting, Success, Failure } result{Result::None};
    Clock clock;
    wl_display* display{};
    wl_registry* registry{};
    wl_seat* seat{};
    gamescope_input_method_manager* manager{};
    gamescope_input_method* input{};
    wl_callback* sync{};
    std::uint32_t serial{};
    bool available{}, completed{};
    double deadline{};
    std::string submitted;
    ~State() {
        if (sync) {
            wl_callback_destroy(sync);
        }
        if (input) {
            gamescope_input_method_destroy(input);
        }
        if (manager) {
            gamescope_input_method_manager_destroy(manager);
        }
        if (seat) {
            wl_seat_destroy(seat);
        }
        if (registry) {
            wl_registry_destroy(registry);
        }
        if (display) {
            wl_display_disconnect(display);
        }
    }
    void synchronize() {
        completed = false;
        sync = wl_display_sync(display);
        static const wl_callback_listener listener{[](void* data, wl_callback*, std::uint32_t) {
            static_cast<State*>(data)->completed = true;
        }};
        wl_callback_add_listener(sync, &listener, this);
        deadline = clock() + 1;
    }
    static void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                       std::uint32_t) {
        auto& s = *static_cast<State*>(data);
        if (std::strcmp(interface, "wl_seat") == 0 && !s.seat) {
            s.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        }
        if (std::strcmp(interface, "gamescope_input_method_manager") == 0 && !s.manager) {
            s.manager = static_cast<gamescope_input_method_manager*>(
                wl_registry_bind(registry, name, &gamescope_input_method_manager_interface, 1));
        }
    }
    static void removed(void*, wl_registry*, std::uint32_t) {}
    static void unavailable(void* data, gamescope_input_method*) {
        auto& s = *static_cast<State*>(data);
        s.available = false;
        s.phase = Phase::Dead;
    }
    static void done(void* data, gamescope_input_method*, std::uint32_t serial) {
        auto& s = *static_cast<State*>(data);
        if (s.phase != Phase::Dead) {
            s.serial = serial;
            s.available = true;
        }
    }
    bool dispatch() {
        // Prepare/read avoids a blocking dispatch if readiness changes between
        // poll and read. This connection has exactly one reader.
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                return false;
            }
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        if (poll(&fd, 1, 0) <= 0) {
            wl_display_cancel_read(display);
        } else if (wl_display_read_events(display) < 0) {
            return false;
        }
        return wl_display_dispatch_pending(display) >= 0 && !wl_display_get_error(display);
    }
};
GamescopeText::GamescopeText(const std::string& socket, Clock clock)
    : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.clock = clock ? std::move(clock) : Clock([] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    });
    const int fd = connect_local_socket(socket);
    if (fd < 0) {
        throw std::runtime_error("Unicode text connection unavailable");
    }
    s.display = wl_display_connect_to_fd(fd);
    if (!s.display) {
        throw std::runtime_error("Unicode text connection unavailable");
    }
    s.registry = wl_display_get_registry(s.display);
    static const wl_registry_listener registry_listener{State::global, State::removed};
    wl_registry_add_listener(s.registry, &registry_listener, &s);
    s.synchronize();
    wl_display_flush(s.display);
}
GamescopeText::~GamescopeText() = default;
bool GamescopeText::ready() {
    auto& s = *state_;
    if (s.phase == State::Phase::Dead) {
        return false;
    }
    if (!s.dispatch()) {
        s.phase = State::Phase::Dead;
        return false;
    }
    if (s.completed) {
        wl_callback_destroy(s.sync);
        s.sync = nullptr;
        s.completed = false;
        if (s.phase == State::Phase::Registry) {
            if (!s.manager || !s.seat) {
                s.phase = State::Phase::Dead;
                return false;
            }
            s.input = gamescope_input_method_manager_create_input_method(s.manager, s.seat);
            static const gamescope_input_method_listener input_listener{State::unavailable, State::done};
            gamescope_input_method_add_listener(s.input, &input_listener, &s);
            s.phase = State::Phase::Input;
            s.synchronize();
            wl_display_flush(s.display);
        } else if (s.phase == State::Phase::Input) {
            s.phase = s.available ? State::Phase::Ready : State::Phase::Dead;
        } else if (s.result == State::Result::Waiting) {
            s.result = State::Result::Success;
        }
    }
    if (s.sync && s.clock() >= s.deadline) {
        if (s.phase != State::Phase::Ready) {
            s.phase = State::Phase::Dead;
        } else if (s.result == State::Result::Waiting) {
            // Delivery is unknown, not disconnected. Keep sync as a barrier to
            // further text/native downs until the late acknowledgement arrives.
            s.result = State::Result::Failure;
        }
    }
    return s.phase == State::Phase::Ready;
}
bool GamescopeText::failed() const {
    return state_->phase == State::Phase::Dead;
}
bool GamescopeText::pending() const {
    return state_->result != State::Result::None;
}
bool GamescopeText::busy() const {
    return state_->phase == State::Phase::Ready && (state_->sync || pending());
}
void GamescopeText::cancel() {
    state_->result = State::Result::None;
    state_->submitted.clear();
}
bool GamescopeText::commit(const std::string& text) {
    auto& s = *state_;
    if (!ready()) {
        return false;
    }
    if (pending()) {
        if (text != s.submitted || s.result == State::Result::Waiting) {
            return false;
        }
        const bool ok = s.result == State::Result::Success;
        cancel();
        return ok;
    }
    if (busy() || text.empty() || text.find('\0') != std::string::npos ||
        !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr) ||
        g_utf8_strlen(text.c_str(), -1) > 32) {
        return false;
    }
    // Gamescope's temporary Unicode keymap holds at most 32 characters.
    gamescope_input_method_set_string(s.input, text.c_str());
    gamescope_input_method_commit(s.input, s.serial);
    s.submitted = text;
    s.result = State::Result::Waiting;
    s.synchronize();
    if (wl_display_flush(s.display) < 0 && errno != EAGAIN) {
        s.phase = State::Phase::Dead;
    }
    return false;
}
} // namespace framekeyboard
