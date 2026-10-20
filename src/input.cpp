#include "framekeyboard/input.hpp"
#include "framekeyboard/output.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
namespace {
const std::map<std::string, int> key_codes = {{"Escape", KEY_ESC},
                                              {"Backquote", KEY_GRAVE},
                                              {"Digit1", KEY_1},
                                              {"Digit2", KEY_2},
                                              {"Digit3", KEY_3},
                                              {"Digit4", KEY_4},
                                              {"Digit5", KEY_5},
                                              {"Digit6", KEY_6},
                                              {"Digit7", KEY_7},
                                              {"Digit8", KEY_8},
                                              {"Digit9", KEY_9},
                                              {"Digit0", KEY_0},
                                              {"Minus", KEY_MINUS},
                                              {"Equal", KEY_EQUAL},
                                              {"Backspace", KEY_BACKSPACE},
                                              {"Tab", KEY_TAB},
                                              {"KeyQ", KEY_Q},
                                              {"KeyW", KEY_W},
                                              {"KeyE", KEY_E},
                                              {"KeyR", KEY_R},
                                              {"KeyT", KEY_T},
                                              {"KeyY", KEY_Y},
                                              {"KeyU", KEY_U},
                                              {"KeyI", KEY_I},
                                              {"KeyO", KEY_O},
                                              {"KeyP", KEY_P},
                                              {"BracketLeft", KEY_LEFTBRACE},
                                              {"BracketRight", KEY_RIGHTBRACE},
                                              {"Backslash", KEY_BACKSLASH},
                                              {"KeyA", KEY_A},
                                              {"KeyS", KEY_S},
                                              {"KeyD", KEY_D},
                                              {"KeyF", KEY_F},
                                              {"KeyG", KEY_G},
                                              {"KeyH", KEY_H},
                                              {"KeyJ", KEY_J},
                                              {"KeyK", KEY_K},
                                              {"KeyL", KEY_L},
                                              {"Semicolon", KEY_SEMICOLON},
                                              {"Quote", KEY_APOSTROPHE},
                                              {"Enter", KEY_ENTER},
                                              {"KeyZ", KEY_Z},
                                              {"KeyX", KEY_X},
                                              {"KeyC", KEY_C},
                                              {"KeyV", KEY_V},
                                              {"KeyB", KEY_B},
                                              {"KeyN", KEY_N},
                                              {"KeyM", KEY_M},
                                              {"Comma", KEY_COMMA},
                                              {"Period", KEY_DOT},
                                              {"Slash", KEY_SLASH},
                                              {"Space", KEY_SPACE},
                                              {"IntlBackslash", KEY_102ND},
                                              {"IntlYen", KEY_YEN},
                                              {"IntlRo", KEY_RO},
                                              {"NumpadComma", KEY_KPCOMMA},
                                              {"Convert", KEY_HENKAN},
                                              {"NonConvert", KEY_MUHENKAN},
                                              {"KanaMode", KEY_KATAKANAHIRAGANA},
                                              {"ZenkakuHankaku", KEY_ZENKAKUHANKAKU},
                                              {"ControlLeft", KEY_LEFTCTRL},
                                              {"ControlRight", KEY_RIGHTCTRL},
                                              {"ShiftLeft", KEY_LEFTSHIFT},
                                              {"ShiftRight", KEY_RIGHTSHIFT},
                                              {"AltLeft", KEY_LEFTALT},
                                              {"AltRight", KEY_RIGHTALT},
                                              {"MetaLeft", KEY_LEFTMETA},
                                              {"MetaRight", KEY_RIGHTMETA},
                                              {"ContextMenu", KEY_COMPOSE},
                                              {"CapsLock", KEY_CAPSLOCK},
                                              {"NumLock", KEY_NUMLOCK},
                                              {"ScrollLock", KEY_SCROLLLOCK},
                                              {"PrintScreen", KEY_SYSRQ},
                                              {"Pause", KEY_PAUSE},
                                              {"F1", KEY_F1},
                                              {"F2", KEY_F2},
                                              {"F3", KEY_F3},
                                              {"F4", KEY_F4},
                                              {"F5", KEY_F5},
                                              {"F6", KEY_F6},
                                              {"F7", KEY_F7},
                                              {"F8", KEY_F8},
                                              {"F9", KEY_F9},
                                              {"F10", KEY_F10},
                                              {"F11", KEY_F11},
                                              {"F12", KEY_F12},
                                              {"Insert", KEY_INSERT},
                                              {"Delete", KEY_DELETE},
                                              {"Home", KEY_HOME},
                                              {"End", KEY_END},
                                              {"PageUp", KEY_PAGEUP},
                                              {"PageDown", KEY_PAGEDOWN},
                                              {"ArrowLeft", KEY_LEFT},
                                              {"ArrowRight", KEY_RIGHT},
                                              {"ArrowUp", KEY_UP},
                                              {"ArrowDown", KEY_DOWN},
                                              {"Numpad0", KEY_KP0},
                                              {"Numpad1", KEY_KP1},
                                              {"Numpad2", KEY_KP2},
                                              {"Numpad3", KEY_KP3},
                                              {"Numpad4", KEY_KP4},
                                              {"Numpad5", KEY_KP5},
                                              {"Numpad6", KEY_KP6},
                                              {"Numpad7", KEY_KP7},
                                              {"Numpad8", KEY_KP8},
                                              {"Numpad9", KEY_KP9},
                                              {"NumpadDecimal", KEY_KPDOT},
                                              {"NumpadAdd", KEY_KPPLUS},
                                              {"NumpadSubtract", KEY_KPMINUS},
                                              {"NumpadMultiply", KEY_KPASTERISK},
                                              {"NumpadDivide", KEY_KPSLASH},
                                              {"NumpadEnter", KEY_KPENTER}};
void checked_ioctl(int fd, unsigned long request, int value) {
    if (ioctl(fd, request, value) < 0) {
        throw std::system_error(errno, std::generic_category(), "uinput setup");
    }
}
void write_event(int fd, unsigned short type, unsigned short code, int value) {
    input_event event{};
    event.type = type;
    event.code = code;
    event.value = value;
    ssize_t written;
    do {
        written = write(fd, &event, sizeof(event));
    } while (written < 0 && errno == EINTR);
    if (written != sizeof(event)) {
        throw std::runtime_error("uinput event write failed");
    }
}
} // namespace

