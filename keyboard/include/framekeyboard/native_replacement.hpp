#pragma once
#include "input.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sys/types.h>
#include <utility>

namespace framekeyboard {
// Version 1: one app-scoped helper, private SOCK_SEQPACKET IPC, metadata-only status.
// Frame IPC is nonblocking; hidden idle sleep can wake on session metadata.
// CDP waits stay in the helper. Steam restores its own fields; an
// app-scoped OpenVR guard restores exact Gamescope overlays independently.
class NativeReplacement : public KeySink {
  public:
    // An owned IPC endpoint can be supplied by isolated receiver tests.
    using Connect = std::function<int()>;
    explicit NativeReplacement(KeySink& local, int transport = -1, Connect connect = {})
        : local_(local), connect_(std::move(connect)), fd_(transport) {}
    ~NativeReplacement() override;
    struct State {
        std::uint64_t request{};
        bool open{}, eligible{}, active{}, available{}, local{};
    };
    void poll(double now);
    // Between hidden frames, wake the idle sleep when session metadata changes.
    void wait_until(std::chrono::steady_clock::time_point deadline);
    void update(bool enabled, bool ready, std::uint64_t acknowledgement, bool fallback, double now,
                bool watch_main_loop = false);
    const State& state() const { return state_; }
    const char* unavailable_reason() const;
    void route(bool remote) { remote_ = remote; }
    bool remote() const { return remote_; }
    void dismiss_request();
    // A prepared update closes the current request instead of reopening native.
    // All other shutdowns retain independent native recovery.
    void shutdown_for_update();
    void retry_now(double now);
    bool take_unsupported_control();
    // Prepare before the VR watchdog starts; descriptors remain stable until shutdown.
    bool prepare_watchdog();
    void watchdog_tick(double now, double slow_work_deadline = 0) const;
    // Main-thread scopes identify external runtime waits. Nested calls share a
    // fixed frame deadline; returning grants only a short completion margin.
    void begin_runtime_wait(double now);
    void end_runtime_wait(double now);
    void frame_completed();
    double runtime_wait_deadline() const { return runtime_until_.load(); }
    void send(int code, int value) override;
    Delivery deliver_key(int code, int value) override;
    void cancel_delivery() override;
    bool pump() override { return local_.pump(); }
    bool take_input_reset() override;
    bool can_resume() const override { return local_.can_resume(); }
    bool text_available() override { return local_.text_available(); }
    bool commit_text(const std::string& text) override;
    bool text_pending() const override { return pending_id_ != 0 || local_.text_pending(); }
    void cancel_text() override;
    bool output_blocked() const override { return !remote_ && local_.output_blocked(); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return local_.shortcut_code(symbol, fallback);
    }
    std::optional<bool> num_lock_state() override { return local_.num_lock_state(); }

  private:
    enum class Failure {
        None,
        DebuggerUnavailable,
        DebuggerBlocked,
        SteamIncompatible,
        InstallTimeout,
        ReplyTimeout,
        ReplacementBusy,
        HelperUnavailable,
        ConnectionLost
    };
    bool packet(const std::string& data);
    bool start(double now);
    const char* failure_code() const;
    void diagnostic(const char* code, double now, std::uint64_t request = 0, int retry = 0);
    void schedule_retry(double now);
    void stop(bool restore_native = true);
    KeySink& local_;
    Connect connect_;
    int fd_{-1};
    pid_t child_{-1};
    int watchdog_read_{-1}, watchdog_write_{-1};
    std::atomic<double> runtime_until_{};
    double runtime_frame_deadline_{};
    unsigned runtime_depth_{};
    State state_;
    Failure failure_{Failure::None};
    bool remote_{}, reset_{}, force_fallback_{}, unsupported_control_{};
    double last_reply_{}, install_until_{}, next_start_{}, now_{}, pending_since_{};
    double retry_delay_{5};
    double log_window_{-1}, last_failure_log_{-60}, logged_retry_{};
    unsigned log_lines_{};
    Failure logged_failure_{Failure::None};
    std::uint64_t event_sequence_{};
    bool enabled_{};
    std::uint64_t progress_{}, sequence_{}, pending_id_{};
    std::string pending_text_;
    std::optional<bool> pending_result_;
    std::uint64_t native_id_{};
    int native_code_{}, native_value_{};
    double native_since_{};
    std::optional<bool> native_result_;
    struct RemoteKey {
        std::uint64_t id, request;
        bool pending;
    };
    std::map<int, RemoteKey> remote_keys_;
    std::set<int> modifiers_, local_keys_;
};

// Keep panel dismissal separate from process lifetime. All visibility decisions use the
// same request snapshot, so closing X cannot acknowledge or reopen the old request.
class ReplacementPresentation {
  public:
    explicit ReplacementPresentation(bool start_hidden = false) : startup_hidden_(start_hidden) {}
    void update(bool enabled, NativeReplacement::State state) {
        if (enabled != enabled_) {
            managed_ = dismissed_ = false;
            bypass_request_ = dismissed_request_ = 0;
        }
        enabled_ = enabled;
        state_ = state;
        if (!state.available) {
            bypass_request_ = dismissed_request_ = 0;
        }
        if (enabled && state.available && state.open) {
            managed_ = true;
            startup_hidden_ = false;
        }
    }
    void dismiss() {
        managed_ = dismissed_ = true;
        dismissed_request_ = state_.request;
    }
    void summon() {
        startup_hidden_ = false;
        dismissed_ = false;
        if (!state_.open) {
            managed_ = false;
        }
    }
    void fallback() {
        bypass_request_ = state_.request;
        managed_ = true;
    }
    bool show() const {
        return !startup_hidden_ &&
               (!enabled_ || (!managed_ && !dismissed_) ||
                (state_.available && state_.open && state_.eligible &&
                 state_.request != bypass_request_ && state_.request != dismissed_request_));
    }
    bool interactive() const {
        return !startup_hidden_ &&
               (!enabled_ || (!managed_ && !dismissed_) || (show() && state_.active));
    }
    bool remote() const { return enabled_ && managed_ && show() && state_.active && !state_.local; }
    bool fallback_requested() const { return state_.open && state_.request == bypass_request_; }

  private:
    NativeReplacement::State state_;
    bool enabled_{}, managed_{}, dismissed_{};
    bool startup_hidden_{};
    std::uint64_t bypass_request_{}, dismissed_request_{};
};
} // namespace framekeyboard
