#pragma once

#include "input.hpp"
#include <deque>

namespace framekeyboard {
// One field-scoped output session. Pointer/legend state stays in KeyboardState;
// transport protocol state stays in the sinks. Only this class orders delivery.
class OutputController : public KeySink {
  public:
    explicit OutputController(KeySink& target) : target_(target) {}
    ~OutputController() override;
    static constexpr std::size_t capacity = 32;
    bool admit(bool produces_output, double now);
    bool admit(bool produces_output) { return admit(produces_output, now_); }
    void native_press(unsigned pointer, const std::vector<int>& codes, int repeat_code, double now);
    bool text_press(unsigned pointer, const std::string& text, double now);
    void release(unsigned pointer);
    void release_modifier(int code);
    void cancel();
    bool tick(double now);
    bool retry();
    bool retry_available() const { return !retry_.empty(); }
    std::string retry_text() const;
    std::optional<std::string> take_status();
    void set_enabled(bool value);
    bool enabled() const { return enabled_; }

    // Lock synchronization uses the same ordered path as ordinary presses.
    void send(int code, int value) override;
    bool pump() override { return target_.pump(); }
    bool take_input_reset() override { return target_.take_input_reset(); }
    bool can_resume() const override { return target_.can_resume(); }
    bool text_available() override { return target_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool text_pending() const override { return !queue_.empty(); }
    void cancel_text() override { cancel(); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return target_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return target_.num_lock_state(); }

  private:
    struct Entry {
        std::string text;
        std::vector<int> codes, acquired;
        std::set<int> released_modifiers;
        std::size_t next{};
        int repeat_code{};
        bool held{}, started{}, waiting{}, repeat{};
        double repeat_at{};
        std::weak_ptr<Entry> owner;
    };
    bool enqueue_text(const std::string& text, std::shared_ptr<Entry> entry = {});
    void drain();
    void fail();
    void drop_holds();
    void release_codes(Entry& entry, std::optional<int> only = {});
    void status(std::string message);
    KeySink& target_;
    std::deque<std::shared_ptr<Entry>> queue_, retry_;
    std::map<unsigned, std::shared_ptr<Entry>> active_;
    std::map<int, std::shared_ptr<Entry>> direct_;
    std::map<int, unsigned> delivered_;
    double now_{};
    bool enabled_{}, failed_{};
    std::string status_;
    bool status_changed_{};
};
} // namespace framekeyboard