int key_code(const std::string& name) {
    const auto found = key_codes.find(name);
    return found == key_codes.end() ? 0 : found->second;
}
bool is_modifier(int code) {
    return code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTSHIFT ||
           code == KEY_RIGHTSHIFT || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
           code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}
UInputSink::UInputSink() {
    fd_ = open("/dev/uinput", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/uinput");
    }
    try {
        checked_ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        checked_ioctl(fd_, UI_SET_EVBIT, EV_LED);
        checked_ioctl(fd_, UI_SET_LEDBIT, LED_NUML);
        for (const auto& [name, code] : key_codes) {
            (void)name;
            checked_ioctl(fd_, UI_SET_KEYBIT, code);
        }
        // Repeat is scheduled by KeyboardState. Enabling EV_REP would double it.
        uinput_setup setup{};
        setup.id.bustype = BUS_VIRTUAL;
        std::strncpy(setup.name, "Full Keyboard", sizeof(setup.name) - 1);
        if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
            throw std::system_error(errno, std::generic_category(), "create virtual keyboard");
        }
    } catch (...) {
        close(fd_);
        fd_ = -1;
        throw;
    }
}
UInputSink::~UInputSink() {
    if (fd_ < 0) {
        return;
    }
    // Destruction also runs after a write failure; never throw during cleanup.
    for (int code : held_) {
        try {
            write_event(fd_, EV_KEY, static_cast<unsigned short>(code), 0);
        } catch (...) {
        }
    }
    try {
        write_event(fd_, EV_SYN, SYN_REPORT, 0);
    } catch (...) {
    }
    ioctl(fd_, UI_DEV_DESTROY);
    if (led_fd_ >= 0) {
        close(led_fd_);
    }
    close(fd_);
}
fs::path UInputSink::event_node() const {
    char name[128]{};
    if (ioctl(fd_, UI_GET_SYSNAME(sizeof(name)), name) < 0) {
        throw std::runtime_error("cannot identify the created input device");
    }
    const fs::path directory = fs::path("/sys/devices/virtual/input") / name;
    if (!fs::exists(directory)) {
        return {};
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto node = entry.path().filename().string();
        if (node.starts_with("event")) {
            return fs::path("/dev/input") / node;
        }
    }
    return {};
}
void UInputSink::send(int code, int value) {
    if (code <= 0 || code > KEY_MAX || value < 0 || value > 2) {
        throw std::runtime_error("invalid key event");
    }
    // Remember down before writing so partial delivery is still released on failure.
    if (value == 1) {
        held_.insert(code);
    }
    write_event(fd_, EV_KEY, static_cast<unsigned short>(code), value);
    write_event(fd_, EV_SYN, SYN_REPORT, 0);
    if (value == 0) {
        held_.erase(code);
    }
}
bool UInputSink::pump() {
    // LED changes are delivered through uinput. Query only our own event node
    // as well, since Linux suppresses an initial LED-off event.
    if (led_fd_ < 0) {
        const auto path = event_node();
        if (!path.empty()) {
            led_fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
    }
    unsigned long leds{};
    if (led_fd_ >= 0 && ioctl(led_fd_, EVIOCGLED(sizeof(leds)), &leds) >= 0) {
        num_lock_ = (leds & (1ul << LED_NUML)) != 0;
    }
    input_event event{};
    while (read(fd_, &event, sizeof(event)) == sizeof(event)) {
        if (event.type == EV_LED && event.code == LED_NUML) {
            num_lock_ = event.value != 0;
        }
    }
    return true;
}

LanguageMap::LanguageMap(const Language& language) : language_(language) {
    context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context_) {
        throw std::runtime_error("cannot create XKB context");
    }
    xkb_rule_names names{language.rules.c_str(), language.model.c_str(), language.keymap.c_str(),
                         language.variant.c_str(), language.options.c_str()};
    keymap_ = xkb_keymap_new_from_names(context_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap_) {
        xkb_context_unref(context_);
        context_ = nullptr;
        throw std::runtime_error("language keymap is unavailable");
    }
}
LanguageMap::~LanguageMap() {
    if (compose_) {
        xkb_compose_state_unref(compose_);
    }
    if (keymap_) {
        xkb_keymap_unref(keymap_);
    }
    if (context_) {
        xkb_context_unref(context_);
    }
}
xkb_keysym_t LanguageMap::symbol(const Key& key, const std::set<int>& modifiers, bool caps,
                                 bool num) const {
    const int code = key_code(key.action);
    // XKB's evdev rules reserve keycodes 0..7. This offset is unrelated to Steam codes.
    auto* state = xkb_state_new(keymap_);
    if (!state) {
        return XKB_KEY_NoSymbol;
    }
    for (int modifier : modifiers) {
        xkb_state_update_key(state, static_cast<xkb_keycode_t>(modifier + 8), XKB_KEY_DOWN);
    }
    if (caps) {
        xkb_state_update_key(state, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
        xkb_state_update_key(state, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    }
    if (num) {
        xkb_state_update_key(state, KEY_NUMLOCK + 8, XKB_KEY_DOWN);
        xkb_state_update_key(state, KEY_NUMLOCK + 8, XKB_KEY_UP);
    }
    const auto symbol = xkb_state_key_get_one_sym(state, static_cast<xkb_keycode_t>(code + 8));
    xkb_state_unref(state);
    return symbol;
}
bool LanguageMap::printable(xkb_keysym_t symbol) {
    const auto ch = xkb_keysym_to_utf32(symbol);
    return (ch >= 32 && ch != 127) || (symbol >= XKB_KEY_dead_grave && symbol <= XKB_KEY_dead_greek);
}
std::string LanguageMap::compose(xkb_keysym_t symbol) {
    if (!compose_) {
        auto* table =
            xkb_compose_table_new_from_locale(context_, "C.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (!table) {
            throw std::runtime_error("Unicode accent composition table unavailable");
        }
        compose_ = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb_compose_table_unref(table);
        if (!compose_) {
            throw std::runtime_error("Cannot initialize accent composition");
        }
    }
    xkb_compose_state_feed(compose_, symbol);
    char buffer[128]{};
    switch (xkb_compose_state_get_status(compose_)) {
    case XKB_COMPOSE_COMPOSING:
        return {};
    case XKB_COMPOSE_COMPOSED:
        xkb_compose_state_get_utf8(compose_, buffer, sizeof(buffer));
        xkb_compose_state_reset(compose_);
        return buffer;
    case XKB_COMPOSE_CANCELLED:
        xkb_compose_state_reset(compose_);
        throw std::runtime_error("This accent combination is unavailable; try again");
    default:
        xkb_keysym_to_utf8(symbol, buffer, sizeof(buffer));
        return buffer;
    }
}
bool LanguageMap::composing() const {
    return compose_ && xkb_compose_state_get_status(compose_) == XKB_COMPOSE_COMPOSING;
}
void LanguageMap::cancel_compose() {
    if (compose_) {
        xkb_compose_state_reset(compose_);
    }
}
std::string LanguageMap::legend(const Key& key, const std::set<int>& modifiers, bool caps,
                                bool num) const {
    if (const auto it = language_.overrides.find(key.id); it != language_.overrides.end()) {
        return it->second;
    }
    if (key.action_kind != ActionKind::Key || key.action == "Space") {
        return key.label;
    }
    const auto symbol = this->symbol(key, modifiers, caps, num);
    char buffer[64]{};
    const int size = xkb_keysym_to_utf8(symbol, buffer, sizeof(buffer));
    // Dead keys have no standalone UTF-8 output. Show the accent the target
    // keymap will compose, instead of falling back to a misleading US legend.
    switch (symbol) {
    case XKB_KEY_dead_grave:
        return "`";
    case XKB_KEY_dead_acute:
        return "´";
    case XKB_KEY_dead_circumflex:
        return "^";
    case XKB_KEY_dead_tilde:
        return "~";
    case XKB_KEY_dead_diaeresis:
        return "¨";
    case XKB_KEY_dead_cedilla:
        return "¸";
    default:
        break;
    }
    if (size > 0 && size < static_cast<int>(sizeof(buffer)) &&
        static_cast<unsigned char>(buffer[0]) >= 32 && static_cast<unsigned char>(buffer[0]) != 127) {
        return buffer;
    }
    return key.label;
}

KeyboardState::KeyboardState(OutputController& output) : sink_(output), output_(&output) {
}
KeyboardState::~KeyboardState() {
    try {
        cancel_all();
    } catch (...) {
    }
}
void KeyboardState::acquire(int code) {
    auto& count = references_[code];
    if (count++ == 0) {
        if (!output_) {
            sink_.send(code, 1);
        }
        // Both controllers can hold the same lock key. Its state changes once
        // per backend key-down, not once per pointer.
        if (code == KEY_CAPSLOCK) {
            caps_ = !caps_;
        }
        if (code == KEY_NUMLOCK) {
            num_ = !num_;
        }
    }
}
void KeyboardState::release(int code) {
    auto found = references_.find(code);
    if (found == references_.end()) {
        return;
    }
    if (--found->second == 0) {
        if (!output_) {
            sink_.send(code, 0);
        }
        references_.erase(found);
    }
}
bool KeyboardState::down(unsigned pointer, const Key& key, double now, bool local, int native_code) {
    if (presses_.contains(pointer)) {
        return false;
    }
    // A second pointer must not turn Copy into Ctrl+Shift+C while a chord is held.
    if (key.action_kind == ActionKind::Shortcut && !references_.empty()) {
        return false;
    }
    Press press;
    press.id = key.id;
    const int code =
        key.action_kind == ActionKind::Key ? (native_code ? native_code : key_code(key.action)) : 0;
    if (is_modifier(code)) {
        press.modifier = code;
        press.used = !key.sticky || references_.contains(code);
        if (!key.sticky) {
            // A momentary Super key must reach the OS even without a chord.
            // Keep its own reference so overlapping hands share one down/up.
            press.codes.push_back(code);
        }
        presses_.emplace(pointer, std::move(press));
        if (!key.sticky) {
            acquire(code);
            if (output_) {
                output_->native_press(pointer, {code}, 0, now);
            }
        }
        return true;
    }
    // A modifier tap latches visually but emits nothing until used. This avoids
    // leaving Ctrl physically held while the user navigates another application.
    auto modifiers_used = modifiers();
    if (key.action_kind == ActionKind::Shortcut) {
        modifiers_used = {KEY_LEFTCTRL};
    }
    for (int modifier : modifiers_used) {
        press.codes.push_back(modifier);
    }
    const int action_code = key.action_kind == ActionKind::Shortcut
                                ? (native_code ? native_code : (key.action == "copy" ? KEY_C : KEY_V))
                                : code;
    press.codes.push_back(action_code);
    if (local) {
        if (code == KEY_CAPSLOCK || code == KEY_NUMLOCK) {
            const bool already_held =
                std::any_of(presses_.begin(), presses_.end(),
                            [code](const auto& item) { return item.second.local_lock == code; });
            press.local_lock = code;
            if (!already_held) {
                if (code == KEY_CAPSLOCK) {
                    caps_ = !caps_;
                } else {
                    num_ = !num_;
                }
            }
        }
        press.codes.clear(); // Local IME keeps visuals/holds but emits no physical keys.
    }
    if (!local && key.action_kind == ActionKind::Key && code != KEY_CAPSLOCK && code != KEY_NUMLOCK &&
        code != KEY_SCROLLLOCK && code != KEY_PAUSE && code != KEY_SYSRQ) {
        press.repeat_code = code;
        press.repeat_at = now + .5;
    }
    for (auto& [id, held] : presses_) {
        (void)id;
        if (held.modifier) {
            held.used = true;
        }
    }
    latched_.clear();
    // Store the release sequence first so an exception cannot strand an earlier down.
    presses_.emplace(pointer, press);
    for (int key_value : press.codes) {
        acquire(key_value);
    }
    if (output_) {
        output_->native_press(pointer, press.codes, press.repeat_code, now);
    }
    return true;
}

bool KeyboardState::up(unsigned pointer) {
    auto found = presses_.find(pointer);
    if (found == presses_.end()) {
        return false;
    }
    auto& press = found->second;
    if (press.modifier && press.used) {
        const bool another_hold = std::any_of(presses_.begin(), presses_.end(), [&](const auto& item) {
            return item.first != pointer && item.second.modifier == press.modifier;
        });
        if (!another_hold) {
            if (output_) {
                output_->release_modifier(press.modifier);
            }
            // Chords borrow held modifiers. Releasing the modifier must remove
            // those borrowed references now, even if the ordinary key stays down.
            for (auto& [id, held] : presses_) {
                (void)id;
                const auto code = std::find(held.codes.begin(), held.codes.end(), press.modifier);
                if (code != held.codes.end()) {
                    release(*code);
                    held.codes.erase(code);
                }
            }
        }
    }
    if (press.modifier && !press.used) {
        if (!latched_.erase(press.modifier)) {
            latched_.insert(press.modifier);
        }
    }
    // Release the ordinary key before its modifiers, including shared chords.
    for (auto it = press.codes.rbegin(); it != press.codes.rend(); ++it) {
        release(*it);
    }
    if (output_) {
        output_->release(pointer);
    }
    presses_.erase(found);
    return true;
}

void KeyboardState::cancel_pointer(unsigned pointer) {
    if (auto found = presses_.find(pointer); found != presses_.end()) {
        // An interrupted modifier is not a tap and must never become latched.
        found->second.used = true;
        up(pointer);
    }
}
void KeyboardState::cancel_all() {
    if (output_) {
        output_->cancel();
    }
    std::exception_ptr failure;
    // Release non-modifiers first; try every release even if one backend call fails.
    for (bool modifiers_only : {false, true}) {
        for (const auto& [code, count] : references_) {
            (void)count;
            if (is_modifier(code) != modifiers_only) {
                continue;
            }
            try {
                if (!output_) {
                    sink_.send(code, 0);
                }
            } catch (...) {
                failure = std::current_exception();
            }
        }
    }
    references_.clear();
    presses_.clear();
    latched_.clear();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
bool KeyboardState::tick(double now) {
    if (output_) {
        return false;
    }
    std::set<int> repeated;
    for (auto& [pointer, press] : presses_) {
        (void)pointer;
        if (press.repeat_code && now >= press.repeat_at) {
            if (repeated.insert(press.repeat_code).second) {
                sink_.send(press.repeat_code, 2);
            }
            // Never replay a backlog after a stall or sleep.
            press.repeat_at = now + .04;
        }
    }
    return false;
}
bool KeyboardState::pressed(const std::string& id) const {
    return std::any_of(presses_.begin(), presses_.end(),
                       [&](const auto& item) { return item.second.id == id; });
}
std::set<int> KeyboardState::modifiers() const {
    auto result = latched_;
    for (const auto& [pointer, press] : presses_) {
        (void)pointer;
        if (press.modifier) {
            result.insert(press.modifier);
        }
    }
    for (const auto& [code, count] : references_) {
        (void)count;
        if (is_modifier(code)) {
            result.insert(code);
        }
    }
    return result;
}
} // namespace framekeyboard
