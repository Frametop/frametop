#pragma once

#include "input.hpp"
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace framekeyboard {
// Frametop integration (FRAMETOP.md). Frametop runs Full Keyboard as its keyboard: its
// input relay says where typing goes (the Frametop desktop or Steam) and when the panel
// should open for a desktop text field, and takes the desktop's typing.
//
// Datagrams on the relay's abstract socket @frametop_relay; it replies to our autobound
// address. We send "kbhello OPEN" (the panel is up for Frametop 0|1) every second and on a
// change, "kbkey CODE 0|1" and "kbtext TEXT". It sends "kb DESKTOP SURFACE" (typing goes
// to the desktop 0|1, the desktop's screens show and Frametop isn't paused 0|1) in reply
// and on a change, "kb show|hide|toggle", and "kb poke SIDE VERB [x y z]" (a fingertip on
// the panel, from ft-screens' hand input: its point in the standing universe).
// Only messages from that name and this user count, and typing goes there only while
// its word is fresh, so a stopped relay never leaves desktop typing addressed to a name
// another process could take.
// A fingertip on the panel: ft-screens' hand input decides hovers and presses, as on its
// own screens, so a hand types here with no laser.
struct HandPoke {
    enum class Verb { Hover, Press, Move, Up, End };
    std::size_t hand{}; // 0 left, 1 right
    Verb verb{};
    std::optional<std::array<double, 3>> point;
};
std::optional<HandPoke> parse_hand_poke(const std::string& text);

class FrametopLink {
  public:
    using Clock = std::function<double()>;
    explicit FrametopLink(std::string relay_name = "frametop_relay", Clock clock = {});
    ~FrametopLink();
    FrametopLink(const FrametopLink&) = delete;
    FrametopLink& operator=(const FrametopLink&) = delete;
    enum class Request { None, Show, Hide, Toggle };
    // Read what the relay sent, and say hello when it's time. Never blocks.
    void poll();
    bool fresh() const;
    bool desktop() const { return fresh() && desktop_; }
    bool surface() const { return fresh() && surface_; }
    Request take_request();
    std::vector<HandPoke> take_pokes() { return std::exchange(pokes_, {}); }
    // Whether the panel is up for Frametop: said in the next hello, at once on a change.
    void set_open(bool open);
    bool send(const std::string& message);
    int fd() const { return fd_; }
    static constexpr double stale_after = 3;

  private:
    std::string relay_;
    Clock clock_;
    int fd_{-1};
    double heard_{-1e9}, hello_{-1e9};
    bool desktop_{}, surface_{}, open_{};
    Request request_{Request::None};
    std::vector<HandPoke> pokes_;
};

// Desktop typing goes to the relay (which types it on the desktop's seat, in the desktop's
// layout); everything else goes to `steam` (gamescope). A held key is released where it
// was pressed, whichever way typing goes by then.
class FrametopSink : public KeySink {
  public:
    FrametopSink(FrametopLink& link, KeySink& steam) : link_(link), steam_(steam) {}
    ~FrametopSink() override;
    void send(int code, int value) override;
    bool pump() override;
    bool take_input_reset() override { return steam_.take_input_reset(); }
    bool can_resume() const override { return steam_.can_resume(); }
    bool text_available() override { return link_.desktop() || steam_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool text_pending() const override { return steam_.text_pending(); }
    void cancel_text() override { steam_.cancel_text(); }
    bool output_blocked() const override { return !link_.desktop() && steam_.output_blocked(); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        // The desktop types keys by their place, like a real keyboard on it.
        return link_.desktop() ? fallback : steam_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override {
        return link_.desktop() ? std::nullopt : steam_.num_lock_state();
    }

  private:
    FrametopLink& link_;
    KeySink& steam_;
    std::map<int, bool> held_; // code -> pressed on the desktop
};
} // namespace framekeyboard
