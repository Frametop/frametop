#pragma once

#include "config.hpp"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

namespace framekeyboard {
class OutputController;
enum class Delivery { Waiting, Success, Failure };
int key_code(const std::string& name);
bool is_modifier(int code);

// The sink receives Linux evdev codes. No renderer or profile can bypass this boundary.
class KeySink {
  public:
    virtual ~KeySink() = default;
    virtual void send(int code, int value) = 0;
    // Repeating the same operation consumes its asynchronous result, not another key.
    // Releases always execute immediately, even with an outstanding down result.
    virtual Delivery deliver_key(int code, int value) {
        send(code, value);
        return Delivery::Success;
    }
    virtual void cancel_delivery() { cancel_text(); }
    virtual bool pump() { return true; }
    // A pause/resume can happen within one pump. The app must still discard old holds.
    virtual bool take_input_reset() { return false; }
    virtual bool can_resume() const { return false; }
    virtual bool text_available() { return false; }
    virtual bool commit_text(const std::string&) { return false; }
    // Optional asynchronous transports retain one commit until the caller consumes its result.
    virtual bool text_pending() const { return false; }
    virtual void cancel_text() {}
    // An outstanding text request can still order native downs after the caller
    // cancels or consumes a timeout. Releases remain allowed through this barrier.
    virtual bool output_blocked() const { return false; }
    virtual int shortcut_code(xkb_keysym_t, int fallback) { return fallback; }
    // Unknown until the receiving system reports its lock state.
    virtual std::optional<bool> num_lock_state() { return {}; }
};
class NullSink : public KeySink {
  public:
    void send(int, int) override {};
};
class UInputSink : public KeySink {
  public:
    UInputSink();
    ~UInputSink() override;
    UInputSink(const UInputSink&) = delete;
    UInputSink& operator=(const UInputSink&) = delete;
    void send(int code, int value) override;
    bool pump() override;
    std::optional<bool> num_lock_state() override { return num_lock_; }
    fs::path event_node() const;

  private:
    int fd_{-1};
    int led_fd_{-1};
    std::set<int> held_;
    std::optional<bool> num_lock_;
};

class LanguageMap {
  public:
    explicit LanguageMap(const Language& language);
    ~LanguageMap();
    LanguageMap(const LanguageMap&) = delete;
    LanguageMap& operator=(const LanguageMap&) = delete;
    std::string legend(const Key& key, const std::set<int>& modifiers, bool caps, bool num) const;
    xkb_keysym_t symbol(const Key& key, const std::set<int>& modifiers, bool caps, bool num) const;
    static bool printable(xkb_keysym_t symbol);
    std::string compose(xkb_keysym_t symbol);
    bool composing() const;
    void cancel_compose();

  private:
    Language language_;
    xkb_context* context_{};
    xkb_keymap* keymap_{};
    xkb_compose_state* compose_{};
};

class KeyboardState {
  public:
    explicit KeyboardState(KeySink& sink) : sink_(sink) {}
    explicit KeyboardState(OutputController& output);
    ~KeyboardState();
    bool down(unsigned pointer, const Key& key, double now, bool local = false, int native_code = 0);
    bool up(unsigned pointer);
    bool pointer_pressed(unsigned pointer) const { return presses_.contains(pointer); }
    void cancel_pointer(unsigned pointer);
    void cancel_all();
    bool tick(double now);
    bool pressed(const std::string& id) const;
    std::set<int> modifiers() const;
    bool caps() const { return caps_; }
    bool num() const { return num_; }
    // Restore the local lock state without sending a lock key to the target.
    void set_num(bool enabled) { num_ = enabled; }
    bool active() const { return !presses_.empty(); }

  private:
    struct Press {
        std::string id;
        int modifier{}, local_lock{};
        bool used{};
        std::vector<int> codes;
        int repeat_code{};
        double repeat_at{};
    };
    void acquire(int code);
    void release(int code);
    KeySink& sink_;
    OutputController* output_{};
    std::map<unsigned, Press> presses_;
    std::map<int, unsigned> references_;
    std::set<int> latched_;
    bool caps_{false}, num_{false};
};
} // namespace framekeyboard
