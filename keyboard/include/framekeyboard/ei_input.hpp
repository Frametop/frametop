#pragma once
#include "input.hpp"
#include "text_input.hpp"
#include <libei.h>

namespace framekeyboard {
// Infer only the documented gamescope-N-ei naming convention. Unknown names
// require an explicit text socket; an empty result disables text delivery.
fs::path gamescope_text_socket(const fs::path& input_socket, const fs::path& explicit_text = {});

// Connect directly to the running Gamescope compositor. This avoids the
// SteamVR startup-time discovery limitation of a newly created uinput device.
class EiSink : public KeySink {
  public:
    explicit EiSink(const fs::path& socket, const fs::path& text_socket = {},
                    GamescopeText::Clock clock = {});
    ~EiSink() override;
    void send(int code, int value) override;
    bool pump() override;
    bool take_input_reset() override;
    bool can_resume() const override { return true; }
    bool text_available() override;
    bool commit_text(const std::string& text) override;
    bool text_pending() const override { return text_ && text_->pending(); }
    bool output_blocked() const override { return text_ && text_->busy(); }
    void cancel_text() override {
        if (text_) {
            text_->cancel();
        }
    }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override;
    std::optional<bool> num_lock_state() override { return num_lock_; }
    EiSink(const EiSink&) = delete;
    EiSink& operator=(const EiSink&) = delete;

  private:
    void read_keymap();
    void service_text();
    bool connect();
    void connection_lost();
    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> xkb_{nullptr, xkb_context_unref};
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> keymap_{nullptr, xkb_keymap_unref};
    xkb_layout_index_t group_{};
    std::optional<bool> num_lock_;
    fs::path socket_, text_socket_;
    std::unique_ptr<GamescopeText> text_;
    GamescopeText::Clock clock_;
    double text_retry_at_{}, text_retry_delay_{1};
    double retry_at_{}, retry_delay_{1}, connect_deadline_{};
    ei* context_{};
    ei_device* keyboard_{};
    bool resumed_{}, disconnected_{}, input_reset_{};
    std::uint32_t sequence_{};
    std::set<int> held_;
};
} // namespace framekeyboard
