#include "framekeyboard/ei_input.hpp"
#include "framekeyboard/local_socket.hpp"
#include <algorithm>
#include <chrono>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace framekeyboard {
fs::path gamescope_text_socket(const fs::path& input_socket, const fs::path& explicit_text) {
    if (!explicit_text.empty()) {
        return explicit_text;
    }
    const auto name = input_socket.filename().string();
    constexpr std::string_view prefix = "gamescope-", suffix = "-ei";
    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) ||
        !name.ends_with(suffix)) {
        return {};
    }
    const auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) {
        return {};
    }
    // Relative names resolve under XDG_RUNTIME_DIR in both libei and Wayland.
    return input_socket.parent_path() / name.substr(0, name.size() - suffix.size());
}
EiSink::EiSink(const fs::path& socket, const fs::path& text_socket, GamescopeText::Clock clock)
    : socket_(socket), text_socket_(gamescope_text_socket(socket, text_socket)),
      clock_(clock ? std::move(clock) : GamescopeText::Clock([] {
          return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      })) {
    try {
        if (!connect()) {
            throw std::runtime_error("Cannot connect to compositor keyboard socket: " + socket.string());
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!pump() && !disconnected_ && std::chrono::steady_clock::now() < deadline) {
            pollfd fd{ei_get_fd(context_), POLLIN, 0};
            poll(&fd, 1, 50);
        }
        if (!resumed_) {
            throw std::runtime_error("Compositor did not provide an active keyboard");
        }
    } catch (...) {
        if (keyboard_) {
            keyboard_ = ei_device_unref(keyboard_);
        }
        if (context_) {
            context_ = ei_unref(context_);
        }
        throw;
    }
}
bool EiSink::connect() {
    if (keyboard_) {
        keyboard_ = ei_device_unref(keyboard_);
    }
    if (context_) {
        context_ = ei_unref(context_);
    }
    keymap_.reset();
    num_lock_.reset();
    group_ = sequence_ = 0;
    held_.clear();
    resumed_ = false;
    disconnected_ = true;
    context_ = ei_new_sender(nullptr);
    if (!context_) {
        return false;
    }
    ei_configure_name(context_, "Full Keyboard");
    const int fd = connect_local_socket(socket_);
    if (fd < 0) {
        return false;
    }
    // libei owns fd, including on setup failure. No handshake wait here;
    // subsequent frames dispatch it, with a bounded negotiation deadline.
    if (ei_setup_backend_fd(context_, fd) != 0) {
        return false;
    }
    disconnected_ = false;
    connect_deadline_ = clock_() + 3;
    return true;
}
void EiSink::connection_lost() {
    input_reset_ |= resumed_ || keyboard_;
    disconnected_ = true;
    resumed_ = false;
    held_.clear();
    num_lock_.reset();
    keymap_.reset();
    text_.reset();
    text_retry_at_ = 0;
    text_retry_delay_ = 1;
    retry_at_ = clock_() + retry_delay_;
    retry_delay_ = std::min(30.0, retry_delay_ * 2);
}
void EiSink::service_text() {
    if (text_socket_.empty() || disconnected_ || !resumed_) {
        return; // Never let Wayland fall back to an unrelated default display.
    }
    if (text_ && text_->ready()) {
        text_retry_delay_ = 1;
    }
    if (text_ && text_->failed()) {
        text_.reset();
        text_retry_at_ = clock_() + text_retry_delay_;
        text_retry_delay_ = std::min(30.0, text_retry_delay_ * 2);
    }
    if (!text_ && clock_() >= text_retry_at_) {
        try {
            text_ = std::make_unique<GamescopeText>(text_socket_.string(), clock_);
        } catch (const std::exception&) {
            text_retry_at_ = clock_() + text_retry_delay_;
            text_retry_delay_ = std::min(30.0, text_retry_delay_ * 2);
        }
    }
}
bool EiSink::text_available() {
    service_text();
    return !disconnected_ && resumed_ && text_ && text_->ready();
}
bool EiSink::commit_text(const std::string& text) {
    return pump() && !input_reset_ && held_.empty() && text_available() && text_->commit(text);
}
bool EiSink::pump() {
    if (disconnected_) {
        if (clock_() < retry_at_) {
            return false;
        }
        if (!connect()) {
            retry_at_ = clock_() + retry_delay_;
            retry_delay_ = std::min(30.0, retry_delay_ * 2);
            return false;
        }
    }
    service_text();
    ei_dispatch(context_);
    while (auto* event = ei_get_event(context_)) {
        auto* device = ei_event_get_device(event);
        switch (ei_event_get_type(event)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, nullptr);
            break;
        case EI_EVENT_DEVICE_ADDED:
            if (!keyboard_ && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                keyboard_ = ei_device_ref(device);
                read_keymap();
            }
            break;
        case EI_EVENT_KEYBOARD_MODIFIERS:
            if (device == keyboard_) {
                group_ = ei_event_keyboard_get_xkb_group(event);
                if (keymap_) {
                    const auto index = xkb_keymap_mod_get_index(keymap_.get(), XKB_MOD_NAME_NUM);
                    if (index != XKB_MOD_INVALID && index < 32) {
                        num_lock_ = (ei_event_keyboard_get_xkb_mods_locked(event) & (1u << index)) != 0;
                    }
                }
            }
            break;
        case EI_EVENT_DEVICE_RESUMED:
            if (device == keyboard_ && !disconnected_) {
                resumed_ = true;
                connect_deadline_ = 0;
                retry_delay_ = 1;
                ei_device_start_emulating(keyboard_, ++sequence_);
            }
            break;
        case EI_EVENT_DEVICE_PAUSED:
        case EI_EVENT_DEVICE_REMOVED:
            if (device == keyboard_) {
                // EIS releases keys when pausing/removing a device. Forget our
                // local holds too, so reconnecting cannot replay a stale chord.
                resumed_ = false;
                // Remember interruption even if RESUMED is in this same batch.
                // No new output is allowed until the app acknowledges and clears its holds.
                input_reset_ = true;
                held_.clear();
                num_lock_.reset();
                if (ei_event_get_type(event) == EI_EVENT_DEVICE_REMOVED) {
                    keyboard_ = ei_device_unref(keyboard_);
                    keymap_.reset();
                    group_ = 0;
                }
            }
            break;
        case EI_EVENT_DISCONNECT:
            connection_lost();
            break;
        default:
            break;
        }
        ei_event_unref(event);
    }
    if (!disconnected_ && connect_deadline_ != 0 && clock_() >= connect_deadline_) {
        connection_lost();
    }
    return resumed_ && !disconnected_;
}
void EiSink::read_keymap() {
    keymap_.reset();
    group_ = 0;
    num_lock_.reset();
    auto* map = ei_device_keyboard_get_keymap(keyboard_);
    if (!map || ei_keymap_get_type(map) != EI_KEYMAP_TYPE_XKB) {
        return;
    }
    const auto size = ei_keymap_get_size(map);
    if (!size || size > 4 * 1024 * 1024) {
        return;
    }
    std::string text(size, '\0');
    if (pread(ei_keymap_get_fd(map), text.data(), size, 0) != static_cast<ssize_t>(size)) {
        return;
    }
    if (!xkb_) {
        xkb_.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
    }
    if (!xkb_) {
        return;
    }
    keymap_.reset(xkb_keymap_new_from_string(xkb_.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS));
}
int EiSink::shortcut_code(xkb_keysym_t symbol, int fallback) {
    pump();
    if (!keymap_) {
        return fallback;
    }
    // Prefer the current target group. If it is non-Latin, applications usually
    // use a Latin group's physical counterpart for Ctrl shortcuts. Never switch
    // the compositor's group or alter its keyboard configuration here.
    const auto groups = xkb_keymap_num_layouts(keymap_.get());
    for (xkb_layout_index_t pass = 0; pass <= groups; ++pass) {
        const auto group = pass == 0 ? group_ : pass - 1;
        if (group >= groups || (pass && group == group_)) {
            continue;
        }
        for (auto code = xkb_keymap_min_keycode(keymap_.get());
             code <= xkb_keymap_max_keycode(keymap_.get()); ++code) {
            const xkb_keysym_t* symbols = nullptr;
            const int count = xkb_keymap_key_get_syms_by_level(keymap_.get(), code, group, 0, &symbols);
            for (int i = 0; i < count; ++i) {
                if (xkb_keysym_to_lower(symbols[i]) == symbol && code >= 8) {
                    return static_cast<int>(code - 8);
                }
            }
        }
    }
    return fallback;
}
bool EiSink::take_input_reset() {
    const bool reset = input_reset_;
    input_reset_ = false;
    return reset;
}
void EiSink::send(int code, int value) {
    // EIS carries physical down/up transitions; the compositor owns repeat.
    if (!pump() || input_reset_ || value == 2) {
        return;
    }
    if (value == 1) {
        if (text_ && text_->busy()) {
            return; // A native down cannot overtake unresolved Unicode delivery.
        }
        if (!held_.insert(code).second) {
            return;
        }
    } else if (!held_.erase(code)) {
        return;
    }
    ei_device_keyboard_key(keyboard_, static_cast<std::uint32_t>(code), value != 0);
    ei_device_frame(keyboard_, ei_now(context_));
    ei_dispatch(context_);
}
EiSink::~EiSink() {
    if (resumed_ && !disconnected_ && keyboard_) {
        for (int code : held_) {
            ei_device_keyboard_key(keyboard_, static_cast<std::uint32_t>(code), false);
        }
        ei_device_frame(keyboard_, ei_now(context_));
        ei_device_stop_emulating(keyboard_);
        ei_dispatch(context_);
    }
    if (keyboard_) {
        ei_device_unref(keyboard_);
    }
    if (context_) {
        ei_unref(context_);
    }
}
} // namespace framekeyboard
